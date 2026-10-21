#include "ipp_server.h"
#include "crashlog.h"
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <esp_netif.h>

// ============================================================
//  IPP / AirPrint 服务端（v2.7.0）
//  协议：RFC 8010（编码）、RFC 8011（语义）
// ============================================================

// --- 操作 ID（RFC 8011）---
#define IPP_OP_PRINT_JOB                0x0002
#define IPP_OP_PRINT_URI                0x0003
#define IPP_OP_VALIDATE_JOB             0x0004
#define IPP_OP_CREATE_JOB               0x0005
#define IPP_OP_GET_JOBS                 0x0007
#define IPP_OP_CANCEL_JOB               0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES       0x0009
#define IPP_OP_GET_PRINTER_ATTRIBUTES   0x000B

// --- 状态码 ---
#define IPP_STATUS_OK                   0x0000
#define IPP_STATUS_OK_IGNORED           0x0001
#define IPP_STATUS_BAD_REQUEST          0x0400
#define IPP_STATUS_NOT_FOUND            0x0406
#define IPP_STATUS_SERVER_ERROR         0x0500

// --- 属性组标签 ---
#define IPP_TAG_OPERATION_ATTRS         0x01
#define IPP_TAG_JOB_ATTRS               0x02
#define IPP_TAG_END                     0x03
#define IPP_TAG_PRINTER_ATTRS           0x04

// --- 值类型（RFC 8010 §3.5.2）---
#define IPP_VAL_INT                     0x21
#define IPP_VAL_BOOLEAN                 0x22
#define IPP_VAL_ENUM                    0x23
#define IPP_VAL_TEXT                    0x41
#define IPP_VAL_NAME                    0x42
#define IPP_VAL_KEYWORD                 0x44
#define IPP_VAL_URI                     0x45
#define IPP_VAL_CHARSET                 0x47
#define IPP_VAL_LANG                    0x48
#define IPP_VAL_MIMETYPE                0x49

// --- 属性名标签（RFC 8011 / IANA）---
#define IPP_A_JOB_ID                    0x21
#define IPP_A_JOB_STATE                 0x23
#define IPP_A_JOB_STATE_REASONS         0x26
#define IPP_A_JOB_URI                   0x45
#define IPP_A_CHARSET_CONFIGURED        0x47
#define IPP_A_CHARSET_SUPPORTED         0x48
// attributes-natural-language 的名标签（RFC 8011 附录 C）= 0x0048
// 注意它与 charset-supported 同号：两者属于不同注册表（name-tag vs value-tag），
// 这里显式区分开，避免误用。
#define IPP_A_ATTR_NATURAL_LANGUAGE     0x0048
#define IPP_A_COMPRESSION_SUPPORTED     0x49
#define IPP_A_DOC_FORMAT_DEFAULT        0x4A
#define IPP_A_DOC_FORMAT_SUPPORTED      0x4B
#define IPP_A_IPP_PRIVACY               0x4C
#define IPP_A_PRINTER_ACCEPTING_JOBS    0x50
#define IPP_A_PRINTER_NAME              0x42
#define IPP_A_PRINTER_STATE             0x33
#define IPP_A_PRINTER_STATE_REASONS     0x34
#define IPP_A_PRINTER_URI_SUPPORTED     0x44
#define IPP_A_PRINTER_UPTIME            0x36
#define IPP_A_URI_SECURITY_SUPPORTED    0x31
#define IPP_A_URI_SECURITY              0x30
#define IPP_A_PRINTER_INFO              0x40
#define IPP_A_PRINTER_MAKE_MODEL        0x41
#define IPP_A_COLOR_SUPPORTED           0x37
#define IPP_A_COPIES_SUPPORTED          0x53
#define IPP_A_MEDIA_SUPPORTED           0x3B
#define IPP_A_SIDES_SUPPORTED           0x3C
#define IPP_A_REQUESTED_ATTRS           0x46
#define IPP_A_MEDIA_READY               0x3F
#define IPP_A_JOB_NAME                  0x42
#define IPP_A_JOB_TEMPLATE              0x3E
#define IPP_A_DOC_FORMAT_ACTUAL         0x48

// --- 打印机状态（RFC 8011）---
#define IPP_PRINTER_IDLE                3
#define IPP_PRINTER_PROCESSING           4
#define IPP_PRINTER_STOPPED             5

static const uint16_t IPP_PORT = 631;

// ---------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------
// 字节级子串搜索。
// ⚠️ 不用 Arduino String::indexOf：它对**含 \0 的短标记**（比如 IPP 的
//    end-of-attributes-tag 单字节 0x03）不可靠 —— 实测始终返回 -1，
//    表现为"作业数据一个字节都没转发"或"每次只转发 5 字节"
//    （keep = markLen+4 把 pending 截断到了 5 字节）。
static int findBytes(const String& hay, const char* needle, size_t nlen) {
  if (needle == NULL || nlen == 0) return -1;
  size_t hl = hay.length();
  if (hl < nlen) return -1;
  for (size_t i = 0; i + nlen <= hl; i++) {
    bool ok = true;
    for (size_t j = 0; j < nlen; j++) {
      if ((uint8_t)hay[i + j] != (uint8_t)needle[j]) { ok = false; break; }
    }
    if (ok) return (int)i;
  }
  return -1;
}

// 大小写不敏感地查找子串（Arduino 的 String::toLowerCase() 是原地修改+返回 void）
static int _indexOfCI(const String& hay, const char* needle) {
  int n = strlen(needle);
  int hl = hay.length();
  for (int i = 0; i + n <= hl; i++) {
    bool ok = true;
    for (int j = 0; j < n; j++) {
      char a = hay[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
      if (a != b) { ok = false; break; }
    }
    if (ok) return i;
  }
  return -1;
}

static void appendU16(String& s, uint16_t v) {
  s += (char)((v >> 8) & 0xFF);
  s += (char)(v & 0xFF);
}
static void appendU32(String& s, uint32_t v) {
  s += (char)((v >> 24) & 0xFF);
  s += (char)((v >> 16) & 0xFF);
  s += (char)((v >> 8) & 0xFF);
  s += (char)(v & 0xFF);
}
static void appendInt32(String& s, int32_t v) { appendU32(s, (uint32_t)v); }

// 追加一个不带名标签的附加值（多值属性的第 2+ 个值）
static void addExtraValue(String& s, uint8_t valTag, const char* val) {
  s += (char)valTag;
  appendU16(s, (uint16_t)strlen(val));
  s += val;
}

static void addIntAttr(String& s, uint16_t nameTag, int32_t v) {
  appendU16(s, nameTag);
  s += (char)IPP_VAL_INT;
  appendU16(s, 4);
  appendInt32(s, v);
}

static void addBoolAttr(String& s, uint16_t nameTag, bool v) {
  appendU16(s, nameTag);
  s += (char)IPP_VAL_BOOLEAN;
  appendU16(s, 1);
  s += (char)(v ? 1 : 0);
}

static void addTextAttr(String& s, uint16_t nameTag, const char* v) {
  appendU16(s, nameTag);
  s += (char)IPP_VAL_NAME;
  appendU16(s, (uint16_t)strlen(v));
  s += v;
}

static void addKeywordAttr(String& s, uint16_t nameTag, const char* v) {
  appendU16(s, nameTag);
  s += (char)IPP_VAL_KEYWORD;
  appendU16(s, (uint16_t)strlen(v));
  s += v;
}

static void addUriAttr(String& s, uint16_t nameTag, const char* v) {
  appendU16(s, nameTag);
  s += (char)IPP_VAL_URI;
  appendU16(s, (uint16_t)strlen(v));
  s += v;
}

// ---------------------------------------------------------------
IppServer::IppServer(UsbPrinter& printer, PrintServer& printSvr)
  : _printer(printer), _printSvr(printSvr), _port(IPP_PORT) {}

String IppServer::getPrinterUri() const {
  return "ipp://" + _localIp() + ":" + String(_port) + "/ipp/print";
}

String IppServer::_localIp() const {
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) return "127.0.0.1";
  return ip.toString();
}

bool IppServer::begin() {
  _listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (_listenFd < 0) {
    Serial.println("[IPP] socket 创建失败");
    return false;
  }
  int yes = 1;
  setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_port);
  if (bind(_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    Serial.printf("[IPP] 绑定 %u 端口失败\n", (unsigned)_port);
    close(_listenFd);
    _listenFd = -1;
    return false;
  }
  if (listen(_listenFd, 4) < 0) {
    Serial.println("[IPP] listen 失败");
    close(_listenFd);
    _listenFd = -1;
    return false;
  }
  fcntl(_listenFd, F_SETFL, fcntl(_listenFd, F_GETFL, 0) | O_NONBLOCK);
  Serial.printf("[IPP] IPP/AirPrint 服务已启动 :%u  %s\n", (unsigned)_port,
                getPrinterUri().c_str());
  return true;
}

void IppServer::task() {
  if (_listenFd < 0) return;
  struct sockaddr_in peer;
  socklen_t plen = sizeof(peer);
  int fd = accept(_listenFd, (struct sockaddr*)&peer, &plen);
  if (fd < 0) return;
  // IPP 客户端可能连上后长时间不发送（Spooler 会先探测），给一个上限
  struct timeval tv = { 10, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  _handleConnection(fd);
  close(fd);
}

// ---------------------------------------------------------------
//  HTTP 解析：只支持 POST + 我们关心的路径
// ---------------------------------------------------------------
void IppServer::_handleConnection(int fd) {
  // 读请求头（到 \r\n\r\n 为止）
  String raw;
  uint8_t buf[1024];
  unsigned long start = millis();
  size_t headerEnd = 0;
  while (millis() - start < 10000) {
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    // 注意：recv 不保证 NUL 终止，必须用 concat(buf, n) 而不是 += (const char*)buf，
    // 否则会读到 buf 之外的栈内存（越界，且会把垃圾当成 HTTP 头解析）。
    raw.concat(buf, (unsigned int)n);
    headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd >= 0) { headerEnd += 4; break; }
    if (raw.length() > 8192) break;   // 请求头不该这么大
  }
  if (headerEnd == 0) return;

  // 提取方法、路径、Content-Type、Content-Length、multipart 边界
  // 请求行格式： METHOD SP PATH SP VERSION
  //   "GET /ipp/printer HTTP/1.1"
  //        ^0  ^3   ^4         ^16
  // ⚠️ sp1 是 method **之后**的空格，sp2 是 path **之后**的空格。
  //    method = [0, sp1)，path = [sp1+1, sp2)。
  //    （曾经写成 method=substring(sp1+1, sp2) —— 拿到的是路径，
  //      于是 method 永远不是 GET/POST，客户端一律收到 405。）
  int sp1 = raw.indexOf(' ');
  int sp2 = raw.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return;
  String method = raw.substring(0, sp1);
  String path = raw.substring(sp1 + 1, sp2);
  if (path.startsWith("http://")) path = path.substring(path.indexOf('/', 7));
  else if (path.startsWith("https://")) path = path.substring(path.indexOf('/', 8));

  String contentType, boundary;
  size_t contentLength = 0;
  // 注意：Arduino 的 String::toLowerCase() 是**原地修改并返回 void**，
  // 不能写成 raw.toLowerCase().indexOf(...)。这里用大小写不敏感的手工查找。
  int lc = _indexOfCI(raw, "content-length:");
  if (lc >= 0) {
    int e = raw.indexOf("\r\n", lc);
    contentLength = (size_t)strtoul(raw.substring(lc + 15, e).c_str(), NULL, 10);
  }
  int ct = _indexOfCI(raw, "content-type:");
  if (ct >= 0) {
    int e = raw.indexOf("\r\n", ct);
    contentType = raw.substring(ct + 13, e);
    contentType.trim();
    int b = contentType.indexOf("boundary=");
    if (b >= 0) {
      boundary = contentType.substring(b + 9);
      boundary.trim();
      if (boundary.startsWith("\"")) boundary = boundary.substring(1);
      int q = boundary.indexOf('"');
      if (q >= 0) boundary = boundary.substring(0, q);
    }
  }

  Serial.printf("[IPP] %s %s  (len=%u, ct=%s)\n", method.c_str(), path.c_str(),
                (unsigned)contentLength, contentType.c_str());

  // ⚠️ IPP 规范里 **Get-Printer-Attributes / Get-Jobs / Get-Job-Attributes 用 HTTP GET**
  //    （无 body），只有 Print-Job / Create-Job / Cancel-Job 用 POST。
  //    iOS/macOS 的 AirPrint 客户端正是用 GET 拿打印机属性的 —— 只支持 POST 会
  //    导致手机端根本发现不了打印机。
  if (method == "GET") {
    uint16_t opId2 = 0;
    if (path.startsWith("/ipp/printer"))       opId2 = IPP_OP_GET_PRINTER_ATTRIBUTES;
    else if (path.startsWith("/ipp/jobs"))    opId2 = IPP_OP_GET_JOBS;
    uint32_t rid = 1;
    if (opId2 == IPP_OP_GET_PRINTER_ATTRIBUTES) {
      String body = _buildPrinterAttributes(rid, "all");
      _sendIppResponse(fd, IPP_STATUS_OK, rid, body);
    } else if (opId2 == IPP_OP_GET_JOBS) {
      String body;
      body += (char)0x01; body += (char)0x01;
      appendU16(body, IPP_STATUS_OK);
      appendU32(body, rid);
      body += (char)IPP_TAG_JOB_ATTRS;
      if (_jobActive) {
        addIntAttr(body, IPP_A_JOB_ID, _jobId);
        addIntAttr(body, IPP_A_JOB_STATE, _jobState);
      }
      body += (char)IPP_TAG_END;
      _sendIppResponse(fd, IPP_STATUS_OK, rid, body);
    } else {
      const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
      send(fd, resp, strlen(resp), 0);
    }
    return;
  }

  if (method != "POST") {
    const char* resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
    send(fd, resp, strlen(resp), 0);
    return;
  }
  if (!path.startsWith("/ipp/")) {
    const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send(fd, resp, strlen(resp), 0);
    return;
  }

  // 请求头之后的字节已经在 raw 里（可能包含 IPP 头的开头）
  String headerBlob = raw.substring(headerEnd);

  // 解析 IPP 请求头
  uint16_t opId = 0;
  uint32_t reqId = 0;
  if (!_parseIppRequest((const uint8_t*)headerBlob.c_str(),
                        headerBlob.length(), opId, reqId)) {
    String body;
    body += (char)0x01; body += (char)0x01;   // version 1.1
    appendU16(body, IPP_STATUS_BAD_REQUEST);
    appendU32(body, reqId);
    body += (char)IPP_TAG_END;
    _sendIppResponse(fd, IPP_STATUS_BAD_REQUEST, reqId, body);
    return;
  }
  Serial.printf("[IPP] operation=0x%04X request-id=%u\n", opId, (unsigned)reqId);

  switch (opId) {
    case IPP_OP_GET_PRINTER_ATTRIBUTES: {
      // requested-attributes（可选）：只回客户端点名要的
      String requested;
      int ra = headerBlob.indexOf("requested-attributes");
      if (ra >= 0) requested = "all";
      String body = _buildPrinterAttributes(reqId, requested);
      _sendIppResponse(fd, IPP_STATUS_OK, reqId, body);
      return;
    }
    case IPP_OP_GET_JOBS: {
      String body;
      body += (char)0x01; body += (char)0x01;
      appendU16(body, IPP_STATUS_OK);
      appendU32(body, reqId);
      body += (char)IPP_TAG_JOB_ATTRS;
      if (_jobActive) {
        addIntAttr(body, IPP_A_JOB_ID, _jobId);
        addIntAttr(body, IPP_A_JOB_STATE, _jobState);
      }
      body += (char)IPP_TAG_END;
      _sendIppResponse(fd, IPP_STATUS_OK, reqId, body);
      return;
    }
    case IPP_OP_GET_JOB_ATTRIBUTES: {
      String body = _buildJobAttributes(reqId, _jobId);
      _sendIppResponse(fd, IPP_STATUS_OK, reqId, body);
      return;
    }
    case IPP_OP_CANCEL_JOB: {
      if (_jobActive) {
        _jobState = IPP_JOB_CANCELED;
        _jobsFailed++;
        _jobActive = false;
      }
      String body;
      body += (char)0x01; body += (char)0x01;
      appendU16(body, IPP_STATUS_OK);
      appendU32(body, reqId);
      body += (char)IPP_TAG_END;
      _sendIppResponse(fd, IPP_STATUS_OK, reqId, body);
      return;
    }
    case IPP_OP_PRINT_JOB:
    case IPP_OP_CREATE_JOB: {
      _handlePrintJob(fd, reqId, contentType.c_str(), boundary.c_str(),
                      contentLength, headerBlob);
      return;
    }
    default: {
      String body;
      body += (char)0x01; body += (char)0x01;
      appendU16(body, IPP_STATUS_BAD_REQUEST);
      appendU32(body, reqId);
      body += (char)IPP_TAG_END;
      _sendIppResponse(fd, IPP_STATUS_BAD_REQUEST, reqId, body);
      return;
    }
  }
}

bool IppServer::_parseIppRequest(const uint8_t* p, size_t n,
                                 uint16_t& opId, uint32_t& reqId) {
  if (n < 8) return false;
  // version(2) + operation-id(2) + request-id(4)
  opId  = (uint16_t)((p[2] << 8) | p[3]);
  reqId = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
          ((uint32_t)p[6] << 8) | (uint32_t)p[7];
  return true;
}

void IppServer::_sendIppResponse(int fd, uint16_t statusCode, uint32_t reqId,
                                 const String& body) {
  char head[160];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/ipp\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   (unsigned)body.length());
  send(fd, head, n, 0);
  if (body.length()) send(fd, body.c_str(), body.length(), 0);
}

// ---------------------------------------------------------------
//  Get-Printer-Attributes
// ---------------------------------------------------------------
String IppServer::_buildPrinterAttributes(uint32_t reqId, const String& requested) {
  String body;
  body.reserve(1024);
  body += (char)0x01; body += (char)0x01;      // version 1.1
  appendU16(body, IPP_STATUS_OK);
  appendU32(body, reqId);
  body += (char)IPP_TAG_OPERATION_ATTRS;
  addKeywordAttr(body, IPP_A_CHARSET_CONFIGURED, "utf-8");
  addKeywordAttr(body, IPP_A_ATTR_NATURAL_LANGUAGE, "en-us");
  body += (char)IPP_TAG_END;

  body += (char)IPP_TAG_PRINTER_ATTRS;
  const char* uri = getPrinterUri().c_str();
  // uri-security：明文 IPP（局域网内）
  addKeywordAttr(body, IPP_A_URI_SECURITY, "none");
  addKeywordAttr(body, IPP_A_URI_SECURITY_SUPPORTED, "none");
  addKeywordAttr(body, IPP_A_IPP_PRIVACY, "none");
  // 名称
  addTextAttr(body, IPP_A_PRINTER_NAME, "ESP32 PrintServer");
  addTextAttr(body, IPP_A_PRINTER_MAKE_MODEL, "ESP32-S3 USB Print Server");
  addTextAttr(body, IPP_A_PRINTER_INFO, "WiFi print server for USB printers");
  addUriAttr(body, IPP_A_PRINTER_URI_SUPPORTED, uri);
  // 状态
  addIntAttr(body, IPP_A_PRINTER_STATE,
             (_jobActive ? IPP_PRINTER_PROCESSING : IPP_PRINTER_IDLE));
  addKeywordAttr(body, IPP_A_PRINTER_STATE_REASONS, "none");
  addIntAttr(body, IPP_A_PRINTER_UPTIME, (int32_t)(millis() / 1000));
  addBoolAttr(body, IPP_A_PRINTER_ACCEPTING_JOBS, true);
  addKeywordAttr(body, IPP_A_MEDIA_READY, "na-letter");
  // 字符集 / 压缩
  addKeywordAttr(body, IPP_A_CHARSET_SUPPORTED, "utf-8");
  addExtraValue(body, IPP_VAL_KEYWORD, "us-ascii");
  addKeywordAttr(body, IPP_A_COMPRESSION_SUPPORTED, "none");
  // 支持的文档格式 —— **关键**：客户端按我们声明的格式来发
  addKeywordAttr(body, IPP_A_DOC_FORMAT_DEFAULT, "application/pdf");
  addKeywordAttr(body, IPP_A_DOC_FORMAT_SUPPORTED, "application/pdf");
  addExtraValue(body, IPP_VAL_KEYWORD, "image/urf");
  addExtraValue(body, IPP_VAL_KEYWORD, "image/pwg-raster");
  addExtraValue(body, IPP_VAL_KEYWORD, "application/vnd.appleRaster");
  addExtraValue(body, IPP_VAL_KEYWORD, "application/octet-stream");
  // 颜色 / 份数 / 介质 / 双面
  addKeywordAttr(body, IPP_A_COLOR_SUPPORTED, "color");
  addKeywordAttr(body, IPP_A_COPIES_SUPPORTED, "1");
  addKeywordAttr(body, IPP_A_MEDIA_SUPPORTED, "na-letter");
  addExtraValue(body, IPP_VAL_KEYWORD, "na_a4");
  addExtraValue(body, IPP_VAL_KEYWORD, "iso_a4_210x297mm");
  addKeywordAttr(body, IPP_A_SIDES_SUPPORTED, "one-sided");
  addExtraValue(body, IPP_VAL_KEYWORD, "two-sided-long-edge");
  body += (char)IPP_TAG_END;
  return body;
}

String IppServer::_buildJobAttributes(uint32_t reqId, int32_t jobId) {
  String body;
  body += (char)0x01; body += (char)0x01;
  appendU16(body, IPP_STATUS_OK);
  appendU32(body, reqId);
  body += (char)IPP_TAG_JOB_ATTRS;
  addIntAttr(body, IPP_A_JOB_ID, jobId);
  addIntAttr(body, IPP_A_JOB_STATE, _jobActive ? _jobState : IPP_JOB_COMPLETED);
  addKeywordAttr(body, IPP_A_JOB_STATE_REASONS, "none");
  addTextAttr(body, IPP_A_JOB_NAME, _jobName);
  char uri[96];
  snprintf(uri, sizeof(uri), "ipp://%s:%u/jobs/%d", _localIp().c_str(),
           (unsigned)_port, (int)jobId);
  addUriAttr(body, IPP_A_JOB_URI, uri);
  body += (char)IPP_TAG_END;
  return body;
}

// ---------------------------------------------------------------
//  Print-Job：流式把文件数据转给打印机
// ---------------------------------------------------------------
bool IppServer::_handlePrintJob(int fd, uint32_t reqId, const char* contentType,
                                const char* boundary, size_t contentLength,
                                const String& headerBlob) {
  if (_jobActive) {
    markStage("ipp-busy", __LINE__);
    String body;
    body += (char)0x01; body += (char)0x01;
    appendU16(body, IPP_STATUS_SERVER_ERROR);
    appendU32(body, reqId);
    body += (char)IPP_TAG_END;
    _sendIppResponse(fd, IPP_STATUS_SERVER_ERROR, reqId, body);
    return false;
  }

  // 占用打印通道（与 :9100 互斥）
  if (!_printSvr.beginExternalJob("ipp")) {
    markStage("ipp-nochan", __LINE__);
    String body;
    body += (char)0x01; body += (char)0x01;
    appendU16(body, IPP_STATUS_SERVER_ERROR);
    appendU32(body, reqId);
    body += (char)IPP_TAG_END;
    _sendIppResponse(fd, IPP_STATUS_SERVER_ERROR, reqId, body);
    return false;
  }

  _jobActive   = true;
  _jobId       = ++_jobId;
  _jobState    = IPP_JOB_PROCESSING;
  _jobBytes    = 0;
  _jobStartMs  = millis();
  Serial.printf("[IPP] 开始作业 job-id=%d len=%u\n", (int)_jobId, (unsigned)contentLength);

  markStage("ipp-streaming", __LINE__);
  { char d[48]; snprintf(d,sizeof(d),"cl=%u hb=%u",(unsigned)contentLength,(unsigned)headerBlob.length()); markIppDiag(d); }
  int written = _streamJobData(fd, boundary, contentLength, headerBlob);

  // 收尾：告诉打印机作业结束
  markStage("ipp-done", __LINE__);
  _jobState = (written > 0) ? IPP_JOB_COMPLETED : IPP_JOB_ABORTED;
  if (written > 0) _jobsAccepted++; else _jobsFailed++;
  _jobActive = false;
  _printSvr.endExternalJob(written > 0, (size_t)written);
  Serial.printf("[IPP] 作业 job-id=%d 结束，转发 %d 字节，状态=%d\n",
                (int)_jobId, written, (int)_jobState);

  String body;
  body += (char)0x01; body += (char)0x01;
  appendU16(body, (written > 0) ? IPP_STATUS_OK : IPP_STATUS_SERVER_ERROR);
  appendU32(body, reqId);
  body += (char)IPP_TAG_JOB_ATTRS;
  addIntAttr(body, IPP_A_JOB_ID, _jobId);
  addIntAttr(body, IPP_A_JOB_STATE, _jobState);
  body += (char)IPP_TAG_END;
  _sendIppResponse(fd, (written > 0) ? IPP_STATUS_OK : IPP_STATUS_SERVER_ERROR,
                   reqId, body);
  return written > 0;
}

// 往打印机写数据
size_t IppServer::_writeToPrinter(const uint8_t* buf, size_t len) {
  if (len == 0) return 0;
  int sent = _printer.sendData(buf, len);
  {
    char d[48];
    snprintf(d, sizeof(d), "wr=%d/%u", sent, (unsigned)len);
    markIppDiag(d);
  }
  if (sent <= 0) {
    // 诊断：把 sendData 的返回码写进 stage（-1=设备不可用 -2=拿不到锁
    // -4=USB 传输失败 -5=冷却保护中 0=提交成功但写出 0 字节）
    char st[24];
    snprintf(st, sizeof(st), "wr%d", sent);
    markStage(st, __LINE__);
    return 0;
  }
  return (size_t)sent;
}

// 流式读取 body，边收边转发。
// 两种 body 形态：
//   A) multipart/related —— 有 boundary，属性是第一段，文件数据在后续段
//   B) 直接跟在 IPP 属性后面 —— 靠 end-of-attributes-tag 定位
// 这里都按"跳过属性段、之后全是文件数据"处理：
//   A 用 boundary 跳过第一段
//   B 扫描属性段找到 end-of-attributes-tag
int IppServer::_streamJobData(int fd, const char* boundary, size_t contentLength,
                             const String& headerBlob) {
  markStage("ipp-stream", __LINE__);
  uint8_t buf[1024];
  size_t total = 0;

  const bool multi = (boundary && *boundary);
  const String endMark = multi ? (String("\r\n--") + boundary) : String();
  // 起点标记：multipart 用第二段的开头；非 multipart 用 IPP 的结束标签。
  // ⚠️⚠️ IPP 的 end-of-attributes-tag 是**单字节 0x03**（RFC 8010 §3.1），
  //    不是"整数 0x03 的 4 字节大端编码"（0x03 00 00 00）——
  //    写成 4 字节就永远匹配不上，表现为"作业数据一个字节都没转发"。
  // 起点标记（用 char* + 长度，配合字节级 findBytes）：
  //   multipart  → "\r\n--<boundary>"（第二段的开头）
  //   非 multipart → 单字节 0x03（IPP 的 end-of-attributes-tag，RFC 8010 §3.1）
  // ⚠️ 必须用**具名** String 变量持有标记：`(expr).c_str()` 指向临时对象，
  //    语句结束就析构 → 悬垂指针。
  // ⚠️ 用显式 uint8_t 数组而不是 String("\x03", 1)：
  //    Arduino String 的 (const char*, unsigned int) 构造函数配合含 \0 的字面量时
  //    行为不可靠（实测 indexOf 永远找不到）。直接用字节数组最稳。
  static const uint8_t kEofTag[1] = { 0x03 };
  String markStr = multi ? (String("\r\n--") + boundary) : String();
  const char*  startMark    = multi ? markStr.c_str() : (const char*)kEofTag;
  const size_t startMarkLen = multi ? markStr.length() : 1;
  // multipart 的宽松版起点标记：部分客户端段之间不严格用 "\r\n--B"，
  // 直接用 "--B"。找不到严格版时用它兜底。
  const String looseMarkStr = multi ? (String("--") + boundary) : String();
  const char*  looseMark    = multi ? looseMarkStr.c_str() : NULL;
  const size_t looseMarkLen = looseMarkStr.length();

  String pending;              // 跨块扫描用，长度受限
  bool   inData = false;       // 是否已进入文件数据区
  bool   skipPartHead = multi; // 数据区开头还要跳过第二段的 MIME 头
  size_t bodyGot = 0;          // 已从 socket 收到的 body 字节数

  // 处理一段数据。返回 true 表示作业数据已结束。
  auto feed = [&](const uint8_t* p, size_t n) -> bool {
    if (inData) {
      // —— 已在数据区 ——
      if (endMark.length()) {
        pending.concat(p, (unsigned int)n);
        int at = findBytes(pending, endMark.c_str(), endMark.length());
        if (at >= 0) {
          size_t usable = (size_t)at;
          while (usable > 0 && (pending[usable-1] == '\r' || pending[usable-1] == '\n')) usable--;
          total += _writeToPrinter((const uint8_t*)pending.c_str(), usable);
          pending = "";
          return true;
        }
        // 没找到完整标记：保留尾巴（标记可能被切断），前面的都转发
        size_t keep = endMark.length() + 4;
        if (pending.length() > keep) {
          size_t send = pending.length() - keep;
          total += _writeToPrinter((const uint8_t*)pending.c_str(), send);
          pending = pending.substring(send);
        }
        return false;
      }
      total += _writeToPrinter(p, n);
      return false;
    }

    // —— 还在找数据区起点 ——
    pending.concat(p, (unsigned int)n);
    int at = findBytes(pending, startMark, startMarkLen);
    if (at < 0 && looseMark) at = findBytes(pending, looseMark, looseMarkLen);
    {
      char d[48];
      snprintf(d, sizeof(d), "m%u[%02X%02X%02X]@%d/%u", (unsigned)startMarkLen,
               (uint8_t)startMark[0], startMarkLen > 1 ? (uint8_t)startMark[1] : 0,
               startMarkLen > 2 ? (uint8_t)startMark[2] : 0, at, (unsigned)pending.length());
      markIppDiag(d);
    }
    if (at < 0) {
      markStage("ipp-scan", __LINE__);
      { char d[48]; snprintf(d,sizeof(d),"scan%u",(unsigned)pending.length()); markIppDiag(d); }
      // 保留足够尾巴以防标记被切断（其余丢弃：那是 IPP 属性，不该转给打印机）
      size_t keep = startMarkLen + 4;
      if (pending.length() > keep + 64) {
        pending = pending.substring(pending.length() - keep);
      }
      return false;
    }
    // 用实际匹配到的那个标记长度来切
    size_t markLen = (at >= 0 && findBytes(pending, startMark, startMarkLen) == at)
                     ? startMarkLen : looseMarkLen;
    pending = pending.substring((size_t)at + markLen);
    inData = true;
    { char d[48]; snprintf(d,sizeof(d),"found@%d",(int)at); markIppDiag(d); }
    // multipart：跳过第二段的 MIME 头（到 \r\n\r\n 为止）
    if (skipPartHead) {
      int he = pending.indexOf("\r\n\r\n");
      if (he >= 0) {
        pending = pending.substring((size_t)he + 4);
        skipPartHead = false;
      } else {
        // MIME 头还没收全，等下一块
        if (pending.length() > 256) pending = pending.substring(pending.length() - 256);
      }
    }
    // 已经攒到的数据（若有）立即转发
    if (pending.length() && !skipPartHead) {
      { char d[48]; snprintf(d,sizeof(d),"data@%u",(unsigned)pending.length()); markIppDiag(d); }
      total += _writeToPrinter((const uint8_t*)pending.c_str(), pending.length());
      pending = "";
    }
    return false;
  };

  // 先处理 headerBlob 里已有的部分
  if (headerBlob.length()) {
    feed((const uint8_t*)headerBlob.c_str(), headerBlob.length());
    bodyGot = headerBlob.length();
  }

  // 持续从 socket 读。
  // ⚠️ recv 是**阻塞**的（socket 只设了 10 秒 SO_RCVTIMEO）——
  //    数据收完后如果还在等 recv，客户端会等到超时才拿到 IPP 响应。
  //    所以必须用 Content-Length 计数，收够就停。
  for (;;) {
    if (contentLength > 0 && bodyGot >= contentLength) break;
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;                 // 超时 / 出错 / 对端关闭 → 收工
    bodyGot += (size_t)n;
    if (feed(buf, (size_t)n)) break;    // 遇到结束标记
  }
  // 兜底：始终没找到数据区起点 → 把手上剩的内容也转发出去。
  // 宁可多发几个字节让打印机报错，也不能"一个字节都没送到"——
  // 后者表现为 IPP 客户端收到"作业失败"但完全不知道原因。
  if (!inData && pending.length()) {
    markStage("ipp-fallback", __LINE__);
    { char d[48]; snprintf(d,sizeof(d),"fb%u",(unsigned)pending.length()); markIppDiag(d); }
    total += _writeToPrinter((const uint8_t*)pending.c_str(), pending.length());
    pending = "";
  }
  markStage("ipp-stream-done", __LINE__);
  { char d[48]; snprintf(d,sizeof(d),"tot=%u",(unsigned)total); markIppDiag(d); }
  _jobBytes = total;
  return (int)total;
}
