#include "usb_printer.h"
#include "printer_db.h"
#include "crashlog.h"
#include <string.h>
#include "esp_task_wdt.h"   // _awaitXfer 在长等待期间需要重置看门狗，避免 loop() 被阻塞触发 panic 重启

static inline size_t szmin(size_t a, size_t b) { return a < b ? a : b; }

// ---------------------------------------------------------------------------
// 打印机身份识别 —— 通用设计，不绑定任何品牌
//
// 优先级：已知 VID:PID 机型库 → IEEE 1284 Device ID → USB 字符串 → 厂商 VID → 通用名。
// 机型库有名称时跳过额外 EP0 探测；Q_NO_CTRL 机型不主动探测控制通道。
// SDK 的 timeout_ms 尚不支持，EP0 软件等待超时后必须保留请求，直到真实完成回调。
// ---------------------------------------------------------------------------
UsbPrinter::UsbPrinter()
  : _state(PRINTER_DISCONNECTED),
    _portStatus(0), _portStatusValid(false),
    _lastStatusMs(0), _lastPollMs(0), _txErrors(0),
    _clientHandle(nullptr), _deviceHandle(nullptr),
    _epOut(0), _epIn(0), _ifaceNum(0),
    _epOutMps(64), _epInMps(64),
    _idxMfg(0), _idxProd(0), _idxSer(0),
    _usbLock(nullptr),
    _ctrlHung(false), _leakedXfers(0), _portPollDisabled(false),
    _lastReidentMs(0),
    _ippIface(false),
    _inXfer(nullptr), _inBusy(false), _inStreamDisabled(false), _inRetryAtMs(0),
    _rxBuf(nullptr), _rxHead(0), _rxTail(0), _rxTotal(0),
    _consecTimeouts(0), _stallUntilMs(0),
    _recoverTrigger(0), _recoverInJob(0), _recoverCount(0), _recoverOk(0),
    _otaClosePending(false), _otaCloseDone(true),
    _openPendingAddr(0), _openPending(false), _closePending(false) {
  memset(&_ui, 0, sizeof(_ui));
}

// ---------------------------------------------------------------
bool UsbPrinter::begin() {
  _usbLock = xSemaphoreCreateMutex();
  _infoLock = xSemaphoreCreateMutex();
  if (_usbLock == nullptr) {
    Serial.println("[USB] 创建互斥锁失败");
    return false;
  }

#if ENABLE_BIDIR_PASSTHROUGH
  // 双向回读环形缓冲。构造函数里不能做动态分配（全局对象在 heap 就绪前就构造了），
  // 所以放在 begin() 里。
  if (_rxBuf == nullptr) {
    _rxBuf = (uint8_t*)malloc(USB_RX_BUFFER_SIZE);
    if (_rxBuf == nullptr) Serial.println("[USB] IN 回读缓冲分配失败，双向状态不可用");
  }
#endif

  usb_host_config_t hostConfig = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    Serial.printf("[USB] Host install 失败: %s\n", esp_err_to_name(err));
    return false;
  }

  // ⚠️ 本 IDF 版本的 usb_host_client_config_t 只支持**异步**模式：
  //    头文件里 is_synchronous 的注释明写 "Set to false for now"，
  //    而那个 union 里也只有 async 一个成员（没有同步模式的字段）。
  //    → 同步模式这条路在本 IDF 上根本不存在，别再试。
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
// ---------------------------------------------------------------
// OTA 写 flash 期间断开打印机（v2.6.6）
//
// 为什么必须断：写 flash 期间 flash cache 被禁用，此时从 flash 取指令执行就 panic。
// USB 驱动的代码就在 flash 上 —— 只要 USB 侧有任何活动（哪怕只是
// usb_host_client_handle_events 里读一下事件队列）就可能撞上。
// 断开设备后 IDF 内部的 usbHostTask 没有设备要处理，panic 概率大幅下降。
//
// ⚠️ 必须由 USB Host 任务自己去关：
//    · 在别的任务里直接调 _closeDevice() → 和 USB 任务并发调 usb_host API，出事
//    · 先挂起 USB 任务再关 → 它可能正持有 _usbLock，挂起即死锁
//    所以这里只置标志，由 task() 在 USB 任务上下文里执行。
// ---------------------------------------------------------------
void UsbPrinter::requestCloseForOta() {
  _otaCloseDone = false;
  _otaClosePending = true;
}

void UsbPrinter::task() {
  if (_clientHandle == nullptr) {
    vTaskDelay(pdMS_TO_TICKS(20));
    return;
  }

  if (xSemaphoreTake(_usbLock, 0) != pdTRUE) {
    vTaskDelay(pdMS_TO_TICKS(2));
    return;
  }

  // 关闭重试也要推进库事件与 client 回调，否则拔出/取消完成可能无法收敛。
  uint32_t evFlags = 0;
  markStageQ("ev-lib", __LINE__);
  usb_host_lib_handle_events(0, &evFlags);
  markStageQ("ev-cli", __LINE__);
  usb_host_client_handle_events(_clientHandle, 0);

  // OTA 请求断开设备：先处理关闭，不执行恢复和新传输。
  if (_otaClosePending) {
    _closeDevice();
    _otaCloseDone = (_deviceHandle == nullptr);
    _otaClosePending = !_otaCloseDone;
    xSemaphoreGive(_usbLock);
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }

  // 在任务上下文里执行打开/关闭，绝不在事件回调中做（见 _handleClientEvent）
  if (_closePending) {
    _closeDevice();
  } else if (_openPending) {
    _openPending = false;
    markStage("open-begin", __LINE__);
    _openDevice(_openPendingAddr);
  } else {
    _clearBulkHalt();
    _resumeInStream();
    markStageQ("ev-poll", __LINE__);
    _pollStatusIfNeeded();
    // 控制通道曾超时但已收到迟到完成回调。若打印机名仍为空，
    // 每隔 60 秒重试一次身份识别，让本应响应 Device ID 的机型（如 HP）在恢复后
    // 能重新显示正确型号，而无需重新插拔。
    if (!_inStreamSuspended && _state == PRINTER_READY && !_ctrlHung && ctrlAllowed() &&
        _deviceHandle != nullptr && _pi.mdl.length() == 0) {
      unsigned long now = millis();
      if (now - _lastReidentMs > 60000) {
        _lastReidentMs = now;
        Serial.println("[USB] 控制通道已恢复，重试识别打印机...");
        _resolveIdentity();
      }
    }

    // IN 回读重试（v2.6.4）：STALL/CRC 中断后到期恢复。
    // EJL 1284.4 是双向协议，打印机每收一个包就回一个 ACK；主机不读的话
    // 打印机的 ACK 缓冲会塞满、然后直接 NAK 批量 OUT，作业就卡死了。
    // 重试前必须清掉端点 HALT，否则刚挂上就又 STALL。
    if (!_inStreamSuspended && !_inStopping && !_closing && !_closePending && !_bulkXfer &&
        !_bulkClearEp && _inStreamDisabled && _inRetryAtMs != 0 &&
        (long)(millis() - _inRetryAtMs) >= 0 && _inXfer == nullptr) {
      _inRetryAtMs = millis() + IN_STREAM_RETRY_MS;
      if (_deviceHandle && _epIn &&
          usb_host_endpoint_halt(_deviceHandle, _epIn) == ESP_OK) {
        _inHalted = true;
        usb_host_endpoint_flush(_deviceHandle, _epIn);
        if (usb_host_endpoint_clear(_deviceHandle, _epIn) == ESP_OK) {
          _inHalted = false;
          _inStreamDisabled = false;
          _startInStream();
        }
      }
    }
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
      if (_state == PRINTER_DISCONNECTED && _deviceHandle == nullptr && !_closing && !_openPending) {
        _openPendingAddr = m->new_dev.address;
        _openPending = true;
      }
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.println("[USB] 设备已拔出");
      markStage("dev-gone", __LINE__);
      if (m->dev_gone.dev_hdl != _deviceHandle) break;
      _closing = true;
      _ui.attached = false;
      _state = PRINTER_DISCONNECTED;
      _closePending = true;
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------
bool UsbPrinter::_openDevice(uint8_t devAddr) {
  if (_deviceHandle != nullptr || _closing) return false;
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
  const uint32_t enumCount = _ui.enumCount;
  memset(&_ui, 0, sizeof(_ui));
  _ui.enumCount = enumCount;
  _ui.attached = true;
  _ui.devAddr  = devAddr;

  const usb_device_desc_t* dd = nullptr;
  if (usb_host_get_device_descriptor(_deviceHandle, &dd) == ESP_OK) {
    _ui.vid    = dd->idVendor;
    _ui.pid    = dd->idProduct;
    _ui.bcdUsb = dd->bcdUSB;
    _ui.bcdDevice = dd->bcdDevice;
    _ui.deviceClass = dd->bDeviceClass;
    _ui.deviceSubClass = dd->bDeviceSubClass;
    _ui.deviceProtocol = dd->bDeviceProtocol;
    _ui.numConfigurations = dd->bNumConfigurations;
    _idxMfg    = dd->iManufacturer;
    _idxProd   = dd->iProduct;
    _idxSer    = dd->iSerialNumber;
    Serial.printf("[USB] addr=%d VID=0x%04X PID=0x%04X bcdUSB=0x%04X\n",
                  devAddr, _ui.vid, _ui.pid, _ui.bcdUsb);
  } else {
    _closeDevice();
    return false;
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
  // 该机型被登记为"不应答控制传输"时，连端口状态请求也不要发。
  if (ctrlAllowed()) _getPortStatus();
#else
  _portPollDisabled = true;   // raw 打印不需要 IEEE1284 状态，默认不碰这个可选请求
#endif
  markStage("port-ok", __LINE__);

  // 身份识别/状态查询会派发事件，期间拔出时不能再回写 READY。
  if (_closing || _closePending || !_ui.attached) {
    _closeDevice();
    return false;
  }
  _startInStream();           // 开启 IN 回读（没有 IN 端点的单向机自动跳过）
  markStage("in-ok", __LINE__);

  _state = PRINTER_READY;
  markStage("ready", __LINE__);
  Serial.printf("[USB] 打印机就绪: %s %s\n", _pi.mfg.c_str(), _pi.mdl.c_str());
  return true;
}

// ---------------------------------------------------------------
// ---------------------------------------------------------------
// 接口选择：多级候选 + 打分，不再死盯着标准打印机类 0x07。
//
// 为什么要放宽 —— 只认 0x07 会把"货真价实但描述符不标准"的打印机直接判死。
// 最硬的证据来自 Linux ipp-usb 源码：部分惠普 MFP（LaserJet MFP M426fdn、
// ColorLaserJet MFP M278-M281 等）的 IPP-over-USB 用的是**非标准**组合
// 255/9/1。它们确实是打印机，却不符合 07/01/xx。
//
// 同时必须防止在复合设备里抢错接口：一体机身上往往还有 HID 按键、
// Mass Storage 读卡器、Imaging(PTP) 扫描头、厂商固件升级口。
// 这些类一律显式排除，只有拿到"真打印机凭据"的接口才允许入选。
// ---------------------------------------------------------------
struct IfaceCandidate {
  uint8_t  num;
  uint8_t  cls, sub, proto;
  uint8_t  epOut, epIn;
  uint16_t outMps, inMps;
  int      score;
};

// 明确不可能承载打印业务的接口类（复合设备里最常见的"搭车"接口）
static bool isDefinitelyNotPrinterClass(uint8_t cls) {
  switch (cls) {
    case 0x01:  // Audio
    case 0x02:  // Communications / CDC
    case 0x03:  // HID（物理按键、触摸屏）
    case 0x06:  // Imaging（PTP/MTP —— 扫描头 / 相机）
    case 0x08:  // Mass Storage（读卡器）
    case 0x09:  // Hub
    case 0x0A:  // CDC Data
    case 0x0B:  // Smart Card
    case 0x0D:  // Content Security（固件加密狗）
    case 0x0E:  // Video
    case 0x10:  // Audio/Video
      return true;
    default:
      return false;
  }
}

bool UsbPrinter::_findPrinterInterface() {
  const usb_config_desc_t* cfg = nullptr;
  if (usb_host_get_active_config_descriptor(_deviceHandle, &cfg) != ESP_OK) {
    Serial.println("[USB] 读取配置描述符失败");
    return false;
  }

  _ifaceCount = 0;
  _ui.configurationValue = cfg->bConfigurationValue;
  if (cfg->wTotalLength < sizeof(usb_config_desc_t)) return false;
  bool seen[256] = {};
  const uint8_t* p = (const uint8_t*)cfg;
  uint16_t totalLen = cfg->wTotalLength;
  int offset = 0;

  // 是否获授权接管厂商私有接口：必须同时满足"机型库放了行"与"确实是已知打印机厂商"，
  // 双凭据是为了不让非打印机设备被误接管。
  const bool allowVendor = printerTestFlag(_ui.vid, _ui.pid, Q_VENDOR_IFACE, false) &&
                           isKnownPrinterVendor(_ui.vid);

  IfaceCandidate best;
  memset(&best, 0, sizeof(best));
  best.score = -1;
  int ippCount = 0;

  while (offset + 2 <= totalLen) {
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)(p + offset);
    if (d->bLength < 2 || offset + d->bLength > totalLen) return false;

    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      if (d->bLength < sizeof(usb_intf_desc_t)) return false;
      const usb_intf_desc_t* itf = (const usb_intf_desc_t*)d;

      // DEVLIST 每个接口只记录一次；当前 claim 仅支持 alternate setting 0。
      if (itf->bAlternateSetting != 0) { offset += d->bLength; continue; }
      if (!seen[itf->bInterfaceNumber] && _ifaceCount < 255) {
        seen[itf->bInterfaceNumber] = true;
        _ifaces[_ifaceCount].cls   = itf->bInterfaceClass;
        _ifaces[_ifaceCount].sub   = itf->bInterfaceSubClass;
        _ifaces[_ifaceCount].proto = itf->bInterfaceProtocol;
        _ifaceCount++;
      }

      IfaceCandidate c;
      memset(&c, 0, sizeof(c));
      c.num   = itf->bInterfaceNumber;
      c.cls   = itf->bInterfaceClass;
      c.sub   = itf->bInterfaceSubClass;
      c.proto = itf->bInterfaceProtocol;

      // 扫描本接口下的端点，直到遇到下一个接口描述符
      int eo = offset + d->bLength;
      int found = 0;
      while (eo + 2 <= totalLen && found < itf->bNumEndpoints) {
        const usb_standard_desc_t* d2 = (const usb_standard_desc_t*)(p + eo);
        if (d2->bLength < 2 || eo + d2->bLength > totalLen) return false;
        if (d2->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;

        if (d2->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
          if (d2->bLength < sizeof(usb_ep_desc_t)) return false;
          const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d2;
          if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
            if (ep->bEndpointAddress & 0x80) {
              c.epIn  = ep->bEndpointAddress;
              c.inMps = ep->wMaxPacketSize;
            } else {
              c.epOut  = ep->bEndpointAddress;
              c.outMps = ep->wMaxPacketSize;
            }
          }
          found++;
        }
        eo += d2->bLength;
      }

      // --- 打分：分数决定最终接管谁 ---
      int score;
      if (c.epOut == 0)                              score = 0;    // 没有 Bulk OUT，一切都免谈
      else if (c.cls == USB_CLASS_PRINTER)           score = (c.proto == USB_PROTOCOL_IPPUSB) ? 100 : 90;
      else if (isDefinitelyNotPrinterClass(c.cls))   score = 0;
      else if (c.cls == 0xFF && allowVendor)         score = 60;   // 惠普 MFP 的 255/9/1 之类
      else if (allowVendor)                          score = 20;   // 其他未知类，仅在授权厂商下尝试
      else                                           score = 0;
      c.score = score;

      if (c.cls == USB_CLASS_PRINTER && c.proto == USB_PROTOCOL_IPPUSB) ippCount++;

      Serial.printf("[USB] 接口 #%d class=%02X/%02X/%02X OUT=0x%02X(%uB) IN=0x%02X(%uB) → %d 分%s\n",
                    c.num, c.cls, c.sub, c.proto,
                    c.epOut, (unsigned)c.outMps, c.epIn, (unsigned)c.inMps,
                    score,
                    score == 0    ? " 跳过" :
                    score >= 90   ? " 标准打印机接口" : " 候选(厂商私有)");

      if (score > best.score) best = c;
    }
    offset += d->bLength;
  }

  if (offset != totalLen || _ifaceCount != cfg->bNumInterfaces) return false;
  if (best.score <= 0 || best.epOut == 0) {
    Serial.printf("[USB] 未找到可用打印接口 VID=%04X PID=%04X"
                  "（可能不是打印机，或走尚未收录的厂商私有协议）\n", _ui.vid, _ui.pid);
    return false;
  }

  _ifaceNum         = best.num;
  _ui.ifaceNum      = best.num;
  _ui.ifaceClass    = best.cls;
  _ui.ifaceSubClass = best.sub;
  _ui.ifaceProtocol = best.proto;
  _ui.ippIfaces     = (uint8_t)(ippCount > 255 ? 255 : ippCount);
  _epOut    = best.epOut;  _epOutMps = best.outMps ? best.outMps : 64;
  _epIn     = best.epIn;   _epInMps  = best.inMps  ? best.inMps  : 64;
  _ui.epOut = _epOut;  _ui.epIn = _epIn;
  _ui.epOutMps = _epOutMps; _ui.epInMps = _epInMps;
  _ippIface = (best.cls == USB_CLASS_PRINTER && best.proto == USB_PROTOCOL_IPPUSB) ||
              printerTestFlag(_ui.vid, _ui.pid, Q_IPP_OVER_USB, false);

  if (best.cls == USB_CLASS_PRINTER && best.sub != USB_SUBCLASS_PRINTER) {
    Serial.printf("[USB] 提示: 接口子类=0x%02X（非标准 0x01），仍尝试接管\n", best.sub);
  }

  Serial.printf("[USB] 选定接口 #%d class=%02X/%02X/%02X(%s) OUT=0x%02X(%uB) IN=0x%02X(%uB) 得分=%d\n",
                _ifaceNum, best.cls, best.sub, best.proto,
                _ippIface            ? "IPP-over-USB" :
                best.proto == USB_PROTOCOL_BIDIR  ? "双向" :
                best.proto == USB_PROTOCOL_UNIDIR ? "单向" :
                best.proto == USB_PROTOCOL_1284   ? "1284.4" : "厂商私有",
                _epOut, (unsigned)_epOutMps, _epIn, (unsigned)_epInMps, best.score);
  return true;
}

// ---------------------------------------------------------------
bool UsbPrinter::_claimInterface() {
  esp_err_t err = usb_host_interface_claim(_clientHandle, _deviceHandle, _ifaceNum, 0);
  if (err != ESP_OK) {
    _ui.lastError = (int)err;
    Serial.printf("[USB] 声明接口失败: %s\n", esp_err_to_name(err));
    return false;
  }
  _ifaceClaimed = true;
  Serial.printf("[USB] 已声明接口 #%d\n", _ifaceNum);
  return true;
}

// ---------------------------------------------------------------
void UsbPrinter::_closeDevice() {
  _closing = true;
  _ui.attached = false;
  _state = PRINTER_DISCONNECTED;
  _stopInStream();
  if (_deviceHandle != nullptr) {
    if (_ifaceClaimed) {
      if (_epOut) {
        usb_host_endpoint_halt(_deviceHandle, _epOut);
        usb_host_endpoint_flush(_deviceHandle, _epOut);
      }
      if (_epIn) {
        usb_host_endpoint_halt(_deviceHandle, _epIn);
        usb_host_endpoint_flush(_deviceHandle, _epIn);
      }
    }
    unsigned long start = millis();
    while ((_ctrlXfer || _bulkXfer || _inXfer) && millis() - start < 100) {
      uint32_t flags = 0;
      usb_host_lib_handle_events(0, &flags);
      usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(5));
    }
    // 没有完成回调就不能 free，也不能丢弃设备句柄；下一轮继续派发和关闭。
    if (_ctrlXfer || _bulkXfer || _inXfer) { _closePending = true; return; }
    if (_ifaceClaimed) {
      if (usb_host_interface_release(_clientHandle, _deviceHandle, _ifaceNum) != ESP_OK) {
        _closePending = true;
        return;
      }
      _ifaceClaimed = false;
    }
    if (usb_host_device_close(_clientHandle, _deviceHandle) != ESP_OK) {
      _closePending = true;
      return;
    }
    _deviceHandle = nullptr;
  }
  _closePending = false;
  _closing = false;
  _ctrlHung = false;
  _inStopping = false;
  _inHalted = false;
  _bulkClearEp = 0;
  _inStreamDisabled = false;
  _inRetryAtMs = 0;
  _portStatusValid = false;
  _epOut = _epIn = 0;
  _ui.epOut = _ui.epIn = 0;
  _ifaceCount = 0;
  _clearDeviceInfo();
  _rxHead = _rxTail = 0;
  _consecTimeouts = 0;
  _stallUntilMs = 0;
#if ENABLE_PORT_STATUS_POLL
  _portPollDisabled = false;
#endif
  Serial.println("[USB] 设备连接已断开");
}

void UsbPrinter::_clearDeviceInfo() {
  if (_infoLock) xSemaphoreTake(_infoLock, portMAX_DELAY);
  _pi.deviceId = "";
  _pi.mfg = ""; _pi.mdl = ""; _pi.cmd = "";
  _pi.cls = ""; _pi.des = ""; _pi.sern = "";
  if (_infoLock) xSemaphoreGive(_infoLock);
  _strMfg = ""; _strProd = ""; _strSer = "";
}

// 所有调用及回调派发均在 _usbLock 下；回调只匹配自己所属的 transfer。
static int transferErrno(usb_transfer_status_t status) {
  switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED: return 0;
    case USB_TRANSFER_STATUS_STALL: return -32;
    case USB_TRANSFER_STATUS_TIMED_OUT: return -110;
    case USB_TRANSFER_STATUS_NO_DEVICE: return -19;
    case USB_TRANSFER_STATUS_OVERFLOW: return -75;
    default: return -5;
  }
}

static int submitErrno(esp_err_t err) {
  if (err == ESP_ERR_NO_MEM) return -12;
  if (err == ESP_ERR_TIMEOUT) return -110;
  if (err == ESP_ERR_NOT_FOUND) return -19;
  return -5;
}

esp_err_t UsbPrinter::_submitXfer(usb_transfer_t* t, bool control) {
  usb_transfer_t*& active = control ? _ctrlXfer : _bulkXfer;
  if (active != nullptr || _closing || _closePending || !_deviceHandle) return ESP_ERR_INVALID_STATE;
  if (!control && !_clearBulkHalt()) return ESP_ERR_INVALID_STATE;
  t->context = this;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_onXferDone(x); };
  esp_err_t err = control ? usb_host_transfer_submit_control(_clientHandle, t)
                          : usb_host_transfer_submit(t);
  if (err == ESP_OK) {
    active = t;
    (control ? _ctrlDone : _bulkDone) = false;
    (control ? _ctrlDetached : _bulkDetached) = false;
  }
  return err;
}

bool UsbPrinter::_isXferDone(usb_transfer_t* t) const {
  return (t == _ctrlXfer && _ctrlDone) || (t == _bulkXfer && _bulkDone);
}

void UsbPrinter::_onXferDone(usb_transfer_t* t) {
  bool control = (t == _ctrlXfer);
  if (!control && t != _bulkXfer) return;
  (control ? _ctrlDone : _bulkDone) = true;
  if (control ? _ctrlDetached : _bulkDetached) {
    // bulk 的 HALT 留给任务/提交路径清除，完成回调不调用可阻塞端点 API。
    (control ? _ctrlXfer : _bulkXfer) = nullptr;
    (control ? _ctrlDetached : _bulkDetached) = false;
    if (control) _ctrlHung = false;
    if (_leakedXfers) --_leakedXfers;
    usb_host_transfer_free(t);
  }
}

bool UsbPrinter::_awaitXfer(usb_transfer_t* t, uint32_t timeout_ms, bool requireCompleted) {
  unsigned long start = millis();
  while (!_isXferDone(t) && millis() - start < timeout_ms) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(0, &flags);
    usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(5));
    yield();
    esp_task_wdt_reset();
  }
  return _isXferDone(t) && (!requireCompleted || t->status == USB_TRANSFER_STATUS_COMPLETED);
}

void UsbPrinter::_releaseXfer(usb_transfer_t* t) {
  bool control = (t == _ctrlXfer);
  if (!control && t != _bulkXfer) {
    // 提交失败：从未进入驱动队列，不计为待回收传输。
    usb_host_transfer_free(t);
    return;
  }
  if (_isXferDone(t)) {
    (control ? _ctrlXfer : _bulkXfer) = nullptr;
    usb_host_transfer_free(t);
    return;
  }
  bool& detached = control ? _ctrlDetached : _bulkDetached;
  if (!detached) { detached = true; ++_leakedXfers; }
  if (control) _ctrlHung = true;
  // EP0 没有公开取消 API；禁止用 endpoint_clear 伪装取消。
}

bool UsbPrinter::_clearBulkHalt() {
  if (!_bulkClearEp) return true;
  if (_bulkXfer || !_deviceHandle || _closing || _closePending) return false;
  if (usb_host_endpoint_clear(_deviceHandle, _bulkClearEp) != ESP_OK) return false;
  _bulkClearEp = 0;
  return true;
}

void UsbPrinter::_releaseBulkXfer(usb_transfer_t* t) {
  if (t == _bulkXfer && !_bulkDone && _deviceHandle != nullptr) {
    const uint8_t ep = t->bEndpointAddress;
    if (usb_host_endpoint_halt(_deviceHandle, ep) == ESP_OK) {
      _bulkClearEp = ep;
      usb_host_endpoint_flush(_deviceHandle, ep);
    }
    _awaitXfer(t, 500, false);
  }
  _releaseXfer(t);
  _clearBulkHalt();
}

// 是否发送 ZLP 由现有机型策略决定，SDK 也提供可选 ZERO_PACK 标志。
bool UsbPrinter::_sendZlp() {
  if (_deviceHandle == nullptr || _epOut == 0 || _bulkXfer || _closing) return false;
  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(64, 0, &t) != ESP_OK) return false;
  t->device_handle = _deviceHandle;
  t->bEndpointAddress = _epOut;
  t->num_bytes = 0;
  if (_submitXfer(t, false) != ESP_OK) { usb_host_transfer_free(t); return false; }
  bool ok = _awaitXfer(t, 1000);
  _releaseBulkXfer(t);
  return ok;
}

bool UsbPrinter::_getStringDesc(uint8_t idx, String& out) {
  if (!idx || !ctrlAllowed()) return false;
  uint8_t data[126];
  int got = _controlTransferLocked(0x80, USB_B_REQUEST_GET_DESCRIPTOR,
                                  (USB_B_DESCRIPTOR_TYPE_STRING << 8) | idx,
                                  0x0409, data, sizeof(data), 1000);
  if (got < 2 || data[1] != USB_B_DESCRIPTOR_TYPE_STRING || data[0] < 2 ||
      data[0] > got || (data[0] & 1)) return false;
  out = "";
  for (int i = 2; i + 1 < data[0]; i += 2) {
    if (data[i + 1] == 0 && data[i] >= 0x20 && data[i] < 0x7F) out += (char)data[i];
  }
  return out.length() > 0;
}

// ---------------------------------------------------------------
bool UsbPrinter::_getDeviceId() {
  if (!ctrlAllowed()) return false;
  uint8_t data[256];
  int got = _controlTransferLocked(0xA1, USB_PRINTER_GET_DEVICE_ID,
                                  0, _ifaceNum, data, sizeof(data), 1500);
  if (got < 2) return false;
  int len = (data[0] << 8) | data[1];
  if (len <= 2 || len > got) return false;
  _pi.deviceId = String((char*)data + 2, len - 2);
  _parseDeviceId();
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
  // 整段持 _infoLock：其他任务通过 getPrinterInfoCopy() 读 _pi。
  // 期间可能有 ~1.5s 控制传输，读者拿不到锁会用自己上次的副本。
  if (_infoLock) xSemaphoreTake(_infoLock, portMAX_DELAY);
  const char* profName = printerProfileName(_ui.vid, _ui.pid);
  bool gotModel = false;

  // 机型库有名称时避免额外 EP0 探测；控制超时会阻止后续请求，直到真实完成。
  if (profName) {
    _pi.mdl = String(profName);
    gotModel = true;
    Serial.printf("[USB] 机型库识别: %s\n", _pi.mdl.c_str());
  }

  // 2) IEEE 1284 Device ID —— 机型库没登记时尝试读取标准身份信息。
  //    对「机型库里登记为不应答控制传输」的机型直接跳过，免得白白悬挂一个 transfer。
  if (!gotModel && ctrlAllowed() && !_ctrlHung) {
    if (_getDeviceId() && _pi.mdl.length()) {
      gotModel = true;
      Serial.printf("[USB] Device ID 识别: %s %s\n", _pi.mfg.c_str(), _pi.mdl.c_str());
    }
  }

  // 3) USB 字符串描述符（仅当控制通道仍然健康、且该机型允许控制传输时才尝试）
  if (!gotModel && ctrlAllowed() && !_ctrlHung) {
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

  // 5) 型号兜底：至少让用户看到 VID:PID，方便照着往 printer_db.cpp 里补表
  if (_pi.mdl.length() == 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "USB Printer %04X:%04X", _ui.vid, _ui.pid);
    _pi.mdl = String(buf);
    Serial.printf("[USB] 未能识别型号，显示为: %s\n", buf);
  }
  if (_infoLock) xSemaphoreGive(_infoLock);
}

String UsbPrinter::_guessVendor(uint16_t vid) {
  const char* n = guessVendorName(vid);
  if (n) return String(n);
  char buf[16];
  snprintf(buf, sizeof(buf), "VID:%04X", vid);
  return String(buf);
}

// ---------------------------------------------------------------
bool UsbPrinter::_getPortStatus() {
  if (_portPollDisabled || !ctrlAllowed()) return false;
  uint8_t status;
  int got = _controlTransferLocked(0xA1, USB_PRINTER_GET_PORT_STATUS,
                                  0, _ifaceNum, &status, 1, 1000);
  if (got != 1) { _portPollDisabled = true; return false; }
  _portStatus = status;
  _portStatusValid = true;
  _lastStatusMs = millis();
  return true;
}

void UsbPrinter::_pollStatusIfNeeded() {
  unsigned long now = millis();
  if (now - _lastPollMs < STATUS_POLL_MS) return;
  _lastPollMs = now;
  if (!_inStreamSuspended && (_state == PRINTER_READY || _state == PRINTER_BUSY)) {
    // 少数机型不支持该请求，失败则静默降级，网页显示「状态不可读」
    _getPortStatus();
  }
}

// 单块批量写入。调用者必须已持有 _usbLock。
// 抽出来的原因：自愈流程要用"短等待"的方式写几个字节做收尾，而正式作业用
// USB_XFER_TIMEOUT_MS 的长等待 —— 逻辑一样，只有等待上限不同。
bool UsbPrinter::_writeChunk(const uint8_t* data, size_t len, size_t& sentOut,
                             int& stOut, uint32_t waitMs) {
  sentOut = 0;
  stOut   = 0;
  if (_deviceHandle == nullptr || _epOut == 0 || len == 0 || _bulkXfer || _closing) return false;

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(len, 0, &t) != ESP_OK) return false;

  memcpy(t->data_buffer, data, len);
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = _epOut;
  t->num_bytes        = len;
  t->timeout_ms       = waitMs;

  esp_err_t e = _submitXfer(t, false);
  if (e != ESP_OK) {
    usb_host_transfer_free(t);
    return false;
  }

  bool ok = _awaitXfer(t, waitMs);
  int st = _isXferDone(t) ? (int)t->status : (int)USB_TRANSFER_STATUS_TIMED_OUT;
  if (ok && t->actual_num_bytes >= 0 && t->actual_num_bytes <= (int)len) {
    sentOut = (size_t)t->actual_num_bytes;
  } else { ok = false; }
  _releaseBulkXfer(t);
  stOut = st;
  return ok;
}

// ---------------------------------------------------------------
int UsbPrinter::sendData(const uint8_t* data, size_t length, uint32_t timeoutMs) {
  if (_deviceHandle == nullptr || _epOut == 0 || _state == PRINTER_DISCONNECTED) return -1;
  if (_usbLock == nullptr) return -1;
  if (length == 0) return 0;

  // 打印机"叫不应"的冷却保护期：立刻失败返回，绝不再傻等 30 秒把 loop() 堵死。
  // 否则 Windows 后台不停重试失败作业，会让网页 / API / OTA 全部长时间无响应。
  if (_stallUntilMs != 0) {
    if ((long)(_stallUntilMs - millis()) > 0) {
      _txErrors++;
      _setLastError(String("打印机无响应，保护中（") +
                   String((int)((_stallUntilMs - millis()) / 1000)) + " 秒后重试）");
      return -5;
    }
    _stallUntilMs = 0;
  }

  // 拿到锁才算独占 USB client，避免与 usb task 并发
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(2000)) != pdTRUE) {
    _txErrors++;
    _setLastError("USB 总线忙，获取锁超时");
    return -2;
  }

  if (!_deviceHandle || !_epOut || _closing || _closePending || _bulkXfer || _inStreamSuspended) {
    xSemaphoreGive(_usbLock);
    return -1;
  }
  _state = PRINTER_BUSY;
  size_t total = 0;
  int rc = 0;

  // 每份作业重新计一次自愈额度
  _recoverInJob = 0;

  while (total < length) {
    if (!_deviceHandle || !_epOut || _closing || _closePending) { rc = -1; break; }

    size_t chunk = szmin((size_t)USB_XFER_CHUNK, length - total);
    if (chunk == 0) break;

    size_t sent = 0;
    int    st   = 0;
    bool   ok   = _writeChunk(data + total, chunk, sent, st, timeoutMs);

    // 写不动先别急着判死：很可能是打印机被上一个半截作业卡住了。
    // 达到阈值就先自愈一次，然后**原地重试**这一块 —— 救回来的话本次作业还能继续，
    // 用户完全无感（否则只能人工断电，这也是本版本要解决的核心痛点）。
    //
    // 注意：下面一旦判定失败就 break 退出本次作业，所以阈值太高会导致
    // "一份作业里根本凑不满次数、第一次失败的作业必然被牺牲"。
    // USB_RECOVER_AFTER 必须保持 1：一写不动就立刻自愈并重试。
    if (!ok) {
      _recoverTrigger++;
#if USB_RECOVER_ENABLED && USB_RECOVER_RETRY
      if (_recoverTrigger >= USB_RECOVER_AFTER &&
          _recoverInJob < USB_RECOVER_MAX_PER_JOB) {
        _recoverTrigger = 0;
        _recoverInJob++;
        // 自愈成功就地重试这一块：救回来本次作业就能接着走，用户完全无感。
        // （锁已在手，所以调的是无锁版本 _recoverLocked）
        if (_recoverLocked()) {
          ok = _writeChunk(data + total, chunk, sent, st, timeoutMs);
        }
      }
#endif
    }

    if (!ok) {
      _txErrors++;
      _setLastError(String("USB 传输失败 status=") + String(st) +
                   (_recoverCount ? "（已尝试自动恢复）" : ""));
      _consecTimeouts++;
      if (_consecTimeouts >= USB_STALL_MAX_TIMEOUTS) {
        _consecTimeouts = 0;
        _stallUntilMs = millis() + USB_STALL_COOLDOWN_MS;
        Serial.printf("[USB] 打印机连续多次无响应，进入 %lus 快速失败保护（保持网页可用）\n",
                      (unsigned long)(USB_STALL_COOLDOWN_MS / 1000));
      }
      rc = -4;
      break;
    }
    _consecTimeouts = 0;   // 只要有一块成功，就认定打印机恢复正常
    _recoverTrigger = 0;
    total += sent;
    if (sent == 0) break;   // 防呆，避免死循环

    // 数据长度是端点包长(MPS)整数倍时，补一个 ZLP 终止本次批量传输（见 _sendZlp 注释）。
    // 注意：必须用请求发出去的 chunk 判断，而非实际回传的 sent。
    // 少数老机把 ZLP 当成错误，已在机型库里登记 Q_NO_ZLP 的会跳过。
    if (wantsZlp() && (chunk % _epOutMps == 0)) {
      _sendZlp();
    }
  }

  if (_state == PRINTER_BUSY) {
    _state = (!_deviceHandle || _closing || _closePending)
                 ? PRINTER_DISCONNECTED : PRINTER_READY;
  }
  xSemaphoreGive(_usbLock);
  return (rc == 0) ? (int)total : rc;
}

// ---------------------------------------------------------------
// 网页软复位遵守 Q_NO_CTRL；无响应的 EP0 请求保留到完成，期间禁止新控制请求。
bool UsbPrinter::softReset() {
  if (_deviceHandle == nullptr || _ctrlHung) return false;
  if (!ctrlAllowed()) {
    // _lastError 与 sendData() 里其他写入点在同一个 _usbLock 保护下，
    // 这里也必须持锁再写，否则与打印任务并发写同一个 String 会崩。
    if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(1000)) == pdTRUE) {
      _setLastError("该机型不响应控制传输（机型库已登记），软复位不可用；"
                   "如需恢复请断电重启打印机");
      xSemaphoreGive(_usbLock);
    }
    Serial.println("[USB] 软复位跳过：本机型不应答控制传输（Q_NO_CTRL）");
    return false;
  }
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  bool ok = !_inStreamSuspended && _classSoftResetLocked();
  xSemaphoreGive(_usbLock);
  return ok;
}

// 打印机类软复位：走 EP0 发 IEEE-1284 SOFT_RESET（bRequest=0x02）。
// 请求经 EP0 发送，不依赖 bulk OUT；是否恢复打印状态取决于设备实现。
// 调用者必须已持有 _usbLock。
bool UsbPrinter::_classSoftResetLocked() {
  if (!ctrlAllowed()) return false;
  return _controlTransferLocked(0x21, USB_PRINTER_SOFT_RESET, 0, _ifaceNum,
                                nullptr, 0, USB_RECOVER_MAX_WAIT_MS) == 0;
}

// ---------------------------------------------------------------
// 故障自愈。
//
// 为什么需要：打印机被"半截作业"卡住时，会一直 NAK 批量 OUT —— 它认为作业还没收完，
// 数据憋在输入 FIFO 里既不打印也不丢弃，于是什么都写不进去。过去唯一的解法是
// 人工拔电，很伤人：用户看到的就是"打印失灵"，而且管理界面还一起陪葬。
//
// 三步走，从"最不打扰"到"最狠"：
//   1. 清端点 HALT —— 上一个被取消的传输可能给设备留了 STALL，撤掉它；
//   2. EP0 上发打印机类 SOFT_RESET —— 不碰已堵死的批量管道，标准指令，
//      用来清掉悬空的作业、复位页状态机、释放输入 FIFO。
//      遵守 ctrlAllowed() 和 _ctrlHung，不绕过控制通道保护。
//   3. 批量管道若能写进去，再补一套"收尾序列"：
//      UEL → @PJL EOJ → UEL → ESC E(PCL 复位) → ESC @(ESC/P 初始化)。
//      半截 PJL/PCL 作业正是主机型(GDI)打印机最常见的卡住原因 —— 显式给个
//      "作业结束"它才会收尾。注意这与"作业开头别发 ESC @"并不冲突：
//      那条规则防的是污染正常作业流，这里是作业已经废掉之后的收尸。
//      ⚠️ 只对不标 Q_NO_ESC_RESET 的机型做：HP @PJL 流、Epson EJL 1284.4
//         封包流的字节是有语义的，塞这套 PJL/PCL 字节只会破坏帧同步。
//         这类机型靠步骤 2 的 SOFT_RESET 复位就够了。
//
// 调用者必须已持有 _usbLock（sendData 是在持锁途中调用的，不能重复申请）。
// ---------------------------------------------------------------
bool UsbPrinter::_recoverLocked() {
#if !USB_RECOVER_ENABLED
  return false;
#else
  if (!_deviceHandle || !_epOut || !_ifaceClaimed || _closing || _closePending ||
      _bulkXfer || _ctrlXfer || _inStreamSuspended) return false;
  if (!failRecoverAllowed() || !_clearBulkHalt()) return false;

  _recoverCount++;
  Serial.println("[USB] 打印机连续无响应，尝试自动恢复（免断电）...");
  markStage("recover", __LINE__);

  // 先停常驻 IN，禁止取消回调在恢复期间续传；尚未完成就交给 task 后续处理。
  _stopInStream();
  if (_inXfer || _closing || _closePending) return false;
  if (usb_host_endpoint_halt(_deviceHandle, _epOut) != ESP_OK) return false;
  _bulkClearEp = _epOut;
  if (usb_host_endpoint_flush(_deviceHandle, _epOut) != ESP_OK || !_clearBulkHalt()) return false;

  // EP0 软复位遵守机型策略和挂起守卫，软件超时不代表取消或永久泄漏。
  bool clsOk = false;
  if (!_ctrlHung && ctrlAllowed()) {
    clsOk = _classSoftResetLocked();
  }
  // 后续写入仍持锁，不能等 task 才恢复需要并行接收 ACK 的常驻 IN。
  _resumeInStream();

  // --- 步骤 3：批量管道能写进去的话，补一套收尾序列 ---
  // ⚠️ 只对"认裸 ESC 字节"的机型做。Q_NO_ESC_RESET（HP @PJL 流、1390 的
  //    EJL 1284.4 / Remote Mode）的字节流是自描述/带帧结构的，往里塞 UEL /
  //    @PJL EOJ / ESC E / ESC @ 只会破坏帧同步，让卡死的机器死得更透。
  //    这类机型的复位完全靠步骤 2 的 SOFT_RESET。
  // ⚠️ 纯透传机型（Q_PASSTHROUGH）**连步骤 3 都不做** —— 固件一个字都不许加。
  //    1390 走 EJL 1284.4，往里灌 PJL/PCL 字节等于往封包流里掺垃圾。
  static const uint8_t kUel[]     = {0x1B, '%', '-', '1', '2', '3', '4', '5', 'X'};
  static const uint8_t kPjlEoj[]  = "@PJL EOJ\n";
  static const uint8_t kPclReset[] = {0x1B, 'E'};   // PCL 打印机复位
  static const uint8_t kEscAt[]    = {0x1B, '@'};   // ESC/P 打印机初始化

  bool   injectOk = !printerTestFlag(_ui.vid, _ui.pid, Q_NO_ESC_RESET, false)
                     && !passthrough();
  size_t sent = 0;
  int    st   = 0;
  bool   wrote = false;
  if (injectOk) {
    wrote = _writeChunk(kUel, sizeof(kUel), sent, st, USB_RECOVER_MAX_WAIT_MS);
    if (wrote) {
      _writeChunk(kPjlEoj, sizeof(kPjlEoj) - 1, sent, st, USB_RECOVER_MAX_WAIT_MS);
      _writeChunk(kUel,    sizeof(kUel),        sent, st, USB_RECOVER_MAX_WAIT_MS);
      _writeChunk(kPclReset, sizeof(kPclReset), sent, st, USB_RECOVER_MAX_WAIT_MS);
      _writeChunk(kEscAt,    sizeof(kEscAt),    sent, st, USB_RECOVER_MAX_WAIT_MS);
    }
  } else {
    Serial.println("[USB] 跳过收尾序列注入：本机型数据流为封包/自描述格式或纯透传模式");
  }

  // 不管成不成，先解除快速失败冷却，让下一次作业/重试能立刻再试
  _consecTimeouts = 0;
  _recoverTrigger = 0;
  _stallUntilMs   = 0;

  bool ok = clsOk || wrote;
  if (ok) _recoverOk++;
  const char* tailMsg = injectOk ? (wrote ? "已下发" : "写不进") : "已跳过";
  Serial.printf("[USB] 自动恢复结束：类软复位=%s 收尾序列=%s（累计 %u 次，救回 %u 次）\n",
                clsOk ? "OK" : "无应答", tailMsg,
                (unsigned)_recoverCount, (unsigned)_recoverOk);
  return ok;
#endif
}

bool UsbPrinter::recover() {
#if !USB_RECOVER_ENABLED
  return false;
#else
  if (_deviceHandle == nullptr || _usbLock == nullptr) return false;
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  bool ok = _recoverLocked();
  xSemaphoreGive(_usbLock);
  return ok;
#endif
}

bool UsbPrinter::failRecoverAllowed() const {
  return !printerTestFlag(_ui.vid, _ui.pid, Q_NO_FAIL_RECOVER, false);
}

// ---------------------------------------------------------------
// --- 双向回读：从 Bulk IN 端点取回打印机的应答 ---
int UsbPrinter::readData(uint8_t* buf, size_t maxLen) {
  if (_rxBuf == nullptr || maxLen == 0) return 0;
  // 取不到锁就直接返回 0：回读绝不能阻塞转发主流程。
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
  size_t n = 0;
  while (n < maxLen && _rxTail != _rxHead) {
    buf[n++] = _rxBuf[_rxTail];
    _rxTail = (_rxTail + 1) % USB_RX_BUFFER_SIZE;
  }
  xSemaphoreGive(_usbLock);
  return (int)n;
}

void UsbPrinter::_startInStream() {
#if ENABLE_BIDIR_PASSTHROUGH
  if (_deviceHandle == nullptr || _epIn == 0 || _closing || _closePending || _inStopping) return;
  if (!bidirAllowed() || _inXfer != nullptr || _bulkXfer || _bulkClearEp || _inHalted) return;
  // USB/IP 接管期间不让常驻 IN 流重启（端点要留给 URB 用）
  if (_inStreamSuspended) return;
  // 允许在"曾被禁用但已到期重试"的情况下重新挂载
  if (_inStreamDisabled && _inRetryAtMs == 0) return;

  size_t sz = _epInMps;
  if (sz < 64) sz = 64;
  if (sz > 512) sz = 512;
  if (usb_host_transfer_alloc(sz, 0, &_inXfer) != ESP_OK) { _inXfer = nullptr; return; }

  _inXfer->device_handle    = _deviceHandle;
  _inXfer->bEndpointAddress = _epIn;
  _inXfer->num_bytes        = sz;
  _inXfer->callback  = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_onInDone(x); };
  _inXfer->context   = (void*)this;

  if (usb_host_transfer_submit(_inXfer) != ESP_OK) {
    usb_host_transfer_free(_inXfer);
    _inXfer = nullptr;
    return;
  }
  _inBusy = true;
  _inStreamDisabled = false;
  _inRetryAtMs = 0;
  Serial.printf("[USB] 已开启 IN 回读 (EP 0x%02X, %uB)\n", _epIn, (unsigned)sz);
#endif
}

void UsbPrinter::_onInDone(usb_transfer_t* t) {
  if (t != _inXfer) return;
  _inBusy = false;
  const int status = (int)t->status;
  const int n = t->actual_num_bytes;
  bool valid = status == USB_TRANSFER_STATUS_COMPLETED && n >= 0 && n <= t->num_bytes;
  bool stopping = _inStopping || _inStreamSuspended || _closing || _closePending;
  if (valid && !stopping && _rxBuf) {
    for (int i = 0; i < n; ++i) {
      int next = (_rxHead + 1) % USB_RX_BUFFER_SIZE;
      if (next == _rxTail) break;
      _rxBuf[_rxHead] = t->data_buffer[i];
      _rxHead = next;
    }
    _rxTotal += n;
  }
  if (valid && !stopping && _deviceHandle && !_inStreamDisabled && bidirAllowed() &&
      usb_host_transfer_submit(t) == ESP_OK) {
    _inBusy = true;
    return;
  }
  _inXfer = nullptr;
  usb_host_transfer_free(t);
  if (!stopping) {
    _inStreamDisabled = true;
    _inRetryAtMs = millis() + IN_STREAM_RETRY_MS;
    Serial.printf("[USB] IN 回读中断 status=%d，稍后重试\n", status);
  }
}

void UsbPrinter::_stopInStream() {
  _inStopping = true;
  _inRetryAtMs = 0;
  if (_inXfer && _inBusy && _deviceHandle && _epIn) {
    if (usb_host_endpoint_halt(_deviceHandle, _epIn) == ESP_OK) {
      _inHalted = true;
      usb_host_endpoint_flush(_deviceHandle, _epIn);
    }
    unsigned long start = millis();
    while (_inXfer && _inBusy && millis() - start < 200) {
      uint32_t flags = 0;
      usb_host_lib_handle_events(0, &flags);
      usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(5));
    }
    if (!_inXfer && !_closing && !_closePending && _inHalted &&
        usb_host_endpoint_clear(_deviceHandle, _epIn) == ESP_OK) _inHalted = false;
  }
  // 回调可能已释放并清空；若仍在飞，保留指针直至迟到回调。
  if (_inXfer && !_inBusy) {
    usb_host_transfer_free(_inXfer);
    _inXfer = nullptr;
  }
  _rxHead = _rxTail = 0;
}

// --- 机型特征库驱动的行为开关 ---
// 未登记过的机型回退到 config.h 的全局默认值，保证"不知道也不至于出错"。
//
// 纯透传模式（Q_PASSTHROUGH）：固件一个字都不许往打印机嘴里塞。
// 适用于自带完整收尾协议的高精度机型（实测 Epson 1390 走 EJL 1284.4 /
// ESC/P2 Remote Mode，驱动自己发 "REMOTE1" + "JE BC=1 00" + ESC 00 00 00）。
// 固件多塞一个裸 FF 就是"作业结束后又开一页"——用户看到的正是
// "打印纸直接出来了，上面没内容"。
bool UsbPrinter::passthrough() const {
  return printerTestFlag(_ui.vid, _ui.pid, Q_PASSTHROUGH, false);
}
bool UsbPrinter::wantsEscReset() const {
  if (passthrough()) return false;          // 纯透传：作业头不许插 ESC @
  return printerTriFlag(_ui.vid, _ui.pid, Q_WANT_ESC_RESET, Q_NO_ESC_RESET,
                        JOB_RESET_ON_CONNECT != 0);
}
bool UsbPrinter::wantsFormfeed() const {
  if (passthrough()) return false;          // 纯透传：作业尾不许补 FF
  return printerTriFlag(_ui.vid, _ui.pid, Q_WANT_FORMFEED, Q_NO_FORMFEED,
                        JOB_APPEND_FORMFEED != 0);
}
PrinterTailPolicy UsbPrinter::tailPolicy() const {
  // 纯透传：作业尾一个字都不补（驱动自己收尾）
  if (passthrough()) return TAIL_NONE;
  uint16_t f = 0;
  if (printerFlags(_ui.vid, _ui.pid, &f)) {
    if (f & Q_TAIL_NONE) return TAIL_NONE;
    if (f & Q_TAIL_FF)   return TAIL_FF;
  }
  return TAIL_AUTO;
}
bool UsbPrinter::textTestPageSupported() const {
  if (printerTestFlag(_ui.vid, _ui.pid, Q_NO_TEXT_TESTPAGE, false)) return false;
  // 收尾策略是 TAIL_NONE 意味着"驱动自己收尾"——也就是页面描述语言类（PCLm/PDF/PS/PJL），
  // 这类主机型一律不认纯文本，测试页发过去只会把它卡住。
  return tailPolicy() != TAIL_NONE;
}
// 注意 Q_NO_* 这类"禁止"标志的默认值必须是 false —— 查不到该机型时
// 代表"没有理由去禁用"，而不是"禁用"。
bool UsbPrinter::wantsZlp() const {
  return !printerTestFlag(_ui.vid, _ui.pid, Q_NO_ZLP, false);
}
bool UsbPrinter::ctrlAllowed() const {
  return !printerTestFlag(_ui.vid, _ui.pid, Q_NO_CTRL, false);
}
bool UsbPrinter::bidirAllowed() const {
#if ENABLE_BIDIR_PASSTHROUGH
  return !printerTestFlag(_ui.vid, _ui.pid, Q_NO_BIDIR, false);
#else
  return false;
#endif
}

String UsbPrinter::getStallReason() const {
  if (!isStalled()) return String("");
  return String("打印机无响应，保护中（") +
         String((int)((_stallUntilMs - millis()) / 1000)) + " 秒后重试）";
}

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


// ===============================================================
//  USB/IP 控制/bulk 转发实验路径；尚未完成 Linux/CUPS 端到端验收。
// ===============================================================

bool UsbPrinter::getIfaceInfo(int idx, uint8_t& cls, uint8_t& sub, uint8_t& proto) const {
  if (idx < 0 || idx >= (int)_ifaceCount) return false;
  cls   = _ifaces[idx].cls;
  sub   = _ifaces[idx].sub;
  proto = _ifaces[idx].proto;
  return true;
}

void UsbPrinter::_resumeInStream() {
  if (!_inStopping || _inStreamSuspended || _inXfer || _bulkXfer || _bulkClearEp ||
      !_deviceHandle || _closing || _closePending) return;
  if (_inHalted) {
    if (usb_host_endpoint_clear(_deviceHandle, _epIn) != ESP_OK) return;
    _inHalted = false;
  }
  _inStopping = false;
  _inStreamDisabled = false;
  _startInStream();
}

bool UsbPrinter::setInStreamSuspended(bool on) {
  if (!_usbLock || xSemaphoreTake(_usbLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  _inStreamSuspended = on;
  bool ok = true;
  if (on) {
    _stopInStream();
    ok = (!_inXfer && !_inHalted && !_bulkXfer && !_ctrlXfer && !_ctrlHung &&
          !_closing && !_closePending && _deviceHandle && _clearBulkHalt());
  } else {
    // raw bulk 尚未回收时保持停止守卫，避免与常驻 IN 抢占同一端点。
    _clearBulkHalt();
    _resumeInStream();
  }
  xSemaphoreGive(_usbLock);
  return ok;
}

// 通用控制传输：USB/IP 的 URB 控制传输直接调它。
// 关键点：**不能**像 _getStringDesc 那样写死 bmRequestType/wValue ——
// Linux 会发各种请求（GET_DESCRIPTOR / SET_CONFIGURATION / GET_STATUS …）。
int UsbPrinter::controlTransferRaw(uint8_t bmRequestType, uint8_t bRequest,
                                   uint16_t wValue, uint16_t wIndex,
                                   uint8_t* data, uint16_t wLength, uint32_t timeoutMs) {
  if (!_usbLock || !_clientHandle) return -19;
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(5000)) != pdTRUE) return -110;
  int rc = _controlTransferLocked(bmRequestType, bRequest, wValue, wIndex,
                                   data, wLength, timeoutMs);
  xSemaphoreGive(_usbLock);
  return rc;
}

int UsbPrinter::_controlTransferLocked(uint8_t bmRequestType, uint8_t bRequest,
                                       uint16_t wValue, uint16_t wIndex,
                                       uint8_t* data, uint16_t wLength, uint32_t timeoutMs) {
  if (!_deviceHandle || _closing || _closePending) return -19;
  if (_ctrlHung || _ctrlXfer) return -110;
  if (wLength > 4096) return -75;
  if (wLength && !data) return -22;
  const bool isIn = (bmRequestType & 0x80) != 0;
  const int setupLen = sizeof(usb_setup_packet_t);
  const int totalLen = setupLen + wLength;
  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(totalLen, 0, &t) != ESP_OK) return -12;
  usb_setup_packet_t setup = {};
  setup.bmRequestType = bmRequestType;
  setup.bRequest = bRequest;
  setup.wValue = wValue;
  setup.wIndex = wIndex;
  setup.wLength = wLength;
  memcpy(t->data_buffer, &setup, setupLen);
  if (!isIn && wLength) memcpy(t->data_buffer + setupLen, data, wLength);
  t->device_handle = _deviceHandle;
  t->bEndpointAddress = isIn ? 0x80 : 0;
  t->num_bytes = totalLen;  // SDK 控制传输使用单个 SETUP + DATA 缓冲。
  t->timeout_ms = timeoutMs;  // SDK 尚不支持，仅作记录；等待由事件循环计时。
  esp_err_t err = _submitXfer(t, true);
  if (err != ESP_OK) {
    usb_host_transfer_free(t);  // 未提交，不在飞，也不计入保留计数。
    return submitErrno(err);
  }
  bool done = _awaitXfer(t, timeoutMs, false);
  int rc = -110;
  if (done) {
    rc = transferErrno(t->status);
    if (rc == 0) {
      const int actual = t->actual_num_bytes;
      if (actual < setupLen) rc = -5;
      else if (actual > totalLen) rc = -75;
      else {
        rc = actual - setupLen;
        // 仅 COMPLETED 且 actual 在 SETUP..SETUP+wLength 范围时拷贝，且先拷贝后释放。
        if (isIn && rc) memcpy(data, t->data_buffer + setupLen, rc);
      }
    }
  }
  _releaseXfer(t);
  if (rc < 0) _setLastError(String("控制传输 errno=") + String(rc));
  char diag[32];
  snprintf(diag, sizeof(diag), "ct:rc%d,len%u", rc, (unsigned)wLength);
  markIppDiag(diag);
  return rc;
}

// 按 URB 精确做一次批量传输（不依赖常驻 IN 流）。
int UsbPrinter::bulkTransferRaw(uint8_t ep, uint8_t* data, uint16_t len, bool isIn,
                                uint32_t timeoutMs) {
  if (!_usbLock || !_clientHandle) return -19;
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(5000)) != pdTRUE) return -110;
  int rc = -19;
  if (!_deviceHandle || _closing || _closePending) { xSemaphoreGive(_usbLock); return rc; }
  if (_bulkXfer || (isIn && _inXfer)) { xSemaphoreGive(_usbLock); return -110; }
  if (ep != (isIn ? _epIn : _epOut) || !(ep & 0x0F) ||
      bool(ep & 0x80) != isIn || (len && !data)) { xSemaphoreGive(_usbLock); return -22; }
  // 不为对齐而扩大主机请求，避免吞掉超出 URB 长度的数据。
  if (isIn && (!_epInMps || len % _epInMps)) { xSemaphoreGive(_usbLock); return -22; }
  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(len ? len : 1, 0, &t) != ESP_OK) {
    xSemaphoreGive(_usbLock); return -12;
  }
  if (!isIn && len) memcpy(t->data_buffer, data, len);
  t->device_handle = _deviceHandle;
  t->bEndpointAddress = ep;
  t->num_bytes = len;
  t->timeout_ms = timeoutMs;
  esp_err_t err = _submitXfer(t, false);
  if (err != ESP_OK) {
    usb_host_transfer_free(t);
    rc = submitErrno(err);
  } else {
    rc = -110;
    if (_awaitXfer(t, timeoutMs, false)) {
      rc = transferErrno(t->status);
      if (!rc) {
        int actual = t->actual_num_bytes;
        if (actual < 0) rc = -5;
        else if (actual > len) rc = -75;
        else {
          if (isIn && actual) memcpy(data, t->data_buffer, actual);
          rc = actual;
        }
      }
    }
    _releaseBulkXfer(t);
  }
  xSemaphoreGive(_usbLock);
  return rc;
}
