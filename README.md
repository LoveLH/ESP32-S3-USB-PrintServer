# ESP32-S3 WiFi Print Server

把任何 USB 打印机变成网络打印机，Windows / macOS / Linux 通过标准 Raw Socket 9100 直接打印，无需安装额外协议。

## 特性

- **通用 USB 打印机支持**：基于 USB Printer Class，兼容市面大部分 USB 接口的打印机
- **自动识别厂商型号**：通过 IEEE 1284 Device ID 自动获取厂商、型号、命令语言
- **Raw Socket 9100**：行业标准打印端口，电脑端直接当网络打印机添加
- **网页管理界面**：实时显示打印机状态、统计、WiFi 配置、固件升级
- **OTA 升级**：支持网页上传、Arduino IDE 网络端口、命令行 espota
- **mDNS 服务发现**：通过 `printserver.local` 访问，无需记忆 IP
- **内置测试页**：可远程触发打印测试页
- **崩溃日志记录**：崩溃现场写入 RTC 内存，重启后通过 API 读取

---

## 目录

- [硬件要求](#硬件要求)
- [Arduino IDE 配置](#arduino-ide-配置)
- [编译与烧录](#编译与烧录)
- [首次配网](#首次配网)
- [添加网络打印机](#添加网络打印机)
  - [Windows](#windows)
  - [macOS](#macos)
  - [Linux (CUPS)](#linux-cups)
- [OTA 升级](#ota-升级)
- [网页管理界面](#网页管理界面)
- [HTTP API](#http-api)
- [兼容性说明](#兼容性说明)
- [故障排查](#故障排查)
- [配置参数](#配置参数)
- [文件结构](#文件结构)
- [许可证](#许可证)

---

## 硬件要求

| 项目 | 规格 |
|------|------|
| 开发板 | ESP32-S3 开发板（需带 USB OTG） |
| Flash | 至少 4MB |
| PSRAM | 建议 2MB 或以上 |
| USB OTG 线 | Type-C / Micro-USB 转 USB-A 母口（**必须是 OTG 线**） |
| 打印机线 | USB-A 公转 USB-B 公（打印机原装线） |
| 供电 | 5V / 1A 以上 |

### 接线

```
[电脑] ──WiFi── [ESP32-S3] ──USB OTG── [打印机]
                    │
              (Type-C 供电 / 烧录)
```

- **标"USB"的接口** → 接 OTG 线 → 打印机（Host 模式）
- **标"UART"/"COM"的接口** → 接电脑烧录，烧录后作为供电口

---

## Arduino IDE 配置

### 1. 安装 ESP32 核心

- **板管理器**搜索 `esp32`，安装版本 **3.3.10**（IDF v5.5.4）

### 2. 工具菜单设置

| 设置项 | 选择 | 说明 |
|--------|------|------|
| Board | `ESP32S3 Dev Module` | |
| USB CDC On Boot | **Disabled** | 必须为 Disabled，否则 USB Host 不可用 |
| USB Mode | `USB-OTG (TinyUSB)` | |
| USB DFU On Boot | Disabled | |
| USB MSC On Boot | Disabled | |
| PSRAM | `OPI PSRAM` | |
| Flash Size | `4MB` | 按实际 Flash 选 |
| Partition Scheme | `Minimal SPIFFS (1.9MB APP / 190KB SPIFFS)` | **必须选含 OTA 的方案** |

### 3. 对应 arduino-cli FQBN

```bash
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs
```

---

## 编译与烧录

### Arduino IDE

1. 打开 `ESP32S3_PrintServer_v2.ino`
2. **工具 → 端口**选择对应的串口
3. 点 **上传**

### arduino-cli

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs" .
arduino-cli upload -p COM3 --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs" .
```

### 烧录失败处理

按住 **BOOT** 按钮 → 按一下 **RST** → 松开 RST → 松开 BOOT，进入下载模式后再上传。

---

## 首次配网

1. 烧录完成，接好 USB OTG 线
2. 串口监视器（115200）应看到类似：
   ```
   [USB] 检测到新设备
   [USB] 打印机就绪
   ```
3. 手机或电脑连接 WiFi **`PrintServer-Setup`**，密码 **`12345678`**
4. 浏览器打开 `http://192.168.4.1`
5. **扫描** → 选择你的 WiFi → 输入密码 → **连接**
6. 连接成功后，设备切换到新 IP，重新访问该 IP

> ESP32 仅支持 2.4GHz WiFi。

---

## 添加网络打印机

### Windows

1. **设置 → 蓝牙和其他设备 → 打印机和扫描仪 → 添加设备**
2. 等待搜索完成后点击 **手动添加**
3. 选择 **使用 TCP/IP 地址或主机名添加打印机**
4. 设备类型：`TCP/IP 设备`，地址填 ESP32 的 IP（或 `printserver.local`）
5. 系统查询可能超时，直接点击 **自定义** → 手动设置
6. 协议选 **Raw**，端口号 **`9100`**
7. **取消勾选 "SNMP 状态已启用"**（重要：否则 Windows 会判定打印机离线）
8. 选择已安装的打印机驱动
9. 打印测试页验证

### macOS

1. **系统设置 → 打印机与扫描器 → 添加打印机**
2. 切换到 **IP** 标签页
3. 协议：**HP Jetdirect - Socket**
4. 地址：ESP32 的 IP（或 `printserver.local`），队列留空
5. **使用 → 选择软件** → 搜索你的打印机型号
6. 添加

### Linux (CUPS)

```bash
# 方法一：Web 界面
浏览器打开 http://localhost:631 → Administration → Add Printer

# 方法二：lpadmin 命令行
sudo lpadmin -p WiFiPrinter -E -v socket://192.168.1.50:9100 -m everywhere
```

---

## OTA 升级

### 方式一：网页上传（推荐）

1. Arduino IDE：**项目 → 导出已编译的二进制文件**（`Ctrl+Alt+S`）
2. 打开 `http://printserver.local`
3. **固件升级** 卡片 → 选择 `.bin` 文件 → 填密码（如有）→ **上传并升级**
4. 进度条完成后自动重启，约 15 秒后刷新页面

### 方式二：Arduino IDE 网络端口

1. **工具 → 端口** 底部出现 `printserver at 192.168.x.x`
2. 选中后直接点上传

> Windows 若看不到网络端口，请安装 [Bonjour Print Services](https://support.apple.com/kb/DL999)。

### 方式三：命令行 espota

```bash
python "%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.10\tools\espota.py" \
  -i 192.168.1.50 -p 3232 -f ESP32S3_PrintServer_v2.ino.bin
```

设置了 OTA 密码需加 `-a yourpassword`。

> **安全提示**：`config.h` 中 `OTA_PASSWORD` 默认为空，局域网内任何人都可刷机。建议设置密码。

---

## 网页管理界面

访问 `http://printserver.local`，每 2 秒自动刷新：

| 卡片 | 内容 |
|------|------|
| **打印机状态** | 就绪/打印中/未连接；1284 端口状态（缺纸/离线/报错）；厂商、型号、命令语言、序列号 |
| **USB 连接** | VID:PID、USB 版本、接口号、传输协议、端点及包长、重枚举次数 |
| **打印任务** | 进行中状态、客户端 IP、已接收/已发往 USB、平均速率、用时 |
| **统计** | 累计作业数、累计数据量、失败作业数、USB 传输错误数、最近作业详情 |
| **WiFi** | 状态、SSID、IP、信号强度；扫描并切换网络 |
| **系统** | 运行时长、可用内存、PSRAM；测试页 / 重置打印机 / 重启设备 |
| **固件升级** | 上传 .bin 升级 |
| **添加打印机指引** | Windows / macOS / Linux 步骤（IP 自动填入） |

---

## HTTP API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/status` | GET | 完整状态 JSON |
| `/api/info` | GET | 设备基本信息 |
| `/api/debug` | GET | 调试信息（崩溃日志、boot 计数） |
| `/api/testpage` | POST | 触发打印测试页 |
| `/api/reset-printer` | POST | 重置 USB 打印机 |
| `/api/restart` | POST | 重启 ESP32 |
| `/api/scan` | GET | 扫描 WiFi |
| `/api/wifi` | POST | 切换 WiFi |
| `/api/ota` | POST (multipart) | OTA 升级，字段名 `firmware` |

---

## 兼容性说明

### 支持

- 任何暴露 **标准 USB Printer Class 接口**（接口类 `0x07`）的打印机
- **GDI / 主机型打印机**（如 Epson Stylus Photo 1390）：渲染在电脑端驱动完成，ESP32 只做字节透明转发

### 不支持

- **仅暴露厂商私有接口**（类 `0xFF`）的机型（部分 Canon PIXMA、部分 HP DeskJet）
- **仅支持 USB High-Speed (480Mbps)** 且不兼容 Full-Speed 的机型（ESP32-S3 仅支持 12Mbps Full-Speed）
- **一体机扫描/传真功能**（仅转发打印通道）
- **双向状态回传**（墨量、卡纸详情）—— 见下文

### 速度

ESP32-S3 的 USB 是 Full-Speed 12Mbps，实际吞吐约 **0.6–1 MB/s**。普通文档秒出，照片级大作业（A3 / 2880dpi）会比较慢，这是硬件限制。

### 双向通信

部分打印机的高级功能（墨量显示、打印头清洗、喷嘴检查）依赖厂商私有双向协议，本项目仅做单向数据转发。如需这些功能，请将打印机 USB 直连电脑。

---

## 故障排查

| 现象 | 原因与处理 |
|------|-----------|
| 串口无 `[USB]` 输出 | 检查 OTG 线、接口 |
| `未找到打印机类接口` | 该机型走厂商私有协议，不支持 |
| 型号显示为 `VID:xxxx` | Device ID 读取失败，不影响打印 |
| Windows 显示离线 | 端口设置取消勾选 SNMP |
| Windows 添加卡在查询中 | 直接点自定义，指定 Raw / 9100 |
| macOS 队列暂停 | 手动点恢复；确认协议为 HP Jetdirect |
| 打印乱码 | 驱动选错；或 `TESTPAGE_PREPEND_ESC_RESET` 不匹配 |
| 打印中途卡住 | 检查 USB 传输错误计数、线缆、供电 |
| `printserver.local` 无法访问 | Windows 缺 mDNS，用 IP 访问 |
| OTA 失败 | 确认分区表选了含 OTA 的方案 |
| Arduino IDE 看不到网络端口 | 装 Bonjour，或用网页 OTA |

---

## 配置参数

编辑 `config.h` 修改以下参数：

| 参数 | 默认 | 说明 |
|------|------|------|
| `MDNS_HOSTNAME` | `printserver` | mDNS 主机名 |
| `RAW_SOCKET_PORT` | `9100` | 打印端口 |
| `OTA_PASSWORD` | 空 | OTA 密码（建议设置） |
| `USB_XFER_CHUNK` | `8192` | 单次 USB 传输大小 |
| `STATUS_POLL_MS` | `2000` | 状态轮询间隔 |
| `TESTPAGE_PREPEND_ESC_RESET` | `1` | ESC/P 机型保持 1，PCL/PS 改 0 |
| `PRINT_BUFFER_SIZE` | `262144` | 打印缓冲区（字节） |
| `PRINTER_NAME` | `WiFi_Printer` | 设备标识名 |

---

## 文件结构

```
ESP32S3_PrintServer_v2/
├── ESP32S3_PrintServer_v2.ino  主程序入口
├── config.h                    配置参数
├── usb_printer.h / .cpp        USB Host 打印机驱动
├── print_server.h / .cpp       Raw Socket 服务器
├── web_ui.h / .cpp             网页管理界面
├── wifi_manager.h / .cpp       WiFi 配网
├── crashlog.h / .cpp           崩溃日志（RTC）
├── .gitignore
├── LICENSE
└── README.md
```

---

## 许可证

MIT License - 详见 [LICENSE](LICENSE)

---

## 致谢

基于 ESP-IDF USB Host 栈与 Arduino-ESP32 核心实现。
