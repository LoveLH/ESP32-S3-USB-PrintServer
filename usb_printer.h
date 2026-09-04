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

#define USB_PRINTER_GET_DEVICE_ID   0x00
#define USB_PRINTER_GET_PORT_STATUS 0x01
#define USB_PRINTER_SOFT_RESET      0x02

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
  bool softReset();

  PrinterState getState() const { return _state; }
  bool isConnected() const { return _state != PRINTER_DISCONNECTED; }
  bool isReady() const { return _state == PRINTER_READY; }

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
  void _parseDeviceId();
  // 统一解析制造商 / 型号（通用优先级，详见 usb_printer.cpp 顶部注释）
  void _resolveIdentity();
  void _pollStatusIfNeeded();
  void _clearDeviceInfo();
  String _guessVendor(uint16_t vid);
};

#endif // USB_PRINTER_H
