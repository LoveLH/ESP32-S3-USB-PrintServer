#include "crashlog.h"
#include "Arduino.h"

RTC_NOINIT_ATTR CrashLog g_crash;

void markStageQ(const char* s, int line) {
  g_crash.stageSeq++;
  g_crash.stageLine = (uint32_t)line;
  g_crash.uptimeAtStage = (uint32_t)(millis() / 1000);
  // ⚠️ 刻意**不写 stageStr**：stageStr 是"崩溃现场"，只由 markStage()（显式埋点）写。
  // 热路径改写自己的 hotStage，这样运行期崩溃时 stageStr 不会被立刻冲掉。
  size_t n = sizeof(g_crash.hotStage) - 1;
  strncpy(g_crash.hotStage, s, n);
  g_crash.hotStage[n] = 0;
}

void markBootStage(const char* s, int line) {
  // 只写 bootStage —— 刻意不碰 stageStr/stageLine，
  // 这样热路径的 markStageQ() 不会把启动阶段的现场冲掉。
  g_crash.bootStageLine = (uint32_t)line;
  size_t n = sizeof(g_crash.bootStage) - 1;
  strncpy(g_crash.bootStage, s, n);
  g_crash.bootStage[n] = 0;
}

void markStage(const char* s, int line) {
  g_crash.stageSeq++;
  g_crash.stageLine = (uint32_t)line;
  g_crash.uptimeAtStage = (uint32_t)(millis() / 1000);
  size_t n = sizeof(g_crash.stageStr) - 1;
  strncpy(g_crash.stageStr, s, n);
  g_crash.stageStr[n] = 0;
  Serial.printf("[STAGE] #%u %s (line %d, t=%us)\n",
                (unsigned)g_crash.stageSeq, s, line,
                (unsigned)g_crash.uptimeAtStage);
}

void markIppDiag(const char* s) {
  size_t n = sizeof(g_crash.ippDiag) - 1;
  strncpy(g_crash.ippDiag, s, n);
  g_crash.ippDiag[n] = 0;
}
