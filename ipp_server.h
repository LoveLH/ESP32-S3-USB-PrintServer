#ifndef IPP_SERVER_H
#define IPP_SERVER_H

// ============================================================
//  IPP / AirPrint 服务端（v2.7.0）
//
//  为什么需要它：Raw 9100 只有装了驱动的电脑能用；手机（iOS / Android）
//  只会说 AirPrint / IPP，要让它们能打印，设备端必须自己当一次打印服务器。
//
//  关键设计决策（决定了整套实现的形态）：
//
//  1) **自己解析 HTTP + IPP，不复用 Arduino 的 WebServer**。
//     IPP 的 Print-Job 请求体里，"属性段"和"文件数据段"必须区分开
//     （multipart/related 要按边界切，非 multipart 要按
//     end-of-attributes-tag 切）。Arduino WebServer 会在解析层就把
//     multipart 拆散、或者把整个 body 缓冲进内存（几十 MB 的照片放不下，
//     S3 只有 2MB PSRAM）。自己解析才能做到"边收边转 USB"。
//
//  2) **独立监听 631 端口**（IPP 标准端口），不与 80 端口的管理页混用。
//     mDNS 广告里用 rp=/ipp/print 指过来，客户端按广告访问。
//
//  3) **不自己渲染**。IPP/AirPrint 的数据流里，客户端已经把文档渲染成
//     位图（Apple Raster / PWG Raster）或直接发 PDF 了，服务器只做协议封装
//     和转发。只有当打印机既不支持 PDF 直印、也不认位图时，才需要做
//     简化 RIP（那是 v2.7.1 的事）。
//
//  协议参考：RFC 8010（IPP 传输与编码）、RFC 8011（IPP 模型与语义）。
// ============================================================

#include <Arduino.h>
#include "usb_printer.h"
#include "print_server.h"

// 作业状态（与 RFC 8011 的 job-state 一致）
enum IppJobState : uint8_t {
  IPP_JOB_PENDING          = 3,
  IPP_JOB_PROCESSING       = 5,
  IPP_JOB_CANCELED         = 7,
  IPP_JOB_ABORTED          = 8,
  IPP_JOB_COMPLETED        = 9,
};

class IppServer {
public:
  IppServer(UsbPrinter& printer, PrintServer& printSvr);

  bool begin();
  // 供 USB Host 之外的任务调用：处理一条新建的连接
  void task();

  uint16_t getPort() const { return _port; }
  // 已经在打印的作业数（同时只允许一份，:9100 也一样）
  bool isBusy() const { return _jobActive; }
  uint32_t getJobsAccepted() const { return _jobsAccepted; }
  uint32_t getJobsFailed()  const { return _jobsFailed; }
  // 供 mDNS 广告与状态页使用
  String getPrinterUri() const;

private:
  UsbPrinter&  _printer;
  PrintServer& _printSvr;
  uint16_t     _port;
  int          _listenFd = -1;

  // --- 作业状态（IPP 侧）---
  bool        _jobActive = false;
  int32_t     _jobId = 0;
  uint8_t     _jobState = IPP_JOB_PENDING;
  char        _jobName[64] = {0};
  char        _clientIP[16] = {0};
  size_t      _jobBytes = 0;
  uint32_t    _jobsAccepted = 0;
  uint32_t    _jobsFailed = 0;
  unsigned long _jobStartMs = 0;

  // 处理一条已 accept 的连接（内部自己控制 socket 生命周期）
  void _handleConnection(int fd);
  // 解析并处理一次 HTTP 请求，返回 true 表示已响应
  bool _handleHttp(int fd, const char* method, const char* path,
                   const char* contentType, const char* boundary,
                   size_t contentLength, String& headerBlob);
  // 解析 IPP 请求头，返回 operation-id / request-id
  bool _parseIppRequest(const uint8_t* p, size_t n,
                        uint16_t& opId, uint32_t& reqId);
  // 构造 IPP 响应并写回 socket
  void _sendIppResponse(int fd, uint16_t statusCode, uint32_t reqId,
                         const String& body);
  // 生成 Get-Printer-Attributes 的属性体
  String _buildPrinterAttributes(uint32_t reqId, const String& requested);
  // 生成作业状态属性
  String _buildJobAttributes(uint32_t reqId, int32_t jobId);
  // 处理 Print-Job：流式把文件数据转给打印机
  bool _handlePrintJob(int fd, uint32_t reqId, const char* contentType,
                       const char* boundary, size_t contentLength,
                       const String& headerBlob);
  // 读取 HTTP body 剩余字节，边读边按 multipart 边界切分，转发给打印机
  int  _streamJobData(int fd, const char* boundary, size_t contentLength,
                      const String& headerBlob);
  // 往打印机写数据（转发），返回实际写入字节数
  size_t _writeToPrinter(const uint8_t* buf, size_t len);

  String _localIp() const;
};

#endif // IPP_SERVER_H
