#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>
#include "auth.h"
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
  Auth          _auth;

  bool   _otaRejected;
  String _otaError;

  // --- 鉴权辅助 ---
  String _reqToken();          // 从 Cookie / Authorization 里取会话令牌
  bool   _sameOrigin();        // 简易 CSRF 校验
  // allowDefaultPass=true 的接口允许"还没改掉出厂密码"时调用（初次配网必须能连 WiFi）
  bool   _requireAuth(bool allowDefaultPass = false);

  // 路由
  void _handleRoot();
  void _handleStatus();
  void _handleAuthState();
  void _handleLogin();
  void _handleLogout();
  void _handleChangePass();
  void _handleFactoryReset();
  void _handleWifiScan();
  void _handleWifiConnect();
  void _handleRestart();
  void _handlePrinterReset();
  void _handleTestPage();
  void _handleOta();        // 上传结束后回调
  void _handleOtaUpload();  // 上传过程中流式写 flash
  void _handleDebug();      // 读取 RTC 崩溃日志
  void _handleNotFound();

  void _sendPage();
  static String _jsonEscape(const String& s);
};

#endif // WEB_UI_H
