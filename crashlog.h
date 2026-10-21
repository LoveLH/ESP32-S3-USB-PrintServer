#pragma once
#include <stdint.h>
#include "esp_attr.h"   // RTC_NOINIT_ATTR

// 掉电/重启保留的崩溃日志（RTC slow memory，reset 不清零，仅断电清零）
struct CrashLog {
  uint32_t magic;            // 初始化标记，区分“从未初始化”的垃圾值
  uint32_t bootCount;        // 累计启动次数
  uint32_t rebootReason;     // esp_reset_reason()
  uint32_t stageSeq;         // 每次 markStage 自增，用来判断“到了第几步”
  uint32_t stageLine;        // 最后一步的代码行号
  uint32_t uptimeAtStage;    // 打标记时的 uptime（秒）
  char     stageStr[48];     // 最后一步的文本标记（**只由 markStage 写**）
  // 热路径（USB task 每 5ms 一次）的当前阶段。**刻意不覆盖 stageStr** ——
  // 否则运行期崩溃现场会被立刻冲掉，"上次崩在哪"永远查不出来。
  char     hotStage[48];
  // IPP 专用诊断（只有 IPP 任务写，任何其它路径都覆盖不到它）：
  // 记录 "阶段:数值"，比如 "found:1" "data:2048" "wr-1"。
  // stageStr 会被打印机重新枚举（markStage("ready")）覆盖，不适合做 IPP 诊断。
  char     ippDiag[48];
  // "上一次启动"卡死/崩溃前最后到达的 stage（不在 setup 里被覆盖，专门留给诊断读）
  uint32_t lastCrashLine;
  char     lastCrashStage[48];
  // 启动阶段专用字段（v2.7.0）：**只由 markBootStage() 写**，热路径的
  // markStageQ() 绝不碰它。存在的意义：固件启动到一半崩溃、bootloader 回退到
  // 旧分区后，stageStr 早被热路径的 "ev-idle" 覆盖，崩溃现场就丢了；
  // 而 bootStage 能把"卡在启动哪一步"原样留到下次启动，读 /api/debug 就能定位。
  char     bootStage[48];
  uint32_t bootStageLine;
};

extern RTC_NOINIT_ATTR CrashLog g_crash;

// 在初始化关键步骤调用，记录“当前进行到哪一步”
void markStage(const char* s, int line);

// 同上，但不打串口（用于每隔几毫秒就会跑到的热路径，避免 UART 阻塞主循环）
void markStageQ(const char* s, int line);

// 启动阶段埋点：只写 bootStage，**热路径绝不碰**，所以它能安全地保存
// "本次启动卡在哪一步"。setup 开头要把上一轮的 bootStage 搬到 lastCrashStage
// （见 ESP32S3_PrintServer_v2.ino 里的用法），这样固件启动崩溃后即使
// bootloader 回退到旧分区，/api/debug 依然能报出崩溃点。
void markBootStage(const char* s, int line);

// IPP 专用诊断（只有 IPP 任务写，不受其它路径覆盖）
void markIppDiag(const char* s);
