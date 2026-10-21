#include "usbip_server.h"
#include "crashlog.h"
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <string.h>

// ============================================================
//  USB/IP 服务端（v2.8.0）
//  协议格式见 usbip_server.h 的详细注释。
//  ⚠️ 所有多字节字段都是**大端**（网络字节序）—— 与 Linux 的 usbip 一致。
// ============================================================

#define USBIP_VERSION        0x0111
#define OP_REQ_DEVLIST       0x8005
#define OP_REP_DEVLIST       0x0005
#define OP_REQ_IMPORT        0x8003
#define OP_REP_IMPORT        0x0003

#define USBIP_CMD_SUBMIT     0x0001
#define USBIP_CMD_UNLINK      0x0002
#define USBIP_RET_SUBMIT     0x0003
#define USBIP_RET_UNLINK      0x0004

#define ST_OK                0x00000000
#define ST_NA                0x00000001

#define USBIP_DIR_OUT        0
#define USBIP_DIR_IN         1

// URB 头 48 字节
#define URB_HDR_LEN          48
// RET_SUBMIT / RET_UNLINK 头也是 48 字节
#define RET_HDR_LEN          48

// 设备信息固定部分 312 字节 + 每个接口 4 字节
#define DEVINFO_FIXED_LEN    312
#define IFACE_INFO_LEN       4

// --- 大端写入辅助 ---
static inline void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v);
}
static inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint16_t get16(const uint8_t* p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t get32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// ---------------------------------------------------------------
bool UsbIpServer::begin(uint16_t port) {
  _port = port;
  _listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (_listenFd < 0) { Serial.println("[USBIP] socket 失败"); return false; }
  int yes = 1;
  setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_port);
  if (bind(_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    Serial.printf("[USBIP] 绑定 %u 端口失败\n", (unsigned)_port);
    close(_listenFd); _listenFd = -1;
    return false;
  }
  if (listen(_listenFd, 2) < 0) {
    Serial.println("[USBIP] listen 失败");
    close(_listenFd); _listenFd = -1;
    return false;
  }
  fcntl(_listenFd, F_SETFL, fcntl(_listenFd, F_GETFL, 0) | O_NONBLOCK);
  Serial.printf("[USBIP] USB/IP 服务已启动 :%u（供远端 Linux 的 CUPS 接管）\n", (unsigned)_port);
  return true;
}

void UsbIpServer::task() {
  if (_listenFd < 0) return;
  if (_clientFd < 0) {
    _acceptClient();
    return;
  }
  // 有客户端时读一个报文
  uint8_t peek;
  int r = recv(_clientFd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
  if (r == 0) { _closeClient(); return; }
  if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) _closeClient(); return; }
  if (_attached) _handleUrb();
  else         _handleOperation();
}

bool UsbIpServer::_acceptClient() {
  struct sockaddr_in peer;
  socklen_t plen = sizeof(peer);
  int fd = accept(_listenFd, (struct sockaddr*)&peer, &plen);
  if (fd < 0) return false;
  struct timeval tv = { 20, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  _clientFd = fd;
  _attached = false;
  char ip[16] = {0};
  strncpy(ip, inet_ntoa(peer.sin_addr), sizeof(ip) - 1);
  Serial.printf("[USBIP] 客户端接入 %s\n", ip);
  return true;
}

void UsbIpServer::_closeClient() {
  if (_clientFd >= 0) { close(_clientFd); _clientFd = -1; }
  if (_attached) {
    // 把 Bulk IN 端点还给打印机（恢复常驻回读流）
    _printer.setInStreamSuspended(false);
    _attached = false;
    Serial.println("[USBIP] 客户端断开，USB 交还给打印服务");
  }
}

bool UsbIpServer::_readExact(uint8_t* buf, size_t len, uint32_t timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < len) {
    if (millis() - start > timeoutMs) return false;
    int n = recv(_clientFd, buf + got, len - got, 0);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { delay(2); continue; }
      return false;
    }
    got += (size_t)n;
  }
  return true;
}

bool UsbIpServer::_writeAll(const uint8_t* buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = send(_clientFd, buf + sent, len - sent, 0);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { delay(2); continue; }
      return false;
    }
    sent += (size_t)n;
  }
  return true;
}

// ---------------------------------------------------------------
//  设备信息块：312 字节固定部分 + 每个接口 4 字节
// ---------------------------------------------------------------
uint32_t UsbIpServer::_buildDeviceInfo(uint8_t* out, uint32_t outCap) {
  memset(out, 0, DEVINFO_FIXED_LEN);
  const UsbInfo& ui = _printer.getUsbInfo();

  // path[256]：空即可，Linux 不用它
  // busid[32]：必须和 IMPORT 请求里的 busid 一致
  char busid[32] = {0};
  snprintf(busid, sizeof(busid), "%d-%d", 1, (int)ui.devAddr);
  memcpy(out + 256, busid, strnlen(busid, 32));

  put32(out + 288, 1);          // busnum
  put32(out + 292, ui.devAddr);  // devnum
  // speed：Full-Speed = 2
  put32(out + 296, (ui.bcdUsb >= 0x0300) ? 3 : 2);
  put16(out + 300, ui.vid);
  put16(out + 302, ui.pid);
  put16(out + 304, 0);           // bcdDevice（不重要）
  out[306] = ui.ifaceClass;
  out[307] = ui.ifaceSubClass;
  out[308] = ui.ifaceProtocol;
  out[309] = 0;                  // bConfigurationValue
  out[310] = 1;                  // bNumConfigurations
  int n = _printer.getIfaceCount();
  if (n > 8) n = 8;
  out[311] = (uint8_t)n;

  uint32_t used = DEVINFO_FIXED_LEN;
  for (int i = 0; i < n; i++) {
    if (used + IFACE_INFO_LEN > outCap) break;
    uint8_t c = 0, s = 0, p = 0;
    if (!_printer.getIfaceInfo(i, c, s, p)) break;
    out[used++] = c;
    out[used++] = s;
    out[used++] = p;
    out[used++] = 0;             // padding
  }
  return used;
}

bool UsbIpServer::getExportInfo(uint16_t& vid, uint16_t& pid, int& ifaces) {
  const UsbInfo& ui = _printer.getUsbInfo();
  vid = ui.vid; pid = ui.pid; ifaces = _printer.getIfaceCount();
  return _printer.isConnected();
}

// ---------------------------------------------------------------
//  操作报文（未 attach 阶段）
// ---------------------------------------------------------------
void UsbIpServer::_handleOperation() {
  uint8_t hdr[8];
  if (!_readExact(hdr, 8, 10000)) { _closeClient(); return; }
  uint16_t version = get16(&hdr[0]);
  uint16_t command = get16(&hdr[2]);
  uint32_t status = get32(&hdr[4]);
  (void)status;

  if (version != USBIP_VERSION) {
    Serial.printf("[USBIP] 版本不匹配：0x%04X（期望 0x%04X）\n", version, USBIP_VERSION);
    uint8_t rep[8];
    put16(&rep[0], USBIP_VERSION);
    put16(&rep[2], OP_REP_DEVLIST);
    put32(&rep[4], ST_NA);
    _writeAll(rep, 8);
    _closeClient();
    return;
  }

  if (command == OP_REQ_DEVLIST) {
    Serial.println("[USBIP] OP_REQ_DEVLIST");
    _sendDevlist();
    return;
  }
  if (command == OP_REQ_IMPORT) {
    char busid[32];
    if (!_readExact((uint8_t*)busid, 32, 10000)) { _closeClient(); return; }
    busid[31] = 0;
    Serial.printf("[USBIP] OP_REQ_IMPORT busid=%s\n", busid);
    // ⚠️ 能不能做 USB/IP，取决于**控制传输是否可用** ——
    //    Linux 枚举设备必须靠我们回应 GET_DESCRIPTOR，控制传输不通就一切免谈。
    //    Epson 1390 这类机型对 EP0 100% 不应答（机型库标了 Q_NO_CTRL），
    //    直接在这里拒绝导出：Linux 不会 attach（省得它一直重试超时），
    //    我们的打印服务也不会被白白占住 USB 设备。
    // 控制通道必须**健康**：EP0 上不能有泄漏的在飞传输（那会让所有控制传输失败）。
    const bool ctrlHealthy = !_printer.isCtrlHung() && _printer.getLeakedXfers() == 0;
    const bool canExport = _printer.isConnected() && _printer.ctrlAllowed() && ctrlHealthy;
    if (_printer.isConnected() && !ctrlHealthy) {
      Serial.printf("[USBIP] 拒绝导出：控制通道不健康（泄漏 %u 个传输）\n",
                    (unsigned)_printer.getLeakedXfers());
    }
    if (_printer.isConnected() && !canExport) {
      Serial.println("[USBIP] 拒绝导出：该机型不支持控制传输（GET_DESCRIPTOR 不通）");
    }
    _sendImportReply(canExport);
    if (canExport) {
      // 接管 Bulk IN：停掉常驻回读流，把端点让给 URB
      _printer.setInStreamSuspended(true);
      _attached = true;
      Serial.println("[USBIP] 设备已导出，进入 URB 模式");
    }
    return;
  }
  Serial.printf("[USBIP] 未知操作 0x%04X\n", command);
  _closeClient();
}

void UsbIpServer::_sendDevlist() {
  uint8_t hdr[8];
  put16(&hdr[0], USBIP_VERSION);
  put16(&hdr[2], OP_REP_DEVLIST);
  put32(&hdr[4], ST_OK);
  _writeAll(hdr, 8);

  const bool has = _printer.isConnected();
  uint8_t cnt[4];
  put32(cnt, has ? 1 : 0);
  _writeAll(cnt, 4);
  if (!has) return;

  uint8_t buf[DEVINFO_FIXED_LEN + 8 * IFACE_INFO_LEN];
  uint32_t n = _buildDeviceInfo(buf, sizeof(buf));
  _writeAll(buf, n);
}

void UsbIpServer::_sendImportReply(bool ok) {
  uint8_t hdr[8];
  put16(&hdr[0], USBIP_VERSION);
  put16(&hdr[2], OP_REP_IMPORT);
  put32(&hdr[4], ok ? ST_OK : ST_NA);
  _writeAll(hdr, 8);
  if (!ok) return;
  uint8_t buf[DEVINFO_FIXED_LEN + 8 * IFACE_INFO_LEN];
  uint32_t n = _buildDeviceInfo(buf, sizeof(buf));
  _writeAll(buf, n);
}

// ---------------------------------------------------------------
//  URB 报文（attach 之后）
// ---------------------------------------------------------------
void UsbIpServer::_handleUrb() {
  uint8_t h[URB_HDR_LEN];
  if (!_readExact(h, URB_HDR_LEN, 20000)) { _closeClient(); return; }

  uint32_t command = get32(&h[0]);
  uint32_t seqnum  = get32(&h[4]);
  uint32_t devid   = get32(&h[8]);
  uint32_t dir     = get32(&h[12]);
  uint32_t ep      = get32(&h[16]);
  uint32_t xferLen = get32(&h[24]);
  (void)devid;

  if (command == USBIP_CMD_UNLINK) {
    Serial.printf("[USBIP] UNLINK seq=%u\n", (unsigned)seqnum);
    _sendRetUnlink(seqnum, 0);
    return;
  }
  if (command != USBIP_CMD_SUBMIT) {
    _errCount++;
    _closeClient();
    return;
  }

  // OUT 方向：报文后面跟着 transfer_buffer_length 字节的数据
  uint8_t* outBuf = NULL;
  if (dir == USBIP_DIR_OUT && xferLen > 0) {
    if (xferLen > 64 * 1024) { _errCount++; _closeClient(); return; }
    outBuf = (uint8_t*)malloc(xferLen);
    if (outBuf == NULL) { _errCount++; _closeClient(); return; }
    if (!_readExact(outBuf, xferLen, 20000)) {
      free(outBuf); _errCount++; _closeClient(); return;
    }
  }

  _urbCount++;
  if (ep == 0) {
    // ---- 控制传输 ----
    const uint8_t* setup = &h[40];
    uint8_t bmRequestType = setup[0];
    uint8_t bRequest      = setup[1];
    uint16_t wValue       = get16(&setup[2]);
    uint16_t wIndex       = get16(&setup[4]);
    uint16_t wLength      = get16(&setup[6]);
    const bool isIn = (bmRequestType & 0x80) != 0;

    uint8_t resp[512];
    uint16_t want = wLength;
    if (want > sizeof(resp)) want = sizeof(resp);
    if (!isIn) want = 0;

    // ⚠️ 超时给 3 秒（不是 1 秒）：实测传输**本身能完成**，只是完成回调
    //    晚于 1 秒阈值 —— 1 秒时 _awaitXfer 先判超时，而回调随后才到
    //    （leakedXfers 不涨正是这个特征：_releaseXfer 看到 _xferDone=true 就 free 了）。
    //    Linux 的 usbip 客户端对单个 URB 的容忍度是秒级，给足余量。
    int rc = _printer.controlTransferRaw(bmRequestType, bRequest, wValue, wIndex,
                                         isIn ? resp : outBuf, want, 3000);
    if (rc < 0) {
      _errCount++;
      if (outBuf) free(outBuf);
      _sendRetSubmit(seqnum, -19 /*ENODEV*/, NULL, 0);   // -ENODEV
      return;
    }
    if (outBuf) free(outBuf);
    _sendRetSubmit(seqnum, 0, isIn ? resp : NULL, (uint32_t)rc);
    return;
  }

  // ---- 批量传输 ----
  uint8_t  epAddr = (uint8_t)(ep & 0x7F);
  const bool isIn = (dir == USBIP_DIR_IN);
  if (isIn) epAddr |= 0x80;

  uint8_t buf[4096];
  uint16_t len = (uint16_t)(xferLen > sizeof(buf) ? sizeof(buf) : xferLen);
  if (isIn) {
    int rc = _printer.bulkTransferRaw(epAddr, buf, len, true, 5000);
    if (rc < 0) { _errCount++; _sendRetSubmit(seqnum, -19, NULL, 0); return; }
    _sendRetSubmit(seqnum, 0, buf, (uint32_t)rc);
  } else {
    if (outBuf == NULL) { _sendRetSubmit(seqnum, 0, NULL, 0); return; }
    int rc = _printer.bulkTransferRaw(epAddr, outBuf, len, false, 15000);
    free(outBuf);
    if (rc < 0) { _errCount++; _sendRetSubmit(seqnum, -19, NULL, 0); return; }
    _sendRetSubmit(seqnum, 0, NULL, 0);
  }
}

void UsbIpServer::_sendRetSubmit(uint32_t seqnum, int32_t status,
                                 const uint8_t* data, uint32_t len) {
  uint8_t h[RET_HDR_LEN];
  memset(h, 0, sizeof(h));
  put32(&h[0],  USBIP_RET_SUBMIT);
  put32(&h[4],  seqnum);
  put32(&h[8],  0);              // devid
  put32(&h[12], 0);              // direction
  put32(&h[16], 0);              // ep
  put32(&h[20], (uint32_t)status);
  put32(&h[24], len);            // actual_length
  put32(&h[28], 0);              // start_frame
  put32(&h[32], 0xFFFFFFFF);     // number_of_packets（非 ISO）
  put32(&h[36], 0);              // error_count
  // h[40..47] setup —— 应答里不用，全 0
  _writeAll(h, sizeof(h));
  if (data != NULL && len > 0) _writeAll(data, len);
}

void UsbIpServer::_sendRetUnlink(uint32_t seqnum, int32_t status) {
  uint8_t h[RET_HDR_LEN];
  memset(h, 0, sizeof(h));
  put32(&h[0],  USBIP_RET_UNLINK);
  put32(&h[4],  seqnum);
  put32(&h[20], (uint32_t)status);
  _writeAll(h, sizeof(h));
}
