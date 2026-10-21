#ifndef AIRPRINT_MDNS_H
#define AIRPRINT_MDNS_H

// ============================================================
//  AirPrint / IPP 的 mDNS 广告（v2.7.0）
//
//  为什么必须自己写：AirPrint 的免驱发现有两个硬条件，
//  Arduino 的 MDns 封装**两个都给不了**：
//
//   1) 服务类型要是 "_ipp._tcp"（IPP）或 "_pdl-datastream._tcp"（旧 AirPrint）
//   2) **必须额外挂 "_universal" 子类型** —— iOS/macOS 看到 _ipp._tcp 时
//      会先查有没有 _universal，有才认为是"免驱 AirPrint 打印机"，
//      没有就退回去只当普通 IPP 打印机（还是要装驱动）。
//      Arduino 的 addService()/addServiceTxt() 都不支持子类型。
//
//  所以这里直接调 ESP-IDF 原生的 mdns API（mdns_service_subtype_add_for_host）。
//
//  TXT 记录里几个关键字段（AirPrint 客户端会读）：
//    rp  — resource path，客户端据此拼出 IPP 端点（这里是 /ipp/print）
//    ty  — 打印机类型（"EPSON-Photo-1390" 之类）
//    usb-MDL / usb-EF / usb-pdl — 型号与协议描述
//    pri — 优先级
//    admin-uri — 管理页地址
// ============================================================

#include <Arduino.h>

class AirPrintMdns {
public:
  // 在 MDns.begin() 之后调用。hostName 建议用纯 ASCII（无 .local）
  // ippPort: IPP 服务端口（631）；printerName: 展示给用户看的名字
  static bool advertise(const char* hostName, uint16_t ippPort,
                        const char* printerName, const char* model,
                        const char* adminUri, uint16_t adminPort);
};

#endif // AIRPRINT_MDNS_H
