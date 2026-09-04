#include "wifi_manager.h"

WiFiManager::WiFiManager()
  : _mode(WSTATE_NONE), _lastReconnectAttempt(0),
    _scanInProgress(false), _scanResultCache("[]") {}

void WiFiManager::begin() {
  _hostname = MDNS_HOSTNAME;

  // 從 NVS 讀取已儲存的認證
  if (_loadCredentials() && _ssid.length() > 0) {
    Serial.printf("[WiFi] 嘗試連線到 '%s'...\n", _ssid.c_str());
    if (_connectStation()) {
      Serial.printf("[WiFi] ✓ 已連線！IP: %s\n", getIP().c_str());
      return;
    }
    Serial.println("[WiFi] ✗ 連線失敗，切換到 AP 設定模式");
  } else {
    Serial.println("[WiFi] 尚未設定 WiFi，啟動 AP 設定模式");
  }

  _startAP();
}

void WiFiManager::task() {
  if (_mode == WSTATE_STATION && !isConnected()) {
    unsigned long now = millis();
    if (now - _lastReconnectAttempt > RECONNECT_INTERVAL) {
      _lastReconnectAttempt = now;
      Serial.println("[WiFi] 連線中斷，嘗試重連...");
      WiFi.reconnect();
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

  // 停止 AP
  WiFi.softAPdisconnect(true);

  if (_connectStation()) {
    Serial.printf("[WiFi] ✓ 已連線！IP: %s\n", getIP().c_str());
    return true;
  }

  Serial.println("[WiFi] ✗ 連線失敗，恢復 AP 模式");
  _startAP();
  return false;
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
