#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "config.h"

enum WifiState {
  WSTATE_NONE,
  WSTATE_AP_SETUP,   // 設定用 AP 模式
  WSTATE_STATION     // 已連線到家用 WiFi
};

class WiFiManager {
public:
  WiFiManager();

  // 初始化：嘗試連線已儲存的 WiFi，失敗則進入 AP 模式
  void begin();

  // 主迴圈處理（自動重連 / 後備 AP / 訊號採樣）
  void task();

  // 設定新的 WiFi 認證並儲存
  bool setCredentials(const String& ssid, const String& password);

  // 清除已儲存的 WiFi 認證（恢復出廠：設備會重新進入 AP 配網模式）
  void clearCredentials();

  // 取得狀態
  WifiState getMode() const { return _mode; }
  bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
  String getIP() const;
  String getSSID() const { return _ssid; }
  String getHostname() const { return _hostname; }
  int getRSSI() const { return WiFi.RSSI(); }

  // --- 網路品質 / 抗抖動統計（v2.5.0）---
  uint32_t getLinkEpoch() const { return _linkEpoch; }       // 每次重新連上 +1，用於重建 mDNS / 重開監聽
  uint32_t getDisconnectCount() const { return _disconnectCount; }
  uint32_t getReconnectCount() const { return _reconnectCount; }
  unsigned long getOfflineMs() const;                        // 已離線多久（在線返回 0）
  int  getRssiMin() const;                                   // 最近一分鐘最差訊號（0 = 還沒採樣）
  int  getRssiAvg() const;                                   // 最近一分鐘平均訊號（0 = 還沒採樣）
  bool isFallbackAp() const { return _fallbackAp; }          // 是否已開啟後備 AP
  unsigned long getRetryIntervalMs() const { return _retryMs; }

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

  // --- 連線健康度 ---
  bool          _wasConnected;
  unsigned long _downSinceMs;      // 何時斷的（在線時為 0）
  unsigned long _lastRetryMs;
  unsigned long _retryMs;          // 當前退避間隔
  uint32_t      _failStreak;       // 進入本次離線後已重試幾次
  uint32_t      _linkEpoch;
  uint32_t      _disconnectCount;
  uint32_t      _reconnectCount;
  bool          _fallbackAp;

  // --- 訊號品質採樣 ---
  int           _rssiHist[WIFI_RSSI_HISTORY];
  uint8_t       _rssiIdx;
  uint8_t       _rssiCount;
  unsigned long _rssiSampleMs;

  // 异步扫描状态
  bool        _scanInProgress;
  String      _scanResultCache;   // 最近一次扫描的 JSON 缓存

  bool _loadCredentials();
  void _saveCredentials();
  bool _connectStation();
  void _startAP();
  void _applyStationTuning();     // 关掉 modem sleep 等稳定性设置
  void _onLinkLost();
  void _onLinkRegained();
  void _startFallbackAp();
  void _sampleRssi();
  // 异步扫描：触发并缓存结果，供 web handler 读取
  void _startScanAsync();
  String _consumeScanResult();
};

#endif // WIFI_MANAGER_H
