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

// --- 全局对象 ---
UsbPrinter   usbPrinter;
WiFiManager  wifiMgr;
PrintServer  printSvr(usbPrinter);
WebUI        webUI(wifiMgr, usbPrinter, printSvr);

// --- USB Host 后台任务 ---
TaskHandle_t usbTaskHandle = NULL;

// --- 服务启动标记（配网后才连上 WiFi 的场景需要补启动）---
static bool mdnsStarted = false;
static bool otaStarted  = false;
static bool wdtArmed    = false;

void usbHostTask(void* param) {
  Serial.println("[Task] USB Host 任务已启动");
  while (true) {
    usbPrinter.task();
    vTaskDelay(pdMS_TO_TICKS(1));   // task() 内部已有 5ms 节奏，这里只做让出
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

  // 把“上一次启动”卡死/崩溃前最后到达的 stage 存到 lastCrashStage（setup 里的
  // markStage("boot") 会覆盖 stageStr，所以崩溃点必须先搬到这里才能被 /api/debug 读到）
  if (g_crash.magic == 0xC0DEBEEF) {        // 非首次启动（首次已被上面 memset 清零）
    if (g_crash.stageStr[0] != 0 && strncmp(g_crash.stageStr, "boot", 4) != 0) {
      strncpy(g_crash.lastCrashStage, g_crash.stageStr, sizeof(g_crash.lastCrashStage) - 1);
      g_crash.lastCrashStage[sizeof(g_crash.lastCrashStage) - 1] = 0;
      g_crash.lastCrashLine = g_crash.stageLine;
    } else {
      g_crash.lastCrashStage[0] = 0;
      g_crash.lastCrashLine = 0;
    }
  }
  markStage("boot", __LINE__);

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

  printSvr.task();
  webUI.task();

  if (otaStarted) ArduinoOTA.handle();

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
    Serial.printf("[HB] uptime=%lus wifi=%s usb=%s heap=%uKB\r\n",
      millis() / 1000,
      wifiMgr.isConnected() ? "STA" : "AP/none",
      usbPrinter.isConnected() ? "yes" : "no",
      ESP.getFreeHeap() / 1024);
  }

  delay(1);
}
