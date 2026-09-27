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
  int  sendData(const uint8_t* data, size_t len);  // 可在 loop 任务中调用
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

  // --- 由机型特征库决定的行为开关（未登记的机型回退到 config.h 全局默认）---
  bool wantsEscReset() const;    // 作业开头要不要发 ESC @
  bool wantsFormfeed() const;    // 作业结束要不要补 FF
  bool wantsZlp() const;         // 要不要补零长度包
  bool ctrlAllowed() const;      // 能不能发控制传输（Device ID / 字符串描述符 / 端口状态）
  bool bidirAllowed() const;     // 要不要回读 Bulk IN
  bool failRecoverAllowed() const;  // 打印机拒收时允不允许尝试自愈

  const UsbInfo&     getUsbInfo() const { return _ui; }
  const PrinterInfo& getPrinterInfo() const { return _pi; }

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
  String   getLastError() const { return _lastError; }

  // 诊断：EP0 是否有悬挂传输、以及为此故意泄漏的 transfer 数量
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
  String        _lastError;

  usb_host_client_handle_t _clientHandle;
  usb_device_handle_t      _deviceHandle;
  uint8_t  _epOut, _epIn, _ifaceNum;
  uint16_t _epOutMps, _epInMps;
  uint8_t  _idxMfg, _idxProd, _idxSer;
  String   _strMfg, _strProd, _strSer;

  SemaphoreHandle_t _usbLock;   // 保证 client API 同一时刻只有一个任务在调

  // 控制传输完成标志（用成员变量，避免回调写栈上已销毁的局部变量导致崩溃）
  volatile bool _xferDone;

  // 本 ESP-IDF 版本 usb_transfer_t::timeout_ms 明确“currently not supported yet”，
  // 且 EP0（默认控制管道）没有公开的取消 API。一旦设备对某个控制请求长时间 NAK，
  // 传输就会永远不回调 —— 此时绝不能 usb_host_transfer_free()（IDF 要求
  // “The transfer must not be in-flight”），否则驱动内部残留野指针，稍后 panic。
  // 因此：超时就故意泄漏该 transfer，并置 _ctrlHung 永久停发控制请求。
  bool     _ctrlHung;         // EP0 上有传输悬挂，禁止再发控制请求
  uint32_t _leakedXfers;      // 因悬挂而故意不释放的 transfer 个数
  bool     _portPollDisabled; // 该机型不支持/不响应端口状态请求，停止轮询
  unsigned long _lastReidentMs; // 上次尝试重新识别的时间戳（用于控制通道恢复后的重试）
  bool     _ippIface;         // 当前接管的是 IPP-over-USB 接口

  // --- 双向回读（Bulk IN）---
  // 常驻一条 IN 传输：打印机不吐数据时它会一直 NAK 挂着（Bulk 只用空闲带宽，
  // 不占总线也不占 CPU），吐数据就回调进环形缓冲，readData() 再取走。
  usb_transfer_t* _inXfer;
  bool            _inBusy;            // 传输是否仍在飞行中
  bool            _inStreamDisabled;  // IN 端点报错后不再续传
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

  // 单块批量写入。waitMs 为该块的最长等待；ok 时 sentOut 为实际写入字节数，
  // stOut 返回 USB 传输状态（用于错误提示）。调用者必须已持有 _usbLock。
  bool _writeChunk(const uint8_t* data, size_t len, size_t& sentOut,
                   int& stOut, uint32_t waitMs);
  // 打印机类软复位（走 EP0，不碰批量管道）。调用者必须已持有 _usbLock。
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
  // 按真实时间等待传输回调（不能靠 timeout_ms，该字段在本 IDF 版本无效）
  bool _awaitXfer(usb_transfer_t* t, uint32_t timeout_ms);
  // 安全收尾：仅在传输确实已回调时释放；否则泄漏并标记 _ctrlHung
  void _releaseXfer(usb_transfer_t* t);
  // 批量端点收尾：可用 halt→flush 合法取消后再释放
  void _releaseBulkXfer(usb_transfer_t* t);
  // 发送 ZLP（零长度包）终止批量传输：当数据长度是端点包长(MPS)整数倍时，
  // 必须显式发 ZLP，否则打印机 USB 栈会一直等更多数据，表现为“正在打印”却不出纸。
  bool _sendZlp();
  void _parseDeviceId();
  // 统一解析制造商 / 型号（通用优先级，详见 usb_printer.cpp 顶部注释）
  void _resolveIdentity();
  void _pollStatusIfNeeded();
  void _clearDeviceInfo();
  String _guessVendor(uint16_t vid);
};

#endif // USB_PRINTER_H
