#include "usb_printer.h"
#include "crashlog.h"
#include <string.h>

static inline size_t szmin(size_t a, size_t b) { return a < b ? a : b; }

// ---------------------------------------------------------------------------
// 打印机身份识别 —— 通用设计，不绑定任何品牌
//
// 优先级（高 → 低）：
//   1. IEEE 1284 GET_DEVICE_ID —— USB 打印机类标准请求，返回 "MFG:..;MDL:..;CMD:.."。
//      这是唯一能拿到「真实型号」的通用手段，绝大多数标准打印机都支持，
//      所以任何合规打印机插上就能显示自己的名字，不需要改代码。
//   2. VID:PID 已知机型表 —— 只给「不应答控制传输」的机型兜底。
//      典型是 Epson Stylus Photo 1390 这类 GDI / 主机型打印机：它对 ESP32 USB Host
//      的所有控制请求长时间 NAK，而本 IDF 版本 usb_transfer_t::timeout_ms 无效、
//      EP0 又没有公开的取消接口，请求会永远悬挂（只能故意泄漏 transfer）。
//      这类机型必须跳过控制传输，否则名字读不到、还白等一场。
//   3. USB 字符串描述符（iProduct / iManufacturer）
//   4. 厂商 VID 查表
//   5. 兜底显示 "USB Printer VID:PID"，至少让用户能照着补表
//
// 想让自己的打印机显示具体型号：只要它响应 Device ID，什么都不用改；
// 只有当它像 1390 一样完全不应答控制传输时，才需要在下面的表里加一行。
// ---------------------------------------------------------------------------
struct KnownPrinter {
  uint16_t    vid;
  uint16_t    pid;
  const char* name;
  bool        noCtrl;   // true = 该机型不应答控制传输，跳过 Device ID / 字符串描述符
};
static const KnownPrinter KNOWN_PRINTERS[] = {
  // 已实测：Epson Stylus Photo 1390 对 ESP32 USB Host 的控制传输全部不应答
  {0x04B8, 0x0007, "Epson Stylus Photo 1390", true},
  {0x04B8, 0x0005, "Epson Stylus Photo 1390", true},
};
static const KnownPrinter* findKnownPrinter(uint16_t vid, uint16_t pid) {
  for (const KnownPrinter& k : KNOWN_PRINTERS)
    if (k.vid == vid && k.pid == pid) return &k;
  return nullptr;
}

// 常见打印机厂商 VID（Device ID 读不到时的兜底）
struct VidEntry { uint16_t vid; const char* name; };
static const VidEntry VID_TABLE[] = {
  {0x04B8, "EPSON"},   {0x04F9, "Brother"},  {0x03F0, "HP"},
  {0x04A9, "Canon"},   {0x413C, "Dell"},     {0x04E8, "Samsung"},
  {0x043D, "Lexmark"}, {0x0924, "Xerox"},    {0x0482, "Kyocera"},
  {0x05CA, "Ricoh"},   {0x06A3, "Konica Minolta"}, {0x0390, "OKI"},
  {0x04DA, "Panasonic"}, {0x0930, "Toshiba"}, {0x04DD, "Sharp"},
  {0x0A5F, "Zebra"},   {0x1203, "TSC"},      {0x0519, "Star"},
  {0x1A86, "QinHeng"}, {0x1FC9, "Gprinter"}, {0x0483, "STMicro"},
  {0x17EF, "Lenovo"},  {0x04C5, "Fujitsu"},
};

UsbPrinter::UsbPrinter()
  : _state(PRINTER_DISCONNECTED),
    _portStatus(0), _portStatusValid(false),
    _lastStatusMs(0), _lastPollMs(0), _txErrors(0),
    _clientHandle(nullptr), _deviceHandle(nullptr),
    _epOut(0), _epIn(0), _ifaceNum(0),
    _epOutMps(64), _epInMps(64),
    _idxMfg(0), _idxProd(0), _idxSer(0),
    _usbLock(nullptr),
    _xferDone(false), _ctrlHung(false), _leakedXfers(0), _portPollDisabled(false),
    _openPendingAddr(0), _openPending(false), _closePending(false) {
  memset(&_ui, 0, sizeof(_ui));
}

// ---------------------------------------------------------------
bool UsbPrinter::begin() {
  _usbLock = xSemaphoreCreateMutex();
  if (_usbLock == nullptr) {
    Serial.println("[USB] 创建互斥锁失败");
    return false;
  }

  usb_host_config_t hostConfig = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    Serial.printf("[USB] Host install 失败: %s\n", esp_err_to_name(err));
    return false;
  }

  usb_host_client_config_t clientConfig = {
    .is_synchronous = false,
    .max_num_event_msg = 5,
    .async = {
      .client_event_callback = _clientEventCallback,
      .callback_arg = this,
    }
  };
  err = usb_host_client_register(&clientConfig, &_clientHandle);
  if (err != ESP_OK) {
    Serial.printf("[USB] Client 注册失败: %s\n", esp_err_to_name(err));
    return false;
  }

  Serial.println("[USB] Host 就绪，等待打印机接入...");
  return true;
}

// ---------------------------------------------------------------
// 只在 USB Host 任务里跑。拿不到锁就直接跳过本轮——
// 绝不与转发任务并发调用 usb_host_client_handle_events()。
void UsbPrinter::task() {
  if (_clientHandle == nullptr) {
    vTaskDelay(pdMS_TO_TICKS(20));
    return;
  }

  if (xSemaphoreTake(_usbLock, 0) != pdTRUE) {
    vTaskDelay(pdMS_TO_TICKS(2));
    return;
  }

  // 在任务上下文里执行打开/关闭，绝不在事件回调中做（见 _handleClientEvent）
  if (_closePending) {
    _closePending = false;
    _closeDevice();
  } else if (_openPending) {
    _openPending = false;
    markStage("open-begin", __LINE__);
    _openDevice(_openPendingAddr);
  } else {
    // 这三步是“就绪之后”的热路径，用静默埋点定位崩溃位置（不打串口，避免拖慢循环）
    uint32_t evFlags = 0;
    markStageQ("ev-lib", __LINE__);
    usb_host_lib_handle_events(0, &evFlags);
    markStageQ("ev-cli", __LINE__);
    usb_host_client_handle_events(_clientHandle, 0);
    markStageQ("ev-poll", __LINE__);
    _pollStatusIfNeeded();
    markStageQ("ev-idle", __LINE__);
  }

  xSemaphoreGive(_usbLock);
  vTaskDelay(pdMS_TO_TICKS(5));
}

// ---------------------------------------------------------------
void UsbPrinter::_clientEventCallback(const usb_host_client_event_msg_t* m, void* arg) {
  ((UsbPrinter*)arg)->_handleClientEvent(m);
}

void UsbPrinter::_handleClientEvent(const usb_host_client_event_msg_t* m) {
  switch (m->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      Serial.printf("[USB] 检测到新设备，地址 %d\n", m->new_dev.address);
      markStage("new-dev", __LINE__);
      // 仅置标志，真正打开延后到 task() 循环里做。
      // 原因：本回调由 usb_host_client_handle_events() 内部触发，若在此直接同步
      // 执行 _openDevice()（其内部又会调用 usb_host_client_handle_events 等待传输
      // 完成），会造成重入，破坏 USB 主机内部状态，导致后续 ISR/任务访问坏数据而
      // panic 重启。所以必须回到任务主循环上下文再操作。
      if (_state == PRINTER_DISCONNECTED && !_openPending) {
        _openPendingAddr = m->new_dev.address;
        _openPending = true;
      }
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.println("[USB] 设备已拔出");
      markStage("dev-gone", __LINE__);
      _closePending = true;
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------
bool UsbPrinter::_openDevice(uint8_t devAddr) {
  markStage("open-start", __LINE__);
  esp_err_t err = usb_host_device_open(_clientHandle, devAddr, &_deviceHandle);
  if (err != ESP_OK) {
    _ui.lastError = (int)err;
    Serial.printf("[USB] 打开设备失败: %s\n", esp_err_to_name(err));
    return false;
  }

  _state = PRINTER_CONNECTED;
  markStage("open-ok", __LINE__);
  _ui.enumCount++;
  _ui.lastError = 0;
  _portStatusValid = false;
  _clearDeviceInfo();
  _ui.attached = true;
  _ui.devAddr  = devAddr;

  const usb_device_desc_t* dd = nullptr;
  if (usb_host_get_device_descriptor(_deviceHandle, &dd) == ESP_OK) {
    _ui.vid    = dd->idVendor;
    _ui.pid    = dd->idProduct;
    _ui.bcdUsb = dd->bcdUSB;
    _idxMfg    = dd->iManufacturer;
    _idxProd   = dd->iProduct;
    _idxSer    = dd->iSerialNumber;
    Serial.printf("[USB] addr=%d VID=0x%04X PID=0x%04X bcdUSB=0x%04X\n",
                  devAddr, _ui.vid, _ui.pid, _ui.bcdUsb);
  }
  markStage("desc-ok", __LINE__);

  // 字符串描述符缓存清空。真正的身份识别放到 claim 接口之后，
  // 由 _resolveIdentity() 统一按优先级处理。
  _strMfg = _strProd = _strSer = "";

  if (!_findPrinterInterface()) {
    Serial.println("[USB] 未找到打印机类接口（可能不是打印机，或走厂商私有协议）");
    markStage("iface-fail", __LINE__);
    _closeDevice();
    return false;
  }
  markStage("iface-ok", __LINE__);

  if (!_claimInterface()) {
    markStage("claim-fail", __LINE__);
    _closeDevice();
    return false;
  }
  markStage("claim-ok", __LINE__);

  // 身份识别统一放在 claim 接口之后：GET_DEVICE_ID 的 wIndex 需要接口号。
  // 顺序很关键 —— 旧实现先读字符串描述符，打印机一 NAK 就置 _ctrlHung，
  // 导致后面真正能拿到型号的 Device ID 请求被直接跳过。改为统一入口。
  _resolveIdentity();
  markStage("ident-ok", __LINE__);
#if ENABLE_PORT_STATUS_POLL
  _getPortStatus();
#else
  _portPollDisabled = true;   // raw 打印不需要 IEEE1284 状态，默认不碰这个可选请求
#endif
  markStage("port-ok", __LINE__);

  _state = PRINTER_READY;
  markStage("ready", __LINE__);
  Serial.printf("[USB] 打印机就绪: %s %s\n", _pi.mfg.c_str(), _pi.mdl.c_str());
  return true;
}

// ---------------------------------------------------------------
bool UsbPrinter::_findPrinterInterface() {
  const usb_config_desc_t* cfg = nullptr;
  if (usb_host_get_active_config_descriptor(_deviceHandle, &cfg) != ESP_OK) {
    Serial.println("[USB] 读取配置描述符失败");
    return false;
  }

  const uint8_t* p = (const uint8_t*)cfg;
  uint16_t totalLen = cfg->wTotalLength;
  int offset = 0;

  while (offset + 2 <= totalLen) {
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)(p + offset);
    if (d->bLength == 0) break;

    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* itf = (const usb_intf_desc_t*)d;

      if (itf->bInterfaceClass == USB_CLASS_PRINTER) {
        _ifaceNum         = itf->bInterfaceNumber;
        _ui.ifaceNum      = itf->bInterfaceNumber;
        _ui.ifaceClass    = itf->bInterfaceClass;
        _ui.ifaceSubClass = itf->bInterfaceSubClass;
        _ui.ifaceProtocol = itf->bInterfaceProtocol;
        _epOut = 0; _epIn = 0;

        if (itf->bInterfaceSubClass != USB_SUBCLASS_PRINTER) {
          Serial.printf("[USB] 提示: 接口子类=0x%02X（非标准 0x01），仍尝试接管\n",
                        itf->bInterfaceSubClass);
        }

        // 扫描本接口下的端点，直到遇到下一个接口描述符
        int eo = offset + d->bLength;
        int found = 0;
        while (eo + 2 <= totalLen && found < itf->bNumEndpoints) {
          const usb_standard_desc_t* d2 = (const usb_standard_desc_t*)(p + eo);
          if (d2->bLength == 0) break;
          if (d2->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;

          if (d2->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d2;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
              if (ep->bEndpointAddress & 0x80) {
                _epIn = ep->bEndpointAddress;
                _epInMps = ep->wMaxPacketSize;
              } else {
                _epOut = ep->bEndpointAddress;
                _epOutMps = ep->wMaxPacketSize;
              }
            }
            found++;
          }
          eo += d2->bLength;
        }

        _ui.epOut = _epOut; _ui.epIn = _epIn;
        _ui.epOutMps = _epOutMps; _ui.epInMps = _epInMps;

        Serial.printf("[USB] 打印机接口 #%d 协议=%s OUT=0x%02X(%uB) IN=0x%02X(%uB)\n",
                      _ifaceNum,
                      itf->bInterfaceProtocol == USB_PROTOCOL_BIDIR  ? "双向" :
                      itf->bInterfaceProtocol == USB_PROTOCOL_UNIDIR ? "单向" :
                      itf->bInterfaceProtocol == USB_PROTOCOL_1284   ? "1284.4" : "其他",
                      _epOut, (unsigned)_epOutMps, _epIn, (unsigned)_epInMps);

        if (_epOut != 0) return true;   // 拿到 Bulk OUT 端点即成功
        Serial.println("[USB] 该接口没有 Bulk OUT 端点，继续找下一个");
      }
    }
    offset += d->bLength;
  }

  return false;
}

// ---------------------------------------------------------------
bool UsbPrinter::_claimInterface() {
  esp_err_t err = usb_host_interface_claim(_clientHandle, _deviceHandle, _ifaceNum, 0);
  if (err != ESP_OK) {
    _ui.lastError = (int)err;
    Serial.printf("[USB] 声明接口失败: %s\n", esp_err_to_name(err));
    return false;
  }
  Serial.printf("[USB] 已声明接口 #%d\n", _ifaceNum);
  return true;
}

// ---------------------------------------------------------------
void UsbPrinter::_closeDevice() {
  if (_deviceHandle != nullptr) {
    // 接口里若还有排队的批量传输，interface_release 会失败（"interface currently
    // can not be freed"），进而 device_close 报 INVALID_STATE、设备句柄永远关不掉。
    // 先 halt+flush 取消批量端点上的残留传输。
    if (_epOut != 0) {
      usb_host_endpoint_halt(_deviceHandle, _epOut);
      usb_host_endpoint_flush(_deviceHandle, _epOut);
    }
    // 把 flush 产生的 CANCELED 回调派发掉，避免驱动仍持有引用
    unsigned long s = millis();
    while ((millis() - s) < 100) usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(10));

    esp_err_t r1 = usb_host_interface_release(_clientHandle, _deviceHandle, _ifaceNum);
    esp_err_t r2 = usb_host_device_close(_clientHandle, _deviceHandle);
    if (r1 != ESP_OK || r2 != ESP_OK) {
      Serial.printf("[USB] 关闭设备: release=%s close=%s\n",
                    esp_err_to_name(r1), esp_err_to_name(r2));
    }
    _deviceHandle = nullptr;
  }
  _state = PRINTER_DISCONNECTED;
  _portStatusValid = false;
  _epOut = 0; _epIn = 0;
  _ui.attached = false;
  _ui.epOut = 0; _ui.epIn = 0;
  _clearDeviceInfo();

  // 设备已断开，EP0 管道随设备一起销毁，之前的悬挂传输不可能再被这条管道引用。
  // 因此把“停发控制请求”的闸门复位，否则拔插一次之后就永远读不到 Device ID。
  // 注意：那些悬挂的 transfer 仍然刻意不 free（驱动可能仍持有指针，宁可泄漏
  // 约 350 字节／次，也绝不重蹈 2.0.2 释放飞行中传输导致 panic 的覆辙）。
  _ctrlHung = false;
  _xferDone = false;
#if ENABLE_PORT_STATUS_POLL
  _portPollDisabled = false;
#endif
  Serial.println("[USB] 设备连接已断开");
}

void UsbPrinter::_clearDeviceInfo() {
  _pi.deviceId = "";
  _pi.mfg = ""; _pi.mdl = ""; _pi.cmd = "";
  _pi.cls = ""; _pi.des = ""; _pi.sern = "";
  _strMfg = ""; _strProd = ""; _strSer = "";
}

// ---------------------------------------------------------------
// 按“真实时间”等待传输回调。
// 注意：绝不能依赖 usb_transfer_t::timeout_ms —— SDK 头文件明确写着
// “Timeout (in milliseconds) of the packet (currently not supported yet)”，
// 该字段在本 IDF 版本被完全忽略。设备若持续 NAK（打印机暖机时很常见），
// 传输就永远不会回调，必须由我们自己按时间放弃等待。
bool UsbPrinter::_awaitXfer(usb_transfer_t* t, uint32_t timeout_ms) {
  unsigned long start = millis();
  while (!_xferDone && (millis() - start) < timeout_ms) {
    usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(5));
  }
  return _xferDone && t->status == USB_TRANSFER_STATUS_COMPLETED;
}

// 传输收尾。IDF 要求 “The transfer must not be in-flight when attempting to
// free it”，而 EP0 没有公开的取消/flush 接口（usb_host_endpoint_halt/flush 只
// 适用于已 claim 接口下的端点）。所以传输没回调时：
//   宁可泄漏几百字节，也绝不 free —— 否则驱动内部队列残留野指针，
//   随后处理事件时必然 panic（就是“插上打印机走到 ready 就重启”的元凶）。
void UsbPrinter::_releaseXfer(usb_transfer_t* t) {
  if (_xferDone) {
    usb_host_transfer_free(t);
    return;
  }
  _leakedXfers++;
  _ctrlHung = true;       // EP0 已被悬挂传输占住，后续控制请求全部放弃
  markStage("ctrl-hung", __LINE__);
  Serial.printf("[USB] 警告: 控制传输无响应，已放弃并保留该 transfer（累计 %u 个）\n",
                (unsigned)_leakedXfers);
}

// 批量端点属于已 claim 的接口，可以合法地 halt→flush 取消排队传输。
// flush 会让传输以 CANCELED 状态回调，回调到达后才可安全释放。
void UsbPrinter::_releaseBulkXfer(usb_transfer_t* t) {
  if (_xferDone) {
    usb_host_transfer_free(t);
    return;
  }
  if (_deviceHandle != nullptr && _epOut != 0) {
    usb_host_endpoint_halt(_deviceHandle, _epOut);
    usb_host_endpoint_flush(_deviceHandle, _epOut);
    unsigned long s = millis();
    while (!_xferDone && (millis() - s) < 500) {
      usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(5));
    }
    usb_host_endpoint_clear(_deviceHandle, _epOut);
  }
  if (_xferDone) {
    usb_host_transfer_free(t);
  } else {
    _leakedXfers++;       // 仍未回调 —— 同样绝不 free
    markStage("bulk-hung", __LINE__);
  }
}

bool UsbPrinter::_getStringDesc(uint8_t idx, String& out) {
  if (idx == 0 || _deviceHandle == nullptr) return false;
  if (_ctrlHung) return false;      // EP0 已悬挂，再发也是徒劳且危险

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(256, 0, &t) != ESP_OK) return false;

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = 0;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context    = (void*)this;
  t->timeout_ms = 1000;

  usb_setup_packet_t* sp = (usb_setup_packet_t*)t->data_buffer;
  sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                      USB_BM_REQUEST_TYPE_RECIP_DEVICE;
  sp->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
  sp->wValue   = (USB_B_DESCRIPTOR_TYPE_STRING << 8) | idx;
  sp->wIndex   = 0x0409;   // English (US)
  sp->wLength  = 126;
  t->num_bytes = sizeof(usb_setup_packet_t) + 126;

  bool ok = false;
  if (usb_host_transfer_submit_control(_clientHandle, t) == ESP_OK) {
    if (_awaitXfer(t, t->timeout_ms) &&
        t->actual_num_bytes > (int)sizeof(usb_setup_packet_t) + 2) {
      uint8_t* d = t->data_buffer + sizeof(usb_setup_packet_t);
      int chars = (d[0] - 2) / 2;      // UTF-16LE，这里只取 ASCII 低字节
      out = "";
      for (int i = 0; i < chars && i < 63; i++) {
        char c = (char)d[2 + i * 2];
        if (c >= 0x20 && c < 0x7F) out += c;
      }
      ok = out.length() > 0;
    }
  }
  _releaseXfer(t);
  return ok;
}

// ---------------------------------------------------------------
bool UsbPrinter::_getDeviceId() {
  if (_deviceHandle == nullptr) return false;
  if (_ctrlHung) return false;

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(320, 0, &t) != ESP_OK) return false;

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = 0;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context    = (void*)this;
  t->timeout_ms = 1500;

  usb_setup_packet_t* sp = (usb_setup_packet_t*)t->data_buffer;
  sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                      USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  sp->bRequest = USB_PRINTER_GET_DEVICE_ID;
  sp->wValue   = 0;
  sp->wIndex   = _ifaceNum;
  sp->wLength  = 256;
  t->num_bytes = sizeof(usb_setup_packet_t) + 256;

  if (usb_host_transfer_submit_control(_clientHandle, t) == ESP_OK) {
    if (_awaitXfer(t, t->timeout_ms) &&
        t->actual_num_bytes > (int)sizeof(usb_setup_packet_t) + 2) {
      uint8_t* d = t->data_buffer + sizeof(usb_setup_packet_t);
      int len = (d[0] << 8) | d[1];
      if (len > 2 && len <= 256) {
        _pi.deviceId = String((char*)(d + 2), len - 2);
        Serial.printf("[USB] Device ID: %s\n", _pi.deviceId.c_str());
        _parseDeviceId();
      }
    } else {
      Serial.println("[USB] 读取 Device ID 失败（部分机型不支持），改用描述符信息");
    }
  }

  _releaseXfer(t);
  return _pi.deviceId.length() > 0;
}

// ---------------------------------------------------------------
void UsbPrinter::_parseDeviceId() {
  // 形如: MFG:EPSON;CMD:ESCPL2,BDC,D4;MDL:Stylus Photo 1390;CLS:PRINTER;DES:...
  _pi.mfg = _pi.mdl = _pi.cmd = _pi.cls = _pi.des = _pi.sern = "";

  int start = 0;
  while (start < (int)_pi.deviceId.length()) {
    int sc = _pi.deviceId.indexOf(';', start);
    if (sc < 0) sc = _pi.deviceId.length();
    String kv = _pi.deviceId.substring(start, sc);
    int c = kv.indexOf(':');
    if (c > 0) {
      String k = kv.substring(0, c); k.trim(); k.toUpperCase();
      String v = kv.substring(c + 1); v.trim();
      if      (k == "MFG")                 _pi.mfg  = v;
      else if (k == "MDL")                 _pi.mdl  = v;
      else if (k == "CMD")                 _pi.cmd  = v;
      else if (k == "CLS")                 _pi.cls  = v;
      else if (k == "DES")                 _pi.des  = v;
      else if (k == "SN" || k == "SERN")   _pi.sern = v;
    }
    start = sc + 1;
  }

  // 兜底（厂商 VID / 字符串描述符 / 通用名）统一交给 _resolveIdentity() 处理，
  // 这里只负责解析 Device ID 本身的键值。
}

// ---------------------------------------------------------------
// 统一解析「制造商 / 型号」，通用优先级见本文件顶部注释。
// 必须在 claim 接口之后调用：GET_DEVICE_ID 的 wIndex 需要接口号。
// ---------------------------------------------------------------
void UsbPrinter::_resolveIdentity() {
  const KnownPrinter* kp = findKnownPrinter(_ui.vid, _ui.pid);
  bool gotModel = false;

  // 1) IEEE 1284 Device ID —— 唯一能拿到真实型号的通用手段。
  //    对「已知不应答控制传输」的机型直接跳过，免得白白悬挂一个 transfer。
  if (!(kp && kp->noCtrl) && !_ctrlHung) {
    if (_getDeviceId() && _pi.mdl.length()) {
      gotModel = true;
      Serial.printf("[USB] Device ID 识别: %s %s\n", _pi.mfg.c_str(), _pi.mdl.c_str());
    }
  }

  // 2) VID:PID 已知机型表（不应答控制传输的 GDI 机型走这里）
  if (!gotModel && kp) {
    _pi.mdl = String(kp->name);
    gotModel = true;
    Serial.printf("[USB] VID:PID 表识别: %s\n", _pi.mdl.c_str());
  }

  // 3) USB 字符串描述符（仅当控制通道仍然健康时才尝试，避免再次悬挂）
  if (!gotModel && !_ctrlHung) {
    if (_idxProd) _getStringDesc(_idxProd, _strProd);
    if (_idxMfg)  _getStringDesc(_idxMfg,  _strMfg);
    if (_idxSer)  _getStringDesc(_idxSer,  _strSer);
    if (_strProd.length()) {
      _pi.mdl = _strProd;
      gotModel = true;
      Serial.printf("[USB] 字符串描述符识别: %s\n", _strProd.c_str());
    }
  }

  // 4) 制造商兜底：Device ID 的 MFG → 字符串描述符 → 厂商 VID 表 → VID:xxxx
  if (_pi.mfg.length() == 0)
    _pi.mfg = _strMfg.length() ? _strMfg : _guessVendor(_ui.vid);

  // 5) 型号兜底：至少让用户看到 VID:PID，方便照着往 KNOWN_PRINTERS 里补表
  if (_pi.mdl.length() == 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "USB Printer %04X:%04X", _ui.vid, _ui.pid);
    _pi.mdl = String(buf);
    Serial.printf("[USB] 未能识别型号，显示为: %s\n", buf);
  }
}

String UsbPrinter::_guessVendor(uint16_t vid) {
  for (const VidEntry& e : VID_TABLE) {
    if (e.vid == vid) return String(e.name);
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "VID:%04X", vid);
  return String(buf);
}

// ---------------------------------------------------------------
bool UsbPrinter::_getPortStatus() {
  if (_deviceHandle == nullptr) return false;
  if (_ctrlHung || _portPollDisabled) return false;

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(32, 0, &t) != ESP_OK) return false;

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = 0;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context    = (void*)this;
  t->timeout_ms = 1000;

  usb_setup_packet_t* sp = (usb_setup_packet_t*)t->data_buffer;
  sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                      USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  sp->bRequest = USB_PRINTER_GET_PORT_STATUS;
  sp->wValue   = 0;
  sp->wIndex   = _ifaceNum;
  sp->wLength  = 1;
  t->num_bytes = sizeof(usb_setup_packet_t) + 1;

  bool ok = false;
  if (usb_host_transfer_submit_control(_clientHandle, t) == ESP_OK) {
    if (_awaitXfer(t, t->timeout_ms) &&
        t->actual_num_bytes >= (int)sizeof(usb_setup_packet_t) + 1) {
      _portStatus      = t->data_buffer[sizeof(usb_setup_packet_t)];
      _portStatusValid = true;
      _lastStatusMs    = millis();
      ok = true;
    }
  }
  _releaseXfer(t);
  if (!ok) {
    // 该机型（如 Epson 1390 这类 GDI 打印机）不响应 IEEE1284 端口状态请求，
    // 不必反复重试：raw 打印只用批量端点，端口状态纯属可选信息。
    _portPollDisabled = true;
    Serial.println("[USB] 该机型不支持端口状态查询，已停止轮询（不影响打印）");
  }
  return ok;
}

void UsbPrinter::_pollStatusIfNeeded() {
  unsigned long now = millis();
  if (now - _lastPollMs < STATUS_POLL_MS) return;
  _lastPollMs = now;
  if (_state == PRINTER_READY || _state == PRINTER_BUSY) {
    // 少数机型不支持该请求，失败则静默降级，网页显示「状态不可读」
    _getPortStatus();
  }
}

// ---------------------------------------------------------------
int UsbPrinter::sendData(const uint8_t* data, size_t length) {
  if (_deviceHandle == nullptr || _epOut == 0 || _state == PRINTER_DISCONNECTED) return -1;
  if (_usbLock == nullptr) return -1;
  if (length == 0) return 0;

  // 拿到锁才算独占 USB client，避免与 usb task 并发
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(2000)) != pdTRUE) {
    _txErrors++;
    _lastError = "USB 总线忙，获取锁超时";
    return -2;
  }

  _state = PRINTER_BUSY;
  size_t total = 0;
  int rc = 0;

  while (total < length) {
    if (_deviceHandle == nullptr || _epOut == 0) { rc = -1; break; }

    size_t chunk = szmin((size_t)USB_XFER_CHUNK, length - total);
    usb_transfer_t* t = nullptr;
    if (usb_host_transfer_alloc(chunk, 0, &t) != ESP_OK) { rc = -3; break; }

    memcpy(t->data_buffer, data + total, chunk);
    t->device_handle    = _deviceHandle;
    t->bEndpointAddress = _epOut;
    t->num_bytes        = chunk;
    t->timeout_ms       = 10000;

    _xferDone = false;
    t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
    t->context = (void*)this;

    esp_err_t e = usb_host_transfer_submit(t);
    if (e != ESP_OK) {
      usb_host_transfer_free(t);
      _lastError = String("提交传输失败: ") + esp_err_to_name(e);
      rc = -3;
      break;
    }

    bool   ok   = _awaitXfer(t, t->timeout_ms);
    size_t sent = ok ? (size_t)t->actual_num_bytes : 0;
    int    st   = (int)t->status;
    _releaseBulkXfer(t);

    if (!ok) {
      _txErrors++;
      _lastError = String("USB 传输失败 status=") + String(st);
      rc = -4;
      break;
    }
    total += sent;
    if (sent == 0) break;   // 防呆，避免死循环
  }

  if (_state == PRINTER_BUSY) {
    _state = (_deviceHandle == nullptr) ? PRINTER_DISCONNECTED : PRINTER_READY;
  }
  xSemaphoreGive(_usbLock);
  return (rc == 0) ? (int)total : rc;
}

// ---------------------------------------------------------------
bool UsbPrinter::softReset() {
  if (_deviceHandle == nullptr || _ctrlHung) return false;
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(32, 0, &t) != ESP_OK) {
    xSemaphoreGive(_usbLock);
    return false;
  }

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = 0;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context    = (void*)this;
  t->timeout_ms = 1500;

  usb_setup_packet_t* sp = (usb_setup_packet_t*)t->data_buffer;
  sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                      USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  sp->bRequest = USB_PRINTER_SOFT_RESET;
  sp->wValue   = 0;
  sp->wIndex   = _ifaceNum;
  sp->wLength  = 0;
  t->num_bytes = sizeof(usb_setup_packet_t);

  bool ok = false;
  if (usb_host_transfer_submit_control(_clientHandle, t) == ESP_OK) {
    ok = _awaitXfer(t, t->timeout_ms);
  }
  _releaseXfer(t);
  xSemaphoreGive(_usbLock);

  Serial.println(ok ? "[USB] 打印机已软复位" : "[USB] 软复位失败");
  return ok;
}

// ---------------------------------------------------------------
String UsbPrinter::getStateText() const {
  switch (_state) {
    case PRINTER_DISCONNECTED: return "未连接";
    case PRINTER_CONNECTED:    return "已连接(初始化中)";
    case PRINTER_READY:        return "就绪";
    case PRINTER_BUSY:         return "打印中";
    case PRINTER_ERROR:        return "错误";
  }
  return "未知";
}

String UsbPrinter::getStatusText() const {
  if (_state == PRINTER_DISCONNECTED) return "未连接";
  if (_state == PRINTER_CONNECTED)    return "初始化中";
  if (!_portStatusValid)              return "就绪（状态不可读）";
  if (_portStatus & PS_PAPER_EMPTY)   return "缺纸";
  if (!(_portStatus & PS_NOT_ERROR))  return "打印机报错";
  if (!(_portStatus & PS_SELECTED))   return "离线 / 未选中";
  if (_state == PRINTER_BUSY)         return "打印中";
  return "就绪";
}
