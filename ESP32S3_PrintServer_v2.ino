// ============================================================
//  ESP32-S3 WiFi 打印服务器  v2.0（通用版）
//
//  硬件：ESP32-S3（实测 4MB Flash / 2MB PSRAM，XMC Flash + AP_3v3 PSRAM，非 N16R8）
//  原理：ESP32-S3 USB Host → 打印机
//        WiFi Raw Socket (:9100) ← Windows / macOS
//        mDNS 自动发现 + Web 管理界面 + OTA 升级
//
//  烧录配置（Arduino IDE / arduino-cli）：
//    Board: ESP32S3 Dev Module
//    USB Mode: default (TinyUSB，释放 USB-OTG 给打印机 Host)
//    CDC On Boot: default (Disabled)   ← 必须关，否则 GPIO0 被占、且无法做 USB Host
//    PSRAM: enabled (QSPI)             ← 实测 2MB，选 enabled 即可
//    Flash Size: 4MB
//    Partition Scheme: min_spiffs (1.9MB APP + OTA + 128KB SPIFFS)
//    Upload Speed: 921600
//  注意：烧录用 IDE 内嵌 arduino-cli；烧录后务必关闭 Arduino IDE 再给板子重新上电，
//        否则 IDE 占用 COM 口的 DTR/RTS 会把 GPIO0 拉低，板子卡在 DOWNLOAD 模式起不来。
//
//  适用：任何暴露「标准 USB 打印机类(07/01)」接口的打印机。
//        页面描述语言由电脑端驱动生成，ESP32 只做透明转发，
//        所以 GDI/主机型打印机同样可用。
// ============================================================

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <Preferences.h>   // 由 wifi_manager.h 间接使用；主文件显式包含以确保 arduino-cli 将其加入链接
#include <WebServer.h>     // 由 web_ui.h 间接使用；显式包含以确保 arduino-cli 发现
#include <Update.h>        // 由 web_ui.h / ArduinoOTA 间接使用；显式包含以确保发现

#include "config.h"
#include "usb_printer.h"
#include "wifi_manager.h"
#include "print_server.h"
#include "web_ui.h"
#include "crashlog.h"
#include "ipp_server.h"
#include "airprint_mdns.h"

// --- 全局对象 ---
UsbPrinter   usbPrinter;
WiFiManager  wifiMgr;
PrintServer  printSvr(usbPrinter);
WebUI        webUI(wifiMgr, usbPrinter, printSvr);
IppServer    ippSvr(usbPrinter, printSvr);

// --- USB Host 后台任务 ---
TaskHandle_t usbTaskHandle = NULL;
// --- 打印服务后台任务（v2.5.0：不再跑在 loop() 里）---

// ⚠️ v2.6.6：OTA 期间的总开关。
// 写 flash（Update.begin 擦除整个 app 分区 + Update.write 逐块写入）期间，
// flash cache 会被短暂禁用，此时**任何从 flash 取指令的代码执行都会 panic**
// （Cache disabled but cached memory region accessed）。
// USB 驱动的代码就在 flash 上 —— 只要 USB 任务在这一刻跑一下就是一次 panic，
// 表现为：固件上传完成 → 设备立刻重启 → 分区写了一半 → bootloader 回退到
// 旧分区 → 版本号没变、OTA 永远失败。
//
// 所以 OTA 期间必须做到三件事：
//   ① 挂起 USB Host 任务（不碰 USB 控制器）
//   ② 断开打印机设备（让 IDF 内部的 usbHostTask 也无事可做）
//   ③ 打印任务**不要挂起**（它订阅了 WDT，挂起就无法喂狗 → 60 秒后 WDT panic），
//      改成"暂停业务 + 主动取消 WDT 订阅"。
volatile bool g_otaBusy = false;

TaskHandle_t printTaskHandle = NULL;

// --- 服务启动标记（配网后才连上 WiFi 的场景需要补启动）---
static bool mdnsStarted = false;
static bool otaStarted  = false;
static bool wdtArmed    = false;
static uint32_t lastLinkEpoch = 0;

void usbHostTask(void* param) {
  Serial.println("[Task] USB Host 任务已启动");
  while (true) {
    usbPrinter.task();
    vTaskDelay(pdMS_TO_TICKS(1));   // task() 内部已有 5ms 节奏，这里只做让出
  }
}

// 打印服务独立任务 + 独立核心。
// 以前它跑在 loop() 里：打印机拒收数据时一次 USB 批量传输会阻塞 30 秒，
// 这段时间整个 loop() 停摆 —— 网页打不开、API 无响应、OTA 推不进去，
// 而 Windows 打印后台还在不停重试，板子等于被永久锁死。
// 现在就算 USB 侧卡住，管理界面依旧可用，用户能自己看到状态并处理。
void printTask(void* param) {
  Serial.println("[Task] 打印服务任务已启动（core " + String(PRINT_TASK_CORE) + "）");
  bool subscribed = false;
  while (true) {
    // ⚠️ v2.6.6：OTA 期间**不能挂起本任务**。
    // 本任务订阅了 WDT，挂起后就没人喂狗了 —— 写 flash 往往要几十秒，
    // 超过 WDT_TIMEOUT_SEC(60s) 必然 WDT panic，OTA 一样失败。
    // 正确做法：OTA 期间主动**取消自己的 WDT 订阅**、暂停业务，等 OTA 结束再补回来。
    if (g_otaBusy) {
      if (subscribed) {
        esp_task_wdt_delete(NULL);
        subscribed = false;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // 看门狗可能还没初始化（本任务在 setup 里创建），成功了才算订阅上
    if (!subscribed) {
      if (esp_task_wdt_add(NULL) == ESP_OK) subscribed = true;
    }
    if (subscribed) esp_task_wdt_reset();

    printSvr.task();
    vTaskDelay(pdMS_TO_TICKS(PRINT_TASK_DELAY_MS));
  }
}

// --- IPP / AirPrint 服务任务（v2.7.0）---
// 独立 TCP 任务监听 631 端口：自己解析 HTTP + IPP（Arduino WebServer 无法
// 区分 IPP 的"属性段"和"文件数据段"，也没法把几十 MB 的作业流式转出去）。
void ippTask(void* param) {
  Serial.println("[Task] IPP/AirPrint 服务任务已启动");
  while (true) {
    ippSvr.task();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// --- mDNS ---
void setupMDNS() {
  if (mdnsStarted) return;

  if (!MDNS.begin(MDNS_HOSTNAME)) {
    Serial.println("[mDNS] 启动失败");
    return;
  }

  // 打印服务（Raw Socket / AppSocket / JetDirect）
  MDNS.addService("pdl-datastream", "tcp", RAW_SOCKET_PORT);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "pdl", "application/octet-stream");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "ty", PRINTER_NAME);

  // 通用打印机服务，方便部分系统发现
  MDNS.addService("printer", "tcp", RAW_SOCKET_PORT);
  MDNS.addServiceTxt("printer", "tcp", "rp", "raw");
  MDNS.addServiceTxt("printer", "tcp", "note", "ESP32-S3 USB print server");

  // 管理界面
  MDNS.addService("http", "tcp", WEB_SERVER_PORT);

  // --- AirPrint / IPP 免驱发现（v2.7.0）---
  // 必须用 IDF 原生 API 挂 "_universal" 子类型，Arduino 的 MDns 封装做不到。
  // 没有这个子类型，iOS/macOS 会把它当普通 IPP 打印机，客户端还得装驱动。
  {
    char adminUri[96];
    snprintf(adminUri, sizeof(adminUri), "http://%s.local:%d", MDNS_HOSTNAME,
             WEB_SERVER_PORT);
    const char* model = "ESP32-USB-PrintServer";
    markBootStage("mdns-airprint", __LINE__);
    if (AirPrintMdns::advertise(MDNS_HOSTNAME, ippSvr.getPort(),
                               PRINTER_NAME, model, adminUri, WEB_SERVER_PORT)) {
      Serial.printf("[mDNS]   AirPrint: ipp://%s.local:%d/ipp/print\n",
                    MDNS_HOSTNAME, ippSvr.getPort());
    }
    markBootStage("mdns-done", __LINE__);
  }

  mdnsStarted = true;
  Serial.printf("[mDNS] 已注册 %s.local\n", MDNS_HOSTNAME);
  Serial.printf("[mDNS]   打印: socket://%s.local:%d\n", MDNS_HOSTNAME, RAW_SOCKET_PORT);
  Serial.printf("[mDNS]   管理: http://%s.local\n", MDNS_HOSTNAME);
}

// --- OTA ---
void setupOTA() {
  if (otaStarted) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPort(OTA_PORT);

  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "固件" : "文件系统";
    Serial.printf("[OTA] 开始更新 %s\n", type.c_str());
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] 更新完成，即将重启");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int last = -1;
    int pct = (progress * 100) / total;
    if (pct != last && pct % 10 == 0) {
      last = pct;
      Serial.printf("[OTA] 进度: %d%%\n", pct);
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] 错误 [%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("认证失败");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("启动失败（分区表不支持 OTA？）");
    else if (error == OTA_CONNECT_ERROR) Serial.println("连接失败");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("接收失败");
    else if (error == OTA_END_ERROR)     Serial.println("结束失败");
  });

  ArduinoOTA.begin();
  otaStarted = true;
  Serial.printf("[OTA] 已启用，端口 %d，主机名 %s\n", OTA_PORT, OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) == 0) {
    Serial.println("[OTA] 警告：未设置密码，局域网内任何人都能刷机");
  }
}

// --- LED ---
void setLED(int mode) {
  switch (mode) {
    case LED_WIFI_OK:  digitalWrite(LED_PIN, HIGH); break;
    case LED_ERROR:    digitalWrite(LED_PIN, LOW);  break;
    default: break;   // 打印中闪烁在 loop 里处理
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // 初始化 RTC 崩溃日志（reset 保留，断电清零）
  if (g_crash.magic != 0xC0DEBEEF) {
    memset(&g_crash, 0, sizeof(g_crash));
    g_crash.magic = 0xC0DEBEEF;
  }
  g_crash.bootCount++;
  g_crash.rebootReason = 0;   // esp_reset_reason 头在 Arduino 环境不易包含，暂置 0

  // 把“上一次启动”卡死/崩溃前最后到达的 stage 存到 lastCrashStage。
  // ⚠️ v2.7.0：改成从 **bootStage** 搬，而不是 stageStr。
  //    stageStr 会被热路径的 markStageQ()（USB task 每 5ms 一次）覆盖成 "ev-idle"，
  //    固件启动到一半崩溃时真正的现场早就被冲掉了；bootStage 只由
  //    markBootStage() 写，能原样保存"卡在启动的哪一步"。
  if (g_crash.magic == 0xC0DEBEEF) {        // 非首次启动（首次已被上面 memset 清零）
    if (g_crash.bootStage[0] != 0) {
      strncpy(g_crash.lastCrashStage, g_crash.bootStage, sizeof(g_crash.lastCrashStage) - 1);
      g_crash.lastCrashStage[sizeof(g_crash.lastCrashStage) - 1] = 0;
      g_crash.lastCrashLine = g_crash.bootStageLine;
    } else {
      g_crash.lastCrashStage[0] = 0;
      g_crash.lastCrashLine = 0;
    }
  }
  markStage("boot", __LINE__);
  markBootStage("boot", __LINE__);

  Serial.println();
  Serial.println("========================================");
  Serial.printf("  ESP32-S3 WiFi 打印服务器 v%s\n", FW_VERSION);
  Serial.println("  打印协议: Raw Socket (AppSocket/JetDirect)");
  Serial.println("========================================");
  Serial.println();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.printf("[SYS] Free Heap : %u KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("[SYS] Free PSRAM: %u KB\n", ESP.getFreePsram() / 1024);
  Serial.printf("[SYS] Flash     : %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.println();

  // 1. USB Host
  Serial.println("=== 初始化 USB Host ===");
  if (usbPrinter.begin()) {
    xTaskCreatePinnedToCore(
      usbHostTask, "usb_host", USB_HOST_TASK_STACK_SIZE, NULL,
      USB_HOST_TASK_PRIORITY, &usbTaskHandle, 1
    );
  } else {
    Serial.println("[USB] 初始化失败，请检查接线");
  }
  Serial.println();

  // 2. WiFi
  Serial.println("=== 初始化 WiFi ===");
  wifiMgr.begin();
  Serial.println();

  // 3. 网络服务（开机就已连上 WiFi 的情况）
  if (wifiMgr.isConnected()) {
    setupMDNS();
    setupOTA();
  }

  // 4. 打印服务器
  Serial.println("=== 初始化打印服务器 ===");
  printSvr.begin();
  Serial.println();

  // 5. Web 管理界面
  Serial.println("=== 初始化 Web 管理界面 ===");
  webUI.begin();
  Serial.println();

  // 6. 看门狗
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms      = (uint32_t)WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask  = 0,
    .trigger_panic   = true,
  };
  esp_err_t e = esp_task_wdt_init(&wdtConfig);
  if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_add(NULL);
    wdtArmed = true;
  } else {
    Serial.printf("[SYS] 看门狗初始化失败: %s（不影响使用）\n", esp_err_to_name(e));
  }

  Serial.println("========================================");
  Serial.println("  系统就绪");
  if (wifiMgr.isConnected()) {
    Serial.printf("  打印地址: socket://%s:%d\n", wifiMgr.getIP().c_str(), RAW_SOCKET_PORT);
    Serial.printf("  管理界面: http://%s.local  (http://%s)\n",
                  MDNS_HOSTNAME, wifiMgr.getIP().c_str());
  } else {
    Serial.printf("  请连接 WiFi '%s' 后访问 http://%s\n", AP_SSID, wifiMgr.getIP().c_str());
  }
  Serial.println("========================================");

  setLED(wifiMgr.isConnected() ? LED_WIFI_OK : LED_ERROR);

  // 7. 打印服务独立任务（必须在 WDT 初始化之后，任务里会把自己挂上看门狗）
  lastLinkEpoch = wifiMgr.getLinkEpoch();
  xTaskCreatePinnedToCore(
    printTask, "print_svr", PRINT_TASK_STACK_SIZE, NULL,
    PRINT_TASK_PRIORITY, &printTaskHandle, PRINT_TASK_CORE
  );

  Serial.println("[SYS] 打印任务已启动 core " + String(PRINT_TASK_CORE));

  // 8. IPP / AirPrint 服务（v2.7.0）—— 独立 631 端口，手机免驱打印用
  markBootStage("ipp-listen", __LINE__);
  if (ippSvr.begin()) {
    markBootStage("ipp-task", __LINE__);
    // 栈给足：IPP 的 HTTP 解析在栈上有 1KB + 2KB 两个缓冲，
    // 再加 lwIP 调用链，8KB 偏紧（曾疑似因此启动即崩）。
    xTaskCreatePinnedToCore(
      ippTask, "ipp_svr", 16384, NULL, 2, NULL, 0
    );
    Serial.printf("[SYS] IPP/AirPrint 服务已启动 :%u\n", ippSvr.getPort());
  }
  markBootStage("boot-done", __LINE__);
}

// ============================================================
void loop() {
  if (wdtArmed) esp_task_wdt_reset();

  wifiMgr.task();

  // 通过网页配网后 WiFi 才连上——这里补启动 mDNS 和 OTA
  if (wifiMgr.isConnected() && wifiMgr.getMode() == WSTATE_STATION) {
    if (!mdnsStarted) setupMDNS();
    if (!otaStarted)  setupOTA();
  }

  // 打印服务已独立成任务，打印过程可能阻塞，绝不能再放回 loop()
  webUI.task();

  if (otaStarted) ArduinoOTA.handle();

  // WiFi 重新连上后，mDNS 和 :9100 监听有时会跟着失效 —— 重建一次。
  // 典型症状是"能 ping 通板子，但电脑上的打印机显示离线/发不出去"。
  if (wifiMgr.getLinkEpoch() != lastLinkEpoch) {
    uint32_t prev = lastLinkEpoch;
    lastLinkEpoch = wifiMgr.getLinkEpoch();
    if (prev != 0) {                       // 0 = 还没跑过 setup 里的初始化
      Serial.println("[SYS] 检测到 WiFi 链路重建，正在恢复 mDNS 与打印端口…");
      MDNS.end();
      mdnsStarted = false;
      setupMDNS();
      printSvr.onNetworkUp();
      setLED(LED_WIFI_OK);
    }
  }

  // 打印中 LED 闪烁
  static unsigned long lastBlink = 0;
  if (printSvr.getCurrentJob().active) {
    if (millis() - lastBlink > 200) {
      lastBlink = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  }

  // 诊断心跳：每 3 秒打印一次状态（确认固件活着、未崩溃重启）
  static unsigned long lastHB = 0;
  if (millis() - lastHB > 3000) {
    lastHB = millis();
    Serial.printf("[HB] uptime=%lus wifi=%s(%ddBm) usb=%s heap=%uKB job=%s\r\n",
      millis() / 1000,
      wifiMgr.isConnected() ? "STA" : "AP/none",
      wifiMgr.getRSSI(),
      usbPrinter.isConnected() ? "yes" : "no",
      ESP.getFreeHeap() / 1024,
      printSvr.getCurrentJob().active ? "busy" : "idle");
  }

  // 内存自愈：长时间低堆 + 当前没在打印 → 自动重启。
  // 碎片化到一定程度后偶发的分配失败会变成永久故障，重启成本远低于上门服务。
#if HEAP_GUARD_ENABLE
  static unsigned long lowHeapSince = 0;
  if (ESP.getFreeHeap() < (size_t)HEAP_GUARD_MIN_KB * 1024) {
    if (lowHeapSince == 0) {
      lowHeapSince = millis();
      Serial.printf("[SYS] 警告：可用内存低于 %uKB（%u 字节）\n",
                    (unsigned)HEAP_GUARD_MIN_KB, ESP.getFreeHeap());
    } else if (millis() - lowHeapSince > (unsigned long)HEAP_GUARD_HOLD_MS &&
               !printSvr.getCurrentJob().active) {
      Serial.printf("[SYS] 内存持续过低超过 %lus，自动重启以避免耗尽卡死\n",
                    (unsigned long)(HEAP_GUARD_HOLD_MS / 1000));
      delay(200);
      ESP.restart();
    }
  } else {
    lowHeapSince = 0;
  }
#endif

  delay(1);
}
