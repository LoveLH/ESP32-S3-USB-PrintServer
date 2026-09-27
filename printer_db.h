#ifndef PRINTER_DB_H
#define PRINTER_DB_H

// ============================================================
//  机型行为特征库（商业化通用适配的核心）
//
//  为什么需要它：
//    固件对打印数据是「透明转发」，不需要理解任何页面描述语言，
//    所以 PCL / PS / ESC-POS / GDI(主机型) 天然都能打。真正决定
//    "某一台打印机能不能用、好不好用"的从来不是打印语言，而是它在
//    USB 层的一些坏脾气，比如：
//      · 对全部控制传输长时间 NAK（爱普生 1390）→ 不能发 Device ID
//      · 作业开头被塞 ESC @ 就卡住不出纸（HP 走 @PJL 流）
//      · 最后一页压在缓冲里，必须补 FF 才肯走纸（多数 HP DeskJet）
//      · 暴露的根本不是标准打印机类接口，而是厂商私有接口（部分惠普 MFP）
//
//  这些坏脾气无法从描述符里推断，只能一台台实测积累 —— 这就是本表，
//  也是这套方案最难被抄走的部分：代码可以开源，真机调校数据库不行。
//
//  用法：查不到就回退到 config.h 里的全局默认值。
//  Q_WANT_* 表示"即便全局默认关闭也要开"，Q_NO_* 表示"即便全局默认开启也要关"，
//  两者都没写就用全局默认。
// ============================================================

#include <stdint.h>
#include <stddef.h>

// --- 行为特征位 ---
#define Q_WANT_ESC_RESET   0x0001u  // 作业开头要发 ESC @（多数 ESC/P 爱普生、针式机）
#define Q_NO_ESC_RESET     0x0002u  // 千万别发 ESC @（HP 等 @PJL 流机型）
#define Q_WANT_FORMFEED    0x0004u  // 作业结束补 FF 才出纸
#define Q_NO_FORMFEED      0x0008u  // 不要补 FF（有的机 FF 会空走一张纸）
#define Q_NO_CTRL          0x0010u  // 完全不应答控制传输：禁发 Device ID / 字符串描述符 / 端口状态
#define Q_NO_ZLP           0x0020u  // 不要补零长度包（极少数老机会把 ZLP 当成错误）
#define Q_VENDOR_IFACE     0x0040u  // 允许接管厂商私有(0xFF)批量接口
#define Q_NO_BIDIR         0x0080u  // 不要回读 bulk IN（个别机会持续吐无意义的回读数据）
#define Q_IPP_OVER_USB     0x0100u  // 已知 IPP-over-USB 设备（描述符哪怕不是标准 7/1/4）
#define Q_NO_FAIL_RECOVER  0x0200u  // 别尝试故障自愈（极少数机型被收尾序列打死，实锤后再关）

// --- 作业收尾策略（v2.5.7）---------------------------------------------
// 作业结束后"要不要补发点什么"因打印语言而异，不能一刀切：
//   自描述格式（PCLm/PDF、PostScript、PJL+PCL）—— 驱动自己收尾，补任何东西都是
//     在作业结束之后又开一个作业，打印机就无限等下一页（HP M129-M134 实测：
//     补一个裸 FF 就常驻"正在打印中"，必须断电才能恢复）。
//   流式格式（ESC/P、ESC/POS、老式 GDI 裸流）—— 往往没有收尾标记，必须补 FF 才出纸。
// 两者都没写 = Q_TAIL_AUTO：看作业末尾字节自动判断（安全兜底，推荐）。
#define Q_TAIL_NONE        0x0400u  // 作业结束**什么都不补**（PCLm/PDF、PS、PJL 系）
#define Q_TAIL_FF          0x0800u  // 作业结束**无条件补 FF**（ESC/P、ESC/POS）

// 主机型（PCLm / PCL3GUI / GDI 等）不认纯文本：固件自带的文本测试页发过去，
// 它收下字节却既不渲染也不结束作业，面板就一直卡在"正在打印"、必须人工断电。
// 这类机器必须禁用文本测试页（网页上直接不给点），只能用真实 Windows 驱动作业验证链路。
#define Q_NO_TEXT_TESTPAGE 0x1000u  // 禁用固件自带的纯文本测试页

#define PID_ANY            0xFFFFu  // PID 通配：这一行对该厂商所有产品生效

struct PrinterProfile {
  uint16_t    vid;
  uint16_t    pid;
  const char* name;    // 精确匹配时的兜底型号名；通配行为 nullptr
  uint16_t    flags;
};

// 取某 VID:PID 的合并特征位（精确项 | 厂商通配项）。
// 返回 true 表示"查到了资料"，false 表示"资料里没有这台机器"。
bool      printerFlags(uint16_t vid, uint16_t pid, uint16_t* flagsOut);

// 精确匹配的兜底型号名，没有则 nullptr
const char* printerProfileName(uint16_t vid, uint16_t pid);

// 单一行为位查询：查无此机时返回 defaultValue
bool      printerTestFlag(uint16_t vid, uint16_t pid, uint16_t flag, bool defaultValue);

// 三态查询：Q_WANT_* 强制开、Q_NO_* 强制关、两者都没写则用 defaultValue
bool      printerTriFlag(uint16_t vid, uint16_t pid,
                         uint16_t onFlag, uint16_t offFlag, bool defaultValue);

// 是不是已知打印机厂商（决定要不要冒险接管厂商私有接口）
bool      isKnownPrinterVendor(uint16_t vid);
const char* guessVendorName(uint16_t vid);

#endif // PRINTER_DB_H
