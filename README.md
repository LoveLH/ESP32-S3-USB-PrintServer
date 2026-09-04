# ESP32-S3 WiFi 打印服务器 v2.1.2

把任何 USB 打印机变成网络打印机，Windows 和 macOS 都能直接打印。
当前目标机型：**EPSON Stylus Photo 1390**（USB VID `0x04B8`）。

---

## 目录

- [v2 相比原版改了什么](#v2-相比原版改了什么)
- [硬件与接线](#硬件与接线)
- [Arduino IDE 设置（关键）](#arduino-ide-设置关键)
- [首次配网](#首次配网)
- [Windows 添加打印机](#windows-添加打印机)
- [macOS 添加打印机](#macos-添加打印机)
- [OTA 升级（三种方式）](#ota-升级三种方式)
- [网页管理界面](#网页管理界面)
- [哪些打印机能用](#哪些打印机能用)
- [Epson 1390 特别注意](#epson-1390-特别注意)
- [故障排查](#故障排查)

---

## v2 相比原版改了什么

原版是为 Brother HL-1110 写死的，v2 做了通用化改造：

| 项目 | 原版 | v2 |
|------|------|-----|
| 打印机识别 | 只认 Brother VID `0x04F9` | 解析 **IEEE 1284 Device ID**，自动识别厂商/型号/命令语言，兼容 23 家常见厂商 VID |
| 打印机状态 | 无 | 每 2 秒轮询 **GET_PORT_STATUS**，网页显示缺纸 / 离线 / 报错 |
| USB 信息 | 只有 VID/PID | 设备地址、USB 版本、接口号、传输协议、端点及包长、重枚举次数 |
| 传输粒度 | 每个包 64 字节，慢 | 单次批量传输最大 **8192 字节**，实测快一个数量级 |
| 线程安全 | 打印转发任务和 USB 任务会**并发调用** `usb_host_client_handle_events()`（ESP-IDF 明令禁止，会随机崩） | 加互斥锁，同一时刻只有一个任务碰 USB client |
| OTA | 只在开机已连 WiFi 时启动，网页配网后 OTA 失效 | 连上 WiFi 后自动补启动 |
| 网页 OTA | 无 | 有，浏览器上传 .bin 直接升级 |
| 打印测试页 | 无 | 有 |
| mDNS | 只有 `pdl-datastream` | 增加 `_printer._tcp` |

> **关于打印机识别的补充（v2.1.2）**：部分机型（如 Epson Stylus Photo 1390）对
> IEEE 1284 Device ID、字符串描述符等 USB 控制传输**一律不响应**（请求会挂死）。
> 这类机型固件会自动改用内置 `KNOWN_PRINTERS` 表，按 **VID:PID** 直接查厂商/型号。
> 想支持新机型，在 `usb_printer.cpp` 的 `KNOWN_PRINTERS[]` 里加一行即可。
> 注意：**这只影响网页上显示的名字，不影响打印**——打印走 Bulk OUT，与设备识别无关。

---

## 硬件与接线

| 项目 | 规格 |
|------|------|
| 开发板 | ESP32-S3（本项目实测机型：**4MB Flash / 2MB PSRAM**，已验证稳定运行） |
| USB OTG 转接线 | **Type-C 公 → USB-A 母**（必须是 OTG 线，不是普通充电线） |
| 打印机线 | USB-A 公 → USB-B 公（打印机原装线） |
| 电源 | 5V / 1A 以上给 ESP32 供电 |

```
[电脑] ──WiFi── [ESP32-S3] ──USB OTG── [打印机]
                    │
              (另一个 Type-C 供电 / 烧录)
```

- **标 "USB" 的接口** → 接 OTG 线 → 打印机（Host 模式）
- **标 "UART"/"COM" 的接口** → 接电脑烧录，之后当供电口

Epson 1390 是市电自供电，不需要 ESP32 反向供电，这点很省心。

---

## Arduino IDE 设置（关键）

用 Arduino IDE 打开 `ESP32S3_PrintServer_v2.ino`，在 **工具** 菜单里设置：

| 设置项 | 选择 | 说明 |
|--------|------|------|
| Board | **ESP32S3 Dev Module** | |
| USB CDC On Boot | **Disabled** | ⚠️ 必须为 Disabled，否则 TinyUSB 会占用 USB 外设，Host 起不来 |
| USB Mode | **USB-OTG (TinyUSB)** | |
| USB DFU On Boot | Disabled | |
| USB MSC On Boot | Disabled | |
| PSRAM | **OPI PSRAM** | |
| Flash Size | **4MB** | 按你板子的实际 Flash 选 |
| Partition Scheme | **Minimal SPIFFS (1.9MB APP / 190KB SPIFFS)** | ⚠️ 该方案自带双 app 分区，**支持 OTA**；选 "No OTA" 系列会导致 OTA 失效 |
| Port | 你的 COM 口 | |

> **本项目实测通过的配置**：4MB Flash + 2MB PSRAM + `Minimal SPIFFS`，固件约 1.06MB，可稳定 OTA。
> 如果你的板子是 16MB Flash，改用 `16M Flash (3MB APP/9.9MB FATFS)` 即可，同样支持 OTA。
> 当前已安装的 esp32 核心版本：**3.3.10**（IDF v5.5.4）。

对应的 arduino-cli FQBN（命令行编译时用）：

```
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs
```

烧录失败时：按住 **BOOT** → 按一下 **RST** → 松开 RST → 再松开 BOOT。

---

## 首次配网

1. 烧录完成后，用 OTG 线把打印机接到 ESP32 的 USB 口
2. 另一个 Type-C 接 5V 电源
3. 串口监视器（115200）应看到：

```
[USB] 检测到新设备，地址 1
[USB] addr=1 VID=0x04B8 PID=0x0005 bcdUSB=0x0110
[USB] 打印机接口 #0 协议=双向 OUT=0x01(64B) IN=0x82(64B)
[USB] Device ID: MFG:EPSON;CMD:ESCPL2,BDC...;MDL:Stylus Photo 1390;CLS:PRINTER;
[USB] 打印机就绪: EPSON Stylus Photo 1390
```

4. 手机或电脑连 WiFi **PrintServer-Setup**，密码 `12345678`
5. 浏览器打开 `http://192.168.4.1`
6. 点 **扫描** → 选你的 WiFi → 输密码 → **连接**
7. 连上后设备切到新 IP，重新访问该 IP

> ESP32 只支持 **2.4GHz** WiFi，5GHz 搜不到。

---

## Windows 添加打印机

**第 0 步：先装驱动。** 到爱普生官网下载「Stylus Photo 1390 驱动程序」并安装。
（驱动是装在 Windows 上的，ESP32 不参与渲染，只转发字节。）

1. 设置 → 蓝牙和其他设备 → **打印机和扫描仪** → 添加设备
2. 等几秒，点「**手动添加**」
3. 选「**使用 TCP/IP 地址或主机名添加打印机**」→ 下一步
4. 设备类型：**TCP/IP 设备**
   主机名或 IP 地址：填 ESP32 的 IP（或 `printserver.local`）
5. 系统会尝试查询设备 —— **这一步必然失败或很慢**，直接点「**自定义**」→「设置」
6. 协议选 **Raw**，端口号 `9100`
7. ⚠️ **务必取消勾选「SNMP 状态已启用」**
   —— 不取消的话，Windows 收不到 SNMP 回应，会把打印机判定为「离线」，打印任务全部卡住
8. 驱动选已安装的 Epson Stylus Photo 1390
9. 打印测试页验证

---

## macOS 添加打印机

**第 0 步：装驱动。** Epson 官网下载 macOS 驱动；若系统版本太新装不上，可用开源方案 **Gutenprint**（提供了 Stylus Photo 1390 的 PPD）。

1. 系统设置 → 打印机与扫描器 → **添加打印机**
2. 切到 **IP** 标签页
3. 协议：**HP Jetdirect - Socket**
4. 地址：ESP32 的 IP（或 `printserver.local`），队列留空
5. 「使用」→ **选择软件** → 搜索 `Stylus Photo 1390`
6. 添加
7. 如果队列显示「离线」，手动点「**恢复**」

> 不要选 IPP。这台机器不是 IPP Everywhere / AirPrint 设备，ESP32 端也没有实现 IPP。

---

## OTA 升级（三种方式）

### 前提

- 分区表必须是 `16M Flash (3MB APP/9.9MB FATFS)`（含 OTA 分区）
- 电脑与 ESP32 在同一网段
- 建议在**空闲时**升级，别在打印大作业时刷

### 方式 A：网页上传（最推荐，不依赖 mDNS）

1. Arduino IDE 里 **项目 → 导出已编译的二进制文件**（`Ctrl+Alt+S`）
2. 固件生成在 sketch 目录下的 `build/esp32.esp32.esp32s3/` 里，文件名 `ESP32S3_PrintServer_v2.ino.bin`
3. 浏览器打开 `http://printserver.local`（或 IP）
4. 「固件升级（OTA）」→ 选这个 .bin → 填密码（若设置了）→ **上传并升级**
5. 进度条走完自动重启，约 15 秒后页面自动刷新

### 方式 B：Arduino IDE 网络端口

1. **工具 → 端口**，列表底部应出现网络端口 `printserver at 192.168.x.x`
2. 选中它，直接点上传

> Windows 上如果看不到网络端口，通常是缺少 mDNS（Bonjour）服务。
> 装一个 Bonjour Print Services，或者直接用方式 A / C。

### 方式 C：命令行 espota

```bat
python "%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.10\tools\espota.py" ^
  -i 192.168.1.50 -p 3232 -f ESP32S3_PrintServer_v2.ino.bin
```

设了密码就加 `-a 你的密码`。

### 安全提示

`config.h` 里 `OTA_PASSWORD` 默认为空 —— 意味着**局域网内任何人都能刷机**。
建议改成自己的密码，改完后 Arduino IDE 上传和网页上传都要填。

---

## 网页管理界面

打开 `http://printserver.local`，每 2 秒自动刷新，共 8 个卡片：

| 卡片 | 内容 |
|------|------|
| **打印机状态** | 就绪/打印中/未连接；1284 端口状态（正常/缺纸/离线/报错）；厂商、型号、命令语言、序列号；状态更新时刻；原始 Device ID |
| **USB 连接** | 连接状态；VID:PID；USB 版本；设备地址；打印机接口号与类型；传输协议（单向/双向/1284.4）；Bulk OUT/IN 端点及包长；重枚举次数 |
| **打印任务** | 是否进行中；客户端 IP；已接收 / 已发往 USB；平均速率；已用时 |
| **统计** | 累计作业数、累计数据量、失败作业数、USB 传输错误数、最近一次作业详情 |
| **WiFi** | 状态、SSID、IP、信号强度；扫描并切换网络 |
| **系统** | 运行时长、可用内存、PSRAM；打印测试页 / 重置打印机 / 重启设备 |
| **固件升级** | 上传 .bin 升级 |
| **添加打印机指引** | Windows 与 macOS 的逐步说明（IP 自动填入） |

> 如果「1284 端口」显示 **状态不可读**，说明该机型不响应 `GET_PORT_STATUS` 请求，属正常现象，不影响打印。

---

## 哪些打印机能用

### ✅ 能用

- **暴露标准 USB 打印机类接口（接口类 `0x07`）的机型** —— Epson 大部分机型、Brother、HP LaserJet、多数针式打印机与热敏票据机
- **GDI / 主机型（Windows-only）打印机也照样能用** —— 因为渲染是在电脑上由驱动完成的，ESP32 只做字节透明转发，完全不需要理解页面语言

**判断方法：** 插上后看串口。出现「找到打印机类接口 #x」就支持。

### ❌ 不行或受限

- **只暴露厂商私有接口（类 `0xFF`）的机型** —— 部分 Canon PIXMA、部分 HP DeskJet。需要抓包逆向私有协议，本项目不支持
- **只支持 USB High-Speed(480Mbps) 且不兼容 Full-Speed 的机型** —— ESP32-S3 只有 12Mbps Full-Speed
- **一体机的扫描 / 传真功能** —— 只转发打印通道
- **双向状态回传**（墨量、卡纸详情）—— 见下一节
- **USB 总线供电的便携打印机** —— ESP32 供不出足够电流

### ⚠️ 速度预期

ESP32-S3 的 USB 是 Full-Speed 12Mbps，实际吞吐约 **0.6–1 MB/s**。
普通文档秒出；但照片级大作业（A3 / 2880dpi）动辄几十 MB，会比较慢，这是硬件限制，不是故障。

---

## Epson 1390 特别注意

1. **打印头清洗、喷嘴检查、墨量显示做不了。**
   这些走的是爱普生私有双向协议，本服务器只做单向数据转发。
   需要维护喷头时，还是得把打印机 USB 直连电脑。

2. **驱动必须装在各自电脑上**，ESP32 不提供任何驱动。

3. **测试页会先发 `ESC @` 复位打印机**（ESC/P 指令）。
   如果你换成 PCL / PostScript / 热敏票据机，把 `config.h` 里的
   `TESTPAGE_PREPEND_ESC_RESET` 改成 `0`，否则可能打出乱码。

4. **打印质量建议在驱动里量力而行。** 2880dpi 的 A3 照片数据量非常大，
   日常用 720/1440dpi 体验会好很多。

---

## 故障排查

| 现象 | 排查 |
|------|------|
| 串口完全没有 `[USB]` 信息 | 用的是不是 OTG 线？接的是不是标 "USB" 的口？ |
| 「未找到打印机类接口」 | 该机型走厂商私有协议，本项目不支持 |
| 打印机识别成 `VID:xxxx` | Device ID 读取失败，会退回 VID 查表；不影响打印 |
| Windows 显示「离线」 | 端口设置里**取消勾选 SNMP 状态已启用** |
| Windows 添加时卡在「正在查询」 | 直接点「自定义」手动指定 Raw / 9100 |
| macOS 队列一直暂停 | 手动点「恢复」；确认协议是 HP Jetdirect - Socket |
| 打印出乱码 | 驱动选错了；或 `TESTPAGE_PREPEND_ESC_RESET` 与实际机型不匹配 |
| 打印到一半卡住 | 看网页「USB 传输错误」计数；检查 USB 线与供电 |
| 网页打不开 | 确认电脑与 ESP32 同网段；换 IP 直连试试 |
| `printserver.local` 打不开 | Windows 缺 mDNS，用 IP 访问 |
| OTA 上传失败「Update.begin 失败」 | 分区表选成了 No OTA 系列 |
| Arduino IDE 看不到网络端口 | 装 Bonjour，或改用网页 OTA |
| 大作业打印很慢 | 正常，Full-Speed USB 上限约 1 MB/s |

---

## 文件结构

```
ESP32S3_PrintServer_v2/
├── ESP32S3_PrintServer_v2.ino  主程序：初始化、任务调度、mDNS、OTA
├── config.h                    所有可调参数（端口、缓冲区、OTA 密码等）
├── usb_printer.h / .cpp        USB Host 打印机驱动：枚举、Device ID、端口状态、数据发送
├── print_server.h / .cpp       Raw Socket :9100 服务器、数据转发、测试页
├── web_ui.h / .cpp             HTTP 管理界面、状态 API、网页 OTA
├── wifi_manager.h / .cpp       WiFi 配网（AP 模式 + NVS 保存）
├── crashlog.h / .cpp           崩溃现场记录（写入 RTC 内存，重启后可从 /api/debug 读取）
├── .gitignore                  排除 build/ 等构建产物
├── LICENSE                     MIT
└── README.md
```

---

## 主要可调参数（config.h）

| 参数 | 默认 | 说明 |
|------|------|------|
| `MDNS_HOSTNAME` | `printserver` | 访问域名 |
| `RAW_SOCKET_PORT` | `9100` | 打印端口 |
| `OTA_PASSWORD` | 空 | **建议设置** |
| `USB_XFER_CHUNK` | `8192` | 单次 USB 传输大小，调小可提升兼容性、降低速度 |
| `STATUS_POLL_MS` | `2000` | 打印机状态轮询间隔 |
| `TESTPAGE_PREPEND_ESC_RESET` | `1` | ESC/P 机型保持 1，PCL/PS 机型改 0 |
| `PRINT_BUFFER_SIZE` | 256KB | PSRAM 缓冲 |

---

*基于原版 ESP32-S3 WiFi Print Server（Brother HL-1110 版）改造，v2.1.2 通用化。*
