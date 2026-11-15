# ESP32-S3 WiFi Print Server

**当前版本：v2.9.5**

把 USB 打印机变成网络打印机。电脑装原厂驱动走 **Raw 9100**，手机走 **IPP / AirPrint**，
有 Linux 主机的话还能用 **USB/IP** 让它整机接管。

```
[电脑 / 手机 / Linux] ──WiFi── [ESP32-S3] ──USB OTG── [打印机]
```

一句话原理：**驱动跑在电脑上，板子只负责把字节原样搬进 USB。**
固件不理解任何页面描述语言，所以 PCL / PostScript / ESC/P / ESC/POS / PJL / PCLm /
GDI（主机型）全都能打 —— 渲染是客户端的事，板子一个字节都不改。

---

## 目录

- [三条通路怎么选](#三条通路怎么选)
- [核心设计：纯透传](#核心设计纯透传)
- [支持与不支持](#支持与不支持)
- [硬件与接线](#硬件与接线)
- [编译与烧录](#编译与烧录)
- [首次配网](#首次配网)
- [添加打印机](#添加打印机)
- [网页管理界面](#网页管理界面)
- [HTTP API](#http-api)
- [机型行为特征库](#机型行为特征库)
- [作业生命周期](#作业生命周期)
- [故障排查](#故障排查)
- [配置参数](#配置参数)
- [文件结构](#文件结构)
- [已知限制](#已知限制)
- [版本历史](#版本历史)

---

## 三条通路怎么选

| 通路 | 端口 | 谁用 | 渲染在哪 |
|---|---|---|---|
| **Raw 9100** | `9100` | 装了驱动的 Windows / macOS / Linux | 客户端驱动。**画质最好、兼容性最广** |
| **IPP / AirPrint** | `631` | 手机、平板、免驱发现的 macOS | 客户端渲染成 PDF / 位图，板子只做协议封装 + 转发 |
| **USB/IP** | `3240` | 装了 CUPS 的 Linux 主机（NAS / 树莓派 / 软路由） | 完全在 Linux 上，板子只做 USB 虚拟化 |

三条通路**互斥** —— 同一时刻只受理一个作业。两条流同时写 USB 会把数据交错成乱码。

选法：

- 只打电脑 → **Raw 9100**，没理由用别的
- 想手机直接打 → **IPP**，但要求打印机自己能吃 PDF 或位图（见[不支持](#支持与不支持)）
- 家里有 Linux 且在跑 CUPS → **USB/IP**，服务端渲染能力（Ghostscript、全套 PPD）免费获得

---

## 核心设计：纯透传

> **板子不调用、也没有「打印机驱动」这回事。**

驱动把文档渲染成打印机能懂的字节流（ESC/P2、PCLm、PJL…），经 9100 发给板子，
板子原样转发到 USB 批量端点。所以：

- 分辨率、色彩、纸型、双面……全部在电脑端生效，板子不参与
- 板子唯一的职责是**别破坏驱动发出来的字节**
- 「选高质量相片」打出来不对 = 字节被动了手脚或链路丢了，**不是板子缺驱动**

老牌打印服务器（D-Link DPR-2000 之类）能做到「通用支持所有打印机」，
靠的不是更懂打印机，恰恰是**一个字节都不加**。本项目默认走同一套保守策略：

| | 传统打印服务器 | 本项目（v2.6.5 起默认） |
|---|---|---|
| 作业开头 | 不动 | 不动（`JOB_RESET_ON_CONNECT=0`） |
| 作业结尾 | 不动 | 不动（`JOB_APPEND_FORMFEED=0`） |
| 控制传输 | 基本不碰 | 只在识别型号时发，且按机型库授权 |

未登记的机型插上就能用；只有**有实测依据**的机型才在 `printer_db.cpp` 里登记例外
（`Q_PASSTHROUGH` 就是「一个字都不许塞」）。反过来做 —— 默认自作主张补字节、逐台打补丁 ——
就是 Epson 1390 彩色相片出白纸的老路。

> USB 物理层意义的「透传」（电脑的 USB 直接出现在打印机口上）在本硬件上做不到：
> ESP32-S3 只有一路 USB OTG，当 Host 连打印机就没有 Device 侧接电脑。
> 但「网络进、USB 出」的软件透传效果等价，还能多出跨网段、mDNS、网页管理。

---

## 支持与不支持

### 支持

| 场景 | 说明 |
|---|---|
| 标准 USB Printer Class（接口类 `0x07`） | 最常见，插上就能用 |
| GDI / 主机型打印机 | 渲染在电脑端完成，板子透明转发。实测 Epson Stylus Photo 1390、HP LaserJet MFP |
| 厂商私有接口（`0xFF`） | 机型库放行后接管。已放行 HP（含非标准 `255/9/1`）、Canon |
| IPP-over-USB（`7/1/4`） | 能识别并接管，目前仍透传不代理 |
| 热敏票据机 / 标签机 | Zebra、TSC、Star、Gprinter、QinHeng 已在库中 |

机型库共 26 条，机制见[机型行为特征库](#机型行为特征库)。

### 不支持

| 场景 | 原因 |
|---|---|
| 高速打印机（High-Speed 480Mbps 独占） | ESP32-S3 只有 Full-Speed 12Mbps，实测 0.6–1 MB/s，A3 / 2880dpi 大图明显慢 |
| 一体机的扫描 / 传真 | 只转发打印通道 |
| **老光栅机的手机免驱打印** | 手机发的是 PDF / URF（设备无关位图），要变成墨色 / 分辨率 / 抖动序列**仍是驱动的工作**。1390 这类 ESP/P2 老机请用电脑打，或用支持 Raw 9100 的第三方 App |
| 打印期间的双向状态查询 | 回读支持（见下），但同一时刻只受理一个连接，打印中的查询连接会被拒绝 |
| 打印机自身固件卡死 | 只能断电重启，软件复位无效（见[已知限制](#已知限制)） |

### 双向回读（v2.4.0 起）

Bulk IN 上常挂一条读传输，把打印机吐出来的字节原样写回 TCP：
Windows 端口可勾「启用双向支持」，PJL 状态查询、缺纸 / 卡纸应答、票据机 ACK 都能回传。

实测 HP 机型在常驻回读下会拒收打印数据，机型库已对 HP 整族关闭回读（`Q_NO_BIDIR`）。
你的机器被回读干扰，就给它加这个标志。

---

## 硬件与接线

| 项目 | 规格 |
|---|---|
| 开发板 | ESP32-S3（**必须带 USB OTG**） |
| Flash | 4MB |
| PSRAM | 2MB 或以上（实测 XMC 4MB Flash + 2MB PSRAM 可用） |
| USB OTG 线 | Type-C / Micro-USB 转 USB-A 母口，**必须是 OTG 线**，普通充电线不行 |
| 打印机线 | USB-A 公转 USB-B 公（打印机原装线） |
| 供电 | 5V / 1A 以上 |

接线：

- 标 **USB** 的口 → OTG 线 → 打印机（Host 模式）
- 标 **UART / COM** 的口 → 接电脑烧录，烧录后作为供电口

> ⚠️ 板载 USB-Serial/JTAG 口（VID:PID `303A:1001`）**不是** OTG 口，接错枚举不到打印机。

---

## 编译与烧录

### 1. 钉死 ESP32 核心 3.3.10

板管理器搜索 `esp32`，装 **3.3.10**。

> 🔴 **不要升级到 3.3.11。**
> 实测升上去之后 **USB Host 不再枚举打印机**：网页显示「未连接」、`enumCount=0`、`vid=0000`，
> 断电重启打印机、重插 USB 全都无效 —— 看起来像硬件坏了，其实只是核心变了。
>
> 判别信号：编译突然整机重编（数分钟）+ 产物体积变化
> （3.3.10 ≈ 1,089,824 字节，3.3.11 ≈ 1,076,736 字节）。
>
> 退回（装 / 卸都很慢，10 分钟量级，低 CPU 属正常）：
> ```bash
> arduino-cli core uninstall esp32:esp32        # 注意：不能带 @版本号
> arduino-cli core install  esp32:esp32@3.3.10
> ```

### 2. 工具菜单 / FQBN

| 设置项 | 选择 |
|---|---|
| Board | `ESP32S3 Dev Module` |
| USB CDC On Boot | **Disabled**（必须，否则 USB Host 不可用） |
| USB Mode | `USB-OTG (TinyUSB)` |
| USB DFU / MSC On Boot | Disabled |
| PSRAM | `OPI PSRAM` |
| Flash Size | `4MB` |
| Partition Scheme | `Minimal SPIFFS (1.9MB APP / 190KB SPIFFS)` —— 必须含 OTA |

```
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs
```

### 3. 编译

Arduino IDE：打开 `ESP32S3_PrintServer_v2.ino` → 选串口 → 上传。

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs" --build-path build .
```

> 只给 `--build-path`，**不要同时给 `--output-dir`**。指向同一目录会报 `are the same file`，
> 是假错，`.bin` 其实已经生成好了。

### 4. 烧录失败

按住 **BOOT** → 点一下 **RST** → 松开 RST → 松开 BOOT，进下载模式后重试。

---

## 首次配网

1. 烧录完成，接好 OTG 线
2. 串口监视器（115200）应看到 `[USB] 打印机就绪`
3. 连 WiFi **`PrintServer-Setup`**，密码 **`12345678`**
4. 打开 `http://192.168.4.1`，用 `admin` / `admin` 登录
5. **扫描** → 选 WiFi → 输密码 → **连接**
6. 设备切到新 IP 后页面自动跳转，用同一套账号再登录一次
7. 进 **系统 → 修改管理员密码**（不改的话，除配网外的所有写操作都会被 `403` 挡住）

> ESP32 只支持 2.4GHz WiFi。

**忘记管理员密码**：长按 `BOOT` 约 10 秒（串口会打印提示），密码清空回 `admin`。
没有实体按键的板子把 `AUTH_FACTORY_RESET_PIN` 改成 `-1` 关掉此功能。

**换路由器后失联**：等 **3 分钟**。固件判定长时间离线会自己开出 `PrintServer-Setup`，
连上去重配即可，不用拆机重刷。

---

## 添加打印机

### Windows

1. **设置 → 蓝牙和其他设备 → 打印机和扫描仪 → 添加设备**
2. 搜索完成后点 **手动添加**
3. 选 **使用 TCP/IP 地址或主机名添加打印机**
4. 类型 `TCP/IP 设备`，地址填板子 IP（或 `printserver.local`）
5. 查询大概率超时 → 直接点 **自定义**
6. 协议 **Raw**，端口 **`9100`**
7. **取消勾选「SNMP 状态已启用」**（重要，否则 Windows 判定打印机离线）
8. 选打印机驱动
9. 用**真实打印作业**验证，不要用固件测试页（理由见[故障排查](#故障排查)）

### macOS

**系统设置 → 打印机与扫描器 → 添加打印机** → **IP** 标签页 →
协议 **HP Jetdirect - Socket** → 填 IP → **使用 → 选择软件** 搜型号。

### Linux（CUPS）

```bash
sudo lpadmin -p WiFiPrinter -E -v socket://192.168.1.50:9100 -m everywhere
# 或网页：http://localhost:631 → Administration → Add Printer
```

### 手机（IPP / AirPrint）

同一 WiFi 下，iOS 直接从「打印」里选到设备；Android 需装支持 IPP 的打印服务。
前提是打印机自己认识 PDF / PWG Raster / URF（见[不支持](#支持与不支持)）。
对 1390 这类老照片机，手机端只能走支持 Raw 9100 的第三方 App。

### Linux 主机接管（USB/IP）

```bash
sudo modprobe vhci-hcd
sudo usbip list -r 192.168.1.50          # 看到 busid，形如 1-3
sudo usbip attach -r 192.168.1.50 -b 1-3 # 打印机出现在本地 lsusb
```

之后 CUPS 里就有这台 USB 打印机，用它那套完整驱动 + Ghostscript 驱动。
**注意**：USB/IP 占用期间 `:9100` 不可用，两条通路互斥。

---

## 网页管理界面

`http://printserver.local`。未登录整页盖登录框；登录后 5 个页签，每 2 秒刷新：

| 页签 | 内容 |
|---|---|
| **概览** | 打印机状态与型号、当前作业（已接收 / 已发 USB / 回读 / 速率 / 用时）、累计统计、运行时长、内存 |
| **打印机** | USB 链路全量信息（VID:PID、接口、协议、端点、包长、是否 IPP-over-USB、重枚举次数）、Device ID、序列号；测试页 / 软复位按钮（异步，结果走 toast） |
| **网络** | WiFi、SSID、IP、**当前 / 近 1 分钟最差 / 均值信号**、离线时长、断线与重连次数、重试间隔、后备 AP 状态；扫描切换网络 |
| **系统** | 修改管理员密码、OTA 升级、重启、恢复出厂 |
| **添加打印机** | Windows / macOS 图文步骤，IP 自动填入 |

默认密码未改时顶部挂横幅；打印机进入「无响应保护期」也会提示。

---

## HTTP API

除状态和登录外，**所有接口都要登录**（Cookie `PSID` 或 `Authorization: Bearer <token>`）。
跨站请求会被拒绝（校验 `Origin` 与 `Host` 一致）。

| 端点 | 方法 | 登录 | 说明 |
|---|---|---|---|
| `/api/auth/state` | GET | 否 | `authenticated` / `needChange` / 是否被锁定 |
| `/api/auth/login` | POST | — | `user` `pass`，成功返回会话 Cookie |
| `/api/auth/logout` | POST | 是 | 销毁当前会话 |
| `/api/auth/passwd` | POST | 是 | `oldpass` `newpass` `newpass2`，成功后重签会话 |
| `/api/status` | GET | 是 | 完整状态 JSON（打印机 / USB / 作业 / 统计 / 网络 / 异步操作结果） |
| `/api/debug` | GET | 是 | 崩溃日志、boot 计数、悬挂传输计数 |
| `/api/wifi/scan` | GET | 是 | 后台扫描，返回上次结果（非阻塞） |
| `/api/wifi/connect` | POST | 是¹ | `ssid` `password`，返回新 IP |
| `/api/testpage` | POST | 是² | 提交测试页，**立即返回**，轮询 `/api/status` 的 `ops.testpage` |
| `/api/printer/reset` | POST | 是² | 同上，结果看 `ops.usbReset` |
| `/api/restart` | POST | 是² | 重启 |
| `/api/factory-reset` | POST | 是¹ | 清除 WiFi + 管理员密码并重启 |
| `/api/ota` | POST multipart | 是² | OTA，字段名 `firmware`，可加 `?pw=` 传 `OTA_PASSWORD` |

¹ 允许在「还没改掉出厂密码」时调用 —— 配网和救场不能被锁死。
² `needChange` 状态返回 `403 {"error":"default-password"}`。

`ops` 状态码：`0` 无请求 / `1` 执行中 / `2` 成功 / `3` 失败。
异步化是因为这些操作要发 USB 控制或批量传输，遇到不应答的打印机会阻塞几十秒。

---

## 机型行为特征库

兼容性的关键从来不是「认不认得出这台打印机」，而是**知不知道它的坏脾气**：

- 对所有控制传输长时间 NAK（Epson 1390）→ 不能发 Device ID
- 作业开头被塞 `ESC @` 就卡住不出纸（HP 走 `@PJL` 流）
- 作业结束补 FF 反而永久等待下一页（PCLm 尾部自带 `%%EOF`）
- 暴露的不是标准打印机类，而是厂商私有接口（部分 HP MFP）

这些**无法从描述符推断，只能一台台实测**。`printer_db.cpp` 维护 `VID:PID → 特征位` 表，
命中后覆盖 `config.h` 的全局默认值。

### 特征位

| 标志 | 含义 |
|---|---|
| `Q_WANT_ESC_RESET` | 作业开头**要**发 `ESC @`（多数 ESC/P 爱普生、针式机） |
| `Q_NO_ESC_RESET` | **千万别**发 `ESC @`（HP 等 `@PJL` 流：发了卡在「正在打印文档」） |
| `Q_TAIL_NONE` | 作业结束**什么都不补**（PCLm / PDF / PS / PJL，自带收尾标记） |
| `Q_TAIL_FF` | 作业结束**无条件补 FF**（ESC/P、ESC/POS，流式格式没有收尾标记） |
| `Q_NO_CTRL` | 完全不应答控制传输，禁发 Device ID / 字符串描述符 / 端口状态（1390） |
| `Q_PASSTHROUGH` | **纯透传**：作业头尾、异常路径、自愈统统不注入任何字节 |
| `Q_NO_FAIL_RECOVER` | 禁止故障自愈序列（少数机型被收尾序列打死） |
| `Q_NO_ZLP` | 不补零长度包（极少数老机把 ZLP 当错误） |
| `Q_NO_TEXT_TESTPAGE` | 禁用纯文本测试页（主机型收下不渲染也不结束，会卡死到断电） |
| `Q_VENDOR_IFACE` | 允许接管厂商私有（`0xFF`）批量接口 |
| `Q_NO_BIDIR` | 不回读 Bulk IN |
| `Q_IPP_OVER_USB` | 已知 IPP-over-USB 设备（描述符哪怕不是标准 `7/1/4`） |
| `Q_WANT_FORMFEED` / `Q_NO_FORMFEED` | **已废弃**，用 `Q_TAIL_FF` / `Q_TAIL_NONE` |

两个都没写 = `Q_TAIL_AUTO`：按作业末尾字节自动判断（安全兜底）。

### 补录一台新机

1. 插上打印机，看 `/api/status` 记下 `usb.vid` / `usb.pid` / `usb.protocol`
2. 发一份**真实打印作业**，看 `job.headHex` / `job.tailHex`：

   | `headHex` 解码 | 数据流 | 收尾策略 |
   |---|---|---|
   | `1B 40` / `1B 25 2D 31 32 33 34 35 58` | 裸 ESC/P | `Q_TAIL_FF`，可配 `Q_WANT_ESC_RESET` |
   | `00 00 00 1B 01 40 45 4A 4C`（`ESC 01 @EJL 1284.4`） | Epson Exit Packet Mode | **`Q_PASSTHROUGH`**（一个字都别加） |
   | `25 50 44 46` / `25 50 43 4C 6D` | PCLm / PDF（主机型） | `Q_NO_ESC_RESET` + `Q_TAIL_NONE` |
   | `1B 25 ... 40 50 4A 4C` | PJL + PCL | `Q_NO_ESC_RESET` + `Q_TAIL_NONE` |

   判断依据是 `tailHex`：驱动自己已经发了收尾（`%%EOF` / `UEL` / `@PJL EOJ` /
   `JE BC=1 00` / `ESC 00 00 00`）时，固件再补 FF 就是「作业结束后又开一页」。

3. 在 `PROFILES` 里加一行，`PID` 填 `PID_ANY` 表示对该厂商全线生效：

   ```c
   {0x04B8, 0x9999, "Epson 某某机型", Q_NO_CTRL | Q_PASSTHROUGH},
   ```

4. **只填有实测依据的标志**。没证据的机型让它走全局默认更安全
5. 重编译 + OTA

> ⚠️ 别改 `config.h` 的全局默认值去迁就一台机器 —— 那会影响所有未登记的机型。

---

## 作业生命周期

`:9100` 同时只受理一个连接（有意设计：半路混进另一条数据流会打印错位）。

```
客户端连接
  ├─ 15s 内没收到任何数据        → 判定空连接，丢弃
  ├─ 收到数据
  │    ├─ 全部字节成功下发 + TCP 静默 1.5s → 主动 shutdown(SHUT_WR) 发 FIN，作业完成
  │    ├─ 传输失败 / 打印机断连           → _abortJob()：标记失败 + 干净关闭 + 按机型复位
  │    ├─ 120s 没成功下发任何字节         → 判定打印机失联，释放通道
  │    ├─ 10 分钟未结束                   → 强制回收通道
  │    └─ 60s 双向都无收发                → 最终兜底断开
  └─ 通道被占用时的连接                   → 立即拒绝并计数（stats.rejected）
```

**为什么要主动关闭**：Raw 9100 是字节流，没有长度字段也没有 EOJ 握手。
Windows 发完作业后**等服务端关 TCP**，旧固件在等 Windows 先关 —— 双方互等到系统超时（约 60 秒），
表现为队列长时间停在「正在打印」、后续全堵住。

v2.6.0 起，数据全量下发 + TCP 静默 `JOB_DATA_IDLE_END_MS`(1500ms) 就判定作业结束；
**v2.6.3 起这一步显式发 FIN** —— 只 `stop()` 的话客户端经常收不到 FIN
（lwIP 在半关闭连接上直接释放，或 abort 成 RST），队列照样挂着等超时。

**收尾策略按机型区分**，固件「好心」补的换页符反而会让打印机永久等下一页：

| 打印语言 | 驱动自带收尾 | 固件补 FF |
|---|---|---|
| PCLm / PDF | `%%EOF` | 不补 |
| PCL5/6 + PJL | `UEL` + `@PJL EOJ` | 不补 |
| PostScript | `0x04` | 不补 |
| ESC/P（1390） | 无 | 补（但 1390 走 `Q_PASSTHROUGH`，实际不补） |
| ESC/POS（票据机） | 无 | 补 |

---

## 故障排查

| 现象 | 处理 |
|---|---|
| 串口无 `[USB]` 输出 | 是不是用了普通充电线；确认接在「USB」口不是「UART」口 |
| 打印机一直「未连接」、`enumCount=0` | **先确认核心是 3.3.10**。然后断电重启打印机、重插 USB |
| `未找到可用打印接口` | 描述符里没有标准打印机类也没被放行的私有接口。记下 `vid/pid`，在 `printer_db.cpp` 补一行 + `Q_VENDOR_IFACE` |
| 型号显示成 `VID:xxxx` | Device ID 读失败（该机型配了 `Q_NO_CTRL`）。不影响打印 |
| Windows 打印机显示离线 | 端口设置里取消勾选 SNMP |
| Windows 添加时卡在查询 | 直接点自定义，指定 Raw / 9100 |
| macOS 队列暂停 | 手动恢复，确认协议是 HP Jetdirect |
| 打印乱码 | 驱动选错；或 `TESTPAGE_PREPEND_ESC_RESET` 与机型不匹配 |
| 用着用着失灵，重启才好 | 空连接占住通道（Windows 双向探测、被取消作业留下的半开连接）。固件会自动回收，不用重启 |
| **固件测试页打不出来** | **主机型（GDI / PCL3GUI / PCLm）只认官方驱动生成的数据流**，纯文本测试页不出纸，**这不算故障**。网页上该按钮已自动隐藏，请用真实作业验证 |
| **HP 卡在「正在打印文档」不出纸** | 作业头被塞 `ESC @` 打乱 PJL 解析。`JOB_RESET_ON_CONNECT` 与 `TESTPAGE_PREPEND_ESC_RESET` 必须 0，库里 HP 全线已是 `Q_NO_ESC_RESET` |
| 队列任务全堆着，前一份一直「打印中」 | 作业结束没发 FIN。v2.6.3 已修；自查：固件 `lastJob` 显示「已完成」但 Windows 队列还挂着 → 就是这个 |
| `sent=0` / `USB 传输失败 status=0` | **打印机在拒收**：缺粉、卡纸、残留作业、面板报错。**断电重启打印机**并检查耗材，固件只能快速失败保护 |
| 打印到一半重复好多份 | Windows 后台在重试失败作业。看 `stats.failedJobs` 与 `job.bytesSent` 对不对得上，一般是打印机侧先出问题 |
| 多页大任务中断 + 重复内容（1390 实锤） | `job.headHex` 若是 `1B 01 40 45 4A 4C 20 31 32 38 34 2E 34`，走的是 EJL 1284.4 封包流，头尾绝不能注入裸控制字节。v2.9.5 起该机型 `Q_PASSTHROUGH + Q_NO_FAIL_RECOVER + Q_NO_ZLP` |
| 打印机出问题时网页也卡住 | v2.5.0 起打印服务已独立成 FreeRTOS 任务，管理页始终可用 |
| `bootCount` 持续增长 | 板子在崩溃循环。**先物理拔掉打印机**，等 uptime 稳定上涨后再 OTA。`/api/debug` 有 `lastCrashStage` |
| OTA 失败 / 403 `default-password` | 还在用 `admin/admin`，先登录改密码 |
| OTA 推上去版本号没变 | 固件启动就崩，bootloader 回退旧分区了。看 `/api/debug` 的 `bootStage` / `lastCrashStage` |
| 忘掉管理员密码 | 长按 BOOT 10 秒清空（串口会打印提示） |
| 换路由器后失联 | 等 3 分钟，自动开 `PrintServer-Setup` 热点重配 |
| WiFi 连着但打印卡、时断时续 | 看「网络 → 近 1 分钟最差信号」，长期低于 -75 dBm 请挪近或加中继；`WIFI_DISABLE_SLEEP` 保持 1 |
| `printserver.local` 打不开 | Windows 缺 mDNS，直接用 IP；Arduino IDE 看不到网络端口就装 [Bonjour Print Services](https://support.apple.com/kb/DL999) |

---

## 配置参数

编辑 `config.h`。

> ⚠️ 带 `JOB_` / `ENABLE_` / `USB_` 前缀的打印相关项**只是「查不到该机型时的全局默认值」**。
> 库里登记过的机型以 `Q_*` 标志为准 —— 要改某台机器的行为改库，不要改这里。

### 身份与网络

| 参数 | 默认 | 说明 |
|---|---|---|
| `FW_VERSION` | `2.9.5` | 固件版本 |
| `PRINTER_NAME` | `ESP32_PrintServer` | 设备标识名 |
| `MDNS_HOSTNAME` | `printserver` | 浏览器用 `http://printserver.local` |
| `RAW_SOCKET_PORT` | `9100` | JetDirect / AppSocket |
| `WEB_SERVER_PORT` | `80` | 管理页 |
| `USBIP_PORT` | `3240` | USB/IP 服务端口 |
| `AP_SSID` / `AP_PASSWORD` | `PrintServer-Setup` / `12345678` | 配网热点 |

### USB 与传输

| 参数 | 默认 | 说明 |
|---|---|---|
| `USB_XFER_CHUNK` | `8192` | 单次批量传输大小 |
| `USB_XFER_TIMEOUT_MS` | `30000` | 单块传输等待上限（走纸时会 NAK，要留余量） |
| `PRINT_CHUNK` | `16384` | 每次从 TCP 取多少字节 |
| `PRINT_BUFFER_SIZE` | `256 * 1024` | PSRAM 打印缓冲 |
| `CTRL_WRITE_TIMEOUT_MS` | `2000` | 控制性小写入（`ESC @`、`FF`）超时。**别用 30s**：两三个字节都不收说明机器已死，等 30 秒只会把 `task()` 堵住 |
| `ENABLE_BIDIR_PASSTHROUGH` | `1` | 读 Bulk IN 并回写 TCP |
| `USB_RX_BUFFER_SIZE` | `4 * 1024` | IN 端点回读环形缓冲 |
| `IN_STREAM_RETRY_MS` | `3000` | 回读中断后多久重试（1284.4 机型必需） |
| `ENABLE_PORT_STATUS_POLL` | `0` | **保持 0**：不少 GDI 机型对它长时间 NAK，而本 IDF 的 `timeout_ms` 无效、EP0 无取消接口，会留下永久悬挂的传输 |
| `USB_HOST_TASK_STACK_SIZE` | `10240` | 栈不能低于此值 |

### 故障自愈

| 参数 | 默认 | 说明 |
|---|---|---|
| `USB_RECOVER_ENABLED` | `1` | 失败时尝试拉回打印机（halt → SOFT_RESET → 收尾序列） |
| `USB_RECOVER_AFTER` | `1` | **必须为 1**：写失败后立刻结束作业，「连续 2 次」在同一份作业里永远凑不满 |
| `USB_RECOVER_MAX_PER_JOB` | `2` | 单份作业最多自愈几次 |
| `USB_RECOVER_MAX_WAIT_MS` | `1200` | 自愈各步骤等待上限 |
| `USB_STALL_MAX_TIMEOUTS` | `3` | 连续几次无回调判定打印机失联 |
| `USB_STALL_COOLDOWN_MS` | `15000` | 冷却期，期间新作业立刻失败返回，保住网页响应 |

### 作业生命周期

| 参数 | 默认 | 说明 |
|---|---|---|
| `JOB_NODATA_TIMEOUT_MS` | `15000` | 连上后一直没数据 → 判定空连接 |
| **`JOB_DATA_IDLE_END_MS`** | `1500` | **数据全量下发后 TCP 静默多久判定作业结束**并主动发 FIN |
| **`FIN_RELEASE_MS`** | `200` | 发 FIN 后多久才 `stop()`。**不能设 0**：FIN 没刷出去就 close 会 abort 成 RST，客户端照样读不到结束信号 |
| `JOB_IDLE_TIMEOUT_MS` | `60000` | 异常连接最终兜底，必须大于 `USB_XFER_TIMEOUT_MS` |
| `JOB_TX_STALL_MS` | `120000` | 作业期内这么久没成功下发过任何字节 → 释放通道 |
| `JOB_MAX_DURATION_MS` | `600000` | 单作业最长占用 10 分钟 |
| `JOB_RESET_ON_CONNECT` | `0` | 作业开头发 `ESC @`。Epson 可设 1，**HP 必须 0** |
| `JOB_APPEND_FORMFEED` | `0` | 作业结束补 FF。**保持 0** —— 多补一个就是「结束后又开一页」 |
| `TESTPAGE_PREPEND_ESC_RESET` | `0` | ESC/P 可设 1；**PCL / PS / HP 必须 0** |

### 任务与看门狗

| 参数 | 默认 | 说明 |
|---|---|---|
| `PRINT_TASK_CORE` | `0` | 打印任务跑哪个核心（`loop()` 在 core1，分开才真正并行） |
| `PRINT_TASK_PRIORITY` / `_STACK_SIZE` | `2` / `8192` | |
| `USB_HOST_TASK_PRIORITY` | `5` | |
| `WDT_TIMEOUT_SEC` | `60` | |
| `LED_PIN` | `2` | 无外接 LED 不影响功能 |

### WiFi

| 参数 | 默认 | 说明 |
|---|---|---|
| `WIFI_DISABLE_SLEEP` | `1` | 关掉 modem sleep。**保持 1**，节能模式造成数百毫秒抖动 |
| `WIFI_RETRY_MIN_MS` / `_MAX_MS` | `5000` / `60000` | 重连退避上下限 |
| `WIFI_REASSOC_AFTER` | `3` | 失败几次后升级为完整重新关联 |
| `WIFI_DRIVER_RESET_AFTER` | `8` | 再失败几次后复位 WiFi 驱动 |
| `WIFI_FALLBACK_AP_MS` | `180000` | 离线多久开后备 AP（`0` 关闭） |
| `WIFI_RSSI_SAMPLE_MS` / `_HISTORY` | `2000` / `30` | 信号采样（约 1 分钟窗口） |

### 安全与内存

| 参数 | 默认 | 说明 |
|---|---|---|
| `AUTH_ENABLED` | `1` | 调试可临时置 0 |
| `AUTH_DEFAULT_USER` / `_PASS` | `admin` / `admin` | 出厂账号，登录后强制修改 |
| `AUTH_SESSION_TIMEOUT_MS` | `2h` | 会话空闲过期 |
| `AUTH_MAX_SESSIONS` | `6` | 槽位满时**淘汰最久没活动的**，不会被拒 |
| `AUTH_MAX_FAILS` | `5` | 连续错几次开始锁定（指数退避，上限 30 分钟） |
| `AUTH_FACTORY_RESET_PIN` | `0` | 长按 10 秒清空密码，`-1` 关闭 |
| `OTA_PASSWORD` | 空 | **建议设置**，否则局域网内谁都能刷机 |
| `HEAP_GUARD_MIN_KB` / `_HOLD_MS` | `24` / `30000` | 堆低于该值并持续这么久 → 空闲时自动重启，防碎片化变永久故障 |

---

## 文件结构

```
ESP32S3_PrintServer_v2/
├── ESP32S3_PrintServer_v2.ino  主程序（setup / loop / 全局对象 / 任务启动）
├── config.h                    配置参数（机型库未命中时的全局默认值）
├── printer_db.h / .cpp         机型行为特征库（VID:PID → Q_* 标志，26 条）
├── usb_printer.h / .cpp        USB Host 驱动（枚举、匹配、控制/批量传输、自愈、回读）
├── print_server.h / .cpp       Raw Socket 9100（独立 FreeRTOS 任务）
├── ipp_server.h / .cpp         IPP 服务端，独立监听 631，边收边转 USB
├── airprint_mdns.h / .cpp      mDNS：_ipp._tcp + _universal 子类型（AirPrint 免驱发现）
├── usbip_server.h / .cpp       USB/IP 服务端（3240），把 URB 翻译成真实 USB 传输
├── web_ui.h / .cpp             网页管理界面 + HTTP API + 鉴权接入
├── wifi_manager.h / .cpp       配网 / 三级重连 / 退避 / 后备 AP / 信号采样
├── auth.h / .cpp               管理员鉴权（SHA-256 加盐、会话、暴力破解锁定）
├── crashlog.h / .cpp           崩溃日志（RTC 内存，重启后经 /api/debug 读出）
├── LICENSE                     MIT
└── README.md
```

---

## 已知限制

1. **单作业通道**。三条通路共享一个 USB 出口，同一时刻只受理一个作业（有意设计）。
   批量排队时建议在 Windows 端口里取消勾选「启用双向支持」。
2. **打印机内部死锁无法软件复位**。打印机自身固件 / FIFO 卡死（`sent=0`、
   `status=0` 且面板报错）时，端点 halt/flush/clear、SOFT_RESET、收尾序列实测都无效，
   **只能断电重启打印机**。根治需要设备句柄重枚举或可控 VBUS 的硬件兜底，尚未实现。
3. **只有 Full-Speed 12Mbps**，实测吞吐 0.6–1 MB/s。高速机 / 大幅面照片会很慢，
   升级路径是 ESP32-P4（内置 High-Speed PHY）。
4. **主机型打印机不支持手机免驱**。手机发 PDF / URF，设备端做栅格化需要 Ghostscript 级
   的引擎和几十 MB 内存 —— 这是硬件边界，不是实现难度问题。树莓派能行是因为它跑完整 Linux。
5. **IPP-over-USB 只识别不代理**。能认出 `7/1/4` 接口，但还不做 IPP 请求代理。
6. **USB/IP 尚未完成 Linux / CUPS 端到端验收**。只转发当前 claim 的打印接口端点，
   不是完整的多接口 USB 虚拟化。

---

## 版本历史

| 版本 | 日期 | 要点 |
|---|---|---|
| **v2.9.5** | 2026-11-15 | USB 传输所有权重构；1390 关掉破坏性自愈（详见下） |
| v2.9.x | 2026-10-22 起 | USB/IP 调试与 USB 传输层收口，逐条见 `git log` |
| v2.8.x | 2026-10-21 | USB/IP 导出第一期：远端 Linux 的 CUPS 接管打印机 |
| v2.7.x | 2026-10-21 | IPP / AirPrint 前端（631）+ mDNS `_universal` 免驱发现 |
| v2.6.x | 2026-09~10 | 纯透传成为全局默认、作业结束主动发 FIN、OTA 不再断开打印机 |
| v2.5.x | 2026-09 | 管理员登录、打印服务独立成 FreeRTOS 任务、WiFi 三级重连、后备 AP |
| v2.4.0 | 2026-08 | 机型行为特征库、双向回读、USB 接口多级打分 |

### v2.9.5 — 传输所有权 + 1390 收尾

- **ctrl / bulk 各用独立 transfer 句柄与完成标记**，取代共享的 `_xferDone`；
  超时后所有权交给对应的迟到回调，不再错误 free 在飞传输（这类野指针会让驱动队列
  残留后稍后 panic，症状是「走到 ready 才崩」）。
- **关闭路径可重试**：拿不到完成回调就不 free、也不丢设备句柄，下一轮继续派发与关闭。
- 常驻 IN 可挂起 / 恢复，USB/IP 接管期间不与 URB 抢端点。
- 身份识别顺序：机型库 → IEEE 1284 Device ID → USB 字符串 → 厂商 VID → 通用名。
  库里已有名称时跳过注定失败的控制探测（零泄漏）。
- **1390 加 `Q_NO_FAIL_RECOVER` + `Q_NO_ZLP`**：该机型自愈实际只剩 halt + flush，
  等于在数据流里挖洞 → 单页光栅被截断（凑不齐 `JE BC=1 00` 收尾上下文）→ 空走出纸；
  多页每个中断点留一个缺口 → 错行 / 乱码。宁可让这份作业失败重打，也不破坏数据流。

### v2.8.x — USB/IP

板子把 USB 设备导出给远端 Linux：Linux 发 URB，固件翻译成真实的控制 / 批量传输。
需要「通用控制传输」和「按 URB 精确收发」（常驻 IN 流会跟 URB 抢端点，所以接管期间挂起）。
`busid` 形如 `1-<设备地址>`，必须与 IMPORT 请求一致。

### v2.7.x — IPP / AirPrint

- 独立 631 端口，自己解析 HTTP + IPP，不用 Arduino `WebServer` ——
  后者会把 multipart 拆散或整个 body 缓冲进内存，而一张 A4 彩色光栅几 MB，
  2MB PSRAM 下**必须边收边转 USB**。
- 免驱发现的关键是 `_ipp._tcp` 的 **`_universal` 子类型**。Arduino 的
  `MDns.addService()` 不暴露子类型，得调 ESP-IDF 原生 `mdns_service_subtype_add_for_host()`。
- IPP 规范里 `Get-Printer-Attributes` 等查询用 **HTTP GET**（无 body），只有
  `Print-Job` / `Create-Job` / `Cancel-Job` 用 POST —— 只实现 POST 的话手机端发现不了设备。
- 定位文件数据起点：`end-of-attributes-tag` 是**单字节 `0x03`**（RFC 8010 §3.1），
  不是 4 字节大端编码；匹配要用字节级 `findBytes()`，`String::indexOf` 对含 `\0` 的标记不可靠。

### v2.6.x 关键修复

- **v2.6.8** OTA 不再 `usb_host_device_close()` 断开打印机 —— 副作用是升级后 1390
  不再自动重枚举，得靠人拔插 USB 线。只挂起自己的 USB 任务就够了。
- **v2.6.6** 写 flash 期间必须停掉所有碰 USB 的代码：`Update.begin()` 擦除分区时 flash cache
  被禁用，任何从 flash 取指令都会 panic（Cache disabled but cached memory region accessed）。
  症状极迷惑：上传全成功 → 重启 → 分区写一半 → bootloader 回退 → **版本号还是旧的**。
  另有坑：打印任务订阅了 WDT，`vTaskSuspend` 挂起后没人喂狗，照样 panic ——
  要让它暂停业务并自己取消 WDT 订阅。
- **v2.6.4/2.6.5** Epson 1390 彩照出白纸 → 引入 `Q_PASSTHROUGH`，并把
  `JOB_APPEND_FORMFEED` 默认改为 0。驱动尾部已有
  `REMOTE1` / `JE BC=1 00` / `ESC 00 00 00` 完整收尾，再补裸 `FF` 就是又开一页。
- **v2.6.3** 陷阱记录：`WiFiClient::operator bool()` 返回 `connected()`，而对端发来 FIN
  后它立刻变 false —— 但 socket 还在，且**正是我们欠对端一个 FIN**。
  所以判断「连接还能不能用」必须看 `fd()`，**绝不能写 `if (_client)`**。

---

## 许可证

MIT License — 详见 [LICENSE](LICENSE)

## 致谢

基于 ESP-IDF USB Host 栈与 Arduino-ESP32 核心实现。
接口打分策略参考了 Linux `ipp-usb` 对惠普 MFP 非标准接口组合的处理。
