#include "web_ui.h"
#include "crashlog.h"
#include "esp_task_wdt.h"

// 任务句柄与 OTA 总开关（定义在 .ino 里）。
//
// ⚠️ v2.6.6：写 flash 期间必须把**所有碰 USB 的东西**都停掉。
// 原因：Update.begin() 会擦除整个 app 分区（1.9MB），Update.write() 逐块写入，
// 这段时间 flash cache 被禁用 —— 此时任何"从 flash 取指令"的代码执行都会 panic
// （Cache disabled but cached memory region accessed）。USB 驱动的代码就在 flash 上。
// 实测症状：固件上传完全部成功 → 设备立刻重启 → 分区写了一半 →
// bootloader 回退到旧分区 → **版本号纹丝不动**，OTA 永远失败。
//
// 三件事缺一不可：
//   ① g_otaBusy = true  → 打印任务暂停业务并**取消 WDT 订阅**（它订阅了 WDT，
//      挂起它就没人喂狗，超过 60 秒照样 WDT panic —— 所以不能挂起它）
//   ② 挂起 usbTaskHandle → 我们的 USB Host 任务不碰 USB 控制器
//   ③ 断开打印机设备 → 让 IDF 内部的 usbHostTask 也无事可做
extern TaskHandle_t usbTaskHandle;
extern volatile bool g_otaBusy;
// 全局 UsbPrinter 对象（定义在 .ino 里）
extern UsbPrinter usbPrinter;
static bool s_usbTaskSuspended = false;

static void otaSuspendPrintTask() {
  // ① 通知打印任务：暂停业务 + 取消 WDT 订阅（不要挂起它！）
  g_otaBusy = true;

  // ② 挂起 USB Host 任务（它没订阅 WDT，挂起是安全的）→ 我们的 USB 任务不碰 USB 控制器。
  //
  // ⚠️ v2.6.8：**不再断开打印机设备**。
  //    v2.6.6/2.6.7 为了让 IDF 内部 usbHostTask 也闲下来，会先断开打印机 ——
  //    副作用是**设备重启后打印机不再自动重新枚举**（实测 1390 升级后
  //    usb.attached=false，得靠人拔插 USB 线）。
  //    实测证明：只要打印机状态正常（不卡在半截作业上），写 flash 期间
  //    挂起我们自己的 USB 任务就够了，OTA 本来就是概率性成功（HP 撞了 4 次才成，
  //    1390 两次都成）—— 不需要靠断开来提高成功率。
  if (usbTaskHandle != NULL && usbTaskHandle != xTaskGetCurrentTaskHandle()) {
    vTaskSuspend(usbTaskHandle);
    s_usbTaskSuspended = true;
  }
}
static void otaResumePrintTask() {
  if (s_usbTaskSuspended && usbTaskHandle != NULL) {
    vTaskResume(usbTaskHandle);
    s_usbTaskSuspended = false;
  }
  g_otaBusy = false;
}

static String hex4(uint16_t v) { char b[8]; snprintf(b, sizeof(b), "%04X", v); return String(b); }
static String hex2(uint8_t v)  { char b[4]; snprintf(b, sizeof(b), "%02X", v); return String(b); }

String WebUI::_jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if ((uint8_t)c < 0x20) { /* 忽略控制字符 */ }
    else o += c;
  }
  return o;
}

WebUI::WebUI(WiFiManager& wifi, UsbPrinter& printer, PrintServer& printSvr)
  : _server(WEB_SERVER_PORT), _wifi(wifi), _printer(printer), _printSvr(printSvr),
    _otaRejected(false) {}

// ===============================================================
//  鉴权
// ===============================================================
String WebUI::_reqToken() {
  if (_server.hasHeader("Cookie")) {
    String c = _server.header("Cookie");
    int i = c.indexOf(AUTH_SESSION_COOKIE "=");
    if (i >= 0) {
      String t = c.substring(i + strlen(AUTH_SESSION_COOKIE) + 1);
      int e = t.indexOf(';');
      if (e >= 0) t = t.substring(0, e);
      t.trim();
      if (t.length()) return t;
    }
  }
  if (_server.hasHeader("Authorization")) {
    String a = _server.header("Authorization");
    if (a.startsWith("Bearer ")) return a.substring(7);
  }
  // ⚠️ v2.6.7 兜底：WebServer 会把请求里的 cookie 自动解析进 arg() 集合，
  // 这条路**不受 collectHeaders 限制**。在 multipart upload 回调里，
  // hasHeader("Cookie") 偶尔取不到值（实测：带 Cookie 头上传固件会被判成
  // "未登录"），但 arg("PSID") 一直都在 —— 浏览器发起的 OTA 正是靠 Cookie 鉴权，
  // 走不通就等于网页上的固件上传按钮完全不能用。
  if (_server.hasArg(AUTH_SESSION_COOKIE)) {
    String t = _server.arg(AUTH_SESSION_COOKIE);
    t.trim();
    if (t.length()) return t;
  }
  return String();
}

// 简易 CSRF 防护：跨站请求一定带 Origin，这里要求它和 Host 一致
bool WebUI::_sameOrigin() {
  if (!_server.hasHeader("Origin")) return true;
  // Host 用专门的接口读，不依赖 collectHeaders 里是否登记了该名字
  String host = _server.hostHeader();
  if (!host.length())                return true;   // 老式请求没有 Host：不放行反而不合理
  String rest = _server.header("Origin");
  int sl = rest.indexOf("//");
  if (sl >= 0) rest = rest.substring(sl + 2);
  int sp = rest.indexOf('/');
  if (sp > 0) rest = rest.substring(0, sp);
  return rest.equalsIgnoreCase(host);
}

bool WebUI::_requireAuth(bool allowDefaultPass) {
#if !AUTH_ENABLED
  (void)allowDefaultPass;
  return true;
#else
  if (_server.method() != HTTP_GET && !_sameOrigin()) {
    _server.send(403, "application/json; charset=utf-8", "{\"error\":\"cross-origin\"}");
    return false;
  }
  String tok = _reqToken();
  if (!_auth.validate(tok)) {
    _server.send(401, "application/json; charset=utf-8", "{\"error\":\"unauthorized\"}");
    return false;
  }
  // 出厂默认密码还挂着：除了"配 WiFi"和"改密码"之外一律拒绝，
  // 逼着用户先把默认口令换掉再上线。
  if (!allowDefaultPass && !_auth.hasCustomPassword()) {
    _server.send(403, "application/json; charset=utf-8",
                 "{\"error\":\"default-password\",\"needChange\":true}");
    return false;
  }
  return true;
#endif
}

// ===============================================================
void WebUI::begin() {
  _auth.begin();

  // 关键：WebServer 默认只收集 Authorization / ETag 两个请求头，
  // 其余一律 _collectHeader 里被 "header skipped" 丢掉。
  // 不在这里登记 Cookie / Origin，hasHeader("Cookie") 永远为假 →
  // 登录成功但之后每个请求都 401。
  static const char* kCollect[] = {"Cookie", "Origin"};
  _server.collectHeaders(kCollect, 2);

  _server.on("/",                     HTTP_GET,  [this]() { _handleRoot(); });
  _server.on("/api/auth/state",       HTTP_GET,  [this]() { _handleAuthState(); });
  _server.on("/api/auth/login",       HTTP_POST, [this]() { _handleLogin(); });
  _server.on("/api/auth/logout",      HTTP_POST, [this]() { _handleLogout(); });
  _server.on("/api/auth/passwd",      HTTP_POST, [this]() { _handleChangePass(); });
  _server.on("/api/status",           HTTP_GET,  [this]() { _handleStatus(); });
  _server.on("/api/wifi/scan",        HTTP_GET,  [this]() { _handleWifiScan(); });
  _server.on("/api/wifi/connect",     HTTP_POST, [this]() { _handleWifiConnect(); });
  _server.on("/api/restart",          HTTP_POST, [this]() { _handleRestart(); });
  _server.on("/api/printer/reset",    HTTP_POST, [this]() { _handlePrinterReset(); });
  _server.on("/api/testpage",         HTTP_POST, [this]() { _handleTestPage(); });
  _server.on("/api/factory-reset",    HTTP_POST, [this]() { _handleFactoryReset(); });
  _server.on("/api/ota",              HTTP_POST,
             [this]() { _handleOta(); },
             [this]() { _handleOtaUpload(); });
  _server.on("/api/debug",            HTTP_GET,  [this]() { _handleDebug(); });
  _server.onNotFound([this]() { _handleNotFound(); });

  _server.begin();
  Serial.printf("[Web] 管理界面已启动 :%d（写操作需登录）\n", WEB_SERVER_PORT);
}

void WebUI::task() {
  _server.handleClient();
  _auth.task();
}

// 读取 RTC 崩溃日志（reset 不清零，断电才清），用于无串口时定位崩溃点
void WebUI::_handleDebug() {
  if (!_requireAuth()) return;
  String j = "{";
  j += "\"bootCount\":" + String(g_crash.bootCount) + ",";
  j += "\"rebootReason\":" + String(g_crash.rebootReason) + ",";
  j += "\"stageSeq\":" + String(g_crash.stageSeq) + ",";
  // ⚠️ 这些 stage 字符串必须走 _jsonEscape —— 它们来自 RTC（结构体布局变更后
  //    可能残留旧数据），里面可能有控制字符。直接塞进 JSON 会让整个响应
  //    变成**非法 JSON**，所有排障脚本都解析失败（v2.7.3 踩过）。
  j += "\"stage\":\"" + _jsonEscape(String(g_crash.stageStr)) + "\",";
  j += "\"stageLine\":" + String(g_crash.stageLine) + ",";
  j += "\"lastCrashStage\":\"" + _jsonEscape(String(g_crash.lastCrashStage)) + "\",";
  j += "\"lastCrashLine\":" + String(g_crash.lastCrashLine) + ",";
  // bootStage：本次启动走到哪一步（只由 markBootStage 写，热路径不碰）。
  // 固件启动到一半崩溃 + bootloader 回退后，lastCrashStage 就是上一轮的 bootStage，
  // 直接指向崩溃点。
  j += "\"bootStage\":\"" + _jsonEscape(String(g_crash.bootStage)) + "\",";
  j += "\"bootStageLine\":" + String(g_crash.bootStageLine) + ",";
  j += "\"ippDiag\":\"" + _jsonEscape(String(g_crash.ippDiag)) + "\",";
  j += "\"uptimeAtStage\":" + String(g_crash.uptimeAtStage) + ",";
  j += "\"ctrlHung\":" + String(_printer.isCtrlHung() ? "true" : "false") + ",";
  j += "\"leakedXfers\":" + String(_printer.getLeakedXfers()) + ",";
  j += "\"uptime\":" + String(millis() / 1000) + ",";
  j += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"fw\":\"" FW_VERSION "\"";
  j += "}";
  _server.send(200, "application/json; charset=utf-8", j);
}

// ---------------------------------------------------------------
void WebUI::_handleRoot() {
  _sendPage();
}

void WebUI::_handleAuthState() {
  String tok = _reqToken();
  bool ok = _auth.validate(tok);
  String j = "{";
  j += "\"authRequired\":" + String(AUTH_ENABLED ? "true" : "false") + ",";
  j += "\"authenticated\":" + String(ok ? "true" : "false") + ",";
  j += "\"needChange\":" + String(_auth.hasCustomPassword() ? "false" : "true") + ",";
  j += "\"user\":\"" + _jsonEscape(_auth.getUser()) + "\",";
  j += "\"sessions\":" + String(_auth.activeSessions()) + ",";
  j += "\"locked\":" + String(_auth.isLockedOut() ? "true" : "false") + ",";
  j += "\"lockMs\":" + String(_auth.lockoutRemainingMs()) + ",";
  j += "\"failCount\":" + String(_auth.failCount());
  j += "}";
  _server.send(200, "application/json; charset=utf-8", j);
}

// 会话 Cookie：HttpOnly 挡 JS 读取，SameSite=Strict 挡跨站自动带上
static String psCookie(const String& tok, bool drop = false) {
  return String(AUTH_SESSION_COOKIE) + "=" + tok +
         "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
         String(drop ? 0 : AUTH_SESSION_TIMEOUT_MS / 1000);
}

void WebUI::_handleLogin() {
  if (!_sameOrigin()) { _server.send(403, "application/json", "{\"success\":false,\"error\":\"cross-origin\"}"); return; }

  // 已经拿着有效会话的浏览器重复登录（刷新页面、多标签打开）——直接续期，不新占槽位。
  // 否则每次刷新都烧掉一个会话槽，很快就满了。
  String cur = _reqToken();
  if (cur.length() && _auth.validate(cur)) {
    _server.sendHeader("Set-Cookie", psCookie(cur));
    String j2 = "{\"success\":true,\"needChange\":";
    j2 += (_auth.hasCustomPassword() ? "false" : "true");
    j2 += "}";
    _server.send(200, "application/json; charset=utf-8", j2);
    return;
  }

  String tok, err;
  if (_auth.login(_server.arg("user"), _server.arg("pass"), tok, err)) {
    _server.sendHeader("Set-Cookie", psCookie(tok));
    String j = "{\"success\":true,\"needChange\":";
    j += (_auth.hasCustomPassword() ? "false" : "true");
    j += "}";
    _server.send(200, "application/json; charset=utf-8", j);
  } else {
    _server.send(401, "application/json; charset=utf-8",
                 "{\"success\":false,\"error\":\"" + _jsonEscape(err) + "\"}");
  }
}

void WebUI::_handleLogout() {
  _auth.logout(_reqToken());
  _server.sendHeader("Set-Cookie", psCookie("", true));
  _server.send(200, "application/json", "{\"success\":true}");
}

void WebUI::_handleChangePass() {
  String tok = _reqToken();
  if (!_auth.validate(tok)) {
    _server.send(401, "application/json", "{\"success\":false,\"error\":\"请先登录\"}");
    return;
  }
  String op = _server.arg("oldpass");
  String np = _server.arg("newpass");
  String err;
  if (_server.arg("newpass2") != np) {
    _server.send(400, "application/json; charset=utf-8", "{\"success\":false,\"error\":\"两次输入的新密码不一致\"}");
    return;
  }
  if (!_auth.changePassword(op, np, err)) {
    _server.send(400, "application/json; charset=utf-8",
                 "{\"success\":false,\"error\":\"" + _jsonEscape(err) + "\"}");
    return;
  }
  // 改密会踢掉所有会话，这里顺手给当前浏览器签一个新的，用户不用重新登录
  String tok2, err2;
  if (_auth.login(_auth.getUser(), np, tok2, err2)) {
    _server.sendHeader("Set-Cookie", psCookie(tok2));
  }
  _server.send(200, "application/json; charset=utf-8", "{\"success\":true}");
}

void WebUI::_handleFactoryReset() {
  // 恢复出厂允许在"还没改默认密码"时也执行（本身就是兜底手段）
  if (!_requireAuth(true)) return;
  _server.send(200, "application/json", "{\"success\":true}");
  delay(300);
  _wifi.clearCredentials();
  _auth.clearPassword();
  Serial.println("[SYS] 恢复出厂设置，即将重启");
  ESP.restart();
}

void WebUI::_handleStatus() {
  if (!_requireAuth(true)) return;    // 只读数据，登录即可看
  const UsbInfo&     ui = _printer.getUsbInfo();
  const PrinterInfo& pi = _printer.getPrinterInfo();
  PrintJob job = _printSvr.getCurrentJob();

  String j = "{";
  j += "\"wifi\":{";
  j += "\"connected\":" + String(_wifi.isConnected() ? "true" : "false") + ",";
  j += "\"ssid\":\"" + _jsonEscape(_wifi.getSSID()) + "\",";
  j += "\"ip\":\"" + _wifi.getIP() + "\",";
  j += "\"rssi\":" + String(_wifi.getRSSI()) + ",";
  // v2.5.0：链路质量（排"网络抖动"用）
  j += "\"rssiMin\":" + String(_wifi.getRssiMin()) + ",";
  j += "\"rssiAvg\":" + String(_wifi.getRssiAvg()) + ",";
  j += "\"offlineMs\":" + String(_wifi.getOfflineMs()) + ",";
  j += "\"disconnects\":" + String(_wifi.getDisconnectCount()) + ",";
  j += "\"reconnects\":" + String(_wifi.getReconnectCount()) + ",";
  j += "\"retryMs\":" + String(_wifi.getRetryIntervalMs()) + ",";
  j += "\"fallbackAp\":" + String(_wifi.isFallbackAp() ? "true" : "false") + ",";
  j += "\"mode\":\"" + String(_wifi.getMode() == WSTATE_STATION ? "station" : "ap") + "\"},";

  j += "\"printer\":{";
  j += "\"state\":" + String((int)_printer.getState()) + ",";
  j += "\"stateText\":\"" + _jsonEscape(_printer.getStateText()) + "\",";
  j += "\"connected\":" + String(_printer.isConnected() ? "true" : "false") + ",";
  j += "\"ready\":" + String(_printer.isReady() ? "true" : "false") + ",";
  j += "\"mfg\":\"" + _jsonEscape(pi.mfg) + "\",";
  j += "\"mdl\":\"" + _jsonEscape(pi.mdl) + "\",";
  j += "\"cmd\":\"" + _jsonEscape(pi.cmd) + "\",";
  j += "\"cls\":\"" + _jsonEscape(pi.cls) + "\",";
  j += "\"des\":\"" + _jsonEscape(pi.des) + "\",";
  j += "\"sern\":\"" + _jsonEscape(pi.sern) + "\",";
  j += "\"deviceId\":\"" + _jsonEscape(pi.deviceId) + "\",";
  j += "\"statusText\":\"" + _jsonEscape(_printer.getStatusText()) + "\",";
  j += "\"portStatus\":" + String(_printer.getPortStatus()) + ",";
  j += "\"portStatusValid\":" + String(_printer.portStatusValid() ? "true" : "false") + ",";
  j += "\"statusAge\":" + String(_printer.portStatusAge()) + ",";
  j += "\"paperOut\":" + String(_printer.isPaperOut() ? "true" : "false") + ",";
  j += "\"offline\":" + String(_printer.portStatusValid() && !_printer.isSelected() ? "true" : "false") + ",";
  j += "\"error\":" + String(_printer.hasError() ? "true" : "false") + ",";
  // 连续无响应时进入快速失败保护（此时网页仍然可用），原因直接告诉用户
  j += "\"stalled\":" + String(_printer.isStalled() ? "true" : "false") + ",";
  j += "\"bidir\":" + String(_printer.isBidirActive() ? "true" : "false") + ",";
  // 回读通道是否处于"暂停、等重试"状态（v2.6.4 起会定时重试）
  j += "\"bidirStalled\":" + String(_printer.isBidirStalled() ? "true" : "false") + ",";
  // 纯透传模式：固件一个字都不往打印机嘴里塞（Epson 1390 等自带收尾协议的机型）
  j += "\"passthrough\":" + String(_printer.passthrough() ? "true" : "false") + ",";
  // 主机型不认纯文本测试页（发过去会把它卡死），网页据此隐藏按钮
  j += "\"stallReason\":\"" + _jsonEscape(_printer.getStallReason()) + "\",";
  j += "\"testPageOk\":" + String(_printer.textTestPageSupported() ? "true" : "false") + "},";

  j += "\"usb\":{";
  j += "\"attached\":" + String(ui.attached ? "true" : "false") + ",";
  j += "\"devAddr\":" + String(ui.devAddr) + ",";
  j += "\"vid\":\"" + hex4(ui.vid) + "\",";
  j += "\"pid\":\"" + hex4(ui.pid) + "\",";
  j += "\"bcdUsb\":\"" + hex4(ui.bcdUsb) + "\",";
  j += "\"iface\":" + String(ui.ifaceNum) + ",";
  j += "\"ifaceClass\":\"" + hex2(ui.ifaceClass) + "\",";
  j += "\"subClass\":\"" + hex2(ui.ifaceSubClass) + "\",";
  j += "\"protocol\":" + String(ui.ifaceProtocol) + ",";
  j += "\"epOut\":\"" + hex2(ui.epOut) + "\",";
  j += "\"epIn\":\"" + hex2(ui.epIn) + "\",";
  j += "\"epOutMps\":" + String(ui.epOutMps) + ",";
  j += "\"epInMps\":" + String(ui.epInMps) + ",";
  // 提示这台机器是标准打印机类还是厂商私有/描述不标准的 IPP 设备
  j += "\"ipp\":" + String(_printer.isIppDevice() ? "true" : "false") + ",";
  j += "\"ippIfaces\":" + String(ui.ippIfaces) + ",";
  j += "\"enumCount\":" + String(ui.enumCount) + ",";
  j += "\"lastError\":" + String(ui.lastError) + "},";

  j += "\"job\":{";
  j += "\"active\":" + String(job.active ? "true" : "false") + ",";
  j += "\"bytesReceived\":" + String(job.bytesReceived) + ",";
  j += "\"bytesSent\":" + String(job.bytesSent) + ",";
  // 打印机经 Bulk IN 回吐、已写回客户端的字节数（>0 说明双向通道通了）
  j += "\"bytesReturned\":" + String(job.bytesReturned) + ",";
  j += "\"client\":\"" + String(job.clientIP) + "\",";
  // 上一份作业最后 24 字节（十六进制）。用来判断驱动有没有自己收尾：
  // UEL "1B 25 2D 31 32 33 34 35 58" / "%%EOF" / "@PJL EOJ" / 以 "0C" 结尾
  // = 已自收尾，固件不该再补 FF。
  j += "\"tailHex\":\"" + _jsonEscape(_printSvr.getJobTailHex()) + "\",";
  // 作业开头 24 字节：%PDF=PCLm、@PJL=PJL 系、%!PS=PostScript、1B 40=ESC/P，一眼看出语言
  j += "\"headHex\":\"" + _jsonEscape(_printSvr.getJobHeadHex()) + "\",";
  j += "\"elapsed\":" + String(job.active ? (millis() - job.startTime) : 0) + "},";

  j += "\"stats\":{";
  j += "\"totalJobs\":" + String(_printSvr.getTotalJobs()) + ",";
  j += "\"totalBytes\":" + String(_printSvr.getTotalBytes()) + ",";
  j += "\"failedJobs\":" + String(_printSvr.getFailedJobs()) + ",";
  j += "\"txErrors\":" + String(_printer.getTxErrors()) + ",";
  j += "\"recover\":" + String(_printer.getRecoverCount()) + ",";
  j += "\"recoverOk\":" + String(_printer.getRecoverOk()) + ",";
  // 累计从 Bulk IN 读回的字节数（打印机侧 -> 板子，含无客户端时被丢弃的）
  j += "\"rxBytes\":" + String(_printer.getRxBytes()) + ",";
  // 因为已有作业在跑而被拒的连接数（Windows 后台重试时会看到它涨）
  j += "\"rejected\":" + String(_printSvr.getRejectedCount()) + ",";
  j += "\"lastJob\":\"" + _jsonEscape(_printSvr.getLastJob()) + "\",";
  j += "\"uptime\":" + String(millis() / 1000) + ",";
  j += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"freePsram\":" + String(ESP.getFreePsram()) + ",";
  j += "\"fw\":\"" + String(FW_VERSION) + "\"},";

  j += "\"ops\":{";
  j += "\"testpage\":" + String((int)_printSvr.testPageState()) + ",";
  j += "\"testpageMsg\":\"" + _jsonEscape(_printSvr.testPageMsg()) + "\",";
  j += "\"usbReset\":" + String((int)_printSvr.resetState()) + ",";
  j += "\"usbResetMsg\":\"" + _jsonEscape(_printSvr.resetMsg()) + "\"}";

  j += "}";
  _server.send(200, "application/json; charset=utf-8", j);
}

void WebUI::_handleWifiScan() {
  if (!_requireAuth()) return;
  _server.send(200, "application/json; charset=utf-8", _wifi.scanNetworks());
}

void WebUI::_handleWifiConnect() {
  // 配网是最基础的功能，出厂默认密码状态下也允许（否则新设备根本连不上网）
  if (!_requireAuth(true)) return;
  String ssid = _server.arg("ssid");
  String pass = _server.arg("password");
  if (ssid.length() == 0) {
    _server.send(400, "application/json", "{\"error\":\"SSID is required\"}");
    return;
  }
  bool ok = _wifi.setCredentials(ssid, pass);
  String j = "{\"success\":" + String(ok ? "true" : "false");
  j += ",\"ip\":\"" + _wifi.getIP() + "\"}";
  _server.send(200, "application/json", j);
}

void WebUI::_handleRestart() {
  if (!_requireAuth()) return;
  _server.send(200, "application/json", "{\"success\":true}");
  delay(300);
  ESP.restart();
}

void WebUI::_handlePrinterReset() {
  if (!_requireAuth()) return;
  // 软复位要走控制传输，打印机不应答时能阻塞几十秒 —— 交给打印任务去做，
  // 网页立刻返回，前端轮询 ops.usbReset 看结果。
  _printSvr.requestPrinterReset();
  _server.send(200, "application/json; charset=utf-8", "{\"success\":true,\"pending\":true}");
}

void WebUI::_handleTestPage() {
  if (!_requireAuth()) return;
  _printSvr.requestTestPage();
  _server.send(200, "application/json; charset=utf-8", "{\"success\":true,\"pending\":true}");
}

void WebUI::_handleNotFound() {
  _server.send(404, "text/plain", "Not Found");
}

// ---------------------------------------------------------------
//  静态页面（放 flash，不像以前那样每次请求都在堆上拼一个 30KB 的 String）
// ---------------------------------------------------------------
static const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b1220">
<title>WiFi 打印服务器</title>
<style>
:root{
  --bg:#0b1220;--card:#141c2e;--card2:#1b2537;--line:#26314a;--tx:#e6ecf7;--tx2:#93a2bd;--tx3:#68788f;
  --ok:#34d399;--okb:#0d3b31;--warn:#fbbf24;--warnb:#412f0a;--err:#f87171;--errb:#48181a;
  --acc:#4f8cff;--acc2:#2f6ae0;
}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  background:var(--bg);color:var(--tx);min-height:100vh;-webkit-font-smoothing:antialiased;line-height:1.5}
.wrap{max-width:1080px;margin:0 auto;padding:10px 14px 48px}
header.top{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 2px 12px}
.brand{display:flex;align-items:center;gap:11px;min-width:0}
.ico{width:38px;height:38px;border-radius:10px;background:linear-gradient(135deg,var(--acc),#7c3aed);
  display:flex;align-items:center;justify-content:center;font-size:19px;flex:none}
.brand .t{font-size:1.02rem;font-weight:650;letter-spacing:.3px;white-space:nowrap}
.brand .s{font-size:.74rem;color:var(--tx3);margin-top:1px;overflow:hidden;text-overflow:ellipsis}
.chip{font-size:.72rem;color:var(--tx2);background:var(--card2);border:1px solid var(--line);
  padding:4px 10px;border-radius:999px;white-space:nowrap}
nav.tabs{display:flex;gap:4px;overflow-x:auto;padding:6px 0 10px;border-bottom:1px solid var(--line);margin-bottom:14px}
.tab{flex:none;background:transparent;border:none;color:var(--tx3);font-size:.86rem;font-weight:600;
  padding:8px 14px;border-radius:9px;cursor:pointer;transition:.15s;white-space:nowrap;font-family:inherit}
.tab:hover{color:var(--tx2);background:var(--card)}
.tab.active{color:#fff;background:var(--acc)}
.panel{display:none}.panel.on{display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:13px;padding:15px 17px}
.card h2{font-size:.74rem;text-transform:uppercase;letter-spacing:.08em;color:var(--tx3);margin-bottom:11px;font-weight:700}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:7px 0;border-bottom:1px solid var(--line)}
.row:last-child{border-bottom:none}
.k{color:var(--tx2);font-size:.83rem;white-space:nowrap}
.v{font-weight:550;font-size:.83rem;text-align:right;word-break:break-all}
.badge{display:inline-block;padding:2px 10px;border-radius:999px;font-size:.73rem;font-weight:650}
.b-green{background:var(--okb);color:var(--ok)}.b-red{background:var(--errb);color:var(--err)}
.b-yellow{background:var(--warnb);color:var(--warn)}.b-gray{background:var(--card2);color:var(--tx2)}
.b-blue{background:#12335e;color:#93c5fd}
.btn{padding:9px 15px;border:none;border-radius:9px;font-size:.85rem;font-weight:600;cursor:pointer;
  transition:.16s;font-family:inherit;background:var(--card2);color:var(--tx)}
.btn:hover{opacity:.86}
.btn-primary{background:var(--acc);color:#fff}.btn-primary:hover{background:var(--acc2)}
.btn-danger{background:var(--errb);color:var(--err);border:1px solid #7f2b2b}
.btn-ghost{background:transparent;border:1px solid var(--line);color:var(--tx2)}
.btn-sm{padding:7px 12px;font-size:.8rem}
.btn:disabled{opacity:.45;cursor:not-allowed}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
select,input[type=text],input[type=password],input[type=file]{width:100%;padding:9px 11px;margin-bottom:9px;
  background:#0f1729;border:1px solid var(--line);border-radius:9px;color:var(--tx);font-size:.88rem;font-family:inherit}
select:focus,input:focus{outline:none;border-color:var(--acc)}
input::placeholder{color:#54637a}
.bar{height:6px;background:var(--card2);border-radius:3px;overflow:hidden;margin-top:10px}
.fill{height:100%;background:var(--acc);width:0;transition:width .25s}
.hint{color:var(--tx3);font-size:.77rem;line-height:1.65;margin-top:9px}
.hint b{color:var(--tx2)}
.banner{display:flex;gap:10px;align-items:flex-start;padding:11px 14px;border-radius:11px;
  font-size:.83rem;margin-bottom:13px;line-height:1.6}
.banner.warn{background:var(--warnb);border:1px solid #7a5a12;color:#fde68a}
.banner.err{background:var(--errb);border:1px solid #7f2b2b;color:#fecaca}
.banner.ok{background:var(--okb);border:1px solid #166e56;color:#a7f3d0}
.box-none{display:none}
ol{margin:0;padding-left:19px;color:var(--tx2);font-size:.8rem;line-height:1.85}
ol b{color:var(--tx)}
.two{display:grid;grid-template-columns:1fr 1fr;gap:18px}
@media(max-width:640px){.two{grid-template-columns:1fr}.brand .t{font-size:.95rem}}
.pulse{animation:pu 1.4s infinite}@keyframes pu{0%,100%{opacity:1}50%{opacity:.42}}
.mask{position:fixed;inset:0;background:rgba(6,10,20,.86);backdrop-filter:blur(4px);
  display:flex;align-items:center;justify-content:center;padding:20px;z-index:50}
.login{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:24px 22px;width:100%;max-width:372px}
.login h2{font-size:1.05rem;margin-bottom:6px}
#toast{position:fixed;left:50%;transform:translateX(-50%);bottom:22px;z-index:80;display:flex;
  flex-direction:column;gap:8px;align-items:center;pointer-events:none}
.tst{background:#1e2a42;border:1px solid var(--line);color:var(--tx);padding:9px 16px;border-radius:10px;
  font-size:.82rem;box-shadow:0 8px 26px rgba(0,0,0,.45);animation:fade 2.6s forwards}
.tst.ok{border-color:#166e56;color:#a7f3d0}.tst.err{border-color:#7f2b2b;color:#fecaca}
@keyframes fade{0%{opacity:0;transform:translateY(8px)}12%{opacity:1;transform:none}
  82%{opacity:1}100%{opacity:0;transform:translateY(6px)}}
.foot{text-align:center;color:#3c4a63;font-size:.72rem;margin-top:22px}
code{background:#0f1729;padding:1px 5px;border-radius:4px;font-size:.78rem;color:#93c5fd}
</style>
</head>
<body>
<div class="wrap">
  <header class="top">
    <div class="brand">
      <div class="ico">&#x1F5A8;</div>
      <div>
        <div class="t">WiFi 打印服务器</div>
        <div class="s" id="topSub">正在读取状态…</div>
      </div>
    </div>
    <div style="display:flex;gap:8px;align-items:center">
      <span class="chip" id="topFw">—</span>
      <button class="btn btn-ghost btn-sm box-none" id="btnLogout" onclick="doLogout()">退出</button>
    </div>
  </header>

  <div class="banner warn box-none" id="needChange">
    <span>&#9888;</span>
    <div><b>正在使用出厂默认密码。</b>出于安全考虑，除「连接 WiFi」和「修改密码」外的所有操作已被锁定。
    请到 <b>系统 → 修改管理员密码</b> 里先改掉默认口令。</div>
  </div>
  <div class="banner err box-none" id="prStall">
    <span>&#9888;</span>
    <div id="prStallTxt"></div>
  </div>

  <nav class="tabs">
    <button class="tab active" id="tab-ov" onclick="tab('ov')">概览</button>
    <button class="tab" id="tab-pr" onclick="tab('pr')">打印机</button>
    <button class="tab" id="tab-net" onclick="tab('net')">网络</button>
    <button class="tab" id="tab-sys" onclick="tab('sys')">系统</button>
    <button class="tab" id="tab-gd" onclick="tab('gd')">添加打印机</button>
  </nav>

  <!-- ========== 概览 ========== -->
  <div class="panel on" id="pnl-ov">
    <div class="grid">
      <div class="card">
        <h2>打印机</h2>
        <div class="row"><span class="k">状态</span><span id="p-state" class="badge b-gray">—</span></div>
        <div class="row"><span class="k">1284 端口</span><span id="p-port" class="badge b-gray">—</span></div>
        <div class="row"><span class="k">型号</span><span class="v" id="p-mdl">—</span></div>
        <div class="row"><span class="k">制造商</span><span class="v" id="p-mfg">—</span></div>
        <div class="row"><span class="k">命令语言</span><span class="v" id="p-cmd">—</span></div>
      </div>

      <div class="card">
        <h2>当前作业</h2>
        <div class="row"><span class="k">进行中</span><span id="j-active" class="badge b-gray">空闲</span></div>
        <div class="row"><span class="k">客户端</span><span class="v" id="j-client">—</span></div>
        <div class="row"><span class="k">已接收</span><span class="v" id="j-recv">—</span></div>
        <div class="row"><span class="k">已发往 USB</span><span class="v" id="j-sent">—</span></div>
        <div class="row"><span class="k">回读已回收</span><span class="v" id="j-ret">—</span></div>
        <div class="row"><span class="k">速率 / 用时</span><span class="v" id="j-rate">—</span></div>
        <div class="hint">Raw Socket :9100 &middot; 同一时刻只接受一个连接</div>
      </div>

      <div class="card">
        <h2>统计</h2>
        <div class="row"><span class="k">累计作业</span><span class="v" id="s-jobs">0</span></div>
        <div class="row"><span class="k">累计数据量</span><span class="v" id="s-bytes">0 B</span></div>
        <div class="row"><span class="k">失败作业</span><span class="v" id="s-fail">0</span></div>
        <div class="row"><span class="k">USB 传输错误</span><span class="v" id="s-txerr">0</span></div>
        <div class="row"><span class="k">忙时拒绝连接</span><span class="v" id="s-rej">0</span></div>
        <div class="row"><span class="k">最近一次</span><span class="v" id="s-last">—</span></div>
      </div>

      <div class="card">
        <h2>设备</h2>
        <div class="row"><span class="k">运行时长</span><span class="v" id="y-up">—</span></div>
        <div class="row"><span class="k">可用内存</span><span class="v" id="y-heap">—</span></div>
        <div class="row"><span class="k">可用 PSRAM</span><span class="v" id="y-psram">—</span></div>
        <div class="row"><span class="k">固件版本</span><span class="v" id="y-fw">—</span></div>
      </div>
    </div>
  </div>

  <!-- ========== 打印机 ========== -->
  <div class="panel" id="pnl-pr">
    <div class="grid">
      <div class="card">
        <h2>USB 链路</h2>
        <div class="row"><span class="k">连接状态</span><span id="u-conn" class="badge b-gray">—</span></div>
        <div class="row"><span class="k">VID : PID</span><span class="v" id="u-vidpid">—</span></div>
        <div class="row"><span class="k">USB 版本</span><span class="v" id="u-ver">—</span></div>
        <div class="row"><span class="k">设备地址</span><span class="v" id="u-addr">—</span></div>
        <div class="row"><span class="k">打印机接口</span><span class="v" id="u-iface">—</span></div>
        <div class="row"><span class="k">传输协议</span><span class="v" id="u-proto">—</span></div>
        <div class="row"><span class="k">Bulk OUT</span><span class="v" id="u-epout">—</span></div>
        <div class="row"><span class="k">Bulk IN</span><span class="v" id="u-epin">—</span></div>
        <div class="row"><span class="k">IPP-over-USB</span><span id="u-ipp" class="badge b-gray">—</span></div>
        <div class="row"><span class="k">重枚举次数</span><span class="v" id="u-enum">—</span></div>
        <div class="hint" id="p-raw">—</div>
      </div>

      <div class="card">
        <h2>打印机详情</h2>
        <div class="row"><span class="k">序列号</span><span class="v" id="p-sn">—</span></div>
        <div class="row"><span class="k">设备类别</span><span class="v" id="p-cls">—</span></div>
        <div class="row"><span class="k">端口状态</span><span class="v" id="p-status">—</span></div>
        <div class="row"><span class="k">状态更新</span><span class="v" id="p-age">—</span></div>
        <div class="hint">打印测试页和软复位都是异步执行：点一下按钮后设备会在空闲时处理，
          遇到不应答的打印机最多要等 30 秒，期间网页保持可用。</div>
        <div class="btns">
          <button class="btn btn-primary btn-sm" id="btn-testpage" onclick="testPage()">打印测试页</button>
          <button class="btn btn-sm" onclick="prnReset()">软复位打印机</button>
        </div>
        <div class="hint" id="tp-note" style="display:none">
          这台打印机是主机型（只认电脑驱动产生的数据流），固件自带的纯文本测试页打不出来，
          还会把它卡在“正在打印”。已禁用该按钮 —— 请用电脑打印一页来验证链路。
        </div>
        <div class="hint" id="op-msg"></div>
      </div>
    </div>
  </div>

  <!-- ========== 网络 ========== -->
  <div class="panel" id="pnl-net">
    <div class="grid">
      <div class="card">
        <h2>WiFi 状态</h2>
        <div class="row"><span class="k">状态</span><span id="w-state" class="badge b-gray">—</span></div>
        <div class="row"><span class="k">网络</span><span class="v" id="w-ssid">—</span></div>
        <div class="row"><span class="k">IP 地址</span><span class="v" id="w-ip">—</span></div>
        <div class="row"><span class="k">信号（当前）</span><span class="v" id="w-rssi">—</span></div>
        <div class="row"><span class="k">近 1 分钟最差</span><span class="v" id="w-rssimin">—</span></div>
        <div class="row"><span class="k">近 1 分钟均值</span><span class="v" id="w-rssiavg">—</span></div>
        <div class="row"><span class="k">离线时长</span><span class="v" id="w-offline">—</span></div>
        <div class="row"><span class="k">断线 / 重连次数</span><span class="v" id="w-cnt">—</span></div>
        <div class="row"><span class="k">当前重试间隔</span><span class="v" id="w-retry">—</span></div>
        <div class="row"><span class="k">后备配网 AP</span><span id="w-fb" class="badge b-gray">未开启</span></div>
        <div class="hint">信号长期低于 <b>-75 dBm</b> 建议挪近路由器或加中继；低于 <b>-60 dBm</b> 才算健康。</div>
      </div>

      <div class="card">
        <h2>连接到别的 WiFi</h2>
        <select id="w-sel"><option value="">-- 先点扫描 --</option></select>
        <input type="password" id="w-pass" placeholder="WiFi 密码" autocomplete="off">
        <div class="btns">
          <button class="btn btn-sm" onclick="scanWifi()">扫描网络</button>
          <button class="btn btn-primary btn-sm" onclick="connectWifi()">连接</button>
        </div>
        <div class="hint">仅支持 2.4GHz。连上后设备会换 IP，页面会自动跳转到新地址。
          如果以后忘了 WiFi 密码或路由器换了 SSID，设备离线 3 分钟后会自动开一个
          <b>PrintServer-Setup</b> 热点，连上去就能重新配置。</div>
      </div>
    </div>
  </div>

  <!-- ========== 系统 ========== -->
  <div class="panel" id="pnl-sys">
    <div class="grid">
      <div class="card">
        <h2>修改管理员密码</h2>
        <input type="password" id="pw-old" placeholder="当前密码" autocomplete="current-password">
        <input type="password" id="pw-new" placeholder="新密码（至少 4 位）" autocomplete="new-password">
        <input type="password" id="pw-new2" placeholder="再输一次新密码" autocomplete="new-password">
        <div class="btns"><button class="btn btn-primary btn-sm" onclick="changePwd()">保存新密码</button></div>
        <div class="hint">改完密码所有登录会话会被踢下线（当前浏览器会自动重新登录）。
          <b>忘记密码的救场办法</b>：按住设备上的 BOOT 键 10 秒不放，即可清空密码回到出厂状态。</div>
      </div>

      <div class="card">
        <h2>固件升级（OTA）</h2>
        <input type="file" id="fw-file" accept=".bin">
        <input type="password" id="fw-pw" placeholder="OTA 密码（config.h 未设就留空）">
        <div class="btns"><button class="btn btn-primary btn-sm" onclick="doOta()">上传并升级</button></div>
        <div class="bar"><div class="fill" id="fw-bar"></div></div>
        <div class="hint" id="fw-msg">选择 Arduino IDE 导出的 .bin 文件，上传后设备自动重启。</div>
      </div>

      <div class="card">
        <h2>运维操作</h2>
        <div class="hint" style="margin-top:0">长时间运行后如果出现打印异常，先试重启；
          仍不行再恢复出厂（会清掉 WiFi 配置和管理员密码，需要重新配网）。</div>
        <div class="btns">
          <button class="btn btn-sm" onclick="restart()">重启设备</button>
          <button class="btn btn-danger btn-sm" onclick="factoryReset()">恢复出厂设置</button>
        </div>
      </div>
    </div>
  </div>

  <!-- ========== 添加打印机 ========== -->
  <div class="panel" id="pnl-gd">
    <div class="card">
      <h2>在电脑上添加这台打印机</h2>
      <div class="two">
        <div>
          <p class="k" style="margin-bottom:8px">Windows 10 / 11</p>
          <ol>
            <li>先装好 <b><span class="pmdl">该打印机</span></b> 的官方驱动</li>
            <li>设置 → 蓝牙和其他设备 → 打印机和扫描仪 → <b>添加设备</b></li>
            <li>点「<b>手动添加</b>」→「<b>使用 TCP/IP 地址或主机名添加打印机</b>」</li>
            <li>设备类型选 <b>TCP/IP 设备</b>，主机名填 <b><span class="ipv">—</span></b></li>
            <li>查询失败时点「<b>自定义</b>」→ 协议选 <b>Raw</b>，端口 <b>9100</b></li>
            <li><b>务必取消勾选「SNMP 状态已启用」</b></li>
            <li>驱动选已安装的 <b><span class="pmdl">该打印机</span></b></li>
          </ol>
        </div>
        <div>
          <p class="k" style="margin-bottom:8px">macOS</p>
          <ol>
            <li>先装 <span class="pmdl">该打印机</span> 的官方驱动（或用 Gutenprint）</li>
            <li>系统设置 → 打印机与扫描器 → <b>添加打印机</b></li>
            <li>切到 <b>IP</b> 标签页</li>
            <li>协议选 <b>HP Jetdirect - Socket</b></li>
            <li>地址填 <b><span class="ipv">—</span></b>，队列留空</li>
            <li>「使用」选 <b>选择软件</b> → 搜 <span class="pmdl">该打印机</span> 的型号</li>
            <li>添加后如果显示离线，手动点「<b>恢复</b>」</li>
          </ol>
        </div>
      </div>
      <div class="hint">驱动装在各自电脑上，ESP32 只做 USB 数据透明转发 —— 这也是它对所有页面描述语言
        （PCL / PostScript / ESC-POS / PJL / GDI 主机型）通用的原因。打印地址 <code>socket://<span class="ipv2">—</span>:9100</code></div>
    </div>
  </div>

  <div class="foot">ESP32-S3 WiFi Print Server</div>
</div>

<!-- ========== 登录遮罩 ========== -->
<div class="mask" id="loginMask">
  <div class="login">
    <h2>管理员登录</h2>
    <p class="hint" id="lgHint" style="margin-bottom:14px">查看和修改设置需要管理员身份。</p>
    <input id="lgUser" type="text" autocomplete="username" placeholder="账号">
    <input id="lgPass" type="password" autocomplete="current-password" placeholder="密码">
    <div class="btns"><button class="btn btn-primary" style="flex:1" onclick="doLogin()">登录</button></div>
    <div class="hint" id="lgMsg"></div>
    <div class="hint" style="border-top:1px solid var(--line);padding-top:10px;margin-top:12px">
      新设备默认账号 <b>admin</b> / 密码 <b>admin</b>。<br>
      忘记密码：按住设备上的 BOOT 键 10 秒不放即可清空。
    </div>
  </div>
</div>

<div id="toast"></div>

<script>
function $(i){return document.getElementById(i)}
function kb(b){if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(1)+' KB';return (b/1048576).toFixed(2)+' MB'}
function ups(s){var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),x=s%60;
  return (d?d+'天':'')+h+'时'+m+'分'+x+'秒'}
function usbVer(h){while(h.length<4)h='0'+h;return parseInt(h.substr(0,2),16)+'.'+h.substr(2)}
function setB(id,cls,txt){var e=$(id);if(!e)return;e.className='badge '+cls;e.textContent=txt}
function show(id,on){var e=$(id);if(e)e.style.display=on?'':'none'}
function toast(m,t){
  var d=document.createElement('div');d.className='tst '+(t||'');d.textContent=m;
  $('toast').appendChild(d);setTimeout(function(){if(d.parentNode)d.parentNode.removeChild(d)},2700);
}
function tab(t){
  var ids=['ov','pr','net','sys','gd'];
  for(var i=0;i<ids.length;i++){
    $('pnl-'+ids[i]).className='panel'+(ids[i]===t?' on':'');
    $('tab-'+ids[i]).className='tab'+(ids[i]===t?' active':'');
  }
}
var AUTH={authRequired:true,authenticated:false,needChange:false};

function post(url,body){
  return fetch(url,{method:'POST',credentials:'same-origin',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(r){return r.json().catch(function(){return {success:r.ok}}) })
    .then(function(d){
      if(d&&d.error==='unauthorized'){needLogin();return {fail:true}}
      if(d&&d.error==='default-password'){toast('请先把出厂默认密码改掉','err');return {fail:true}}
      return d||{};
    });
}

function needLogin(){
  AUTH.authenticated=false;
  $('loginMask').style.display='flex';
  $('lgMsg').textContent='';
  $('btnLogout').style.display='none';
}
function hideLogin(){ $('loginMask').style.display='none'; $('btnLogout').style.display=''; }

function doLogin(){
  var u=$('lgUser').value,p=$('lgPass').value;
  $('lgMsg').textContent='登录中…';
  post('/api/auth/login','user='+encodeURIComponent(u)+'&pass='+encodeURIComponent(p))
  .then(function(d){
    if(d.success){
      $('lgPass').value='';$('lgMsg').textContent='';
      hideLogin();AUTH.authenticated=true;AUTH.needChange=d.needChange;
      toast('登录成功','ok');refresh();
    } else {
      $('lgMsg').textContent=d.error||'登录失败';
      if(d.fail===false){}
    }
  });
}
function doLogout(){
  post('/api/auth/logout','').then(function(){
    needLogin();AUTH.authenticated=false;toast('已退出登录','ok');
  });
}
function changePwd(){
  var o=$('pw-old').value,n=$('pw-new').value,n2=$('pw-new2').value;
  if(!o||!n){ toast('请填写完整','err'); return; }
  post('/api/auth/passwd','oldpass='+encodeURIComponent(o)+'&newpass='+encodeURIComponent(n)+'&newpass2='+encodeURIComponent(n2))
  .then(function(d){
    if(d.success){ toast('密码已更新','ok'); $('pw-old').value='';$('pw-new').value='';$('pw-new2').value='';
      AUTH.needChange=false;refresh(); }
    else toast(d.error||'修改失败','err');
  });
}

function pollAuth(){
  return fetch('/api/auth/state',{credentials:'same-origin'})
    .then(function(r){return r.json()})
    .then(function(a){
      AUTH=a;
      if(!a.authRequired||a.authenticated){hideLogin();}else{needLogin();}
      show('needChange', a.authenticated && a.needChange);
    }).catch(function(){});
}

var lastOps={t:-1,r:-1};
function refresh(){
  if(document.hidden) return Promise.resolve();
  return fetch('/api/status',{credentials:'same-origin'})
   .then(function(r){
     if(r.status===401){needLogin();throw new Error('401')}
     return r.json();
   })
   .then(function(d){
     var ip = d.wifi.connected ? d.wifi.ip : '192.168.4.1';
     $('topSub').textContent = (d.printer.mdl||'打印机') + '  ·  ' + ip;
     $('topFw').textContent = 'v' + d.stats.fw;
     $('y-up').textContent = ups(d.stats.uptime);
     $('y-heap').textContent = (d.stats.freeHeap/1024).toFixed(0)+' KB';
     $('y-psram').textContent = (d.stats.freePsram/1024).toFixed(0)+' KB';
     $('y-fw').textContent = d.stats.fw;

     // 打印机
     var ps=d.printer.state;
     if(ps===3) setB('p-state','b-green pulse','打印中');
     else if(ps===2) setB('p-state','b-green','就绪');
     else if(ps===1) setB('p-state','b-yellow','初始化中');
     else setB('p-state','b-red','未连接');

     if(!d.printer.portStatusValid){ setB('p-port','b-gray','状态不可读'); }
     else if(d.printer.paperOut){ setB('p-port','b-red','缺纸'); }
     else if(d.printer.error){ setB('p-port','b-red','打印机报错'); }
     else if(d.printer.offline){ setB('p-port','b-yellow','离线'); }
     else { setB('p-port','b-green','正常'); }

     var mdlName = d.printer.mdl || '该打印机';
     var pm=document.querySelectorAll('.pmdl');
     for(var i=0;i<pm.length;i++) pm[i].textContent=mdlName;
     $('p-mdl').textContent=d.printer.mdl||'—';
     $('p-mfg').textContent=d.printer.mfg||'—';
     $('p-cmd').textContent=d.printer.cmd||'—';
     $('p-sn').textContent=d.printer.sern||'—';
     $('p-cls').textContent=d.printer.cls||'—';
     $('p-status').textContent=d.printer.statusText||'—';
     var tpOk=(d.printer.testPageOk!==false);
     $('btn-testpage').style.display=tpOk?'':'none';
     $('tp-note').style.display=tpOk?'none':'';
     $('p-age').textContent=d.printer.portStatusValid?(d.printer.statusAge/1000).toFixed(1)+' 秒前':'—';
     $('p-raw').textContent=d.printer.deviceId?('Device ID: '+d.printer.deviceId):'（该机型未提供 Device ID）';

     var st=d.printer.stalled;
     show('prStall',st);
     if(st)$('prStallTxt').innerHTML='<b>打印机连续无响应，已进入保护期。</b>此期间新作业会立刻失败返回，'+
       '但网页保持可用。原因：'+esc(d.printer.stallReason)+'。请先检查打印机是否缺纸、卡纸或处于错误状态。';

     // USB
     setB('u-conn', d.usb.attached?'b-green':'b-red', d.usb.attached?'已连接':'未连接');
     $('u-vidpid').textContent=d.usb.attached?(d.usb.vid+':'+d.usb.pid):'—';
     $('u-ver').textContent=d.usb.attached?usbVer(d.usb.bcdUsb):'—';
     $('u-addr').textContent=d.usb.attached?d.usb.devAddr:'—';
     $('u-iface').textContent=d.usb.attached?('#'+d.usb.iface+'  ('+d.usb.ifaceClass+'/'+d.usb.subClass+')'):'—';
     $('u-proto').textContent=d.usb.attached
       ?(d.usb.protocol===2?'双向 (1284)':d.usb.protocol===1?'单向':d.usb.protocol===3?'IEEE 1284.4':'其他 '+d.usb.protocol):'—';
     $('u-epout').textContent=d.usb.attached?('0x'+d.usb.epOut+'  '+d.usb.epOutMps+' 字节/包'):'—';
     $('u-epin').textContent=d.usb.attached?('0x'+d.usb.epIn+'  '+d.usb.epInMps+' 字节/包'):'—';
     if(d.usb.attached){ if(d.usb.ipp) setB('u-ipp','b-blue','是（'+d.usb.ippIfaces+' 个接口）');
       else setB('u-ipp','b-gray','否'); } else setB('u-ipp','b-gray','—');
     $('u-enum').textContent=d.usb.enumCount;

     // 作业
     if(d.job.active){
       setB('j-active','b-green pulse','进行中');
       var rate=d.job.elapsed>500?(d.job.bytesReceived/1024/(d.job.elapsed/1000)).toFixed(1)+' KB/s':'—';
       $('j-rate').textContent=rate+' / '+(d.job.elapsed/1000).toFixed(1)+' 秒';
     }else{
       setB('j-active','b-gray','空闲');
       $('j-rate').textContent='—';
     }
     $('j-client').textContent=d.job.client||'—';
     $('j-recv').textContent=kb(d.job.bytesReceived);
     $('j-sent').textContent=kb(d.job.bytesSent);
     $('j-ret').textContent=kb(d.job.bytesReturned);

     // 统计
     $('s-jobs').textContent=d.stats.totalJobs;
     $('s-bytes').textContent=kb(d.stats.totalBytes);
     $('s-fail').textContent=d.stats.failedJobs;
     $('s-txerr').textContent=d.stats.txErrors;
     $('s-rej').textContent=d.stats.rejected;
     $('s-last').textContent=d.stats.lastJob||'—';

     // WiFi
     if(d.wifi.connected) setB('w-state','b-green','已连接');
     else setB('w-state', d.wifi.mode==='ap'?'b-yellow':'b-red', d.wifi.mode==='ap'?'配网模式':'已断线');
     $('w-ssid').textContent=d.wifi.ssid||'—';
     $('w-ip').textContent=d.wifi.ip;
     $('w-rssi').textContent=d.wifi.connected?(d.wifi.rssi+' dBm'):'—';
     $('w-rssimin').textContent=d.wifi.rssiMin?(d.wifi.rssiMin+' dBm'):'—';
     $('w-rssiavg').textContent=d.wifi.rssiAvg?(d.wifi.rssiAvg+' dBm'):'—';
     $('w-offline').textContent=d.wifi.offlineMs>0?ups(Math.floor(d.wifi.offlineMs/1000)):'在线';
     $('w-cnt').textContent=d.wifi.disconnects+' 次 / '+d.wifi.reconnects+' 次';
     $('w-retry').textContent=(d.wifi.retryMs/1000)+' 秒';
     if(d.wifi.fallbackAp) setB('w-fb','b-yellow','已开启'); else setB('w-fb','b-gray','未开启');

     var ips=document.getElementsByClassName('ipv');
     for(var k=0;k<ips.length;k++) ips[k].textContent=ip;
     var ips2=document.getElementsByClassName('ipv2');
     for(var k2=0;k2<ips2.length;k2++) ips2[k2].textContent=ip;

     // 异步操作结果
     if(d.ops){
       if(lastOps.t!==d.ops.testpage){
         lastOps.t=d.ops.testpage;
         if(d.ops.testpage===2) toast('测试页：'+d.ops.testpageMsg,'ok');
         else if(d.ops.testpage===3) toast('测试页失败：'+d.ops.testpageMsg,'err');
       }
       if(lastOps.r!==d.ops.usbReset){
         lastOps.r=d.ops.usbReset;
         if(d.ops.usbReset===2) toast(d.ops.usbResetMsg,'ok');
         else if(d.ops.usbReset===3) toast('复位失败：'+d.ops.usbResetMsg,'err');
       }
     }
   }).catch(function(e){});
}
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,function(c){
  return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}

function scanWifi(){
  var sel=$('w-sel');sel.innerHTML='<option>扫描中…</option>';
  fetch('/api/wifi/scan',{credentials:'same-origin'}).then(function(r){
    if(r.status===401){needLogin();throw new Error('401')} return r.json()})
  .then(function(n){
    sel.innerHTML='<option value="">-- 选择网络 --</option>';
    n.sort(function(a,b){return b.rssi-a.rssi});
    n.forEach(function(x){
      var o=document.createElement('option');
      o.value=x.ssid;o.textContent=x.ssid+'  ('+x.rssi+' dBm)'+(x.secure?'  需要密码':'');
      sel.appendChild(o);
    });
    if(!n.length) toast('没有扫描到网络','err');
  }).catch(function(){});
}
function connectWifi(){
  var s=$('w-sel').value,p=$('w-pass').value;
  if(!s){toast('请先扫描并选择网络','err');return}
  post('/api/wifi/connect','ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent(p))
  .then(function(d){
    if(d.success){toast('连接成功，新 IP: '+d.ip+'，正在跳转…','ok');
      setTimeout(function(){location.href='http://'+d.ip},4000);
    } else toast('连接失败，请检查密码','err');
  });
}
function restart(){ if(confirm('确定重启设备？')){post('/api/restart','').then(function(){
  toast('重启中…','ok');setTimeout(function(){location.reload()},11000)});} }
function factoryReset(){
  if(!confirm('恢复出厂会清除 WiFi 配置和管理员密码，之后需要重新配网。确定继续？')) return;
  if(!confirm('再确认一次：真的要恢复出厂设置吗？')) return;
  post('/api/factory-reset','').then(function(){
    toast('正在清除并重启…','ok');setTimeout(function(){location.reload()},11000)});
}
function testPage(){post('/api/testpage','').then(function(d){
  toast(d.pending?'已提交，设备空闲时执行':'已提交','ok')})}
function prnReset(){post('/api/printer/reset','').then(function(d){
  toast(d.pending?'已提交，设备空闲时执行':'已提交','ok')})}

function doOta(){
  var fi=$('fw-file');
  if(!fi.files||!fi.files[0]){msg('请先选择 .bin 固件文件','#f87171');return}
  var pw=$('fw-pw').value;
  var url='/api/ota'+(pw?('?pw='+encodeURIComponent(pw)):'');
  var fd=new FormData();fd.append('firmware',fi.files[0],fi.files[0].name);
  var x=new XMLHttpRequest();x.open('POST',url,true);x.withCredentials=true;
  x.upload.onprogress=function(e){if(e.lengthComputable){
    var p=Math.round(e.loaded*100/e.total);$('fw-bar').style.width=p+'%';msg('上传中 '+p+'%','#93a2bd')}};
  x.onload=function(){
    var ok=false,em='';
    try{var j=JSON.parse(x.responseText);ok=j.success;em=j.error||''}catch(e){em=x.responseText}
    if(ok){msg('上传成功，设备正在重启，约 15 秒后自动刷新','#34d399');
      setTimeout(function(){location.reload()},15000)}
    else{msg('升级失败：'+em,'#f87171');$('fw-bar').style.width='0%'}
  };
  x.onerror=function(){msg('网络错误','#f87171')};
  x.send(fd);
  msg('开始上传…','#93a2bd');
}
function msg(t,c){var e=$('fw-msg');e.textContent=t;e.style.color=c}

$('lgPass').addEventListener('keydown',function(e){if(e.key==='Enter')doLogin()});
pollAuth().then(function(){refresh()});
setInterval(function(){pollAuth()},20000);
setInterval(function(){refresh()},2000);
document.addEventListener('visibilitychange',function(){if(!document.hidden)refresh()});
</script>
</body>
</html>
)rawliteral";

void WebUI::_sendPage() {
  _server.send_P(200, PSTR("text/html; charset=utf-8"), PAGE_HTML);
}

// ---------------------------------------------------------------
// 网页 OTA：multipart 上传，边收边写 flash，不占用大内存
void WebUI::_handleOtaUpload() {
  HTTPUpload& up = _server.upload();

  if (up.status == UPLOAD_FILE_START) {
    _otaError = "";
    _otaRejected = false;

    // 1) 必须已登录，且不是出厂默认密码（默认密码刷机 = 形同虚设）
    if (!_reqToken().length() || !_auth.validate(_reqToken()) || !_auth.hasCustomPassword()) {
      _otaRejected = true;
      _otaError = "未登录或仍在使用出厂默认密码";
      Serial.println("[OTA] 拒绝：鉴权未通过");
      return;
    }

    // 2) config.h 里另外配的 OTA 密码（可选）
    if (strlen(OTA_PASSWORD) > 0) {
      if (_server.arg("pw") != String(OTA_PASSWORD)) {
        _otaRejected = true;
        _otaError = "密码错误";
        Serial.println("[OTA] 拒绝：密码错误");
        return;
      }
    }

    Serial.printf("[OTA] 开始写入: %s\n", up.filename.c_str());

    // 写 flash 前先把打印任务挂起，避免 USB 主机在 flash 操作期间活动导致崩溃
    otaSuspendPrintTask();

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      _otaError = "Update.begin 失败（分区表不支持 OTA？）";
      Update.printError(Serial);
      otaResumePrintTask();
    }
    return;
  }

  if (_otaRejected || _otaError.length()) return;

  if (up.status == UPLOAD_FILE_WRITE) {
    // 整个上传过程都挡住了 loop()，这里必须喂狗，否则 WDT 会在中途把板子 reset
    esp_task_wdt_reset();
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      _otaError = "写入 flash 失败";
      Update.printError(Serial);
      otaResumePrintTask();
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      _otaError = "固件校验/结束失败";
      Update.printError(Serial);
      otaResumePrintTask();
    } else {
      Serial.println("[OTA] 写入完成");
    }
    return;
  }

  if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    _otaError = "上传被中断";
    otaResumePrintTask();
  }
}

void WebUI::_handleOta() {
  if (_otaRejected || _otaError.length()) {
    otaResumePrintTask();
    Update.abort();
    String j = "{\"success\":false,\"error\":\"" + _jsonEscape(_otaError) + "\"}";
    _server.send(500, "application/json; charset=utf-8", j);
    return;
  }
  _server.send(200, "application/json", "{\"success\":true}");
  delay(400);
  ESP.restart();
}
