#ifndef USB_PRINTER_H
#define USB_PRINTER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "usb/usb_host.h"
#include "config.h"

// --- USB Printer Class ---
#define USB_CLASS_PRINTER       0x07
#define USB_SUBCLASS_PRINTER    0x01
#define USB_PROTOCOL_UNIDIR     0x01  // 单向
#define USB_PROTOCOL_BIDIR      0x02  // 双向
#define USB_PROTOCOL_1284       0x03  // IEEE 1284.4
#define USB_PROTOCOL_IPPUSB     0x04  // IPP over USB（驱动打印/免驱打印的新接口）

#define USB_PRINTER_GET_DEVICE_ID   0x00
#define USB_PRINTER_GET_PORT_STATUS 0x01
#define USB_PRINTER_SOFT_RESET      0x02

// --- 作业收尾策略 ---
// 作业发完之后固件要不要再补点东西，取决于打印语言，不能一刀切。
// 见 printer_db.h 里 Q_TAIL_NONE / Q_TAIL_FF 的说明。
enum PrinterTailPolicy {
  TAIL_AUTO = 0,  // 看作业末尾字节自动判断（默认，最安全）
  TAIL_NONE = 1,  // 什么都不补 —— PCLm/PDF、PostScript、PJL 系
  TAIL_FF   = 2   // 无条件补 FF —— ESC/P、ESC/POS、老式 GDI 裸流
};

// --- IEEE 1284 端口状态位 ---
#define PS_PAPER_EMPTY  0x20   // 1 = 缺纸
#define PS_SELECTED     0x10   // 1 = 已选中/在线
#define PS_NOT_ERROR    0x08   // 1 = 无错误

enum PrinterState {
  PRINTER_DISCONNECTED = 0,
  PRINTER_CONNECTED    = 1,   // 已枚举，正在初始化
  PRINTER_READY        = 2,
  PRINTER_BUSY         = 3,
  PRINTER_ERROR        = 4
};

// USB 链路信息（网页会显示）
struct UsbInfo {
  bool     attached;
  uint8_t  devAddr;
  uint16_t vid;
  uint16_t pid;
  uint16_t bcdUsb;
  uint16_t bcdDevice;
  uint8_t  deviceClass, deviceSubClass, deviceProtocol;
  uint8_t  configurationValue, numConfigurations;
  uint8_t  ifaceNum;
  uint8_t  ifaceClass;
  uint8_t  ifaceSubClass;
  uint8_t  ifaceProtocol;
  uint8_t  epOut;
  uint8_t  epIn;
  uint16_t epOutMps;
  uint16_t epInMps;
  uint8_t  ippIfaces; // 该设备上的 IPP-over-USB(7/1/4) 接口数量
  uint32_t enumCount;   // 累计插拔/重枚举次数
  int      lastError;   // 最近一次 esp_err
};

// IEEE 1284 Device ID 解析结果
struct PrinterInfo {
  String deviceId;   // 原始字符串
  String mfg;        // MFG  制造商
  String mdl;        // MDL  型号
  String cmd;        // CMD  支持的页面描述语言
  String cls;        // CLS  设备类别
  String des;        // DES  描述
  String sern;       // SN   序列号
};

class UsbPrinter {
public:
  UsbPrinter();

  bool begin();
  void task();                                    // 仅在 USB Host 任务中调用
  // 写数据到 Bulk OUT。timeoutMs 是单块等待上限，默认 USB_XFER_TIMEOUT_MS(30s)，
  // 给大数据流用；**控制性小写入（ESC @、FF）必须传短值**（见 CTRL_WRITE_TIMEOUT_MS），
  // 否则打印机已死时这两三个字节也能把 task() 堵住 30 秒 —— 实测这就是
  // "打印中断后 :9100 既不 accept 也不读数据"的直接原因。
  int  sendData(const uint8_t* data, size_t len,
                uint32_t timeoutMs = USB_XFER_TIMEOUT_MS);
  // 从 Bulk IN 端点取回打印机的应答（非阻塞，立刻返回已有字节数；无数据返回 0）
  int  readData(uint8_t* buf, size_t maxLen);
  bool softReset();

  // 故障自愈：打印机被半截作业卡住时会一直 NAK 批量 OUT（以往只能人工断电）。
  // 这里做三件事：清端点 HALT → EP0 上发打印机类 SOFT_RESET → 下发收尾序列。
  // 返回 true 表示"值得再试一次写入"；false 表示没救回来（那就真的只能断电）。
  // 作业收尾策略：结束后到底补不补东西（见 printer_db.h 的 Q_TAIL_* 说明）
  PrinterTailPolicy tailPolicy() const;
  // 能不能用固件自带的纯文本测试页（主机型一律 false，见 Q_NO_TEXT_TESTPAGE）
  bool textTestPageSupported() const;

  bool recover();
  uint32_t getRecoverCount() const { return _recoverCount; }
  uint32_t getRecoverOk()    const { return _recoverOk;    }

  // OTA 写 flash 期间断开打印机（v2.6.6）。
  // 写 flash 时 flash cache 被禁用，USB 驱动的代码在 flash 上，碰一下就 panic。
  // 断开设备后 IDF 内部的 usbHostTask 也无事可做，panic 概率大幅下降。
  //
  // ⚠️ 必须由 USB Host 任务自己去关（requestCloseForOta + 等 otaCloseFinished），
  //    不能在别的任务里直接调：_closeDevice() 内部会调 usb_host_client_handle_events()，
  //    和 USB 任务并发调用 usb_host API 会出事；
  //    反过来先挂起 USB 任务再关也不行 —— 它可能正持有 _usbLock，挂起即死锁。
  void requestCloseForOta();
  bool otaCloseFinished() const { return _otaCloseDone; }

  PrinterState getState() const { return _state; }
  bool isConnected() const { return _state != PRINTER_DISCONNECTED; }
  bool isReady() const { return _state == PRINTER_READY; }
  bool hasBulkIn() const { return _epIn != 0; }
  bool isIppDevice() const { return _ippIface; }
  uint32_t getRxBytes() const { return _rxTotal; }
  // 打印机连续无响应后的冷却保护期：期间转发会立刻失败返回，避免把 loop() 堵死
  bool isStalled() const { return _stallUntilMs != 0 && ((long)(_stallUntilMs - millis())) > 0; }
  String getStallReason() const;
  // 诊断用：IN 回读通道是不是还开着（部分机型被常驻 IN 请求拖住，需要能看见关没关掉）
  bool isBidirActive() const { return _inXfer != nullptr && !_inStreamDisabled; }
  // 回读通道当前是否处于"已暂停、等重试"状态（v2.6.4 起会定时重试，排障用）
  bool isBidirStalled() const { return _inStreamDisabled; }

  // --- 由机型特征库决定的行为开关（未登记的机型回退到 config.h 全局默认）---
  bool passthrough() const;    // 纯透传模式：一个字节都不许往打印机嘴里塞
  bool wantsEscReset() const;    // 作业开头要不要发 ESC @
  bool wantsFormfeed() const;    // 作业结束要不要补 FF
  bool wantsZlp() const;         // 要不要补零长度包
  bool ctrlAllowed() const;      // 能不能发控制传输（Device ID / 字符串描述符 / 端口状态）
  bool bidirAllowed() const;     // 要不要回读 Bulk IN
  bool failRecoverAllowed() const;  // 打印机拒收时允不允许尝试自愈

  const UsbInfo&     getUsbInfo() const { return _ui; }
  const PrinterInfo& getPrinterInfo() const { return _pi; }   // 仅 USB 任务内部使用
  // v2.9.7：跨任务读取用这个（Web / 打印任务）。_pi 里全是 String，USB 任务重新识别时
  // 会重新分配，别的任务同时拷贝就是 use-after-free。拿不到锁返回 false，out 不动。
  bool getPrinterInfoCopy(PrinterInfo& out, uint32_t waitMs = 200) const {
    if (!_infoLock || xSemaphoreTake(_infoLock, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
    out = _pi;
    xSemaphoreGive(_infoLock);
    return true;
  }

  // --- USB/IP 导出（v2.8.0）---
  // 转发控制/bulk URB，接管期间停止常驻 IN；尚未完成 Linux/CUPS 端到端验收。
  // 仅支持当前 claim 的打印接口端点，不是完整的多接口 USB 虚拟化实现。

  // 通用控制传输。data/wLength 按 bmRequestType 的方向解释：
  //   DIR_IN  → data 是接收缓冲，wLength 是请求的最大长度
  //   DIR_OUT → data 是要发送的数据，wLength 是长度
  // 返回实际数据字节数（不含 SETUP）；失败返回负 Linux errno。
  int controlTransferRaw(uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex,
                         uint8_t* data, uint16_t wLength,
                         uint32_t timeoutMs = 1000);

  // 按 URB 精确做一次批量传输。ep 是**端点地址**（含方向位）。
  // isIn=true  → 从打印机读最多 len 字节到 data，返回读到的字节数
  // isIn=false → 把 data 的 len 字节发给打印机，返回实际写出的字节数
  // <0 失败。
  int bulkTransferRaw(uint8_t ep, uint8_t* data, uint16_t len, bool isIn,
                      uint32_t timeoutMs = USB_XFER_TIMEOUT_MS);

  // USB/IP 模式下停/启常驻 Bulk IN 流（否则会跟 URB 抢端点）。
  bool setInStreamSuspended(bool on);


  // 设备上所有接口的 class/subclass/protocol
  // （仅 DEVLIST 附带接口数组，IMPORT 不附带）。
  int  getIfaceCount() const { return _ifaceCount; }
  bool getIfaceInfo(int idx, uint8_t& cls, uint8_t& sub, uint8_t& proto) const;

  // IEEE 1284 端口状态
  uint8_t       getPortStatus() const { return _portStatus; }
  bool          portStatusValid() const { return _portStatusValid; }
  unsigned long portStatusAge() const { return millis() - _lastStatusMs; }
  bool          isPaperOut() const { return _portStatusValid && (_portStatus & PS_PAPER_EMPTY); }
  bool          isSelected() const { return _portStatusValid && (_portStatus & PS_SELECTED); }
  bool          hasError() const { return _portStatusValid && !(_portStatus & PS_NOT_ERROR); }
  String        getStatusText() const;
  String        getStateText() const;

  uint32_t getTxErrors() const { return _txErrors; }
  // v2.9.7：char[] 而不是 String——多个任务（打印/IPP/usbip 写，Web 读）并发，
  // String 的 realloc 与拷贝交错会 use-after-free；定长缓冲最坏只是读到半截文字。
  String   getLastError() const { char b[sizeof(_lastErrorBuf)]; memcpy(b, (const void*)_lastErrorBuf, sizeof(b)); b[sizeof(b) - 1] = 0; return String(b); }

  // 诊断：EP0 是否挂起、同步 control/bulk 待迟到回调回收数（不含常驻 IN）
  bool     isCtrlHung() const { return _ctrlHung; }
  uint32_t getLeakedXfers() const { return _leakedXfers; }

private:
  PrinterState _state;
  UsbInfo      _ui;
  PrinterInfo  _pi;

  uint8_t       _portStatus;
  bool          _portStatusValid;
  unsigned long _lastStatusMs;
  unsigned long _lastPollMs;
  uint32_t      _txErrors;
  volatile char _lastErrorBuf[160] = {0};
  SemaphoreHandle_t _infoLock = nullptr;   // 保护 _pi（跨任务读取）
  void          _setLastError(const String& s) {
    size_t n = s.length() < sizeof(_lastErrorBuf) - 1 ? s.length() : sizeof(_lastErrorBuf) - 1;
    for (size_t i = 0; i < n; i++) _lastErrorBuf[i] = s[i];
    _lastErrorBuf[n] = 0;
  }

  usb_host_client_handle_t _clientHandle;
  usb_device_handle_t      _deviceHandle;
  uint8_t  _epOut, _epIn, _ifaceNum;
  uint16_t _epOutMps, _epInMps;
  // 设备上所有接口的 class/subclass/protocol（USB/IP DEVLIST 的接口数组）
  struct IfaceBrief { uint8_t cls, sub, proto; };
  IfaceBrief _ifaces[256];
  uint8_t   _ifaceCount = 0;
  // USB/IP 模式下暂停常驻 Bulk IN 流（避免与 URB 抢端点）
  bool      _inStreamSuspended = false;
  // 描述符缓存（v2.9.4）
  uint8_t  _idxMfg, _idxProd, _idxSer;
  String   _strMfg, _strProd, _strSer;

  SemaphoreHandle_t _usbLock;   // 保证 client API 同一时刻只有一个任务在调

  // 每条同步管道独立跟踪；超时后的所有权交给对应迟到回调。
  usb_transfer_t* _ctrlXfer = nullptr;
  usb_transfer_t* _bulkXfer = nullptr;
  bool _ctrlDone = false, _bulkDone = false;
  bool _ctrlDetached = false, _bulkDetached = false;
  bool _closing = false, _ifaceClaimed = false;
  bool _inStopping = false;
  bool _inHalted = false;
  void _resumeInStream();
  uint8_t _bulkClearEp = 0;  // bulk 超时后待解除 HALT 的端点；不在完成回调里操作端点
  bool _clearBulkHalt();
  esp_err_t _submitXfer(usb_transfer_t* t, bool control);
  void _onXferDone(usb_transfer_t* t);
  bool _isXferDone(usb_transfer_t* t) const;
  int _controlTransferLocked(uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex,
                             uint8_t* data, uint16_t wLength, uint32_t timeoutMs);

  // SDK 不支持 timeout_ms；EP0 超时不是取消，必须等真实完成回调。
  bool     _ctrlHung;         // 有未完成的 EP0 请求时阻止新控制请求
  uint32_t _leakedXfers;      // 当前交给迟到回调回收的请求数，不是历史泄漏计数
  bool     _portPollDisabled; // 该机型不支持/不响应端口状态请求，停止轮询
  unsigned long _lastReidentMs; // 上次尝试重新识别的时间戳（用于控制通道恢复后的重试）
  bool     _ippIface;         // 当前接管的是 IPP-over-USB 接口

  // --- 双向回读（Bulk IN）---
  // 常驻一条 IN 传输：无数据时设备可能持续 NAK，由硬件轮询；
  // 完成后回调写入环形缓冲，readData() 再取走。
  usb_transfer_t* _inXfer;
  bool            _inBusy;            // 传输是否仍在飞行中
  bool            _inStreamDisabled;  // IN 端点报错后暂停续传
  unsigned long   _inRetryAtMs;       // IN 流下次尝试恢复的时间（0 = 不重试）
  uint8_t*        _rxBuf;
  volatile int    _rxHead;            // 环形缓冲写指针（在 USB task 上下文里被回调写）
  int             _rxTail;            // 读指针（在 loop task 里被 readData 读）
  uint32_t        _rxTotal;

  uint8_t       _consecTimeouts;   // 连续"传输无回调"次数
  unsigned long _stallUntilMs;     // 冷却截止时间（非 0 = 处于快速失败保护期）
  uint8_t       _recoverTrigger;   // 距上次自愈又累计了多少次无回调
  uint8_t       _recoverInJob;     // 本次作业已经自愈过几次（防止对已死的机器反复空等）
  uint32_t      _recoverCount;     // 自愈尝试次数
  uint32_t      _recoverOk;        // 自愈后确实又能写进去的次数
  // OTA 期间断开设备的请求/完成标志（由 USB 任务自己处理，见 requestCloseForOta）
  volatile bool _otaClosePending;
  volatile bool _otaCloseDone;

  // 单块批量写入。waitMs 为该块的最长等待；ok 时 sentOut 为实际写入字节数，
  // stOut 返回 USB 传输状态（用于错误提示）。调用者必须已持有 _usbLock。
  bool _writeChunk(const uint8_t* data, size_t len, size_t& sentOut,
                   int& stOut, uint32_t waitMs);
  // 打印机类软复位（走 EP0，不碰批量管道）。调用者必须已持有 _usbLock。
  // 遵守 Q_NO_CTRL；软件超时后保留请求直到完成，不得强行 free 或绕过挂起守卫。
  bool _classSoftResetLocked();
  // 自愈主流程。调用者必须已持有 _usbLock（sendData 在持锁途中调用）。
  bool _recoverLocked();
  void _startInStream();
  void _stopInStream();
  void _onInDone(usb_transfer_t* t);

  // 延后到任务循环里执行的动作（避免事件回调内重入 usb_host_client_handle_events）
  uint8_t  _openPendingAddr;
  bool     _openPending;
  bool     _closePending;

  static void _clientEventCallback(const usb_host_client_event_msg_t* m, void* arg);
  void _handleClientEvent(const usb_host_client_event_msg_t* m);
  bool _openDevice(uint8_t devAddr);
  bool _findPrinterInterface();
  bool _claimInterface();
  void _closeDevice();
  bool _getDeviceId();
  bool _getPortStatus();
  bool _getStringDesc(uint8_t idx, String& out);
  // 等待指定 transfer 的回调；未完成时不得读取状态/数据或释放。
  bool _awaitXfer(usb_transfer_t* t, uint32_t timeout_ms, bool requireCompleted = true);
  void _releaseXfer(usb_transfer_t* t);
  // 仅 bulk 可 halt/flush；仍未完成则保留指针，由迟到回调回收。
  void _releaseBulkXfer(usb_transfer_t* t);
  // 仅按现有机型策略发送 ZLP；并非所有 bulk 协议都要求 ZLP。
  bool _sendZlp();
  void _parseDeviceId();
  // 统一解析制造商 / 型号（通用优先级，详见 usb_printer.cpp 顶部注释）
  void _resolveIdentity();
  void _pollStatusIfNeeded();
  void _clearDeviceInfo();
  String _guessVendor(uint16_t vid);
};

#endif // USB_PRINTER_H
