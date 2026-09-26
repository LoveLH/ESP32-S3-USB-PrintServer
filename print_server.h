#ifndef PRINT_SERVER_H
#define PRINT_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include "usb_printer.h"
#include "config.h"

struct PrintJob {
  bool         active;
  size_t       bytesReceived;
  size_t       bytesSent;
  unsigned long startTime;
  String       clientIP;
};

class PrintServer {
public:
  PrintServer(UsbPrinter& printer);

  bool begin();
  void task();

  // 打印一张纯文本测试页（任何打印机都能打）
  bool printTestPage();

  PrintJob  getCurrentJob() const { return _currentJob; }
  uint32_t  getTotalJobs() const { return _totalJobs; }
  uint32_t  getTotalBytes() const { return _totalBytes; }
  uint32_t  getFailedJobs() const { return _failedJobs; }
  String    getLastJob() const { return _lastJob; }
  unsigned long getLastJobMs() const { return _lastJobMs; }

private:
  UsbPrinter& _printer;
  WiFiServer  _server;
  WiFiClient  _client;
  PrintJob    _currentJob;

  uint32_t _totalJobs;
  uint32_t _totalBytes;
  uint32_t _failedJobs;
  String   _lastJob;
  unsigned long _lastJobMs;
  unsigned long _lastActivityMs;  // 最后一次"收到数据 / 成功下发数据"的时间，用于空闲超时判定

  uint8_t* _buffer;
  size_t   _bufferSize;

  void _handleClient();
  // 异常终止当前作业：标记失败、复位打印机（清掉半截作业造成的状态机错位）、关闭连接
  void _abortJob(const String& reason);
  // 给打印机发 ESC @ 复位，清掉上一个（可能中断的）作业残留状态，避免下次打印错位
  void _resetPrinter();
};

#endif // PRINT_SERVER_H
