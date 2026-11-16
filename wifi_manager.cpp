#include "wifi_manager.h"
#include <esp_wifi.h>

WiFiManager::WiFiManager()
  : _mode(WSTATE_NONE),
    _wasConnected(false), _downSinceMs(0), _lastRetryMs(0),
    _retryMs(WIFI_RETRY_MIN_MS), _failStreak(0),
    _linkEpoch(0), _disconnectCount(0), _reconnectCount(0), _fallbackAp(false),
    _rssiIdx(0), _rssiCount(0), _rssiSampleMs(0),
    _scanInProgress(false), _scanResultCache("[]"), _scanSeq(0),
    _scanWanted(false), _scanTryMs(0), _scanWantedMs(0), _scanErr(false),
    _connState(CONN_IDLE), _connActive(false), _connQueued(false), _connBackground(false),
    _connStartMs(0), _apOffAtMs(0), _apRetryMs(0) {
  memset(_rssiHist, 0, sizeof(_rssiHist));
}

// 关掉 WiFi modem sleep —— 这是打印服务器场景里最容易被忽略的一条。
// 默认的节能模式会让 WiFi 周期性休眠几十毫秒，TCP 延迟抖动可达数百毫秒，
// 打印作业出现明显卡顿、偶发断流，Windows 端表现为"打印到一半失败"。
void WiFiManager::_applyStationTuning() {
#if WIFI_DISABLE_SLEEP
  WiFi.setSleep(false);
#endif
  // 自动重连关掉改由 task() 统一管：我们要自己的退避节奏和升级策略，
  // 让 IDF 自己反复重试反而会在弱信号环境下把节奏打乱。
  WiFi.setAutoReconnect(false);
}

void WiFiManager::begin() {
  _hostname = MDNS_HOSTNAME;

  // 從 NVS 讀取已儲存的認證
  if (_loadCredentials() && _ssid.length() > 0) {
    Serial.printf("[WiFi] 嘗試連線到 '%s'...\n", _ssid.c_str());
    if (_connectStation()) {
      Serial.printf("[WiFi] ✓ 已連線！IP: %s\n", getIP().c_str());
      _wasConnected = true;
      _linkEpoch    = 1;
      return;
    }
    Serial.println("[WiFi] ✗ 連線失敗，切換到 AP 設定模式");
  } else {
    Serial.println("[WiFi] 尚未設定 WiFi，啟動 AP 設定模式");
  }

  _startAP();
}

// ---------------------------------------------------------------
//  連線健康度
// ---------------------------------------------------------------
void WiFiManager::_onLinkLost() {
  _wasConnected = false;
  _downSinceMs  = millis();
  _failStreak   = 0;
  _retryMs      = WIFI_RETRY_MIN_MS;
  _lastRetryMs  = millis();
  _disconnectCount++;
  Serial.printf("[WiFi] ✗ 連線中斷（第 %u 次），%lus 後開始重連\n",
                (unsigned)_disconnectCount, (unsigned long)(WIFI_RETRY_MIN_MS / 1000));
}

void WiFiManager::_onLinkRegained() {
  _wasConnected = true;
  _downSinceMs  = 0;
  _failStreak   = 0;
  _retryMs      = WIFI_RETRY_MIN_MS;
  _linkEpoch++;
  memset(_rssiHist, 0, sizeof(_rssiHist));
  _rssiIdx = 0;
  _rssiCount = 0;
  // 原网络回来了：后备热点不再需要（旧版一直开着，_fallbackAp 也不复位）
  if (_fallbackAp) {
    _fallbackAp = false;
    if (_apIsUp()) _apOffAtMs = millis() + (_apHasClients() ? WIFI_AP_LINGER_MS : 1);
  }
  Serial.printf("[WiFi] ✓ 已重新連線，IP: %s（第 %u 次重連，本次握手輪次 %u）\n",
                WiFi.localIP().toString().c_str(),
                (unsigned)_reconnectCount, (unsigned)_linkEpoch);
}

void WiFiManager::_sampleRssi() {
  unsigned long now = millis();
  if (now - _rssiSampleMs < WIFI_RSSI_SAMPLE_MS) return;
  _rssiSampleMs = now;
  int r = WiFi.RSSI();
  if (r == 0) return;                       // 讀不到（斷線或未關聯）
  _rssiHist[_rssiIdx] = r;
  _rssiIdx = (uint8_t)((_rssiIdx + 1) % WIFI_RSSI_HISTORY);
  if (_rssiCount < WIFI_RSSI_HISTORY) _rssiCount++;
}

int WiFiManager::getRssiMin() const {
  if (_rssiCount == 0) return 0;
  int m = 127;
  for (uint8_t i = 0; i < _rssiCount; i++) if (_rssiHist[i] < m) m = _rssiHist[i];
  return m;
}

int WiFiManager::getRssiAvg() const {
  if (_rssiCount == 0) return 0;
  long s = 0;
  for (uint8_t i = 0; i < _rssiCount; i++) s += _rssiHist[i];
  return (int)(s / _rssiCount);
}

unsigned long WiFiManager::getOfflineMs() const {
  if (_downSinceMs == 0) return 0;
  return millis() - _downSinceMs;
}

// 長時間連不上時開一個後備 AP —— 否則設備會徹底失聯，
// 只能拆機重刷。（路由器改了 SSID/密碼、設備被挪遠、客戶換Wi-Fi都會遇到）
void WiFiManager::_startFallbackAp() {
  _fallbackAp = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  _applyStationTuning();
  Serial.println();
  Serial.println("[WiFi] ⚠ 長時間無法連上原有網路，已開啟後備配置 AP");
  Serial.printf("[WiFi]   SSID: %s  密碼: %s\n", AP_SSID, AP_PASSWORD);
  Serial.printf("[WiFi]   管理頁: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("[WiFi]   請連上該熱點重新配置；同時設備仍在背景嘗試連回原網路。");
}

// 扫描结果拼 JSON 时 SSID 必须转义：含 " 或 \ 的 SSID 会产出非法 JSON，
// 前端 JSON.parse 抛错后被 catch 悄悄吞掉，用户看到的就是"点击扫描没反应"。
// （与 WebUI::_jsonEscape 同一套规则。）
static String _escapeJsonStr(const String& s) {
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

// ---------------------------------------------------------------
void WiFiManager::task() {
  if (_mode == WSTATE_STATION) {
    if (isConnected()) {
      if (!_wasConnected) {
        // 斷線後又連上了（可能是我們重連成功，也可能是 AP 自己回來了）
        if (_downSinceMs != 0) _onLinkRegained();
        else { _wasConnected = true; }
      }
      _sampleRssi();
    } else {
      if (_wasConnected || _downSinceMs == 0) _onLinkLost();

      unsigned long now  = millis();
      unsigned long down = getOfflineMs();
      // 用户正在网页上扫描/换网时暂停重连：STA 重连会逐信道找路由，
      // 一边打断扫描（驱动拒绝），一边把连着后备热点的手机踢掉。
      bool userBusy = _connActive || _connQueued || _scanInProgress || _scanWanted ||
                      (_fallbackAp && _apHasClients());
      if (!userBusy && now - _lastRetryMs >= _retryMs) {
        _lastRetryMs = now;
        _reconnectCount++;

        if (_failStreak < WIFI_REASSOC_AFTER) {
          // 第一級：讓驅動自己重連，最快， credential 不用重來
          Serial.printf("[WiFi] 離線 %lus → 嘗試重連（第 %u 次）\n",
                        down / 1000, (unsigned)_reconnectCount);
          WiFi.reconnect();
        } else if (_failStreak < WIFI_DRIVER_RESET_AFTER || _fallbackAp) {
          // 第二級：完整重新關聯（重新走 auth / assoc / DHCP）
          // 后备热点开着时不做第三级：WiFi.mode(WIFI_OFF) 会连热点一起关掉，
          // 重开后手机也得重连（issue #1：换 WiFi 时 192.168.4.1 ping 不通）。
          Serial.printf("[WiFi] 離線 %lus → 重新關聯（第 %u 次）\n",
                        down / 1000, (unsigned)_reconnectCount);
          WiFi.disconnect(false);
          WiFi.begin(_ssid.c_str(), _password.c_str());
        } else {
          // 第三級：整個 WiFi 驅動復位。某些路由器在客戶端長期重連失敗後
          // 會把它拉黑一小段時間，只有重啟驅動才能跳出這個狀態。
          Serial.printf("[WiFi] 離線 %lus → 復位 WiFi 驅動（第 %u 次）\n",
                        down / 1000, (unsigned)_reconnectCount);
          // 注意：WiFi.mode(WIFI_OFF) 会把 softAP 一起停掉。如果后备 AP
          // 开着（用户正连着它改 WiFi），复位后必须立刻补开，否则设备
          // 彻底失联，只能重启/重刷 —— 这正是"换 WiFi 点扫描后死机、
          // ping 不通 192.168.4.1"的根因之一：后备 AP 离线 180s 开、
          // 约 375s 时被这次复位杀掉，且 _fallbackAp 仍为 true 导致永不再开。
          bool hadFallbackAp = _fallbackAp;
          WiFi.mode(WIFI_OFF);
          delay(200);
          WiFi.mode(WIFI_STA);
          _applyStationTuning();
          WiFi.begin(_ssid.c_str(), _password.c_str());
          if (hadFallbackAp) _startFallbackAp();
        }

        _failStreak++;
        // 指數退避：一開始積極重試，失敗多了就放慢，別在信號差的時候白耗電
        if (_retryMs < WIFI_RETRY_MAX_MS) {
          _retryMs *= 2;
          if (_retryMs > WIFI_RETRY_MAX_MS) _retryMs = WIFI_RETRY_MAX_MS;
        }
      }

      if (!_fallbackAp && WIFI_FALLBACK_AP_MS > 0 && down > WIFI_FALLBACK_AP_MS) {
        _startFallbackAp();
      }
    }
  }

  // ---- 配网 AP 模式：已保存过 WiFi 时后台定期试连 ----
  // 停电后路由器比板子起得慢，旧版会永远卡在配网 AP，只能重配/重刷。
  // 只在没有手机连着热点时试（STA 找路由要逐信道扫，会把热点上的手机踢掉）。
  if (_mode == WSTATE_AP_SETUP && _ssid.length() && !_connActive && !_connQueued &&
      !_scanInProgress && !_scanWanted && !_apHasClients() &&
      millis() - _apRetryMs >= WIFI_AP_SETUP_RETRY_MS) {
    _apRetryMs = millis();
    _pendSsid = _ssid;
    _pendPass = _password;
    Serial.printf("[WiFi] 配网模式下后台试连已保存的 '%s'\n", _ssid.c_str());
    _startConnect(true);
  }

  // ---- 网页配网：等 HTTP 响应先发出去（≥300ms）再动射频 ----
  if (_connQueued && millis() - _connStartMs >= 300) {
    _connQueued = false;
    _startConnect(false);
  }
  if (_connActive) _pollConnect();

  // ---- 连上新 WiFi 后延迟关热点 ----
  if (_apOffAtMs && (long)(millis() - _apOffAtMs) >= 0) {
    _apOffAtMs = 0;
    if (_mode == WSTATE_STATION && isConnected() && _apIsUp()) {
      Serial.println("[WiFi] 已连上路由器，关闭配网热点");
      WiFi.softAPdisconnect(true);   // 只关 AP，STA 保留
      _applyStationTuning();
    }
  }

  _scanTask();
}

// ---------------------------------------------------------------
//  扫描（v2.9.7 重写）
//  v2.9.6 的问题：前端每秒轮询一次 /api/wifi/scan，而每次请求都会在上一轮
//  扫描完成后立刻重启扫描并把缓存清成 []——前端永远拿不到结果，射频则一直
//  逐信道跳频，热点上的手机被踢掉，表现为"一直扫描中、ping 不通 192.168.4.1"。
//  现在：只有 start=1 才请求扫描；轮询只读结果；用 seq 判断新结果。
// ---------------------------------------------------------------
void WiFiManager::_scanTask() {
  if (_scanInProgress) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      String json = "[";
      for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;              // 隐藏网络没法选，跳过
        if (json.length() > 1) json += ",";
        json += "{\"ssid\":\"" + _escapeJsonStr(ssid) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
      }
      json += "]";
      WiFi.scanDelete();
      _scanResultCache = json;
      _scanInProgress  = false;
      _scanErr         = false;
      _scanSeq++;
      Serial.printf("[WiFi] ✓ 扫描完成，发现 %d 个网络\n", n);
    } else if (n == WIFI_SCAN_FAILED) {
      WiFi.scanDelete();
      _scanInProgress = false;
      _scanErr        = true;
      _scanSeq++;                                  // 让前端结束等待
      Serial.println("[WiFi] ✗ 扫描失败");
    }
    return;
  }

  if (!_scanWanted) return;
  if (millis() - _scanWantedMs > 15000) {          // 一直启动不了：放弃并告诉前端
    _scanWanted = false;
    _scanErr    = true;
    _scanSeq++;
    Serial.println("[WiFi] ✗ 扫描一直无法启动，放弃");
    return;
  }
  if (_connActive || _connQueued) return;          // 正在连接：驱动会拒绝扫描
  if (millis() - _scanTryMs < 1000) return;        // 启动失败时限速重试
  _scanTryMs = millis();
  if (_startScanAsync()) _scanWanted = false;
}

String WiFiManager::scanNetworks(bool start) {
  if (start && !_scanInProgress && !_scanWanted) {
    _scanWanted   = true;
    _scanWantedMs = millis();
    _scanTryMs    = 0;
  }
  String j = "{\"scanning\":";
  j += (_scanInProgress || _scanWanted) ? "true" : "false";
  j += ",\"seq\":" + String(_scanSeq);
  j += ",\"err\":";
  j += _scanErr ? "true" : "false";
  j += ",\"list\":" + _scanResultCache + "}";
  return j;
}

bool WiFiManager::_startScanAsync() {
  WiFi.scanDelete();
  // 开着热点时缩短每信道驻留：默认 300ms×13 信道，热点长时间不在自己信道上，
  // 手机会判定热点丢失并断开。
  uint32_t dwell = _apIsUp() ? WIFI_SCAN_DWELL_AP_MS : WIFI_SCAN_DWELL_STA_MS;
  // 驱动在 STA 正在连接时会拒绝扫描（ESP_ERR_WIFI_STATE）：必须检查返回值。
  if (WiFi.scanNetworks(true, false, false, dwell) != WIFI_SCAN_RUNNING) {
    Serial.println("[WiFi] 扫描启动失败（可能 STA 正在连接），稍后重试");
    return false;
  }
  _scanInProgress = true;
  Serial.println("[WiFi] 后台扫描已启动...");
  return true;
}

// ---------------------------------------------------------------
//  非阻塞配网（v2.9.7）
//  旧版 setCredentials() 在 HTTP handler 里：先关热点 → 再阻塞 15 秒等连接 →
//  最后才回 HTTP 响应。手机早就断了，页面永远收不到结果；输错密码还会把
//  原来能用的配置覆盖掉。现在：立即回响应，热点全程保留，成功才写 NVS。
// ---------------------------------------------------------------
bool WiFiManager::requestConnect(const String& ssid, const String& password) {
  if (ssid.length() == 0 || ssid.length() > 32) return false;
  if (password.length() > 0 && (password.length() < 8 || password.length() > 63)) return false;
  if (_connActive || _connQueued) return false;
  _pendSsid    = ssid;
  _pendPass    = password;
  _connQueued  = true;
  _connState   = CONN_BUSY;
  _connStartMs = millis();
  _scanWanted  = false;
  return true;
}

void WiFiManager::_startConnect(bool background) {
  _connBackground = background;
  _connActive     = true;
  _connStartMs    = millis();
  if (_scanInProgress) {                 // 扫描和连接不能同时进行
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    _scanInProgress = false;
  }
  if (!background) Serial.printf("[WiFi] 尝试连接 '%s'（热点保持开启）...\n", _pendSsid.c_str());
  // 热点在就保持 AP_STA，绝不在这里关热点
  WiFi.mode(_apIsUp() ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(_hostname.c_str());
  _applyStationTuning();
  WiFi.disconnect(false);
  WiFi.begin(_pendSsid.c_str(), _pendPass.c_str());
}

void WiFiManager::_pollConnect() {
  unsigned long el = millis() - _connStartMs;
  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    _connActive = false;
    bool changed = (_pendSsid != _ssid) || (_pendPass != _password);
    _ssid     = _pendSsid;
    _password = _pendPass;
    _pendPass = "";
    if (changed) _saveCredentials();     // 只有真连上了才落盘
    _mode         = WSTATE_STATION;
    _wasConnected = true;
    _downSinceMs  = 0;
    _failStreak   = 0;
    _retryMs      = WIFI_RETRY_MIN_MS;
    _fallbackAp   = false;
    _linkEpoch++;
    if (!_connBackground) _connState = CONN_OK;
    Serial.printf("[WiFi] ✓ 已連線 '%s'！IP: %s\n", _ssid.c_str(), getStaIP().c_str());
    if (_apIsUp()) {
      // 有人连着热点（正在配网的手机）就多留一会儿，让页面拿到新 IP；否则立即关
      _apOffAtMs = millis() + (_apHasClients() ? WIFI_AP_LINGER_MS : 1);
      if (!_apOffAtMs) _apOffAtMs = 1;
    }
    return;
  }

  // 密码错 / 找不到网络会较快给出明确状态；否则等到超时。
  // 后台试连期间有手机连上热点 → 立即放弃，别让 STA 跳信道把手机踢掉。
  bool hardFail = (el > 6000) && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL);
  if (_connBackground && _apHasClients()) hardFail = true;
  if (!hardFail && el < WIFI_CONNECT_TIMEOUT_MS) return;

  _connActive = false;
  _pendPass   = "";
  WiFi.disconnect(false);
  if (!_connBackground) {
    _connState = CONN_FAIL;
    Serial.printf("[WiFi] ✗ 连接 '%s' 失败（status=%d），保留原配置\n", _pendSsid.c_str(), (int)st);
  }
  if (_mode == WSTATE_STATION && _ssid.length()) {
    // 原来在用的网络还在：回到正常重连流程
    _lastRetryMs = 0;
    WiFi.begin(_ssid.c_str(), _password.c_str());
  }
}

void WiFiManager::clearCredentials() {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.remove(NVS_KEY_SSID);
  _prefs.remove(NVS_KEY_PASS);
  _prefs.end();
  _ssid = "";
  _password = "";
  Serial.println("[WiFi] 已清除保存的 WiFi 认证");
}

String WiFiManager::getIP() const {
  if (_mode == WSTATE_STATION) {
    return WiFi.localIP().toString();
  } else if (_mode == WSTATE_AP_SETUP) {
    return WiFi.softAPIP().toString();
  }
  return "0.0.0.0";
}

void WiFiManager::reconnect() {
  if (_mode == WSTATE_STATION) {
    WiFi.disconnect();
    delay(500);
    _connectStation();
  }
}

bool WiFiManager::_loadCredentials() {
  _prefs.begin(NVS_NAMESPACE, true);  // 唯讀
  _ssid = _prefs.getString(NVS_KEY_SSID, "");
  _password = _prefs.getString(NVS_KEY_PASS, "");
  _hostname = _prefs.getString(NVS_KEY_HOSTNAME, MDNS_HOSTNAME);
  _prefs.end();

  return _ssid.length() > 0;
}

void WiFiManager::_saveCredentials() {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putString(NVS_KEY_SSID, _ssid);
  _prefs.putString(NVS_KEY_PASS, _password);
  _prefs.putString(NVS_KEY_HOSTNAME, _hostname);
  _prefs.end();
  Serial.println("[WiFi] 認證已儲存到 NVS");
}

bool WiFiManager::_connectStation() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(_hostname.c_str());
  _applyStationTuning();
  WiFi.begin(_ssid.c_str(), _password.c_str());

  // 等待連線，最多 15 秒
  int retries = 30;
  while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    _mode = WSTATE_STATION;
    return true;
  }

  WiFi.disconnect();
  return false;
}

void WiFiManager::_startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  _mode = WSTATE_AP_SETUP;

  Serial.printf("[WiFi] AP 模式已啟動\n");
  Serial.printf("[WiFi] SSID: %s\n", AP_SSID);
  Serial.printf("[WiFi] 密碼: %s\n", AP_PASSWORD);
  Serial.printf("[WiFi] 設定頁面: http://%s\n", WiFi.softAPIP().toString().c_str());
}
