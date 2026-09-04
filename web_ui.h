#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>
#include "wifi_manager.h"
#include "usb_printer.h"
#include "print_server.h"
#include "config.h"
#include "crashlog.h"

class WebUI {
public:
  WebUI(WiFiManager& wifi, UsbPrinter& printer, PrintServer& printSvr);

  void begin();
  void task();

private:
  WebServer     _server;
  WiFiManager&  _wifi;
  UsbPrinter&   _printer;
  PrintServer&  _printSvr;

  bool   _otaRejected;
  String _otaError;

  // 路由
  void _handleRoot();
  void _handleStatus();
  void _handleWifiScan();
  void _handleWifiConnect();
  void _handleRestart();
  void _handlePrinterReset();
  void _handleTestPage();
  void _handleOta();        // 上传结束后回调
  void _handleOtaUpload();  // 上传过程中流式写 flash
  void _handleDebug();      // 读取 RTC 崩溃日志
  void _handleNotFound();

  String _generatePage();
  static String _jsonEscape(const String& s);
};

#endif // WEB_UI_H
