#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "config.h"

enum WifiState {
  WSTATE_NONE,
  WSTATE_AP_SETUP,   // AP 模式（設定用）
  WSTATE_STATION     // 已連線到家用 WiFi
};

class WiFiManager {
public:
  WiFiManager();

  // 初始化：嘗試連線已儲存的 WiFi，失敗則進入 AP 模式
  void begin();

  // 主迴圈處理（自動重連）
  void task();

  // 設定新的 WiFi 認證並儲存
  bool setCredentials(const String& ssid, const String& password);

  // 取得狀態
  WifiState getMode() const { return _mode; }
  bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
  String getIP() const;
  String getSSID() const { return _ssid; }
  String getHostname() const { return _hostname; }
  int getRSSI() const { return WiFi.RSSI(); }

  // 掃描可用的 WiFi 網路
  String scanNetworks();

  // 重啟連線
  void reconnect();

private:
  WifiState _mode;
  String _ssid;
  String _password;
  String _hostname;
  Preferences _prefs;
  unsigned long _lastReconnectAttempt;
  static const unsigned long RECONNECT_INTERVAL = 10000;  // 10 秒
  // 异步扫描状态
  bool        _scanInProgress;
  String      _scanResultCache;   // 最近一次扫描的 JSON 缓存
  bool _loadCredentials();
  void _saveCredentials();
  bool _connectStation();
  void _startAP();
  // 异步扫描：触发并缓存结果，供 web handler 读取
  void _startScanAsync();
  String _consumeScanResult();
};

#endif // WIFI_MANAGER_H
