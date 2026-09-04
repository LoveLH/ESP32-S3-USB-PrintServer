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
  char     stageStr[48];     // 最后一步的文本标记（本轮启动，会被 "boot" 覆盖）
  // “上一次启动”卡死/崩溃前最后到达的 stage（不在 setup 里被覆盖，专门留给诊断读）
  uint32_t lastCrashLine;
  char     lastCrashStage[48];
};

extern RTC_NOINIT_ATTR CrashLog g_crash;

// 在初始化关键步骤调用，记录“当前进行到哪一步”
void markStage(const char* s, int line);

// 同上，但不打串口（用于每隔几毫秒就会跑到的热路径，避免 UART 阻塞主循环）
void markStageQ(const char* s, int line);
