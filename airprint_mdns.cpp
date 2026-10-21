#include "airprint_mdns.h"
// ESP-IDF 原生 mDNS API —— Arduino 的 MDns 封装不暴露子类型，
// 而 AirPrint 免驱发现恰恰要求 "_ipp._tcp" 带 "_universal" 子类型。
#include <mdns.h>

bool AirPrintMdns::advertise(const char* hostName, uint16_t ippPort,
                             const char* printerName, const char* model,
                             const char* adminUri, uint16_t adminPort) {
  // ---- 1) _ipp._tcp 服务（IPP-over-HTTP）----
  char rp[64];
  snprintf(rp, sizeof(rp), "/ipp/print");
  char ty[96];
  snprintf(ty, sizeof(ty), "%s", model ? model : "USB-Printer");

  mdns_txt_item_t ippTxt[] = {
    { .key = "rp",  .value = rp },
    { .key = "ty",  .value = ty },
    { .key = "pri", .value = "50" },
    { .key = "usb-MDL", .value = model ? model : "USB-Printer" },
    { .key = "usb-EF",  .value = "URF" },       // 声明支持 URF
    { .key = "usb-pdl", .value = "application/pdf" },
    { .key = "product",  .value = (const char*)"ESP32-S3 Print Server" },
    { .key = "admin-uri", .value = adminUri ? adminUri : "" },
  };
  esp_err_t err = mdns_service_add(hostName, "_ipp", "_tcp", ippPort,
                                   ippTxt, sizeof(ippTxt) / sizeof(ippTxt[0]));
  if (err != ESP_OK) {
    Serial.printf("[mDNS] _ipp._tcp 注册失败: %s\n", esp_err_to_name(err));
  } else {
    Serial.printf("[mDNS] _ipp._tcp 已注册 :%u  rp=%s\n", (unsigned)ippPort, rp);
  }

  // ---- 2) _universal 子类型（AirPrint 免驱发现的关键）----
  // iOS/macOS 看到 _ipp._tcp 会先查这个子类型：没有它就只当普通 IPP 打印机，
  // 客户端还得装驱动。penguin 项目（ESP32 上做 AirPrint）也踩过这个坑：
  // Arduino 的 MDns 封装不暴露子类型，必须调 IDF 原生 API。
  err = mdns_service_subtype_add_for_host(hostName, "_ipp", "_tcp", NULL,
                                         "_universal");
  if (err != ESP_OK) {
    Serial.printf("[mDNS] _universal 子类型注册失败: %s\n", esp_err_to_name(err));
  } else {
    Serial.println("[mDNS] _universal 子类型已注册（AirPrint 免驱发现）");
  }

  // ---- 3) 旧的 AirPrint 打印服务类型（兼容老系统）----
  mdns_txt_item_t pdlTxt[] = {
    { .key = "rp",  .value = rp },
    { .key = "ty",  .value = ty },
    { .key = "usb-MDL", .value = model ? model : "USB-Printer" },
    { .key = "note", .value = (const char*)"ESP32 Print Server" },
  };
  err = mdns_service_add(hostName, "_pdl-datastream", "_tcp", ippPort,
                         pdlTxt, sizeof(pdlTxt) / sizeof(pdlTxt[0]));
  if (err == ESP_OK) {
    Serial.printf("[mDNS] _pdl-datastream._tcp 已注册 :%u\n", (unsigned)ippPort);
  }

  return err == ESP_OK;
}
