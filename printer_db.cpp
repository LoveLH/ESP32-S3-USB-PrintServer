#include "printer_db.h"

// ============================================================
//  实测机型表
//
//  填表原则：**只写有实测依据的条目**。没有证据的机型不要瞎标 flag，
//  让它走 config.h 的全局默认值，比乱配置更安全。
//
//  字段：VID, PID, 兜底型号名, 行为特征位
// ============================================================
static const PrinterProfile PROFILES[] = {
  // ---- EPSON ------------------------------------------------------------
  // Stylus Photo 1390：对 ESP32 USB Host 的所有控制请求长时间 NAK，
  // 而本 IDF 版本 usb_transfer_t::timeout_ms 无效、EP0 又无取消接口，
  // 发了就是永久悬挂。必须跳过一切控制传输，型号靠本表兜底。
  // ESC/P 系 tolerates ESC @，作业前复位能清掉上个作业的残留状态。
  {0x04B8, 0x0007, "Epson Stylus Photo 1390", Q_NO_CTRL | Q_WANT_ESC_RESET | Q_TAIL_FF},
  {0x04B8, 0x0005, "Epson Stylus Photo 1390", Q_NO_CTRL | Q_WANT_ESC_RESET | Q_TAIL_FF},

  // ---- HP ---------------------------------------------------------------
  // 03F0:622A（装 HP LaserJet MFP M129-M134 PCLm-S 驱动）实测：
  // - 作业开头塞 ESC @ 会打乱 @PJL 解析，收下全部数据却不出纸 → Q_NO_ESC_RESET
  // - ⚠️ 作业结束**绝不能补 FF**（Q_TAIL_NONE）。v2.3.2 为了解决"最后一页压在缓冲里"
  //   加过 Q_WANT_FORMFEED，但 PCLm 流的尾部自带 `%%EOF` 收尾，补的那个裸 FF 等于
  //   在作业结束之后又开了一页却永远不给数据 —— 打印机常驻"正在打印中"、
  //   后续作业全进不来，只能人工断电。这个坑 2.5.6 才定位到，别再改回去。
  {0x03F0, 0x622A, "HP DeskJet / LaserJet",
   Q_NO_ESC_RESET | Q_TAIL_NONE | Q_NO_FAIL_RECOVER},

  // 惠普厂商级行为。Linux ipp-usb 源码里明确记录：部分惠普 MFP 的
  // IPP-over-USB 走的是**非标准**接口组合 255/9/1（LaserJet MFP M426fdn、
  // ColorLaserJet MFP M278-M281 等）。如果固件只认标准打印机类 0x07，
  // 这些货真价实的打印机就会被当场拒掉——“不是打印机”。
  // ⚠️ Q_NO_BIDIR：惠普在 v2.4.0 引入"常驻 Bulk IN 回读"后开始整族拒收数据
  //    （03F0:622A 实测：每次首发即 `USB 传输失败 status=0`，30 秒内收不到任何回调）。
  //    现象是打印任务没有一个字节能进打印机。怀疑这类主机型/GDI 机型固件缓冲很小，
  //    持续挂着的 IN 请求会把它拖住。先整族关掉回读保住"能打"，后续逐机型甄别。
  {0x03F0, PID_ANY, nullptr, Q_NO_ESC_RESET | Q_VENDOR_IFACE | Q_NO_BIDIR},

  // ---- Brother ----------------------------------------------------------
  {0x04F9, PID_ANY, nullptr, 0},

  // ---- Canon ------------------------------------------------------------
  // 佳能多数机型响应 Device ID，无需特殊处理；厂商级兜底仅用于
  // 允许在缺少标准打印机类接口时尝试厂商私有批量接口。
  {0x04A9, PID_ANY, nullptr, Q_VENDOR_IFACE},

  // ---- Samsung / Kyocera / Xerox / Lexmark / OKI / Dell / Ricoh ---------
  {0x04E8, PID_ANY, nullptr, 0},
  {0x0482, PID_ANY, nullptr, 0},
  {0x0924, PID_ANY, nullptr, 0},
  {0x043D, PID_ANY, nullptr, 0},
  {0x0390, PID_ANY, nullptr, 0},
  {0x413C, PID_ANY, nullptr, 0},
  {0x05CA, PID_ANY, nullptr, 0},

  // ---- 热敏票据 / 标签机 -------------------------------------------------
  // 票据机多数是单向或仅回 ACK，回读意义不大且部分固件会持续吐状态包。
  {0x0A5F, PID_ANY, nullptr, 0},   // Zebra
  {0x1203, PID_ANY, nullptr, 0},   // TSC
  {0x0519, PID_ANY, nullptr, 0},   // Star
  {0x1FC9, PID_ANY, nullptr, 0},   // Gprinter
  {0x1A86, PID_ANY, nullptr, 0},   // QinHeng
};
static const size_t PROFILES_N = sizeof(PROFILES) / sizeof(PROFILES[0]);

// 已知打印机厂商（Device ID 读不到时的制造商兜底，也是"能否接管私有接口"的凭据）
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
static const size_t VID_TABLE_N = sizeof(VID_TABLE) / sizeof(VID_TABLE[0]);

// ---------------------------------------------------------------------------
bool printerFlags(uint16_t vid, uint16_t pid, uint16_t* flagsOut) {
  uint16_t f = 0;
  bool found = false;
  for (size_t i = 0; i < PROFILES_N; i++) {
    if (PROFILES[i].vid != vid) continue;
    if (PROFILES[i].pid == pid || PROFILES[i].pid == PID_ANY) {
      f |= PROFILES[i].flags;
      found = true;
    }
  }
  if (flagsOut) *flagsOut = found ? f : 0;
  return found;
}

const char* printerProfileName(uint16_t vid, uint16_t pid) {
  for (size_t i = 0; i < PROFILES_N; i++) {
    if (PROFILES[i].vid == vid && PROFILES[i].pid == pid && PROFILES[i].name) {
      return PROFILES[i].name;
    }
  }
  return nullptr;
}

bool printerTestFlag(uint16_t vid, uint16_t pid, uint16_t flag, bool defaultValue) {
  uint16_t f = 0;
  if (!printerFlags(vid, pid, &f)) return defaultValue;
  return (f & flag) != 0;
}

bool printerTriFlag(uint16_t vid, uint16_t pid,
                    uint16_t onFlag, uint16_t offFlag, bool defaultValue) {
  uint16_t f = 0;
  if (!printerFlags(vid, pid, &f)) return defaultValue;
  if (f & onFlag)  return true;
  if (f & offFlag) return false;
  return defaultValue;
}

bool isKnownPrinterVendor(uint16_t vid) {
  for (size_t i = 0; i < VID_TABLE_N; i++)
    if (VID_TABLE[i].vid == vid) return true;
  return false;
}

const char* guessVendorName(uint16_t vid) {
  for (size_t i = 0; i < VID_TABLE_N; i++)
    if (VID_TABLE[i].vid == vid) return VID_TABLE[i].name;
  return nullptr;
}
