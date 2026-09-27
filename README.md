# ESP32-S3 WiFi Print Server

把任何 USB 打印机变成网络打印机，Windows / macOS / Linux 通过标准 Raw Socket 9100 直接打印，无需安装额外协议。

## 特性

- **页面语言无关**：整体是字节透明转发，固件不理解任何页面描述语言，
  因此 PCL / PostScript / ESC-POS / PJL / URF / GDI(主机型) 全部可用
- **宽松的 USB 接口匹配（v2.4.0 起）**：不再只认标准打印机类 `0x07`，
  改为多级候选打分，**跟着 Linux `ipp-usb` 的经验放行惠普 MFP 的非标准组合 `255/9/1`**，
  同时显式排除 HID / Mass Storage / Imaging 等复合设备里的"搭车"接口
- **机型行为特征库**：VID:PID → 该机型的坏脾气（是否禁 `ESC @`、要不要补换页符、
  是否 NAK 控制请求……），见下方 [机型特征库](#机型特征库)
- **双向回读（v2.4.0 起）**：读 Bulk IN 并把打印机的应答写回 TCP，Windows 可真正使用"双向支持"
- **IPP-over-USB 识别**：能认出 `7/1/4` 的免驱接口，为后续免驱打印铺路（暂透传、不代理）
- **自动识别厂商型号**：通过 IEEE 1284 Device ID 自动获取厂商、型号、命令语言
- **Raw Socket 9100**：行业标准打印端口，电脑端直接当网络打印机添加
- **网页管理界面**：实时显示打印机状态、统计、WiFi 配置、固件升级
- **OTA 升级**：支持网页上传、Arduino IDE 网络端口、命令行 espota
- **mDNS 服务发现**：通过 `printserver.local` 访问，无需记忆 IP
- **内置测试页**：可远程触发打印测试页
- **崩溃日志记录**：崩溃现场写入 RTC 内存，重启后通过 API 读取

### v2.5.1：两个致命自锁的修复

这一版修的都是"设备放在现场没人管时会彻底失联"的问题，建议所有 v2.5.0 用户升级。

1. **登录会话不再会把管理员关在门外**（严重）
   v2.5.0 里登录会话上限 6 个，占满后**连正确的管理员密码也登不进去**，且因为"登出"本身需要先有令牌，
   没有任何自救办法，只能干等 2 小时超时或物理断电。
   现在的策略是：**槽位满了就淘汰最久没活动的那个会话**，登录永远不会被拒绝；
   另外，已持有有效会话的浏览器重复登录（刷新页面、多开标签）只是续期，不再每次烧掉一个槽位。

2. **打印通道看门狗：一台坏机器不能占死整台打印服务器**
   `:9100` 同时只受理一个连接（这是有意的设计：半路的 foreign language 数据流混进来会打印错位）。
   但如果打印机根本不收数据，发送会一直失败，客户端（典型是 Windows 后台）又会不停重试，
   于是连接槽被反复占用 —— 其他人发过来的打印永远排不上队，表象就是"打印机没反应"。
   现在增加了两道闸：作业连续 **120 秒**没能成功下发任何字节 → 判定打印机失联并释放通道；
   单个作业最长占用 **10 分钟** → 强制回收。所有异常都会写进 `lastJob` 便于排查。

3. 打印任务独立于 `loop()` 运行后再也没出现 WDT 重启：即便打印机持续 NAK 几十次，
   `bootCount` 保持不变（实测 3 分钟 19 次采样无重启），网页 / API / OTA 全程可用。

### 已知限制（将在后续版本处理）

`:9100` 同时只受理一个连接（这是有意的：两路数据流混进打印机会错位）。如果打印机长时间
不收数据，一次发送最多要等 30 秒才有结果，这段时间内新的连接排不上队；遇上某些客户端
（典型是 Windows 后台）不停重试失败作业时，其他人发来的打印可能需要排队若干轮才轮到。
容忍不及时（服务端仍然存活、网页与 API 全程可用、不会崩溃），下一步会把 USB 等待改成
非阻塞状态机来根治。

### v2.5.0：稳定性 + 权限

- **管理员登录**：所有写操作（改 WiFi、刷固件、重启、测试页、恢复出厂）都要登录，
  不再是"谁打开网页都能改"。密码 SHA-256 加盐存 NVS，`HttpOnly` Cookie 会话，
  连续输错会被指数退避锁定；出厂默认密码登录后会强制要求修改
- **打印服务搬出 `loop()`**：跑在独立的 FreeRTOS 任务 + 独立核心上。
  以前打印机一"叫不应"（缺纸、卡纸、固件卡死），一次 USB 传输就会把 `loop()` 阻塞 30 秒，
  网页、API、OTA 全部无响应，而 Windows 后台还在不停重试 —— 设备等于被永久锁死。现在不会
- **网页触发的阻塞操作异步化**：测试页、软复位改成"提交 + 轮询结果"，点一下网页立刻响应
- **WiFi 三级重连 + 指数退避**：`reconnect → 完整重新关联 → WiFi 驱动复位`，
  关掉 WiFi modem sleep（这是打印卡顿/断流的常见元凶）
- **长时间离线自动开后备 AP**：路由器换 SSID/密码、设备被挪远导致失联时，
  离线 3 分钟自动开出 `PrintServer-Setup` 热点，用户能连回来重新配置 —— 不用拆机重刷
- **断链自愈**：WiFi 重连后自动重建 mDNS 和 :9100 监听
  （否则会出现"能 ping 通，但电脑上的打印机离线"）
- **双向回读写保护**：客户端不读数据时不再硬写 TCP，避免 lwIP 缓冲塞满把打印任务卡死
- **内存自愈**：堆长时间低于阈值且当前没在打印时自动重启，避免碎片化发展成永久故障

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
- [机型特征库](#机型特征库)
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

> ⚠️ **必须锁定 3.3.10，不要升级。** Arduino IDE 会自动升级开发板包，实测升级到 **3.3.11** 后
> **USB Host 不再枚举打印机**（网页显示"未连接"、`enumCount=0`、`vid=0000`），
> 断电重启打印机、重插 USB 都无效，看起来像硬件坏了，其实是核心变了。建议关闭 IDE 自动更新。
>
> 判别信号：编译突然整机重编（数分钟）+ 产物体积明显变化（3.3.10 ≈ 1,089,824 字节；3.3.11 ≈ 1,076,736 字节）。
>
> 已升级的话退回 3.3.10：
> ```bash
> arduino-cli core uninstall esp32:esp32
> arduino-cli core install esp32:esp32@3.3.10
> ```
> （`core uninstall` **不能带 `@版本号`**，会报 `version not allowed`。装完后需整机重编译再 OTA。）

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
5. 用出厂账号登录：账号 **`admin`**，密码 **`admin`**
6. **扫描** → 选择你的 WiFi → 输入密码 → **连接**
7. 连接成功后，设备切换到新 IP，页面会自动跳转；用同一套账号密码再登录一次
8. 进 **系统 → 修改管理员密码** 把默认密码换掉（不强改的话，除配网外的写操作会被锁）

> ESP32 仅支持 2.4GHz WiFi。

> **忘记管理员密码怎么办**：长按板子上的 `BOOT` 键约 10 秒不放（串口会打印提示），
> 密码会被清空回 `admin`。没有实体按键的板子可在 `config.h` 里把
> `AUTH_FACTORY_RESET_PIN` 改成 `-1` 关闭这个功能（同时也就失去了这个救场手段）。

> **设备连不上网、网页打不开怎么办**：等 **3 分钟**。固件判定长时间离线后会自动开出
> `PrintServer-Setup` 热点，连上去就能重新配置 —— 不用拆机重刷。
> 期间它也一直在后台尝试连回原来的 WiFi。

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

访问 `http://printserver.local`。未登录时整页盖一层登录框；登录后分 **5 个页签**，每 2 秒自动刷新：

| 页签 | 内容 |
|------|------|
| **概览** | 打印机状态与型号、当前作业（已接收 / 已发往 USB / 回读已回收 / 速率 / 用时）、累计统计、设备运行时长与内存 |
| **打印机** | USB 链路全量信息（VID:PID、接口、协议、端点、包长、是否 IPP-over-USB、重枚举次数）、Device ID、序列号；「打印测试页」「软复位」按钮（异步，结果以 toast 弹出） |
| **网络** | WiFi 状态、SSID、IP、**当前 / 近 1 分钟最差 / 均值信号**、离线时长、断线与重连次数、当前重试间隔、后备 AP 是否已开；扫描并切换网络 |
| **系统** | 修改管理员密码、OTA 升级、重启、恢复出厂 |
| **添加打印机** | Windows / macOS 图文步骤，IP 自动填入当前值 |

顶部在默认密码未改时会挂一条醒目横幅，提示改口令；打印机进入"无响应保护期"时也会给出提示。

---

## HTTP API

除状态和登录结果外，**所有接口都需要登录**（Cookie `PSID` 或 `Authorization: Bearer <token>`）。
跨站请求会被拒绝（校验 `Origin` 与 `Host` 一致）。

| 端点 | 方法 | 需要登录 | 说明 |
|------|------|----------|------|
| `/api/auth/state` | GET | 否 | 当前鉴权状态：`authenticated` / `needChange` / 是否被锁定 |
| `/api/auth/login` | POST | — | 表单字段 `user` `pass`，成功返回会话 Cookie |
| `/api/auth/logout` | POST | 是 | 销毁当前会话 |
| `/api/auth/passwd` | POST | 是 | 字段 `oldpass` `newpass` `newpass2`；成功后自动重签会话 |
| `/api/status` | GET | 是 | 完整状态 JSON（打印机 / USB / 作业 / 统计 / 网络质量 / 异步操作结果） |
| `/api/debug` | GET | 是 | 调试信息（崩溃日志、boot 计数、泄漏计数） |
| `/api/wifi/scan` | GET | 是 | 触发后台扫描并返回上次结果（非阻塞） |
| `/api/wifi/connect` | POST | 是¹ | 字段 `ssid` `password`，成功后返回新 IP |
| `/api/testpage` | POST | 是² | 提交打印测试页请求，**立刻返回**，轮询 `/api/status` 的 `ops.testpage` 看结果 |
| `/api/printer/reset` | POST | 是² | 同上，复位结果看 `ops.usbReset` |
| `/api/restart` | POST | 是² | 重启 ESP32 |
| `/api/factory-reset` | POST | 是¹ | 清除 WiFi 配置 + 管理员密码并重启 |
| `/api/ota` | POST (multipart) | 是² | OTA 升级，字段名 `firmware`；可加 `?pw=` 传 `OTA_PASSWORD` |

¹ 允许在"还没改掉出厂默认密码"时调用（配网 / 救场是基础能力，不能被锁死）。
² 除登录以外，`needChange` 状态下这些接口一律返回 `403 {"error":"default-password"}`，
强迫用户先把出厂口令换掉。

`ops` 状态码：`0` 无请求 / `1` 执行中 / `2` 成功 / `3` 失败。
异步化是因为这类操作要发 USB 控制或批量传输，遇到不应答的打印机会阻塞几十秒 ——
同步执行会把整个管理界面卡死。

---

## 兼容性说明

### 支持

- 任何暴露 **标准 USB Printer Class 接口**（接口类 `0x07`）的打印机
- **GDI / 主机型打印机**（如 Epson Stylus Photo 1390）：渲染在电脑端驱动完成，ESP32 只做字节透明转发
- **厂商私有接口（`0xFF`）打印机**：v2.4.0 起，机型库放行后允许接管。
  已放行的有 HP（含 `255/9/1` 的非标准 IPP 组合）与 Canon；
  其余厂商未知默认不接管，可在 `printer_db.cpp` 里加一行授权（见[机型特征库](#机型特征库)）
- **IPP-over-USB 设备**（`7/1/4`）：能识别并接管；目前仍是透传，暂不代理 IPP 请求

### 不支持 / 有边界

- **仅支持 USB High-Speed (480Mbps)** 且不兼容 Full-Speed 的机型（ESP32-S3 仅 12Mbps Full-Speed）
- **一体机的扫描/传真功能**（只转发打印通道）
- **主机型（GDI）打印机的手机端免驱**：iOS / Android 免驱发的是 PDF / URF，
  GDI 机型根本不认识；要在设备端做 PDF→原机语言的栅格化，MCU 做不到。
  这类机器请在 Windows / macOS / Linux 上装原厂驱动使用

### 速度

ESP32-S3 的 USB 是 Full-Speed 12Mbps，实际吞吐约 **0.6–1 MB/s**。普通文档秒出，照片级大作业（A3 / 2880dpi）会比较慢，这是硬件限制。

### 双向通信（v2.4.0 起）

固件会在 Bulk IN 端点上常挂一条读传输，把打印机的应答**原样写回 TCP**：

- Windows 的端口设置里可以真正勾上 **"启用双向支持"**；
- PJL 状态查询、缺纸/卡纸应答、票据机的 ACK 都能回传；
- 这也是后续做 IPP 代理的前置能力。

打印机的"墨量、打印头清洗"等高级功能走的是厂商私有双向协议，本项目只做透明回传、
不解释内容，所以能否显示取决于打印机自身支不支持明文协议。
若某台机器被回读干扰，可在 `printer_db.cpp` 里给它打 `Q_NO_BIDIR` 关掉。

---

## 机型特征库

兼容性的关键不在"认不认得这台打印机"，而在**知不知道它的坏脾气**。`printer_db.cpp` 里维护
`VID:PID → 行为特征位` 的表，命中的机型会覆盖 `config.h` 的全局默认值。

现有特征位：

| 标志 | 含义 |
|------|------|
| `Q_WANT_ESC_RESET` | 作业开头**要**发 `ESC @`（多数 ESC/P 爱普生、针式机） |
| `Q_NO_ESC_RESET` | **千万别**发 `ESC @`（HP 等走 `@PJL` 流的机型：发了会卡"正在打印文档"不出纸） |
| `Q_WANT_FORMFEED` | 作业结束补 `0x0C` 才肯出最后一页（多数 HP DeskJet） |
| `Q_NO_FORMFEED` | 不要补 FF（部分机型补了会空走一张纸） |
| `Q_NO_CTRL` | 完全不应答控制传输，禁发 Device ID / 字符串描述符 / 端口状态（如 Epson 1390） |
| `Q_NO_ZLP` | 不要补零长度包 |
| `Q_VENDOR_IFACE` | 允许接管厂商私有(`0xFF`)批量接口（HP / Canon 已放行） |
| `Q_NO_BIDIR` | 不要回读 Bulk IN |
| `Q_IPP_OVER_USB` | 已知 IPP-over-USB 设备（即便描述符不是标准 `7/1/4`） |

### 补录一台新机

1. 插上打印机，访问 `/api/status`，记下 `usb.vid` / `usb.pid` 和 `usb.protocol`
2. 打开 `printer_db.cpp`，在 `PROFILES` 里加一行：

   ```c
   {0x04B8, 0x9999, "Epson 某某机型", Q_WANT_ESC_RESET | Q_WANT_FORMFEED},
   ```

   `PID` 填 `PID_ANY` 表示对该厂商所有产品生效（用于厂商级行为，如 HP 全线禁 `ESC @`）
3. **只填有实测依据的标志**。没有证据的机型不要乱标，让它走全局默认值更安全

> 这张表是这个方案里最不容易被复制的部分：代码能开源，几百台真机的调校数据不行。

---

## 故障排查

| 现象 | 原因与处理 |
|------|-----------|
| 串口无 `[USB]` 输出 | 检查 OTG 线、接口 |
| `未找到可用打印接口` | 描述符里既没有标准打印机类也没有被放行的私有接口。到 `/api/status` 记下 `vid/pid`，到 `printer_db.cpp` 补一行并加 `Q_VENDOR_IFACE` |
| 型号显示为 `VID:xxxx` | Device ID 读取失败，不影响打印 |
| Windows 显示离线 | 端口设置取消勾选 SNMP |
| Windows 添加卡在查询中 | 直接点自定义，指定 Raw / 9100 |
| macOS 队列暂停 | 手动点恢复；确认协议为 HP Jetdirect |
| 打印乱码 | 驱动选错；或 `TESTPAGE_PREPEND_ESC_RESET` 不匹配 |
| 打印中途卡住 | 检查 USB 传输错误计数、线缆、供电 |
| 打印机一直"未连接"、`enumCount=0` | 先确认 ESP32 核心是 **3.3.10**（见[安装 ESP32 核心](#1-安装-esp32-核心)）；再断电重启打印机、重插 USB |
| 打印用着用着失灵，重启才好 | 空连接占住了作业（Windows 双向探测、被取消作业留下的半开连接）。v2.3.3 起固件会**自动回收**（15 秒），无需重启 |
| 固件测试页打不出来 | **主机型（GDI / PCL3GUI）打印机**只认官方驱动生成的数据流，纯文本测试页不会出纸 —— **这不算故障**。请用真实打印作业验证链路 |
| HP 卡在"正在打印文档"不出纸 | 把 `JOB_RESET_ON_CONNECT` 和 `TESTPAGE_PREPEND_ESC_RESET` 都设为 `0`（默认已是 0）：作业开头硬塞 `ESC @` 会打乱 HP 的 PJL 解析 |
| `printserver.local` 无法访问 | Windows 缺 mDNS，用 IP 访问 |
| OTA 失败 | 确认分区表选了含 OTA 的方案 |
| Arduino IDE 看不到网络端口 | 装 Bonjour，或用网页 OTA |
| 作业 `sent=0`、`USB 传输失败 status=0` | **打印机自身在拒收数据**：缺粉 / 卡纸 / 残留卡住的作业 / 面板报错。**断电重启打印机**并检查耗材，与固件无关。v2.4.0 起固件会快速失败保护并保持网页可读 |
| 打印机出问题时网页/API 卡住 | ~~v2.4.0：主循环被批量传输占住。~~ **v2.5.0 起打印服务已独立成任务**，管理页始终可用 |
| 忘了我设的管理员密码 | 长按板子 BOOT 键 10 秒清空密码（串口会打印提示），或 `AUTH_FACTORY_RESET_PIN` 设为 `-1` 时用串口改。见[首次配网](#首次配网) |
| 换路由器 / 改 WiFi 密码后设备失联 | 等 3 分钟，设备会自动开出 `PrintServer-Setup` 热点；连上去重新配置即可，不用拆机重刷 |
| WiFi 明明连着但打印很卡、时断时续 | 看管理页「网络 → 近 1 分钟最差信号」。长期低于 -75 dBm 请挪近路由器或加中继；`WIFI_DISABLE_SLEEP` 保持 `1` |
| 重新连上 WiFi 后打印机变成离线 | 固件会在检测到链路重建后自动重启 mDNS 与 :9100 监听。若仍异常，`/api/status` 确认 `wifi.disconnects` 是否增长，必要时手动重启一次 |
| 打印到一半重复了好多份 | Windows 后台在重试失败的作业。看 `/api/status` 的 `stats.failedJobs` 与 `job.bytesSent` 是否对不上 —— 一般是打印机侧先出了问题（缺粉/卡纸），不是固件 |
| `ops.testpage` 一直是 `1`（执行中） | 打印机会把测试页排在既有作业之后，或当前正被 USB 传输占着。若长时间不变，多半是打印机在 NAK，先看 `printer.stalled` |

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
| `TESTPAGE_PREPEND_ESC_RESET` | `0` | ESC/P 机型可设 1；**PCL / PS / HP 必须为 0** |
| `JOB_RESET_ON_CONNECT` | `0` | 每个作业开头发 `ESC @`。Epson 可设 1；**HP 必须为 0**，否则卡"正在打印文档" |
| `JOB_APPEND_FORMFEED` | `1` | 作业结束补换页符 `0x0C`，强制走纸出页（部分 HP 需要） |
| `JOB_NODATA_TIMEOUT_MS` | `15000` | 连上后一直没收到数据 → 判定空连接，自动断开（自愈，免重启） |
| `JOB_IDLE_TIMEOUT_MS` | `60000` | 作业中长时间无收发 → 强制结束。必须大于 `USB_XFER_TIMEOUT_MS`(30000) |
| `ENABLE_BIDIR_PASSTHROUGH` | `1` | 读 Bulk IN 并把打印机应答回写 TCP（真双向支持） |
| `USB_RX_BUFFER_SIZE` | `4096` | IN 端点回读环形缓冲 |
| `USB_STALL_MAX_TIMEOUTS` | `3` | 连续几次传输无回调 → 判定打印机失联 |
| `USB_STALL_COOLDOWN_MS` | `15000` | 冷却时长，期间作业立刻失败返回，保住网页/API 响应 |
| `PRINT_BUFFER_SIZE` | `262144` | 打印缓冲区（字节） |
| `PRINTER_NAME` | `WiFi_Printer` | 设备标识名 |
| `AUTH_ENABLED` | `1` | 是否开启管理后台登录鉴权（调试时可临时置 0） |
| `AUTH_DEFAULT_USER` / `_PASS` | `admin` / `admin` | 出厂账号密码，登录后强制修改 |
| `AUTH_SESSION_TIMEOUT_MS` | `7200000` | 会话空闲多久过期（默认 2 小时） |
| `AUTH_MAX_SESSIONS` | `6` | 最多几个并发会话 |
| `AUTH_MAX_FAILS` | `5` | 连续输错几次开始锁定（锁定时长指数增长，上限 30 分钟） |
| `AUTH_FACTORY_RESET_PIN` | `0` | 长按该 GPIO（BOOT 键）10 秒清空管理员密码；`-1` 关闭 |
| `PRINT_TASK_CORE` | `0` | 打印任务跑在哪个核心（`loop()` 在 core1，分离才能真正并行） |
| `PRINT_TASK_STACK_SIZE` | `8192` | 打印任务栈 |
| `WIFI_DISABLE_SLEEP` | `1` | 关掉 WiFi modem sleep。**强烈建议保持 1**：节能模式会造成数百毫秒级的延迟抖动 |
| `WIFI_RETRY_MIN_MS` / `_MAX_MS` | `5000` / `60000` | 重连退避的下限与上限 |
| `WIFI_REASSOC_AFTER` | `3` | 失败几次后从"重新连接"升级为"完整重新关联" |
| `WIFI_DRIVER_RESET_AFTER` | `8` | 再失败几次后复位整个 WiFi 驱动 |
| `WIFI_FALLBACK_AP_MS` | `180000` | 离线多久后开后备配网 AP（`0` 关闭） |
| `HEAP_GUARD_MIN_KB` / `_HOLD_MS` | `24` / `30000` | 堆低于该值并持续这么久 → 空闲时自动重启（`HEAP_GUARD_ENABLE=0` 关闭） |

> `JOB_RESET_ON_CONNECT`、`JOB_APPEND_FORMFEED`、ZLP、双向回读、控制传输这些都**只是"没查到该机型"时的全局默认值**。
> `printer_db.cpp` 里登记过的机型以库中的 `Q_*` 标志为准（例如 HP 全线已被禁用 `ESC @`）。

### Windows 端口建议

v2.4.0 起固件支持双向回读，端口设置里**可以勾选"启用双向支持"**，Windows 有希望读到真实状态。

但当前仍是**单作业通道**（同一时刻只接受一条连接），状态查询连接会在打印期间被拒绝；
若你在意排 N 份作业时的流畅度，先保持**取消勾选**更省事 —— 多连接队列化在后续版本中处理。

---

## 文件结构

```
ESP32S3_PrintServer_v2/
├── ESP32S3_PrintServer_v2.ino  主程序入口
├── config.h                    配置参数（机型库未命中时的全局默认值）
├── printer_db.h / .cpp         机型行为特征库（VID:PID → 兼容性配置）
├── usb_printer.h / .cpp        USB Host 打印机驱动
├── print_server.h / .cpp       Raw Socket 服务器
├── web_ui.h / .cpp             网页管理界面（含登录鉴权接入）
├── wifi_manager.h / .cpp       WiFi 配网 + 三级重连 / 后退避 / 后备 AP
├── auth.h / .cpp               管理员鉴权（SHA-256 加盐、会话、暴力破解锁定）
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
