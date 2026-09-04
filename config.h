#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  ESP32-S3 WiFi 打印服务器  v2.2.0（通用版）
//
//  适用机型：任何暴露「标准 USB 打印机类」接口的打印机
//            型号由 IEEE 1284 Device ID 自动识别 —— 插上什么就显示什么
//            实测机型：EPSON Stylus Photo 1390（VID 0x04B8 / PID 0x0007）
//  原理：ESP32-S3 做 USB Host 接管打印机，
//        对外提供 Raw Socket(:9100) + mDNS + 网页管理 + OTA
//
//  支持范围：页面描述语言由电脑端驱动负责，ESP32 只做透明转发，
//            不需要理解任何页面语言，GDI / 主机型打印机同样可用。
// ============================================================

#define FW_VERSION "2.2.0"

// --- WiFi 兜底配置（一般用网页配网，这里留空即可）---
#define DEFAULT_WIFI_SSID     ""
#define DEFAULT_WIFI_PASSWORD ""

// --- 首次配网用的 AP ---
#define AP_SSID       "PrintServer-Setup"
#define AP_PASSWORD   "12345678"

// --- 打印服务 ---
// mDNS 对外公布的网络打印机名，可随意改，不影响识别与打印
#define PRINTER_NAME     "ESP32_PrintServer"
#define RAW_SOCKET_PORT  9100          // JetDirect / AppSocket 端口
#define WEB_SERVER_PORT  80
#define MDNS_HOSTNAME    "printserver" // 浏览器打开 http://printserver.local

// --- USB Host ---
#define USB_HOST_TASK_PRIORITY    5
#define USB_HOST_TASK_STACK_SIZE  10240
#define USB_XFER_CHUNK            8192        // 单次 USB 批量传输最大字节
#define PRINT_CHUNK               16384       // 每次从 TCP 取多少字节交给 USB
#define PRINT_BUFFER_SIZE         (256 * 1024)// PSRAM 缓冲
#define STATUS_POLL_MS            2000        // IEEE1284 端口状态轮询间隔
// IEEE1284 端口状态查询（class request）纯属可选信息，raw 打印完全不需要。
// 不少 GDI / 主机型打印机（如 Epson 1390）对它长时间 NAK，而本 IDF 版本
// usb_transfer_t::timeout_ms 无效、EP0 又无取消接口，会留下永久悬挂的传输。故默认关闭，需要时再置 1。
#define ENABLE_PORT_STATUS_POLL   0

// --- 测试页 ---
// ESC/P 系列（爱普生、多数针式机）填 1：测试页前会先发 ESC @ 复位打印机
// PCL / PostScript / 热敏票据机请改成 0，否则可能打出乱码
#define TESTPAGE_PREPEND_ESC_RESET   1

// --- 状态 LED（没有外接 LED 也不影响功能）---
#define LED_PIN          2
#define LED_WIFI_OK      1
#define LED_PRINTING     2
#define LED_ERROR        3

// --- OTA ---
#define OTA_HOSTNAME  "printserver"
#define OTA_PASSWORD  ""     // 留空=不设密码；设了以后 Arduino IDE 与网页 OTA 都要输入
#define OTA_PORT      3232

// --- 看门狗 ---
#define WDT_TIMEOUT_SEC   60

// --- NVS 键名 ---
#define NVS_NAMESPACE     "printsvr"
#define NVS_KEY_SSID      "wifi_ssid"
#define NVS_KEY_PASS      "wifi_pass"
#define NVS_KEY_HOSTNAME  "hostname"

#endif // CONFIG_H
