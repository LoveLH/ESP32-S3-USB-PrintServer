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

  // 主迴圈處理（自動重連 / 後備 AP / 訊號採樣 / 非阻塞配網）
  void task();

  // 清除已儲存的 WiFi 認證（恢復出廠：設備會重新進入 AP 配網模式）
  void clearCredentials();

  // 取得狀態
  WifiState getMode() const { return _mode; }
  bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
  String getIP() const;
  String getStaIP() const { return WiFi.localIP().toString(); }
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

  // 扫描：start=true 才会触发新扫描；返回 {"scanning":bool,"seq":n,"list":[...]}。
  // 轮询请求绝不能隐式重新触发扫描（v2.9.6 的 bug：每次轮询都重启扫描，
  // 射频一直离开 AP 信道，手机掉线，前端永远"扫描中"）。
  String scanNetworks(bool start);

  // 网页配网：非阻塞。立即返回，task() 里推进；结果看 getConnectState()。
  // 只有连接成功才写 NVS，输错密码不会冲掉原来能用的配置。
  bool requestConnect(const String& ssid, const String& password);
  enum ConnState : uint8_t { CONN_IDLE = 0, CONN_BUSY = 1, CONN_OK = 2, CONN_FAIL = 3 };
  ConnState getConnectState() const { return _connState; }

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
  String      _scanResultCache;   // 最近一次扫描的 JSON 数组缓存
  uint32_t    _scanSeq;           // 每完成一次扫描 +1，前端据此判断"新结果到了"
  bool        _scanWanted;        // 网页要求扫描，task() 在射频空闲时启动
  unsigned long _scanTryMs;       // 上次尝试启动扫描的时间（启动失败时限速重试）
  unsigned long _scanWantedMs;    // 何时提出的扫描请求（超时放弃）
  bool        _scanErr;           // 最近一次扫描没能启动/失败

  // 非阻塞配网
  ConnState     _connState;       // 给网页看的结果（后台试连不改它）
  bool          _connActive;      // 有一次连接尝试正在进行
  bool          _connQueued;      // 网页请求已排队，等 HTTP 响应发出去再动射频
  bool          _connBackground;  // 本次是配网 AP 下自动试连已保存的 WiFi（失败不报错）
  unsigned long _connStartMs;
  String        _pendSsid;
  String        _pendPass;
  unsigned long _apOffAtMs;       // 连上后延迟关热点的时刻（0 = 不关）
  unsigned long _apRetryMs;       // 配网 AP 下上次后台试连时间

  bool _apIsUp() const { return (WiFi.getMode() & WIFI_MODE_AP) != 0; }
  bool _apHasClients() { return _apIsUp() && WiFi.softAPgetStationNum() > 0; }
  void _startConnect(bool background);   // 用 _pendSsid/_pendPass 发起连接
  void _pollConnect();
  void _scanTask();

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
  bool _startScanAsync();
};

#endif // WIFI_MANAGER_H
