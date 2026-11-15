#ifndef USBIP_SERVER_H
#define USBIP_SERVER_H

// ============================================================
//  USB/IP 服务端（v2.8.0）
//
//  目标：通过 USB/IP 转发控制与 bulk URB；尚未完成 Linux/CUPS 端到端验收。
//  当前串行处理 URB，未实现真正的异步 UNLINK；不能视为完整 Linux 支持。
//
//  协议格式（Linux 内核 drivers/usbip + uapi/linux/usbip.h，**大端/网络字节序**）：
//
//  1) 操作报文（8 字节头）—— 状态：未 attach
//     0x00 version(2)=0x0111 | 0x02 command(2) | 0x04 status(4)
//     command: OP_REQ_DEVLIST=0x8005  OP_REP_DEVLIST=0x0005
//              OP_REQ_IMPORT =0x8003  OP_REP_IMPORT =0x0003
//
//  2) OP_REP_DEVLIST: 8 字节头 + u32 ndevs + ndevs × (312 字节设备信息 + 4×接口数)
//
//  3) OP_REQ_IMPORT: 8 字节头 + 32 字节 busid
//     OP_REP_IMPORT: 成功时 8 字节头 + 312 字节固定设备信息，不附加接口数组
//     ⚠️ IMPORT 响应**不含 USB 描述符** —— Linux 会自己用控制传输
//        GET_DESCRIPTOR 来取，所以我们不用缓存描述符。
//
//  4) attach 之后切换到 URB 格式（48 字节头）：
//     0x00 command(4)  1=SUBMIT 2=UNLINK
//     0x04 seqnum(4)
//     0x08 devid(4)
//     0x0C direction(4)  0=OUT 1=IN
//     0x10 ep(4)          0=控制传输
//     0x14 transfer_flags(4)
//     0x18 transfer_buffer_length(4)
//     0x1C start_frame(4)
//     0x20 number_of_packets(4)
//     0x24 interval(4)
//     0x28 setup[8]       原始 USB SETUP 包（16 位字段为小端）
//     之后（仅 OUT 方向）跟 transfer_buffer_length 字节的数据
//
//  5) 应答：
//     USBIP_RET_SUBMIT(3)  48 字节头（已含 status/actual_length）+ IN 数据
//     USBIP_RET_UNLINK(4)  48 字节头
//
//  能力边界：只做控制传输 + 当前打印接口的 Bulk IN/OUT；不支持 ISO / Interrupt。
//  与「打印服务器」模式互斥：USB/IP 接管期间我们的常驻 IN 流必须停掉
//  （否则两个都抢 Bulk IN 端点）。
// ============================================================

#include <Arduino.h>
#include "usb_printer.h"

class UsbIpServer {
public:
  explicit UsbIpServer(UsbPrinter& printer) : _printer(printer) {}

  bool begin(uint16_t port = 3240);
  void task();                     // 每次调用处理一次连接上的读写

  uint16_t getPort() const { return _port; }
  bool     isClientAttached() const { return _attached; }
  uint32_t getUrbCount()  const { return _urbCount; }
  uint32_t getErrorCount() const { return _errCount; }
  // 已导出的设备信息（供 /api/status 显示）
  bool getExportInfo(uint16_t& vid, uint16_t& pid, int& ifaces);

private:
  UsbPrinter& _printer;
  uint16_t    _port = 3240;
  int         _listenFd = -1;
  int         _clientFd = -1;
  bool        _attached = false;
  uint32_t    _urbCount = 0;
  uint32_t    _errCount = 0;

  // --- socket 辅助 ---
  bool _acceptClient();
  void _closeClient();
  bool _readExact(uint8_t* buf, size_t len, uint32_t timeoutMs);
  bool _writeAll(const uint8_t* buf, size_t len);

  // --- 协议处理 ---
  void _handleOperation();          // 未 attach 时的操作报文
  void _handleUrb();               // attach 后的 URB 报文
  void _sendDevlist();
  bool _sendImportReply(bool ok);
  void _sendRetSubmit(uint32_t seqnum, int32_t status,
                      const uint8_t* data, uint32_t len);
  void _sendRetUnlink(uint32_t seqnum, int32_t status);
  // 填充设备信息块（312 字节 + 接口数组）
  uint32_t _buildDeviceInfo(uint8_t* out, uint32_t outCap, bool includeInterfaces);
};

#endif // USBIP_SERVER_H
