#include "crashlog.h"
#include "Arduino.h"

RTC_NOINIT_ATTR CrashLog g_crash;

void markStageQ(const char* s, int line) {
  g_crash.stageSeq++;
  g_crash.stageLine = (uint32_t)line;
  g_crash.uptimeAtStage = (uint32_t)(millis() / 1000);
  size_t n = sizeof(g_crash.stageStr) - 1;
  strncpy(g_crash.stageStr, s, n);
  g_crash.stageStr[n] = 0;
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
