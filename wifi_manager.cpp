#include "wifi_manager.h"

WiFiManager::WiFiManager()
  : _mode(WSTATE_NONE),
    _wasConnected(false), _downSinceMs(0), _lastRetryMs(0),
    _retryMs(WIFI_RETRY_MIN_MS), _failStreak(0),
    _linkEpoch(0), _disconnectCount(0), _reconnectCount(0), _fallbackAp(false),
    _rssiIdx(0), _rssiCount(0), _rssiSampleMs(0),
    _scanInProgress(false), _scanResultCache("[]") {
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
                (unsigned)_disconnectCount, WIFI_RETRY_MIN_MS / 1000);
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
      if (now - _lastRetryMs >= _retryMs) {
        _lastRetryMs = now;
        _reconnectCount++;

        if (_failStreak < WIFI_REASSOC_AFTER) {
          // 第一級：讓驅動自己重連，最快， credential 不用重來
          Serial.printf("[WiFi] 離線 %lus → 嘗試重連（第 %u 次）\n",
                        down / 1000, (unsigned)_reconnectCount);
          WiFi.reconnect();
        } else if (_failStreak < WIFI_DRIVER_RESET_AFTER) {
          // 第二級：完整重新關聯（重新走 auth / assoc / DHCP）
          Serial.printf("[WiFi] 離線 %lus → 重新關聯（第 %u 次）\n",
                        down / 1000, (unsigned)_reconnectCount);
          WiFi.disconnect(false);
          WiFi.begin(_ssid.c_str(), _password.c_str());
        } else {
          // 第三級：整個 WiFi 驅動復位。某些路由器在客戶端長期重連失敗後
          // 會把它拉黑一小段時間，只有重啟驅動才能跳出這個狀態。
          Serial.printf("[WiFi] 離線 %lus → 復位 WiFi 驅動（第 %u 次）\n",
                        down / 1000, (unsigned)_reconnectCount);
          WiFi.mode(WIFI_OFF);
          delay(200);
          WiFi.mode(WIFI_STA);
          _applyStationTuning();
          WiFi.begin(_ssid.c_str(), _password.c_str());
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

  // 推进异步扫描
  if (_scanInProgress) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      String json = "[";
      for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
      }
      json += "]";
      WiFi.scanDelete();
      _scanResultCache  = json;
      _scanInProgress   = false;
      Serial.printf("[WiFi] ✓ 扫描完成，发现 %d 个网络\n", n);
    } else if (n == WIFI_SCAN_FAILED) {
      _scanResultCache  = "[]";
      _scanInProgress   = false;
    }
  }
}

bool WiFiManager::setCredentials(const String& ssid, const String& password) {
  _ssid = ssid;
  _password = password;
  _saveCredentials();

  Serial.printf("[WiFi] 已儲存新認證，嘗試連線到 '%s'...\n", _ssid.c_str());

  // 停止 AP（初次配網或後備 AP 都在用同一個 SSID）
  WiFi.softAPdisconnect(true);
  _fallbackAp = false;

  if (_connectStation()) {
    Serial.printf("[WiFi] ✓ 已連線！IP: %s\n", getIP().c_str());
    _wasConnected = true;
    _linkEpoch++;
    return true;
  }

  Serial.println("[WiFi] ✗ 連線失敗，恢復 AP 模式");
  _startAP();
  return false;
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

String WiFiManager::scanNetworks() {
  // 非阻塞：触发后台扫描，立即返回当前缓存
  _startScanAsync();
  return _scanResultCache;
}

void WiFiManager::_startScanAsync() {
  if (_scanInProgress) return;
  // 先消费掉旧结果，避免重启扫描失败
  int n = WiFi.scanComplete();
  if (n >= 0) WiFi.scanDelete();
  WiFi.scanNetworks(true);   // async = true
  _scanInProgress = true;
  Serial.println("[WiFi] 后台扫描已启动...");
}

String WiFiManager::_consumeScanResult() {
  return _scanResultCache;
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
