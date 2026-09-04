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

  uint8_t* _buffer;
  size_t   _bufferSize;

  void _handleClient();
};

#endif // PRINT_SERVER_H
