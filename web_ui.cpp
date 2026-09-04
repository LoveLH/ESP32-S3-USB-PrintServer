#include "web_ui.h"
#include "crashlog.h"

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

void WebUI::begin() {
  _server.on("/",                  HTTP_GET,  [this]() { _handleRoot(); });
  _server.on("/api/status",        HTTP_GET,  [this]() { _handleStatus(); });
  _server.on("/api/wifi/scan",     HTTP_GET,  [this]() { _handleWifiScan(); });
  _server.on("/api/wifi/connect",  HTTP_POST, [this]() { _handleWifiConnect(); });
  _server.on("/api/restart",       HTTP_POST, [this]() { _handleRestart(); });
  _server.on("/api/printer/reset", HTTP_POST, [this]() { _handlePrinterReset(); });
  _server.on("/api/testpage",      HTTP_POST, [this]() { _handleTestPage(); });
  _server.on("/api/ota",           HTTP_POST,
             [this]() { _handleOta(); },
             [this]() { _handleOtaUpload(); });
  _server.on("/api/debug",         HTTP_GET,  [this]() { _handleDebug(); });
  _server.onNotFound([this]() { _handleNotFound(); });

  _server.begin();
  Serial.printf("[Web] 管理界面已启动 :%d\n", WEB_SERVER_PORT);
}

void WebUI::task() {
  _server.handleClient();
}

// 读取 RTC 崩溃日志（reset 不清零，断电才清），用于无串口时定位崩溃点
void WebUI::_handleDebug() {
  String j = "{";
  j += "\"bootCount\":" + String(g_crash.bootCount) + ",";
  j += "\"rebootReason\":" + String(g_crash.rebootReason) + ",";
  j += "\"stageSeq\":" + String(g_crash.stageSeq) + ",";
  j += "\"stage\":\"" + String(g_crash.stageStr) + "\",";
  j += "\"stageLine\":" + String(g_crash.stageLine) + ",";
  j += "\"lastCrashStage\":\"" + String(g_crash.lastCrashStage) + "\",";
  j += "\"lastCrashLine\":" + String(g_crash.lastCrashLine) + ",";
  j += "\"uptimeAtStage\":" + String(g_crash.uptimeAtStage) + ",";
  j += "\"ctrlHung\":" + String(_printer.isCtrlHung() ? "true" : "false") + ",";
  j += "\"leakedXfers\":" + String(_printer.getLeakedXfers()) + ",";
  j += "\"uptime\":" + String(millis() / 1000) + ",";
  j += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"fw\":\"" FW_VERSION "\"";
  j += "}";
  _server.send(200, "application/json", j);
}

// ---------------------------------------------------------------
void WebUI::_handleRoot() {
  _server.send(200, "text/html; charset=utf-8", _generatePage());
}

void WebUI::_handleStatus() {
  const UsbInfo&     ui = _printer.getUsbInfo();
  const PrinterInfo& pi = _printer.getPrinterInfo();
  PrintJob job = _printSvr.getCurrentJob();

  String j = "{";
  j += "\"wifi\":{";
  j += "\"connected\":" + String(_wifi.isConnected() ? "true" : "false") + ",";
  j += "\"ssid\":\"" + _jsonEscape(_wifi.getSSID()) + "\",";
  j += "\"ip\":\"" + _wifi.getIP() + "\",";
  j += "\"rssi\":" + String(_wifi.getRSSI()) + ",";
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
  j += "\"error\":" + String(_printer.hasError() ? "true" : "false") + "},";

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
  j += "\"enumCount\":" + String(ui.enumCount) + ",";
  j += "\"lastError\":" + String(ui.lastError) + "},";

  j += "\"job\":{";
  j += "\"active\":" + String(job.active ? "true" : "false") + ",";
  j += "\"bytesReceived\":" + String(job.bytesReceived) + ",";
  j += "\"bytesSent\":" + String(job.bytesSent) + ",";
  j += "\"client\":\"" + job.clientIP + "\",";
  j += "\"elapsed\":" + String(job.active ? (millis() - job.startTime) : 0) + "},";

  j += "\"stats\":{";
  j += "\"totalJobs\":" + String(_printSvr.getTotalJobs()) + ",";
  j += "\"totalBytes\":" + String(_printSvr.getTotalBytes()) + ",";
  j += "\"failedJobs\":" + String(_printSvr.getFailedJobs()) + ",";
  j += "\"txErrors\":" + String(_printer.getTxErrors()) + ",";
  j += "\"lastJob\":\"" + _jsonEscape(_printSvr.getLastJob()) + "\",";
  j += "\"uptime\":" + String(millis() / 1000) + ",";
  j += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"freePsram\":" + String(ESP.getFreePsram()) + ",";
  j += "\"fw\":\"" + String(FW_VERSION) + "\"}";

  j += "}";
  _server.send(200, "application/json; charset=utf-8", j);
}

void WebUI::_handleWifiScan() {
  _server.send(200, "application/json; charset=utf-8", _wifi.scanNetworks());
}

void WebUI::_handleWifiConnect() {
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
  _server.send(200, "application/json", "{\"success\":true}");
  delay(300);
  ESP.restart();
}

void WebUI::_handlePrinterReset() {
  bool ok = _printer.softReset();
  _server.send(200, "application/json",
               String("{\"success\":") + (ok ? "true" : "false") + "}");
}

void WebUI::_handleTestPage() {
  bool ok = _printSvr.printTestPage();
  _server.send(200, "application/json",
               String("{\"success\":") + (ok ? "true" : "false") + "}");
}

void WebUI::_handleNotFound() {
  _server.send(404, "text/plain", "Not Found");
}

// ---------------------------------------------------------------
// 网页 OTA：multipart 上传，边收边写 flash，不占用大内存
void WebUI::_handleOtaUpload() {
  HTTPUpload& up = _server.upload();

  if (up.status == UPLOAD_FILE_START) {
    _otaError = "";
    _otaRejected = false;

    // 密码校验（config.h 里 OTA_PASSWORD 为空则不校验）
    if (strlen(OTA_PASSWORD) > 0) {
      if (_server.arg("pw") != String(OTA_PASSWORD)) {
        _otaRejected = true;
        _otaError = "密码错误";
        Serial.println("[OTA] 拒绝：密码错误");
        return;
      }
    }

    Serial.printf("[OTA] 开始写入: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      _otaError = "Update.begin 失败（分区表不支持 OTA？）";
      Update.printError(Serial);
    }
    return;
  }

  if (_otaRejected || _otaError.length()) return;

  if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      _otaError = "写入 flash 失败";
      Update.printError(Serial);
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      _otaError = "固件校验/结束失败";
      Update.printError(Serial);
    } else {
      Serial.println("[OTA] 写入完成");
    }
    return;
  }

  if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    _otaError = "上传被中断";
  }
}

void WebUI::_handleOta() {
  if (_otaRejected || _otaError.length()) {
    Update.abort();
    String j = "{\"success\":false,\"error\":\"" + _jsonEscape(_otaError) + "\"}";
    _server.send(500, "application/json; charset=utf-8", j);
    return;
  }
  _server.send(200, "application/json", "{\"success\":true}");
  delay(400);
  ESP.restart();
}

// ---------------------------------------------------------------
String WebUI::_generatePage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>WiFi 打印服务器</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
     background:#0f172a;color:#e2e8f0;min-height:100vh}
.wrap{max-width:1120px;margin:0 auto;padding:20px}
h1{font-size:1.35rem;font-weight:600;margin-bottom:4px;display:flex;align-items:center;gap:10px}
h1 .ver{font-size:.75rem;color:#64748b;font-weight:400}
.sub{color:#64748b;font-size:.82rem;margin-bottom:18px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:14px}
.card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:16px 18px}
.card h2{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:#94a3b8;margin-bottom:12px}
.row{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:7px 0;border-bottom:1px solid #293548}
.row:last-child{border-bottom:none}
.k{color:#94a3b8;font-size:.85rem;white-space:nowrap}
.v{font-weight:500;font-size:.85rem;text-align:right;word-break:break-all}
.badge{display:inline-block;padding:2px 10px;border-radius:999px;font-size:.75rem;font-weight:600}
.b-green{background:#064e3b;color:#6ee7b7}
.b-red{background:#7f1d1d;color:#fca5a5}
.b-yellow{background:#713f12;color:#fde68a}
.b-gray{background:#334155;color:#cbd5e1}
.btn{padding:8px 16px;border:none;border-radius:8px;font-size:.85rem;font-weight:600;cursor:pointer;transition:.2s}
.btn-primary{background:#3b82f6;color:#fff}.btn-primary:hover{background:#2563eb}
.btn-danger{background:#991b1b;color:#fca5a5}.btn-danger:hover{background:#7f1d1d}
.btn-sm{padding:7px 12px;font-size:.8rem}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
select,input[type=text],input[type=password],input[type=file]{width:100%;padding:9px 11px;margin-bottom:9px;
  background:#0f172a;border:1px solid #475569;border-radius:8px;color:#e2e8f0;font-size:.9rem}
select:focus,input:focus{outline:none;border-color:#3b82f6}
.bar{height:6px;background:#334155;border-radius:3px;overflow:hidden;margin-top:9px}
.fill{height:100%;background:#3b82f6;width:0;transition:width .25s}
.hint{color:#64748b;font-size:.78rem;line-height:1.6;margin-top:8px}
ol{margin:0;padding-left:18px;color:#94a3b8;font-size:.82rem;line-height:1.75}
ol b{color:#e2e8f0}
.two{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:620px){.two{grid-template-columns:1fr}}
.pulse{animation:pu 1.4s infinite}
@keyframes pu{0%,100%{opacity:1}50%{opacity:.45}}
.foot{text-align:center;color:#475569;font-size:.76rem;margin:22px 0 8px}
</style>
</head>
<body>
<div class="wrap">
  <h1>&#x1F5A8; WiFi 打印服务器 <span class="ver" id="fwv"></span></h1>
  <div class="sub">ESP32-S3 USB Host 打印共享 &middot; 管理地址 <span id="maddr">—</span></div>

  <div class="grid">

    <div class="card">
      <h2>打印机状态</h2>
      <div class="row"><span class="k">状态</span><span id="p-state" class="badge b-gray">—</span></div>
      <div class="row"><span class="k">1284 端口</span><span id="p-port" class="badge b-gray">—</span></div>
      <div class="row"><span class="k">制造商</span><span class="v" id="p-mfg">—</span></div>
      <div class="row"><span class="k">型号</span><span class="v" id="p-mdl">—</span></div>
      <div class="row"><span class="k">命令语言</span><span class="v" id="p-cmd">—</span></div>
      <div class="row"><span class="k">序列号</span><span class="v" id="p-sn">—</span></div>
      <div class="row"><span class="k">状态更新</span><span class="v" id="p-age">—</span></div>
      <div class="hint" id="p-raw">—</div>
    </div>

    <div class="card">
      <h2>USB 连接</h2>
      <div class="row"><span class="k">连接状态</span><span id="u-conn" class="badge b-gray">—</span></div>
      <div class="row"><span class="k">VID : PID</span><span class="v" id="u-vidpid">—</span></div>
      <div class="row"><span class="k">USB 版本</span><span class="v" id="u-ver">—</span></div>
      <div class="row"><span class="k">设备地址</span><span class="v" id="u-addr">—</span></div>
      <div class="row"><span class="k">打印机接口</span><span class="v" id="u-iface">—</span></div>
      <div class="row"><span class="k">传输协议</span><span class="v" id="u-proto">—</span></div>
      <div class="row"><span class="k">Bulk OUT</span><span class="v" id="u-epout">—</span></div>
      <div class="row"><span class="k">Bulk IN</span><span class="v" id="u-epin">—</span></div>
      <div class="row"><span class="k">重枚举次数</span><span class="v" id="u-enum">—</span></div>
    </div>

    <div class="card">
      <h2>打印任务</h2>
      <div class="row"><span class="k">进行中</span><span id="j-active" class="badge b-gray">空闲</span></div>
      <div class="row"><span class="k">客户端</span><span class="v" id="j-client">—</span></div>
      <div class="row"><span class="k">已接收</span><span class="v" id="j-recv">—</span></div>
      <div class="row"><span class="k">已发送 USB</span><span class="v" id="j-sent">—</span></div>
      <div class="row"><span class="k">平均速率</span><span class="v" id="j-rate">—</span></div>
      <div class="row"><span class="k">已用时</span><span class="v" id="j-time">—</span></div>
      <div class="hint">Raw Socket :9100 &middot; 同一时刻只接受一个连接</div>
    </div>

    <div class="card">
      <h2>统计</h2>
      <div class="row"><span class="k">累计作业</span><span class="v" id="s-jobs">0</span></div>
      <div class="row"><span class="k">累计数据量</span><span class="v" id="s-bytes">0 B</span></div>
      <div class="row"><span class="k">失败作业</span><span class="v" id="s-fail">0</span></div>
      <div class="row"><span class="k">USB 传输错误</span><span class="v" id="s-txerr">0</span></div>
      <div class="row"><span class="k">最近一次</span><span class="v" id="s-last">—</span></div>
    </div>

    <div class="card">
      <h2>WiFi</h2>
      <div class="row"><span class="k">状态</span><span id="w-state" class="badge b-gray">—</span></div>
      <div class="row"><span class="k">网络</span><span class="v" id="w-ssid">—</span></div>
      <div class="row"><span class="k">IP 地址</span><span class="v" id="w-ip">—</span></div>
      <div class="row"><span class="k">信号</span><span class="v" id="w-rssi">—</span></div>
      <select id="w-sel"><option value="">-- 点击扫描选择网络 --</option></select>
      <input type="password" id="w-pass" placeholder="WiFi 密码">
      <div class="btns">
        <button class="btn btn-primary btn-sm" onclick="scanWifi()">扫描</button>
        <button class="btn btn-primary btn-sm" onclick="connectWifi()">连接</button>
      </div>
      <div class="hint">仅支持 2.4GHz；连上后设备会切到新 IP，请重新访问。</div>
    </div>

    <div class="card">
      <h2>系统</h2>
      <div class="row"><span class="k">运行时长</span><span class="v" id="y-up">—</span></div>
      <div class="row"><span class="k">可用内存</span><span class="v" id="y-heap">—</span></div>
      <div class="row"><span class="k">可用 PSRAM</span><span class="v" id="y-psram">—</span></div>
      <div class="btns">
        <button class="btn btn-primary btn-sm" onclick="testPage()">打印测试页</button>
        <button class="btn btn-primary btn-sm" onclick="prnReset()">重置打印机</button>
        <button class="btn btn-danger btn-sm" onclick="restart()">重启设备</button>
      </div>
    </div>

    <div class="card">
      <h2>固件升级（OTA）</h2>
      <input type="file" id="fw-file" accept=".bin">
      <input type="password" id="fw-pw" placeholder="OTA 密码（没设置就留空）">
      <div class="btns"><button class="btn btn-primary btn-sm" onclick="doOta()">上传并升级</button></div>
      <div class="bar"><div class="fill" id="fw-bar"></div></div>
      <div class="hint" id="fw-msg">选择 Arduino IDE 导出的 .bin 文件，上传后设备自动重启。</div>
    </div>

    <div class="card">
      <h2>添加打印机指引</h2>
      <div class="two">
        <div>
          <p class="k" style="margin-bottom:6px">Windows 10 / 11</p>
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
          <p class="k" style="margin-bottom:6px">macOS</p>
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
      <div class="hint">驱动装在各自电脑上，ESP32 只做 USB 数据透明转发。</div>
    </div>

  </div>
  <div class="foot">ESP32-S3 WiFi Print Server</div>
</div>

<script>
function $(id){return document.getElementById(id)}
function kb(b){if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(1)+' KB';return (b/1048576).toFixed(2)+' MB'}
function ups(s){var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),x=s%60;
  return (d?d+'天':'')+h+'时'+m+'分'+x+'秒'}
function usbVer(h){while(h.length<4)h='0'+h;return parseInt(h.substr(0,2),16)+'.'+h.substr(2)}
function setB(id,cls,txt){var e=$(id);e.className='badge '+cls;e.textContent=txt}

function update(){
  fetch('/api/status').then(function(r){return r.json()}).then(function(d){
    var ip = d.wifi.connected ? d.wifi.ip : '192.168.4.1';
    $('maddr').textContent = ip;
    $('fwv').textContent = 'v'+d.stats.fw;
    $('y-up').textContent = ups(d.stats.uptime);
    $('y-heap').textContent = (d.stats.freeHeap/1024).toFixed(0)+' KB';
    $('y-psram').textContent = (d.stats.freePsram/1024).toFixed(0)+' KB';

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

    $('p-mfg').textContent = d.printer.mfg || '—';
    $('p-mdl').textContent = d.printer.mdl || '—';
    // 添加打印机指引里的占位文字，替换成实际检测到的型号（通用，不写死品牌）
    var mdlName = d.printer.mdl || '该打印机';
    var pm = document.querySelectorAll('.pmdl');
    for (var i = 0; i < pm.length; i++) pm[i].textContent = mdlName;
    $('p-cmd').textContent = d.printer.cmd || '—';
    $('p-sn').textContent  = d.printer.sern || '—';
    $('p-age').textContent = d.printer.portStatusValid ? (d.printer.statusAge/1000).toFixed(1)+' 秒前' : '—';
    $('p-raw').textContent = d.printer.deviceId ? ('Device ID: '+d.printer.deviceId) : '（该机型未提供 Device ID）';

    // USB
    setB('u-conn', d.usb.attached?'b-green':'b-red', d.usb.attached?'已连接':'未连接');
    $('u-vidpid').textContent = d.usb.attached ? (d.usb.vid+':'+d.usb.pid) : '—';
    $('u-ver').textContent    = d.usb.attached ? usbVer(d.usb.bcdUsb) : '—';
    $('u-addr').textContent   = d.usb.attached ? d.usb.devAddr : '—';
    $('u-iface').textContent  = d.usb.attached ? ('#'+d.usb.iface+'  ('+d.usb.ifaceClass+'/'+d.usb.subClass+')') : '—';
    $('u-proto').textContent  = d.usb.attached
      ? (d.usb.protocol===2?'双向 (1284)':d.usb.protocol===1?'单向':d.usb.protocol===3?'IEEE 1284.4':'其他 '+d.usb.protocol) : '—';
    $('u-epout').textContent  = d.usb.attached ? ('0x'+d.usb.epOut+'  '+d.usb.epOutMps+' 字节/包') : '—';
    $('u-epin').textContent   = d.usb.attached ? ('0x'+d.usb.epIn) : '—';
    $('u-enum').textContent   = d.usb.enumCount;

    // 任务
    if(d.job.active){
      setB('j-active','b-green pulse','进行中');
      var rate = d.job.elapsed>500 ? (d.job.bytesReceived/1024/(d.job.elapsed/1000)).toFixed(1)+' KB/s' : '—';
      $('j-rate').textContent = rate;
      $('j-time').textContent = (d.job.elapsed/1000).toFixed(1)+' 秒';
    } else {
      setB('j-active','b-gray','空闲');
      $('j-rate').textContent = '—';
      $('j-time').textContent = '—';
    }
    $('j-client').textContent = d.job.client || '—';
    $('j-recv').textContent = kb(d.job.bytesReceived);
    $('j-sent').textContent = kb(d.job.bytesSent);

    // 统计
    $('s-jobs').textContent = d.stats.totalJobs;
    $('s-bytes').textContent = kb(d.stats.totalBytes);
    $('s-fail').textContent = d.stats.failedJobs;
    $('s-txerr').textContent = d.stats.txErrors;
    $('s-last').textContent = d.stats.lastJob || '—';

    // WiFi
    if(d.wifi.connected) setB('w-state','b-green','已连接');
    else setB('w-state', d.wifi.mode==='ap'?'b-yellow':'b-red', d.wifi.mode==='ap'?'配网模式':'未连接');
    $('w-ssid').textContent = d.wifi.ssid || '—';
    $('w-ip').textContent = d.wifi.ip;
    $('w-rssi').textContent = d.wifi.connected ? (d.wifi.rssi+' dBm') : '—';

    var ips = document.getElementsByClassName('ipv');
    for(var i=0;i<ips.length;i++){ ips[i].textContent = ip; }
  }).catch(function(e){ console.error(e) });
}

function scanWifi(){
  var sel=$('w-sel'); sel.innerHTML='<option>扫描中...</option>';
  fetch('/api/wifi/scan').then(function(r){return r.json()}).then(function(n){
    sel.innerHTML='<option value="">-- 选择网络 --</option>';
    n.sort(function(a,b){return b.rssi-a.rssi});
    n.forEach(function(x){
      var o=document.createElement('option');
      o.value=x.ssid; o.textContent=x.ssid+'  ('+x.rssi+' dBm)'+(x.secure?'  需要密码':'');
      sel.appendChild(o);
    });
  });
}

function connectWifi(){
  var s=$('w-sel').value, p=$('w-pass').value;
  if(!s){ alert('请先扫描并选择网络'); return; }
  fetch('/api/wifi/connect',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent(p)})
  .then(function(r){return r.json()}).then(function(d){
    if(d.success){ alert('连接成功，新 IP: '+d.ip+'\n即将跳转...');
      setTimeout(function(){ location.href='http://'+d.ip; },4000);
    } else { alert('连接失败，请检查密码'); }
  });
}

function post(url,okMsg){
  fetch(url,{method:'POST'}).then(function(r){return r.json()}).then(function(d){
    alert(d.success?okMsg:'操作失败'); update();
  });
}
function testPage(){ post('/api/testpage','已发送测试页，请查看打印机') }
function prnReset(){ post('/api/printer/reset','打印机已软复位') }
function restart(){ if(confirm('确定重启设备？')){ fetch('/api/restart',{method:'POST'});
  alert('重启中，约 10 秒后刷新'); setTimeout(function(){location.reload()},10000); } }

function doOta(){
  var fi=$('fw-file');
  if(!fi.files || !fi.files[0]){ msg('请先选择 .bin 固件文件','b-red'); return; }
  var pw=$('fw-pw').value;
  var url='/api/ota'+(pw?('?pw='+encodeURIComponent(pw)):'');
  var fd=new FormData(); fd.append('firmware',fi.files[0],fi.files[0].name);
  var x=new XMLHttpRequest(); x.open('POST',url,true);
  x.upload.onprogress=function(e){
    if(e.lengthComputable){
      var p=Math.round(e.loaded*100/e.total);
      $('fw-bar').style.width=p+'%'; msg('上传中 '+p+'%','#94a3b8');
    }
  };
  x.onload=function(){
    var ok=false,em='';
    try{ var j=JSON.parse(x.responseText); ok=j.success; em=j.error||''; }catch(e){ em=x.responseText }
    if(ok){ msg('上传成功，设备正在重启，约 15 秒后自动刷新','#6ee7b7');
      setTimeout(function(){location.reload()},15000); }
    else { msg('升级失败：'+em,'#fca5a5'); $('fw-bar').style.width='0%'; }
  };
  x.onerror=function(){ msg('网络错误','#fca5a5') };
  x.send(fd);
  msg('开始上传...','#94a3b8');
}
function msg(t,c){ var e=$('fw-msg'); e.textContent=t; e.style.color=c }

update();
setInterval(update,2000);
</script>
</body>
</html>
)rawliteral";
  return html;
}
