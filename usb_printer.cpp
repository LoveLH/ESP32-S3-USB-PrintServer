#include "usb_printer.h"
#include "printer_db.h"
#include "crashlog.h"
#include <string.h>
#include "esp_task_wdt.h"   // _awaitXfer 在长等待期间需要重置看门狗，避免 loop() 被阻塞触发 panic 重启

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
// 只有当它像 1390 一样完全不应答控制传输时，才需要在 printer_db.cpp 里加一行。
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
    _xferDone(false), _ctrlHung(false), _leakedXfers(0), _portPollDisabled(false),
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

  // OTA 请求断开设备：优先处理，别的什么都不做
  if (_otaClosePending) {
    _otaClosePending = false;
    if (_deviceHandle != nullptr) {
      _closeDevice();
      Serial.println("[USB] OTA 期间已断开打印机设备");
    }
    _otaCloseDone = true;
    xSemaphoreGive(_usbLock);
    vTaskDelay(pdMS_TO_TICKS(5));
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
    // 控制通道曾被卡死但已通过 EP0 清除恢复（见 _releaseXfer）。若打印机名仍为空，
    // 每隔 60 秒重试一次身份识别，让本应响应 Device ID 的机型（如 HP）在恢复后
    // 能重新显示正确型号，而无需重新插拔。
    if (_state == PRINTER_READY && !_ctrlHung && ctrlAllowed() &&
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
    if (_inStreamDisabled && _inRetryAtMs != 0 &&
        (long)(millis() - _inRetryAtMs) >= 0 && _inXfer == nullptr) {
      _inRetryAtMs = 0;
      if (_deviceHandle != nullptr && _epIn != 0) {
        usb_host_endpoint_halt(_deviceHandle, _epIn);
        usb_host_endpoint_flush(_deviceHandle, _epIn);
        usb_host_endpoint_clear(_deviceHandle, _epIn);
      }
      _inStreamDisabled = false;
      _startInStream();
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
  // 该机型被登记为"不应答控制传输"时，连端口状态请求也不要发。
  if (ctrlAllowed()) _getPortStatus();
#else
  _portPollDisabled = true;   // raw 打印不需要 IEEE1284 状态，默认不碰这个可选请求
#endif
  markStage("port-ok", __LINE__);

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
    if (d->bLength == 0) break;

    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* itf = (const usb_intf_desc_t*)d;

      // 记录所有接口的 class/subclass/protocol —— USB/IP 的 IMPORT 响应
      // 要逐个上报，Linux 靠这个挑驱动（只报"最佳接口"是不够的）。
      if (_ifaceCount < (int)(sizeof(_ifaces) / sizeof(_ifaces[0]))) {
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
        if (d2->bLength == 0) break;
        if (d2->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;

        if (d2->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
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
  Serial.printf("[USB] 已声明接口 #%d\n", _ifaceNum);
  return true;
}

// ---------------------------------------------------------------
void UsbPrinter::_closeDevice() {
  // 回收常驻的 IN 回读传输。必须排在 release 之前：接口不释放的话，
  // interface_release 会因"仍有排队的批量传输"失败。
  _stopInStream();

  if (_deviceHandle != nullptr) {
    // 接口里若还有排队的批量传输，interface_release 会失败（"interface currently
    // can not be freed"），进而 device_close 报 INVALID_STATE、设备句柄永远关不掉。
    // 先 halt+flush 取消批量端点上的残留传输（OUT 与 IN 都要）。
    if (_epOut != 0) {
      usb_host_endpoint_halt(_deviceHandle, _epOut);
      usb_host_endpoint_flush(_deviceHandle, _epOut);
    }
    if (_epIn != 0) {
      usb_host_endpoint_halt(_deviceHandle, _epIn);
      usb_host_endpoint_flush(_deviceHandle, _epIn);
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
  _rxHead = _rxTail = 0;
  _consecTimeouts = 0;
  _stallUntilMs = 0;
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
bool UsbPrinter::_awaitXfer(usb_transfer_t* t, uint32_t timeout_ms, bool requireCompleted) {
  unsigned long start = millis();
  while (!_xferDone && (millis() - start) < timeout_ms) {
    usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(5));
    // 让出 CPU 并重置看门狗：一次大作业会串行很多块，单块等待最长可达 timeout_ms。
    // 必须在此期间持续 yield（让 WiFi/OTA/Web 任务运行）+ 喂 WDT（loop() 被阻塞时
    // 其顶部的 esp_task_wdt_reset 无法执行），否则会触发 WDT panic 重启，导致打印中断/重发。
    yield();
    esp_task_wdt_reset();
  }
  if (!_xferDone) return false;
  return requireCompleted ? (t->status == USB_TRANSFER_STATUS_COMPLETED) : true;
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
  // 默认控制管道(EP0)上的 STALL 可用 usb_host_endpoint_clear 清除——这是 ESP-IDF 支持的恢复手段。
  // 清除后解除 _ctrlHung，让后续控制请求（含重新识别、软复位）能再尝试，而不是永久失效。
  if (_deviceHandle != nullptr) {
    usb_host_endpoint_clear(_deviceHandle, 0);
  }
  _ctrlHung = false;
  markStage("ctrl-hung", __LINE__);
  Serial.printf("[USB] 警告: 控制传输无响应，已清除 EP0 并恢复控制通道（累计泄漏 %u 个）\n",
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

// 发送 ZLP（零长度包）终止批量传输。
// USB 规范：Bulk OUT 数据长度若是端点包长(MPS)的整数倍，主机必须再发一个
// 零长度包来标志“本次传输结束”。ESP-IDF 的 USB Host 不会自动补 ZLP，若不手动补，
// 打印机（尤其 HP）的 USB 栈会认为传输还没完、一直等后续数据，于是面板卡在
// “正在打印文档”、数据憋在缓冲里永不走纸。补一个 ZLP 即可让它正常收尾打印。
// （数据长度非整数倍时，硬件会用“短包”自然收尾，无需 ZLP，故只在区间整数倍时调用。）
bool UsbPrinter::_sendZlp() {
  if (_deviceHandle == nullptr || _epOut == 0) return false;
  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(64, 0, &t) != ESP_OK) return false;

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = _epOut;
  t->num_bytes        = 0;   // 0 字节 = ZLP
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context  = (void*)this;

  esp_err_t e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    usb_host_transfer_free(t);
    return false;
  }
  bool ok = _awaitXfer(t, 1000);
  usb_host_transfer_free(t);
  return ok;
}

bool UsbPrinter::_getStringDesc(uint8_t idx, String& out) {
  if (idx == 0 || _deviceHandle == nullptr) return false;
  if (_ctrlHung || !ctrlAllowed()) return false;   // EP0 已悬挂/该机型不应答 —— 再发也是徒劳且危险

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
  if (_ctrlHung || !ctrlAllowed()) return false;

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
  const char* profName = printerProfileName(_ui.vid, _ui.pid);
  bool gotModel = false;

  // ⚠️ v2.8.5：**机型库优先**。
  //    原因：一次**失败**的控制传输会永久废掉 EP0 ——
  //    本 IDF 版本 usb_transfer_t::timeout_ms 无效，EP0 也没有公开的取消接口，
  //    超时后不能 free（会留野指针），只能**故意泄漏**那个 transfer；
  //    而仍在飞行中的传输不会因 usb_host_endpoint_clear 而完成 →
  //    **EP0 从此被它永久占住，之后所有控制传输都失败**。
  //    实测 HP LaserJet 正是如此：Device ID 不应答 → 泄漏 1 个 transfer →
  //    EP0 废掉 → USB/IP 的 GET_DESCRIPTOR 永远失败。
  //    机型库里有名字就不必再问打印机 —— 型号显示够用，而 EP0 留着给
  //    USB/IP 的枚举（Linux 会自己读 Device ID，精度不丢）。
  if (profName) {
    _pi.mdl = String(profName);
    gotModel = true;
    Serial.printf("[USB] 机型库识别: %s\n", _pi.mdl.c_str());
  }

  // 1) IEEE 1284 Device ID —— 机型库没登记时的兜底（唯一能拿到真实型号的通用手段）。
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
  if (_deviceHandle == nullptr) return false;
  if (_ctrlHung || _portPollDisabled || !ctrlAllowed()) return false;

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

// 单块批量写入。调用者必须已持有 _usbLock。
// 抽出来的原因：自愈流程要用"短等待"的方式写几个字节做收尾，而正式作业用
// USB_XFER_TIMEOUT_MS 的长等待 —— 逻辑一样，只有等待上限不同。
bool UsbPrinter::_writeChunk(const uint8_t* data, size_t len, size_t& sentOut,
                             int& stOut, uint32_t waitMs) {
  sentOut = 0;
  stOut   = 0;
  if (_deviceHandle == nullptr || _epOut == 0 || len == 0) return false;

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(len, 0, &t) != ESP_OK) return false;

  memcpy(t->data_buffer, data, len);
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = _epOut;
  t->num_bytes        = len;
  t->timeout_ms       = waitMs;

  _xferDone = false;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context = (void*)this;

  esp_err_t e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    usb_host_transfer_free(t);
    return false;
  }

  bool ok = _awaitXfer(t, waitMs);
  int  st = (int)t->status;              // 释放之后就不能再读，先拷出来
  if (ok) sentOut = (size_t)t->actual_num_bytes;
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
      _lastError = String("打印机无响应，保护中（") +
                   String((int)((_stallUntilMs - millis()) / 1000)) + " 秒后重试）";
      return -5;
    }
    _stallUntilMs = 0;
  }

  // 拿到锁才算独占 USB client，避免与 usb task 并发
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(2000)) != pdTRUE) {
    _txErrors++;
    _lastError = "USB 总线忙，获取锁超时";
    return -2;
  }

  _state = PRINTER_BUSY;
  size_t total = 0;
  int rc = 0;

  // 每份作业重新计一次自愈额度
  _recoverInJob = 0;

  while (total < length) {
    if (_deviceHandle == nullptr || _epOut == 0) { rc = -1; break; }

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
      _lastError = String("USB 传输失败 status=") + String(st) +
                   (_recoverCount ? "（已尝试自动恢复）" : "");
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
    _state = (_deviceHandle == nullptr) ? PRINTER_DISCONNECTED : PRINTER_READY;
  }
  xSemaphoreGive(_usbLock);
  return (rc == 0) ? (int)total : rc;
}

// ---------------------------------------------------------------
// 网页"软复位"按钮。
// v2.6.4 撤回 v2.6.2 的 force 旁路：Q_NO_CTRL 机型（如 1390）对 EP0
// class request 100% 不应答，硬发必然泄漏一个 transfer（实测 85 次泄漏
// 把 USB 驱动队列搞坏、设备反复重枚举）。这里直接拒绝并说明原因，
// 比"点了没反应还顺带搞坏 USB 栈"好。
bool UsbPrinter::softReset() {
  if (_deviceHandle == nullptr || _ctrlHung) return false;
  if (!ctrlAllowed()) {
    // _lastError 与 sendData() 里其他写入点在同一个 _usbLock 保护下，
    // 这里也必须持锁再写，否则与打印任务并发写同一个 String 会崩。
    if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(1000)) == pdTRUE) {
      _lastError = "该机型不响应控制传输（机型库已登记），软复位不可用；"
                   "如需恢复请断电重启打印机";
      xSemaphoreGive(_usbLock);
    }
    Serial.println("[USB] 软复位跳过：本机型不应答控制传输（Q_NO_CTRL）");
    return false;
  }
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  bool ok = _classSoftResetLocked();
  xSemaphoreGive(_usbLock);
  return ok;
}

// 打印机类软复位：走 EP0 发 IEEE-1284 SOFT_RESET（bRequest=0x02）。
// 关键点：它压根不碰批量 OUT 管道 —— 打印机把 bulk 口堵死时，这是唯一
// 还有希望送达的标准指令，会清掉半截作业、复位页状态机、释放输入 FIFO。
// 调用者必须已持有 _usbLock。
bool UsbPrinter::_classSoftResetLocked() {
  if (_deviceHandle == nullptr || _ctrlHung || !ctrlAllowed()) return false;

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(32, 0, &t) != ESP_OK) {
    return false;
  }

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = 0;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context    = (void*)this;
  t->timeout_ms = USB_RECOVER_MAX_WAIT_MS;

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

  Serial.println(ok ? "[USB] 打印机已软复位" : "[USB] 软复位失败");
  return ok;
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
//      ⚠️ 这一步带 force=true：即便机型库标了 Q_NO_CTRL 也照样发。
//         1390 实测 recoverOk=0 的根因就是这里被 ctrlAllowed() 永久挡死，
//         自愈只剩清 HALT，等于什么都没做。
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
  if (_deviceHandle == nullptr || _epOut == 0) return false;
  if (!failRecoverAllowed()) return false;

  _recoverCount++;
  Serial.println("[USB] 打印机连续无响应，尝试自动恢复（免断电）...");
  markStage("recover", __LINE__);

  // --- 步骤 1：清掉两个批量端点上可能残留的 HALT ---
  uint8_t eps[2] = {_epOut, _epIn};
  for (int i = 0; i < 2; i++) {
    if (eps[i] == 0) continue;
    usb_host_endpoint_halt(_deviceHandle, eps[i]);
    usb_host_endpoint_flush(_deviceHandle, eps[i]);
    unsigned long s = millis();
    while ((millis() - s) < 200) {
      usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(10));
    }
    usb_host_endpoint_clear(_deviceHandle, eps[i]);
  }

  // --- 步骤 2：EP0 上的打印机类软复位 ---
  // ⚠️ v2.6.4 撤回 v2.6.2 的 force 旁路。
  //   v2.6.2 为了让自愈"有戏"，绕过了 Q_NO_CTRL 硬发软复位。实测证明这是错的：
  //   Epson 1390 对 EP0 class request **100% 不应答**，而本 IDF 版本
  //   usb_transfer_t::timeout_ms 无效、EP0 又无取消接口 → 每发一次泄漏一个
  //   transfer。实测 leakedXfers=85 恰好等于 recover=85，85 个野指针堆在
  //   USB 驱动队列里，把设备搞到反复重枚举（enumCount 1→7），
  //   彩色大作业大量失败、纸出来却是空白的。
  //   结论：对"控制通道不应答"的机型，正确做法是**彻底不碰 EP0**，
  //   自愈只清批量端点的 HALT。宁可机器真要断电，也不能拿 USB 栈去赌。
  bool clsOk = false;
  if (!_ctrlHung && ctrlAllowed()) {
    clsOk = _classSoftResetLocked();
  }

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
  if (_deviceHandle == nullptr || _epIn == 0) return;
  if (!bidirAllowed() || _inXfer != nullptr) return;
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
  _inBusy = false;
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
    // STALL / CRC / 溢出等：暂停续传，稍后清端点 HALT 再重试。
    //
    // ⚠️ v2.6.4 修正：以前这里直接永久禁用（_inStreamDisabled = true 再也不重试），
    // 打印机的回读通道就此死掉，直到重新插拔。对 EJL 1284.4 机型（Epson 1390）
    // 这是**致命**的：1284.4 是双向协议，打印机每收一个包就回一个 ACK，
    // 主机不读 → 打印机 ACK 缓冲塞满 → 直接 NAK 批量 OUT → 作业卡死。
    // 实测 1390 彩色大作业就是这样失败的（bidir=false 却是"应该有回读"的机型）。
    // 现在改为：延迟重试，且重试前必须清端点 HALT，否则立刻又 STALL。
    //
    // ⚠️ v2.6.6 修正：**必须释放这个 transfer 并把 _inXfer 置空**。
    // 传输已经完成（回调被调用），此时 free 是安全的。
    // 以前既没 free 也没置空 → ① 每 STALL 一次泄漏一个 transfer 对象，
    // 内存被慢慢吃光；② _inXfer 永远非空 → 重试条件永远不成立 → 通道实际是死的。
    if (_inXfer == t) {
      usb_host_transfer_free(t);
      _inXfer = nullptr;
    }
    _inStreamDisabled = true;
    _inRetryAtMs = millis() + IN_STREAM_RETRY_MS;
    Serial.printf("[USB] IN 回读中断 (status=%d)，%lums 后重试\n",
                  (int)t->status, (unsigned long)(IN_STREAM_RETRY_MS / 1000));
    return;
  }
  int n = t->actual_num_bytes;
  for (int i = 0; i < n; i++) {
    int next = (_rxHead + 1) % USB_RX_BUFFER_SIZE;
    if (next == _rxTail) break;   // 缓冲满则丢弃新数据：回读数据丢了不影响打印本身
    _rxBuf[_rxHead] = t->data_buffer[i];
    _rxHead = next;
  }
  if (n > 0) _rxTotal += (uint32_t)n;

  // 立刻再挂一条上去，保持回读通道常开
  if (_deviceHandle != nullptr && !_inStreamDisabled && bidirAllowed()) {
    if (usb_host_transfer_submit(t) == ESP_OK) {
      _inBusy = true;
      return;
    }
  }
  // 续传提交失败：同样安排一次重试，不要把回读通道永久判死
  _inStreamDisabled = true;
  _inRetryAtMs = millis() + IN_STREAM_RETRY_MS;
}

void UsbPrinter::_stopInStream() {
  if (_inXfer == nullptr) return;
  esp_err_t e = usb_host_transfer_free(_inXfer);
  if (e != ESP_OK) {
    // 还在飞行中，IDF 不允许直接 free。与批量端点一样走 halt→flush→等回调→clear。
    if (_deviceHandle != nullptr && _epIn != 0) {
      usb_host_endpoint_halt(_deviceHandle, _epIn);
      usb_host_endpoint_flush(_deviceHandle, _epIn);
      unsigned long s = millis();
      while (_inBusy && (millis() - s) < 200) {
        usb_host_client_handle_events(_clientHandle, pdMS_TO_TICKS(5));
      }
      usb_host_endpoint_clear(_deviceHandle, _epIn);
    }
    if (!_inBusy) {
      usb_host_transfer_free(_inXfer);
    } else {
      _leakedXfers++;   // 宁可泄漏也不 free 飞行中的传输
      Serial.println("[USB] 警告: IN 传输未能回收，已泄漏");
    }
  }
  _inXfer = nullptr;
  _inBusy = false;
  _inStreamDisabled = false;
  _inRetryAtMs = 0;      // 重新枚举后 IN 流要能重新挂上，别留着旧的禁用标记
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
//  USB/IP 支持（v2.8.0）
//  让远端 Linux 的 CUPS 直接驱动这台打印机：Linux 发 URB，我们把 URB
//  翻译成真实的 USB 控制/批量传输。
// ===============================================================

bool UsbPrinter::getIfaceInfo(int idx, uint8_t& cls, uint8_t& sub, uint8_t& proto) const {
  if (idx < 0 || idx >= (int)_ifaceCount) return false;
  cls   = _ifaces[idx].cls;
  sub   = _ifaces[idx].sub;
  proto = _ifaces[idx].proto;
  return true;
}

void UsbPrinter::setInStreamSuspended(bool on) {
  if (_inStreamSuspended == on) return;
  _inStreamSuspended = on;
  // ⚠️ 这里**绝不能**直接丢弃 _inXfer 指针：那是一条**在飞**的传输，
  //    IDF 要求 "The transfer must not be in-flight" 才能 free，
  //    强行置空会在驱动内部留下野指针，稍后 panic。
  //    正确做法：只设标志，由 USB task 在自己的上下文里让那条传输
  //    自然完成（_onInDone 负责释放），并且不再重启它。
  if (!on && bidirAllowed() && _epIn != 0) {
    _startInStream();
  }
}

// 通用控制传输：USB/IP 的 URB 控制传输直接调它。
// 关键点：**不能**像 _getStringDesc 那样写死 bmRequestType/wValue ——
// Linux 会发各种请求（GET_DESCRIPTOR / SET_CONFIGURATION / GET_STATUS …）。
int UsbPrinter::controlTransferRaw(uint8_t bmRequestType, uint8_t bRequest,
                                   uint16_t wValue, uint16_t wIndex,
                                   uint8_t* data, uint16_t wLength,
                                   uint32_t timeoutMs) {
  if (_deviceHandle == nullptr) return -1;
  if (_usbLock == nullptr) return -1;
  if (_ctrlHung) { _lastError = "EP0 已挂起，不再发控制请求"; return -1; }

  // ⚠️ 必须持有 _usbLock：usb_host_transfer_submit_control / usb_host_client_handle_events
  //    都不是线程安全的，而 USB task（core 1）也会并发调 client API。
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(5000)) != pdTRUE) {
    _lastError = "USB 总线忙，控制传输拿不到锁";
    markIppDiag("ct:lockfail");
    return -1;
  }

  const bool isIn = (bmRequestType & 0x80) != 0;
  size_t allocSize = sizeof(usb_setup_packet_t) + wLength;
  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(allocSize, 0, &t) != ESP_OK) {
    xSemaphoreGive(_usbLock);
    _lastError = "控制传输：分配失败";
    return -1;
  }
  if (!isIn && data != NULL && wLength > 0) {
    memcpy(t->data_buffer + sizeof(usb_setup_packet_t), data, wLength);
  }

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = 0;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context    = (void*)this;
  t->timeout_ms = timeoutMs;

  usb_setup_packet_t* sp = (usb_setup_packet_t*)t->data_buffer;
  sp->bmRequestType = bmRequestType;
  sp->bRequest      = bRequest;
  sp->wValue        = wValue;
  sp->wIndex        = wIndex;
  sp->wLength       = wLength;
  t->num_bytes      = sizeof(usb_setup_packet_t) + wLength;

  int rc = -1;
  bool done = false;
  int  got = 0;
  if (usb_host_transfer_submit_control(_clientHandle, t) == ESP_OK) {
    // ⚠️ requireCompleted=false：**只看传输是否完成，不看 status**。
    //    ESP-IDF 内部的控制传输（_usb_control_transfer）就是这么判的 ——
    //    DWC2 对 EP0 的短包/零长数据常报非 COMPLETED（STALL），
    //    但**数据是好的**。EP0 的 GET_DESCRIPTOR 正是这种情况：
    //    设备描述符只有 18 字节，而我们按惯例请求 64 字节 → 短包。
    //    之前用"status 必须 COMPLETED"来判，等于把好数据当成失败 ——
    //    这也是 HP LaserJet 的 Device ID 从来读不出来的真正原因。
    done = _awaitXfer(t, timeoutMs, false);
    got = (int)t->actual_num_bytes;
    if (got < 0) got = 0;
  } else {
    _lastError = "控制传输提交失败";
  }

  if (done && got > (int)sizeof(usb_setup_packet_t)) {
    int payload = got - (int)sizeof(usb_setup_packet_t);
    if (isIn && data != NULL) {
      memcpy(data, t->data_buffer + sizeof(usb_setup_packet_t), payload);
    }
    rc = payload;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
      _lastError = String("控制传输短包（收到 ") + String(payload) + " 字节）";
    }
  } else if (!done) {
    _lastError = "控制传输超时";
  } else {
    rc = 0;   // 无数据阶段（SET_CONFIGURATION 之类）也算成功
  }

  {
    // 诊断带上错误原文（不只是长度）—— 之前只看长度，猜了好几轮
    char dbg[48];
    String e = _lastError;
    e.replace("\r", " ");   // ⚠️ Arduino 的 replace() 是原地修改、返回 void
    e.replace("\n", " ");
    if (e.length() > 18) e = e.substring(0, 18);
    snprintf(dbg, sizeof(dbg), "ct2:%d,%d,%u|", rc, got, (unsigned)wLength);
    markIppDiag(dbg);
    markIppDiag(e.c_str());
  }
  _releaseXfer(t);
  xSemaphoreGive(_usbLock);
  return rc;
}

// 按 URB 精确做一次批量传输（不依赖常驻 IN 流）。
int UsbPrinter::bulkTransferRaw(uint8_t ep, uint8_t* data, uint16_t len, bool isIn,
                                uint32_t timeoutMs) {
  if (_deviceHandle == nullptr || _usbLock == nullptr) return -1;
  if (len == 0) return 0;
  if (_stallUntilMs != 0 && (long)(_stallUntilMs - millis()) > 0) {
    _lastError = "打印机无响应，保护中";
    return -5;
  }
  if (xSemaphoreTake(_usbLock, pdMS_TO_TICKS(5000)) != pdTRUE) {
    _lastError = "USB 总线忙，获取锁超时";
    markIppDiag("bulk:lockfail");
    return -2;
  }

  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(len, 0, &t) != ESP_OK) {
    xSemaphoreGive(_usbLock);
    _lastError = "批量传输：分配失败";
    return -1;
  }
  if (!isIn && data != NULL) memcpy(t->data_buffer, data, len);

  _xferDone = false;
  t->device_handle    = _deviceHandle;
  t->bEndpointAddress = ep;
  t->num_bytes        = len;
  t->timeout_ms       = timeoutMs;
  t->callback = [](usb_transfer_t* x) { ((UsbPrinter*)x->context)->_xferDone = true; };
  t->context = (void*)this;

  int rc = -1;
  if (usb_host_transfer_submit(t) == ESP_OK) {
    if (_awaitXfer(t, timeoutMs)) {
      int got = (int)t->actual_num_bytes;
      if (got < 0) got = 0;
      if (isIn && data != NULL && got > 0) {
        memcpy(data, t->data_buffer, got);
      }
      rc = got;
    } else {
      _lastError = "批量传输超时";
    }
  } else {
    _lastError = "批量传输提交失败";
  }
  _releaseBulkXfer(t);
  xSemaphoreGive(_usbLock);
  return rc;
}
