# ESP32-S3 WiFi Print Server

**当前版本：v2.6.0**

把 USB 打印机变成网络打印机。Windows / macOS / Linux 装原厂驱动后，通过标准 **Raw Socket 9100** 直接打印，
不需要装任何打印服务器软件。

```
[电脑] ──WiFi── [ESP32-S3] ──USB OTG── [打印机]
```

核心特性一句话：**固件对打印数据完全透明**，不理解任何页面描述语言，
所以 PCL / PostScript / ESC-P / ESC/POS / PJL / PCLm / GDI（主机型）全部可用 ——
渲染由电脑端驱动完成，板子只负责把字节搬到 USB 上。

---

## 目录

- [先看这个：能不能用](#先看这个能不能用)
- [硬件要求](#硬件要求)
- [编译与烧录](#编译与烧录)
- [首次配网](#首次配网)
- [添加网络打印机](#添加网络打印机)
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

## 先看这个：能不能用

### 驱动在电脑上，板子只当管道

这一条最重要，先说清楚，能省掉大量误解：

> **打印机驱动跑在你的电脑上，不是跑在板子上。**
>
> 电脑上的驱动把文档渲染成打印机能懂的字节流（ESC/P2、PCLm、PJL、PostScript…），
> 通过网络打印端口（9100）发给板子；板子把它**原样转发**到打印机的 USB 批量端点。
>
> 所以：
>
> - 板子**不需要**、也**没有**「调用打印机驱动」这回事
> - 驱动的一切能力（分辨率、色彩、纸型、双面……）都在电脑端生效，板子不参与
> - 板子唯一该做的，是**别破坏驱动发出来的字节**
> - 「选高质量相片」出问题 = 驱动发出来的字节被板子动了手脚（或者链路把它弄丢了），
>   **不是板子没用驱动**
>
> 所谓「纯透传」就是这个意思：板子一个字节都不加、一个字节都不改。
> 机型库里标 `Q_PASSTHROUGH` 的机型走的就是这个模式（实测 Epson 1390）。
>
> ### 两条通路（v2.7.0 起）
>
> | 通路 | 端口 | 谁能用 | 说明 |
> |---|---|---|---|
> | Raw 9100 | `9100` | 装了驱动的电脑 | 驱动在客户端渲染，板子原样转发。**照片质量最好** |
> | IPP / AirPrint | `631` | **手机、平板**（iOS / Android） | 客户端渲染成位图或 PDF 发过来，板子做协议封装 + 转发 |
>
> 两条通路**互斥**（同一时刻只受理一个作业），避免数据流交错成乱码。
> 详见[打印服务器方案的三层结构](#打印服务器方案的三层结构)。

### 为什么默认就该是纯透传

市面上老牌打印服务器（D-Link 友讯 **DPR-2000** 等）之所以能做到「通用支持所有打印机」，
靠的不是它更懂打印机，恰恰是**它什么都不懂、不加**：

| | 传统打印服务器（DPR-2000 等） | 本项目 v2.6.5 起 |
|---|---|---|
| 作业开头 | 不动 | 不动（`JOB_RESET_ON_CONNECT=0`） |
| 作业结尾 | 不动 | 不动（`JOB_APPEND_FORMFEED=0`） |
| 控制传输 | 基本不碰 | 只在需要识别型号时发，且按机型库授权 |
| 打印机状态 | 简单透传 | 透传 + 可选状态轮询 |

**所以未登记的机型插上就能用**——这才是「通用」的真正含义：
默认走最保守的透传，只有**有实测依据**的机型才在 `printer_db.cpp` 里单独登记例外。
反过来做（默认自作主张补字节、逐台打补丁）就是过去的老路，
Epson 1390 的彩色相片白纸就是这么来的。

> ⚠️ USB 物理层意义上的「透传」（电脑的 USB 数据直接出现在打印机口上）在本硬件上做不到：
> ESP32-S3 只有一路 USB OTG，板子做了 Host（连打印机）就没有 Device 侧接电脑。
> 但这不重要 —— 上面这种「网络进、USB 出」的软件透传，在效果上等价，
> 而且能额外提供跨网段、mDNS、网页管理这些 USB 透传给不了的能力。

### 支持

| 场景 | 说明 |
|---|---|
| 标准 USB Printer Class（接口类 `0x07`） | 最常见情况，插上就能用 |
| GDI / 主机型打印机 | 渲染在电脑端驱动完成，板子只透明转发。实测 Epson Stylus Photo 1390、HP LaserJet MFP |
| 厂商私有接口（`0xFF`） | 机型库放行后接管。已放行 HP（含非标准 `255/9/1`）、Canon |
| IPP-over-USB（`7/1/4`） | 能识别并接管，目前仍透传不代理 |
| 热敏票据机 / 标签机 | Zebra、TSC、Star、Gprinter、QinHeng 已在库中 |

### 不支持

| 场景 | 原因 |
|---|---|
| 高速打印机（High-Speed 480Mbps 独占） | ESP32-S3 只有 Full-Speed 12Mbps，实测 0.6–1 MB/s。A3/2880dpi 大图会明显慢 |
| 一体机的扫描 / 传真 | 只转发打印通道 |
| **手机端免驱打印** | iOS/Android 免驱发的是 PDF / URF，主机型（GDI）根本不认识。要在设备端做 PDF→原机语言的栅格化，MCU 做不到。主机型请在 Windows / macOS / Linux 装原厂驱动使用 |
| 打印中需要双向状态查询 | 支持回读（见下），但**同一时刻只受理一个连接**，打印期间的状态查询连接会被拒绝 |

### 双向回读（v2.4.0 起）

固件在 Bulk IN 端点常挂一条读传输，把打印机吐出来的字节**原样写回 TCP**：

- Windows 端口设置里可以真正勾上「启用双向支持」
- PJL 状态查询、缺纸/卡纸应答、票据机 ACK 都能回传
- 这是后续做 IPP 代理的前置能力

**注意**：实测 HP 机型在常驻回读下会拒收打印数据，机型库已对 HP 整族关闭回读（`Q_NO_BIDIR`）。
若你的机器被回读干扰，在 `printer_db.cpp` 里给它加 `Q_NO_BIDIR`。

### 打印服务器方案的三层结构

搞清这一层，就知道自己该做什么、不该做什么。打印链路可以拆成三层：

| 层 | 干什么 | 谁来做 |
|---|---|---|
| 1　渲染层 | 把 PDF / 图片 / 文字变成打印机认识的语言（ESC/P2、PCLm、UEL…） | 电脑上的原厂驱动，或服务端的驱动 |
| 2　协议层 | 网络上用什么协议传 | Raw 9100（本项目）／IPP / AirPrint |
| 3　管道层 | 字节从网络搬到 USB | 打印服务器 |

市面上的方案都是这三层的不同组合：

- **D-Link 友讯 DPR-2000 等打印服务器** = 只有第 3 层。电脑自己装驱动、自己渲染，
  打印服务器只搬字节 —— 所以它「通用支持所有打印机」，但**前提是客户端有驱动**。
- **树莓派 + CUPS** = 第 1 层（CUPS 里的 ESC/P-R 驱动）+ 第 2 层（IPP）+ 第 3 层
  （CUPS 底下的 `usb://` 后端，本质还是管道）。它多出来的能力是
  **服务端渲染**，于是手机发 PDF 也能打。
- **本项目** = 第 2 层（Raw 9100）+ 第 3 层。第 1 层交给客户端的驱动。

**为什么本项目不做服务端渲染（CUPS 路线）**：要在设备端把 PDF 栅格化成原机语言，
需要 Ghostscript 级别的 PDF 引擎和几十 MB 内存，MCU 做不到 —— 这是硬件边界，
不是实现难度问题。树莓派能行是因为它跑的是完整 Linux。

<details>
<summary><b>展开：为什么「在 ESP32 上装 CUPS」不是一个性能问题，而是方向问题</b></summary>

CUPS 不是一个协议，是一整套**用户态软件**。它的依赖链在 MCU 上缺的不是算力：

| CUPS 需要 | ESP32-S3 | 性质 |
|---|---|---|
| POSIX 用户态（`fork`/`pipe`/Unix socket/动态链接 `.so`/沙箱） | 裸机 RTOS，没有进程概念 | **架构性缺失** |
| 过滤器链是**外部可执行进程**（`pdftoraster`、`gstoraster` 用管道串联） | 没有 `fork`/`exec`，起不了子进程 | **架构性缺失** |
| Ghostscript 渲染（一页 A4 300dpi 彩色 = 25MB 位图） | 2MB PSRAM / 240MHz | 算力不足（换芯片可解） |
| 驱动库（几千个 PPD） | 我们的机型库 26 条 | 维护成本（换芯片也解决不了） |

**前两项是架构性的，换芯片也解决不了** —— 即便换 ESP32-P4 也一样：
它虽然有 768KB SRAM + 16MB PSRAM，但**没有 MMU**，跑不了主流 Linux。
所有「ESP32 上跑 Linux」的方案（刷 OpenWrt 之类）本质都是**外挂一颗 Linux 主控**，
那就已经是树莓派路线了，BOM 从 5 元变成 30~80 元。

**但关键在于：CUPS 从来不是跑在打印服务器上的。** 它跑在 **Linux 主机**上。
所以让 ESP32 接进 CUPS 生态，正确姿势不是「把 CUPS 搬过来」，而是
**把 USB 暴露给远端的 Linux** —— 这正是 **USB/IP** 协议
（内核模块 `drivers/usbip`）干的事，已有 ESP32-S3 的开源实现：

```
Linux 主机（已装 CUPS）─── USB/IP over TCP :3240 ───▶ ESP32-S3 ──USB──▶ 打印机
   usbip attach 之后，打印机就出现在 Linux 里，CUPS 用它那套完整驱动库 + Ghostscript 驱动它
```

这样**服务端的渲染能力 100% 免费获得**（因为渲染发生在 Linux 上），
而且不需要我们维护驱动库。

</details>

**那手机免驱打印怎么办**：靠 **IPP 代理**这条路，而不是自己渲染。IPP/AirPrint 的数据流里
**客户端已经把文档渲染成位图了**（Apple Raster / PWG Raster / URF），服务器只做协议封装
和转发，不需要 PDF 引擎 —— 这在 MCU 上是可行的（有 ESP32 上跑 IPP + AirPrint 免驱发现的先例）。
但要注意：**对 1390 这种 ESC/P2 光栅机仍然不行** —— URF 是设备无关位图，要变成
1390 认识的墨色/分辨率/抖动序列，仍然是驱动的工作。
IPP 代理真正能覆盖的是 **IPP-over-USB 打印机**（打印机自己就是 IPP 端点，板子只需代理请求），
这正是本项目已能识别（`7/1/4` 接口）但尚未代理的那部分。

### 按「用户家里有什么」选最优解

| 用户家里有 | 最优解 | 本项目的角色 |
|---|---|---|
| 什么都没有，只要手机能打印 | **本项目的 AirPrint 方案**（插上就用，零依赖） | 主力产品 |
| 有 Linux 主机（NAS / 树莓派 / 软路由 / 旧笔记本） | **USB/IP 方案**：让那台 Linux 的 CUPS 接管 | 加一个固件特性即可，服务端渲染能力免费获得 |
| 打印机本身自带 AirPrint | 打印机自己就行 | 本方案多余 —— **这限定了我们的市场：老打印机 + 不想折腾的人** |

**自家实现的 IPP 服务端的能力边界**（与同类项目一致，如 Xteink X3 的实现）：
实现了 `Get-Printer-Attributes` 和 `Print-Job` 等核心操作，
**不做作业队列、不做认证、不做多客户端并发** ——
适合「单用户个人设备」，不适合替代办公室共享打印服务器。


---

## 硬件要求

| 项目 | 规格 |
|---|---|
| 开发板 | ESP32-S3 开发板（**必须带 USB OTG**） |
| Flash | 4MB |
| PSRAM | 2MB 或以上（实测 XMC 4MB Flash + 2MB PSRAM 可用） |
| USB OTG 线 | Type-C / Micro-USB 转 USB-A 母口，**必须是 OTG 线**，普通充电线不行 |
| 打印机线 | USB-A 公转 USB-B 公（打印机原装线） |
| 供电 | 5V / 1A 以上 |

### 接线

- 标「**USB**」的接口 → 接 OTG 线 → 打印机（Host 模式）
- 标「**UART** / **COM**」的接口 → 接电脑烧录，烧录后作为供电口

> ⚠️ 板载 USB-Serial/JTAG 口（VID:PID `303A:1001`）**不是** OTG 口。接错口枚举不到打印机。

---

## 编译与烧录

### 1. 安装 ESP32 核心

板管理器搜索 `esp32`，安装版本 **3.3.10**（IDF v5.5.4）。

> 🔴 **必须钉死 3.3.10，绝对不要升级。**
> Arduino IDE / arduino-cli 会自动升级开发板包。实测升到 **3.3.11** 后 **USB Host 不再枚举打印机**：
> 网页显示「未连接」、`enumCount=0`、`vid=0000`，断电重启打印机、重插 USB 全都无效 ——
> 看起来像硬件坏了，其实只是核心版本变了。
>
> 判别信号：编译突然整机重编（数分钟）+ 产物体积明显变化
> （3.3.10 ≈ 1,089,824 字节；3.3.11 ≈ 1,076,736 字节）。
>
> 退回：
> ```bash
> arduino-cli core uninstall esp32:esp32        # 注意：不能带 @版本号
> arduino-cli core install  esp32:esp32@3.3.10
> ```
> 装完需整机重编译再 OTA。装/卸都很慢（10 分钟量级），期间低 CPU 属正常。

### 2. 工具菜单设置

| 设置项 | 选择 | 说明 |
|---|---|---|
| Board | `ESP32S3 Dev Module` | |
| USB CDC On Boot | **Disabled** | 必须 Disabled，否则 USB Host 不可用 |
| USB Mode | `USB-OTG (TinyUSB)` | |
| USB DFU On Boot | Disabled | |
| USB MSC On Boot | Disabled | |
| PSRAM | `OPI PSRAM` | |
| Flash Size | `4MB` | 按实际 Flash 选 |
| Partition Scheme | `Minimal SPIFFS (1.9MB APP / 190KB SPIFFS)` | **必须选含 OTA 的方案** |

对应 FQBN：

```bash
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs
```

### 3. 编译

Arduino IDE：打开 `ESP32S3_PrintServer_v2.ino` → 选择串口 → 点上传。

arduino-cli：

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs" .
```

> 编译时只给 `--build-path`，**不要同时给 `--output-dir`**（指向同一目录会报 `are the same file`，
> 是假错，`.bin` 其实已经正确生成）。

### 4. 烧录失败

按住 **BOOT** → 按一下 **RST** → 松开 RST → 松开 BOOT，进入下载模式后重试。

---

## 首次配网

1. 烧录完成，接好 USB OTG 线
2. 串口监视器（115200）应看到 `[USB] 打印机就绪`
3. 手机或电脑连 WiFi **`PrintServer-Setup`**，密码 **`12345678`**
4. 浏览器打开 `http://192.168.4.1`
5. 用出厂账号登录：**`admin` / `admin`**
6. **扫描** → 选 WiFi → 输密码 → **连接**
7. 成功后设备切换到新 IP，页面自动跳转，用同一套账号密码再登录一次
8. 进 **系统 → 修改管理员密码**（不改的话，除配网外的所有写操作都会被 `403` 挡住）

> ESP32 只支持 2.4GHz WiFi。

**忘记管理员密码**：长按板子 `BOOT` 键约 10 秒（串口会打印提示），密码清空回 `admin`。
没有实体按键的板子把 `config.h` 的 `AUTH_FACTORY_RESET_PIN` 改成 `-1` 关闭此功能
（同时也就失去了这个救场手段）。

**设备连不上网、网页打不开**：等 **3 分钟**。固件判定长时间离线后会自动开出
`PrintServer-Setup` 热点，连上去重新配置即可，不用拆机重刷。期间它一直在后台尝试连回原 WiFi。

---

## 添加网络打印机

### Windows

1. **设置 → 蓝牙和其他设备 → 打印机和扫描仪 → 添加设备**
2. 搜索完成后点 **手动添加**
3. 选 **使用 TCP/IP 地址或主机名添加打印机**
4. 设备类型 `TCP/IP 设备`，地址填板子 IP（或 `printserver.local`）
5. 系统查询大概率超时 → 直接点 **自定义** 手动设置
6. 协议 **Raw**，端口 **`9100`**
7. **取消勾选「SNMP 状态已启用」**（重要，否则 Windows 判定打印机离线）
8. 选打印机驱动
9. 用**真实打印作业**验证（不是固件测试页，见[故障排查](#故障排查)）

### macOS

**系统设置 → 打印机与扫描器 → 添加打印机** → 切到 **IP** 标签页 →
协议 **HP Jetdirect - Socket** → 地址填 IP → **使用 → 选择软件** 搜型号。

### Linux (CUPS)

```bash
# Web: http://localhost:631 → Administration → Add Printer
sudo lpadmin -p WiFiPrinter -E -v socket://192.168.1.50:9100 -m everywhere
```

---

## 网页管理界面

访问 `http://printserver.local`。未登录时整页盖登录框；登录后 5 个页签，每 2 秒自动刷新：

| 页签 | 内容 |
|---|---|
| **概览** | 打印机状态与型号、当前作业（已接收 / 已发往 USB / 回读已回收 / 速率 / 用时）、累计统计、运行时长、内存 |
| **打印机** | USB 链路全量信息（VID:PID、接口、协议、端点、包长、是否 IPP-over-USB、重枚举次数）、Device ID、序列号；「打印测试页」「软复位」按钮（异步，结果以 toast 弹出） |
| **网络** | WiFi 状态、SSID、IP、**当前 / 近 1 分钟最差 / 均值信号**、离线时长、断线与重连次数、当前重试间隔、后备 AP 是否已开；扫描并切换网络 |
| **系统** | 修改管理员密码、OTA 升级、重启、恢复出厂 |
| **添加打印机** | Windows / macOS 图文步骤，IP 自动填入 |

顶部在默认密码未改时挂醒目横幅；打印机进入「无响应保护期」也会提示。

---

## HTTP API

除状态和登录外，**所有接口都需要登录**（Cookie `PSID` 或 `Authorization: Bearer <token>`）。
跨站请求会被拒绝（校验 `Origin` 与 `Host` 一致）。

| 端点 | 方法 | 登录 | 说明 |
|---|---|---|---|
| `/api/auth/state` | GET | 否 | 鉴权状态：`authenticated` / `needChange` / 是否被锁定 |
| `/api/auth/login` | POST | — | 表单 `user` `pass`，成功返回会话 Cookie |
| `/api/auth/logout` | POST | 是 | 销毁当前会话 |
| `/api/auth/passwd` | POST | 是 | `oldpass` `newpass` `newpass2`；成功后自动重签会话 |
| `/api/status` | GET | 是 | 完整状态 JSON（打印机 / USB / 作业 / 统计 / 网络质量 / 异步操作结果） |
| `/api/debug` | GET | 是 | 崩溃日志、boot 计数、泄漏传输计数 |
| `/api/wifi/scan` | GET | 是 | 后台扫描，返回上次结果（非阻塞） |
| `/api/wifi/connect` | POST | 是¹ | `ssid` `password`，返回新 IP |
| `/api/testpage` | POST | 是² | 提交测试页，**立刻返回**，轮询 `/api/status` 的 `ops.testpage` |
| `/api/printer/reset` | POST | 是² | 同上，结果看 `ops.usbReset` |
| `/api/restart` | POST | 是² | 重启 ESP32 |
| `/api/factory-reset` | POST | 是¹ | 清除 WiFi + 管理员密码并重启 |
| `/api/ota` | POST multipart | 是² | OTA 升级，字段名 `firmware`；可加 `?pw=` 传 `OTA_PASSWORD` |

¹ 允许在「还没改掉出厂默认密码」时调用 —— 配网和救场是基础能力，不能被锁死。
² `needChange` 状态下返回 `403 {"error":"default-password"}`。

`ops` 状态码：`0` 无请求 / `1` 执行中 / `2` 成功 / `3` 失败。
异步化是因为这些操作要发 USB 控制或批量传输，遇到不应答的打印机会阻塞几十秒。

---

## 机型行为特征库

兼容性的关键不在「认不认得这台打印机」，而在**知不知道它的坏脾气**。

固件对数据是透明的，PCL / PS / ESC-POS / GDI 天然都能打。真正决定「某一台打印机能不能用、好不好用」
的从来不是打印语言，而是它在 USB 层的一些怪癖：

- 对所有控制传输长时间 NAK（爱普生 1390）→ 不能发 Device ID
- 作业开头被塞 `ESC @` 就卡住不出纸（HP 走 `@PJL` 流）
- 作业结束补 FF 反而让它永久等待下一页（PCLm 流尾部自带 `%%EOF`）
- 暴露的根本不是标准打印机类接口，而是厂商私有接口（部分惠普 MFP）

这些**无法从描述符推断，只能一台台实测积累**。`printer_db.cpp` 维护 `VID:PID → 行为特征位` 的表，
命中的机型会覆盖 `config.h` 的全局默认值。

### 特征位

| 标志 | 含义 |
|---|---|
| `Q_WANT_ESC_RESET` | 作业开头**要**发 `ESC @`（多数 ESC/P 爱普生、针式机） |
| `Q_NO_ESC_RESET` | **千万别**发 `ESC @`（HP 等 `@PJL` 流机型：发了会卡「正在打印文档」不出纸） |
| `Q_NO_CTRL` | 完全不应答控制传输，禁发 Device ID / 字符串描述符 / 端口状态（Epson 1390） |
| `Q_TAIL_NONE` | 作业结束**什么都不补**（PCLm / PDF / PS / PJL 系 —— 自带收尾标记，补任何东西都是又开一页） |
| `Q_TAIL_FF` | 作业结束**无条件补 FF**（ESC/P、ESC/POS —— 流式格式没有收尾标记） |
| `Q_NO_FAIL_RECOVER` | 禁止故障自愈序列（少数机型被收尾序列打死） |
| **`Q_PASSTHROUGH`** | **纯透传**：固件一个字都不许往打印机嘴里塞 —— 作业头不补 `ESC @`、作业尾不补 `FF`、异常路径不补 `ESC @`、自愈时绝不注入控制序列。用于自带完整收尾协议的高精度机型（Epson 1390） |
| `Q_NO_TEXT_TESTPAGE` | 禁用固件自带的纯文本测试页（主机型 PCLm / PCL3GUI / GDI 收下但既不渲染也不结束，会卡死到必须断电） |
| `Q_VENDOR_IFACE` | 允许接管厂商私有(`0xFF`)批量接口（HP / Canon 已放行） |
| `Q_NO_BIDIR` | 不回读 Bulk IN |
| `Q_IPP_OVER_USB` | 已知 IPP-over-USB 设备（描述符哪怕不是标准 `7/1/4`） |
| `Q_WANT_FORMFEED` / `Q_NO_FORMFEED` | **已废弃**，见 `Q_TAIL_NONE` / `Q_TAIL_FF` |
| `Q_NO_ZLP` | 不补零长度包（极少数老机） |

两者都没写 = `Q_TAIL_AUTO`：按作业末尾字节自动判断（安全兜底）。

### 补录一台新机

1. 插上打印机，访问 `/api/status`，记下 `usb.vid` / `usb.pid` / `usb.protocol`
2. 发一份真实打印作业，再看 `/api/status` 的 `job.headHex` —— **这一步是重点**，
   它决定该用哪套策略：

   | `job.headHex` 解码后 | 数据流类型 | 收尾策略 |
   |---|---|---|
   | `1B 40` / `1B 25 2D 31 32 33 34 35 58` | 裸 ESC/P | `Q_TAIL_FF`，可配 `Q_WANT_ESC_RESET` |
   | `00 00 00 1B 01 40 45 4A 4C`（`ESC 01 @EJL 1284.4`） | **Epson Exit Packet Mode** —— 驱动要退出 1284.4 封包模式、回到普通 ESC/P2，后面自己会发 `REMOTE1` / `JE BC=1 00` / `ESC 00 00 00` 收尾 | **`Q_PASSTHROUGH`**（一个字都别加） |
   | `25 50 44 46` / `25 50 43 4C 6D` | PCLm / PDF（主机型） | `Q_NO_ESC_RESET` + `Q_TAIL_NONE` |
   | `1B 25 2D 31 32 33 34 35 58 40 50 4A 4C` | PJL + PCL | `Q_NO_ESC_RESET` + `Q_TAIL_NONE` |

   > 判断依据是 **`job.tailHex`**：如果驱动自己已经发了收尾序列
   > （`%%EOF` / `UEL` / `@PJL EOJ` / `JE BC=1 00` / `ESC 00 00 00`），
   > 那固件再补 FF 就是「作业结束后又开一页」。

3. 在 `printer_db.cpp` 的 `PROFILES` 里加一行：

   ```c
   {0x04B8, 0x9999, "Epson 某某机型", Q_NO_CTRL | Q_PASSTHROUGH},
   ```

   `PID` 填 `PID_ANY` 表示对该厂商所有产品生效（厂商级行为，如 HP 全线禁 `ESC @`）
4. **只填有实测依据的标志**。没证据的机型不要乱标，让它走全局默认值更安全
5. 改完重新编译 + OTA

> ⚠️ 补录时**不要改 `config.h` 的全局默认值**去迁就一台机器 —— 那会影响所有未登记的机型。
> 全局值只是「查不到该机型时的兜底」。

> 这张表是这个方案里最不容易被复制的部分：代码能开源，几百台真机的调校数据不行。

---

## 作业生命周期

`:9100` 同时只受理一个连接（有意为之：半路的 foreign language 数据流混进来会打印错位）。

```
客户端连接
  ├─ 15s 内没收到任何数据      → 判定空连接，丢弃（免重启自愈）
  ├─ 收到数据
  │    ├─ 全部字节成功下发 + TCP 静默 1.5s → 主动关闭，作业完成
  │    ├─ 传输失败 / 打印机断连           → _abortJob()：标记失败 + 干净关连接 + 按机型复位
  │    ├─ 120s 没成功下发任何字节        → 判定打印机失联，释放通道
  │    ├─ 10 分钟未结束                  → 强制回收通道
  │    └─ 60s 双向都无收发               → 最终兜底断开
  └─ 通道被占用时的连接          → 立即拒绝并计数（stats.rejected）
```

**为什么要「主动关闭」**：RAW 9100 是字节流协议，没有长度字段也没有 EOJ 握手。
Windows 发完作业后会**等服务端关闭 TCP**，而旧固件在等 Windows 先关闭 —— 双方互等到
系统级超时（实测约 60 秒）。表现是队列长时间停在「正在打印」，后续作业全堵在后面。
v2.6.0 起，数据全量下发后 TCP 静默 `JOB_DATA_IDLE_END_MS`（默认 1500ms）就判定作业结束；
**v2.6.3 起这一步是显式 `shutdown(SHUT_WR)` 发 FIN** —— 只 `stop()` 的话客户端经常收不到 FIN
（lwIP 在半关闭连接上直接释放、或 abort 成 RST），队列依旧挂着等 TCP 超时。

**作业收尾策略按机型区分**：驱动流的收尾标记各不相同，固件「好心」补的换页符反而会让打印机
永久等待下一页。

| 打印语言 | 驱动自带收尾 | 固件是否补 FF |
|---|---|---|
| PCLm / PDF | `%%EOF` | 不补 |
| PCL5/6 + PJL | `UEL` + `@PJL EOJ` | 不补 |
| PostScript | `0x04` (Ctrl-D) | 不补 |
| ESC/P（爱普生 1390） | 无 | 补 |
| ESC/POS（票据机） | 无 | 补 |

---

## 故障排查

| 现象 | 原因与处理 |
|---|---|
| 串口无 `[USB]` 输出 | 检查是不是用了普通充电线而不是 OTG 线；确认接在「USB」口不是「UART」口 |
| 打印机一直「未连接」、`enumCount=0` | **先确认 ESP32 核心是 3.3.10**（见[安装核心](#1-安装-esp32-核心)）。升到 3.3.11 会这样，断电重启打印机、重插 USB 都无效。然后断电重启打印机、重插 USB |
| `未找到可用打印接口` | 描述符里既没有标准打印机类也没有被放行的私有接口。到 `/api/status` 记 `vid/pid`，在 `printer_db.cpp` 补一行并加 `Q_VENDOR_IFACE` |
| 型号显示为 `VID:xxxx` | Device ID 读失败（该机型在库里配了 `Q_NO_CTRL`）。不影响打印 |
| Windows 打印机显示离线 | 端口设置里取消勾选 SNMP |
| Windows 添加卡在查询中 | 直接点自定义，指定 Raw / 9100 |
| macOS 队列暂停 | 手动点恢复，确认协议为 HP Jetdirect |
| 打印乱码 | 驱动选错；或 `TESTPAGE_PREPEND_ESC_RESET` 与机型不匹配 |
| 打印用着用着失灵，重启才好 | 空连接占住了作业通道（Windows 双向探测、被取消作业留下的半开连接）。固件会**自动回收**，无需重启 |
| **固件测试页打不出来** | **主机型（GDI / PCL3GUI / PCLm）打印机只认官方驱动生成的数据流**，纯文本测试页不会出纸 —— **这不算故障**。这类机型网页上测试页按钮已自动隐藏。请用真实打印作业验证链路 |
| **HP 卡在「正在打印文档」不出纸** | 作业开头硬塞 `ESC @` 会打乱 PJL 解析。`JOB_RESET_ON_CONNECT` 和 `TESTPAGE_PREPEND_ESC_RESET` 必须是 `0`（默认已是 0），且机型库已对 HP 全线置 `Q_NO_ESC_RESET` |
| **打印机打完一直不结束，下一份要等很久** | v2.6.0 已修（数据下发完 1.5 秒主动断连），v2.6.3 补上「主动发 FIN」（见版本历史）。若仍出现，看串口有没有 `[Print] 数据已全量下发，TCP 静默 XXXms，正常结束作业` 与 `[Print] 已发 FIN（fd=N），200ms 后释放`；另外检查 Windows 的「保留打印的文档」是否开着（会让已结束记录视觉上保留） |
| **打印队列里的任务全部堆着，前一份一直「打印中」** | v2.6.3 已修：作业结束必须显式 `shutdown(SHUT_WR)` 发 FIN，否则 Spooler 读不到作业结束信号。快速自查：发一份作业，固件 `lastJob` 已显示「已完成」，但 Windows 队列还挂着 → 就是这个问题 |
| `sent=0`、`USB 传输失败 status=0` | **打印机自身在拒收数据**：缺粉 / 卡纸 / 残留卡住的作业 / 面板报错。**断电重启打印机**并检查耗材。固件侧只能快速失败保护，无法替打印机复位。若 `recoverOk` 一直是 0，说明自愈也没救回来 |
| 打印到一半重复了好多份 | Windows 后台在重试失败的作业。看 `/api/status` 的 `stats.failedJobs` 和 `job.bytesSent` 是否对不上 —— 一般是打印机侧先出问题，不是固件 |
| **多页大任务中断、随后重复内容（Epson 1390 实锤）** | 看 `job.headHex`：若解码成 `\x1B\x01@EJL 1284.4`，说明这台走 **Epson EJL 1284.4 封包流**，不是裸 ESC/P。此时作业头尾绝不能注入裸控制字节（`ESC @` / `FF` 会破坏帧同步 → 打印机停止接收 → `bytesReceived` 与 `bytesSent` 出现缺口 → Windows 判定失败 → 自动重试 → 重复内容）。机型库须配 `Q_NO_ESC_RESET`；v2.6.2 起 1390 已按此配置 |
| 打印机出问题时网页/API 卡住 | ~~v2.4.0：主循环被批量传输占住。~~ **v2.5.0 起打印服务已独立成 FreeRTOS 任务**，管理页始终可用 |
| `bootCount` 持续增长 | 板子在崩溃循环。先把打印机物理拔掉，等 uptime 稳定上涨后再 OTA。`/api/debug` 能看到 `lastCrashStage` / `stageLine` |
| OTA 失败 / 403 `default-password` | 还在用出厂密码 `admin/admin`，先登录改密码 |
| 忘了我设的管理员密码 | 长按 BOOT 键 10 秒清空密码（串口会打印提示） |
| 换路由器 / 改 WiFi 密码后失联 | 等 3 分钟，设备自动开 `PrintServer-Setup` 热点，连上去重新配置 |
| WiFi 连着但打印很卡、时断时续 | 看「网络 → 近 1 分钟最差信号」。长期低于 -75 dBm 请挪近或加中继；`WIFI_DISABLE_SLEEP` 保持 `1` |
| 重新连上 WiFi 后打印机变离线 | 固件会自动重建 mDNS 与 :9100 监听。仍异常就看 `wifi.disconnects` 是否增长，必要时手动重启 |
| `printserver.local` 无法访问 | Windows 缺 mDNS，直接用 IP |
| Arduino IDE 看不到网络端口 | 装 [Bonjour Print Services](https://support.apple.com/kb/DL999) |
| **`failedJobs` 持续上涨、`txErrors` 同步上涨，全是「USB 传输失败 status=0」** | **打印机卡在半截作业上持续拒收数据**（Windows 后台不停重试，每次都失败）。固件侧只能快速失败保护，**必须断电重启打印机**再让 Windows 重发。实测 HP LaserJet 出现 `failed=46, txErrors=46` 就是这个状态 |
| **OTA 推上去后版本号没变、又退回旧版** | 固件**启动到一半崩溃**、bootloader 回退到旧分区了。`/api/debug` 的 `bootStage` / `lastCrashStage` 会指出卡在启动的哪一步（`markBootStage` 埋点） |
| **`/api/debug` 返回的 JSON 解析失败** | v2.7.3 及更早版本里 stage 字符串没走转义，遇到 RTC 里的残留控制字符就会产出非法 JSON。**v2.7.4 起已修** |

---

## 配置参数

编辑 `config.h`。

> ⚠️ 下面带 `JOB_` / `ENABLE_` / `USB_` 前缀的打印相关项**只是「查不到该机型时的全局默认值」**。
> `printer_db.cpp` 里登记过的机型以库中的 `Q_*` 标志为准。要改某台机器的行为，改库不要改这里。

### 身份与网络

| 参数 | 默认 | 说明 |
|---|---|---|
| `FW_VERSION` | `2.6.0` | 固件版本 |
| `PRINTER_NAME` | `ESP32_PrintServer` | 设备标识名 |
| `MDNS_HOSTNAME` | `printserver` | mDNS 主机名，浏览器用 `http://printserver.local` |
| `RAW_SOCKET_PORT` | `9100` | JetDirect / AppSocket 打印端口 |
| `AP_SSID` / `AP_PASSWORD` | `PrintServer-Setup` / `12345678` | 配网热点 |

### USB

| 参数 | 默认 | 说明 |
|---|---|---|
| `USB_XFER_CHUNK` | `8192` | 单次 USB 批量传输大小 |
| `USB_XFER_TIMEOUT_MS` | `30000` | 单块传输等待上限（打印机走纸时会 NAK，要留余量） |
| `PRINT_CHUNK` | `16384` | 每次从 TCP 取多少字节交给 USB |
| `PRINT_BUFFER_SIZE` | `256 * 1024` | PSRAM 打印缓冲 |
| `STATUS_POLL_MS` | `2000` | IEEE 1284 端口状态轮询间隔 |
| `ENABLE_BIDIR_PASSTHROUGH` | `1` | 读 Bulk IN 并把应答回写 TCP |
| `USB_RX_BUFFER_SIZE` | `4 * 1024` | IN 端点回读环形缓冲 |
| `ENABLE_PORT_STATUS_POLL` | `0` | IEEE1284 端口状态查询。**保持 0**：不少 GDI 机型对它长时间 NAK，而本 IDF 版本 `timeout_ms` 无效、EP0 无取消接口，会留下永久悬挂的传输 |
| `CTRL_WRITE_TIMEOUT_MS` | `2000` | 控制性小写入（`ESC @`、`FF`）的等待上限。**别用 `USB_XFER_TIMEOUT_MS`**：这两三个字节打印机若都不收说明已经死了，等 30 秒只会把 `task()` 堵住、`:9100` 停摆 |

### 故障自愈

| 参数 | 默认 | 说明 |
|---|---|---|
| `USB_RECOVER_ENABLED` | `1` | 传输失败时自动尝试拉回打印机（halt 端点 → SOFT_RESET → 收尾序列）。自愈里的 SOFT_RESET **带 force**，即便机型标了 `Q_NO_CTRL` 也发 —— 它是 class request，不碰已堵死的批量管道 |
| `USB_RECOVER_AFTER` | `1` | 连续几次无回调就触发。**必须为 1**：某块写失败后立刻 `break` 结束作业，"连续 2 次"在同一份作业里永远凑不满 |
| `USB_RECOVER_MAX_PER_JOB` | `2` | 单份作业最多自愈几次 |
| `USB_RECOVER_MAX_WAIT_MS` | `1200` | 自愈各步骤等待上限 |
| `USB_RECOVER_RETRY` | `1` | 自愈后原地重试失败的那块 |
| `USB_STALL_MAX_TIMEOUTS` | `3` | 连续几次无回调判定打印机失联 |
| `USB_STALL_COOLDOWN_MS` | `15000` | 冷却时长，期间新作业立刻失败返回，保住网页响应 |

### 作业生命周期

| 参数 | 默认 | 说明 |
|---|---|---|
| `JOB_NODATA_TIMEOUT_MS` | `15000` | 连上后一直没数据 → 判定空连接自动断开 |
| **`JOB_DATA_IDLE_END_MS`** | `1500` | **数据全量下发后 TCP 静默多久即判定作业结束**，并主动发 FIN（v2.6.0 修 60 秒不结束，v2.6.3 补上发 FIN） |
| **`FIN_RELEASE_MS`** | `200` | 发出 FIN 之后等多久才 `stop()` 释放 fd。**不能设 0** —— lwIP 在 FIN 还没刷出去时就 close 会 abort 成 RST，客户端照样读不到作业结束信号（v2.6.3 新增） |
| `JOB_IDLE_TIMEOUT_MS` | `60000` | 异常连接最终兜底。必须大于 `USB_XFER_TIMEOUT_MS` |
| `JOB_TX_STALL_MS` | `120000` | 作业期内这么久没成功下发过任何字节 → 释放通道 |
| `JOB_MAX_DURATION_MS` | `600000` | 单作业最长占用通道 10 分钟 |
| `JOB_RESET_ON_CONNECT` | `0` | 作业开头发 `ESC @`。Epson 可设 1，**HP 必须 0** |
| `JOB_APPEND_FORMFEED` | `0` | 作业结束补 FF。**保持 0**（v2.6.5 起默认关闭）—— 驱动自带收尾标记，固件多补一个 FF 就是「作业结束后又开一页」（Epson 1390 彩色相片白纸的根因）。确实需要的机型在 `printer_db.cpp` 里登记 `Q_WANT_FORMFEED` / `Q_TAIL_FF` |
| `TESTPAGE_PREPEND_ESC_RESET` | `0` | ESC/P 机型可设 1；**PCL / PS / HP 必须 0** |

### 任务与看门狗

| 参数 | 默认 | 说明 |
|---|---|---|
| `PRINT_TASK_CORE` | `0` | 打印任务跑在哪个核心（`loop()` 在 core1，分离才真正并行） |
| `PRINT_TASK_PRIORITY` / `_STACK_SIZE` | `2` / `8192` | |
| `USB_HOST_TASK_PRIORITY` / `_STACK_SIZE` | `5` / `10240` | 栈不能低于 10240 |
| `WDT_TIMEOUT_SEC` | `60` | |
| `LED_PIN` | `2` | 无外接 LED 也不影响功能 |

### WiFi

| 参数 | 默认 | 说明 |
|---|---|---|
| `WIFI_DISABLE_SLEEP` | `1` | 关掉 modem sleep。**强烈建议保持 1**，节能模式会造成数百毫秒延迟抖动 |
| `WIFI_RETRY_MIN_MS` / `_MAX_MS` | `5000` / `60000` | 重连退避上下限 |
| `WIFI_REASSOC_AFTER` | `3` | 失败几次后升级为完整重新关联 |
| `WIFI_DRIVER_RESET_AFTER` | `8` | 再失败几次后复位 WiFi 驱动 |
| `WIFI_FALLBACK_AP_MS` | `180000` | 离线多久后开后备配网 AP（`0` 关闭） |
| `WIFI_RSSI_SAMPLE_MS` / `_HISTORY` | `2000` / `30` | 信号采样间隔与保留点数（约 1 分钟） |

### 安全

| 参数 | 默认 | 说明 |
|---|---|---|
| `AUTH_ENABLED` | `1` | 调试时可临时置 0 |
| `AUTH_DEFAULT_USER` / `_PASS` | `admin` / `admin` | 出厂账号密码，登录后强制修改 |
| `AUTH_PASS_MIN_LEN` / `_MAX_LEN` | `4` / `32` | |
| `AUTH_SESSION_TIMEOUT_MS` | `2h` | 会话空闲过期 |
| `AUTH_MAX_SESSIONS` | `6` | 并发会话上限。**槽位满时淘汰最久没活动的那个**，不会被拒绝 |
| `AUTH_MAX_FAILS` | `5` | 连续输错几次开始锁定（指数退避，上限 30 分钟） |
| `AUTH_FACTORY_RESET_PIN` | `0` | 长按该 GPIO 10 秒清空管理员密码，`-1` 关闭 |
| `OTA_PASSWORD` | 空 | **建议设置**，否则局域网内谁都能刷机 |

### 内存

| 参数 | 默认 | 说明 |
|---|---|---|
| `HEAP_GUARD_ENABLE` | `1` | |
| `HEAP_GUARD_MIN_KB` / `_HOLD_MS` | `24` / `30000` | 堆低于该值并持续这么久 → 空闲时自动重启，防碎片化变永久故障 |

---

## 文件结构

```
ESP32S3_PrintServer_v2/
├── ESP32S3_PrintServer_v2.ino  主程序入口（setup / loop / 全局对象）
├── config.h                    配置参数（机型库未命中时的全局默认值）
├── printer_db.h / .cpp         机型行为特征库（VID:PID → Q_* 标志）
├── usb_printer.h / .cpp        USB Host 打印机驱动（枚举、匹配、传输、自愈、回读）
├── print_server.h / .cpp       Raw Socket 9100 服务器（独立 FreeRTOS 任务）
├── web_ui.h / .cpp             网页管理界面 + HTTP API + 登录鉴权接入
├── wifi_manager.h / .cpp       WiFi 配网 / 三级重连 / 退避 / 后备 AP / 信号采样
├── auth.h / .cpp               管理员鉴权（SHA-256 加盐、会话、暴力破解锁定）
├── crashlog.h / .cpp           崩溃日志（RTC 内存，重启后经 /api/debug 读出）
├── LICENSE                     MIT
└── README.md
```

---

## 已知限制

1. **单作业通道**。`:9100` 同时只受理一个连接（有意设计）。打印期间状态查询连接会被拒绝。
   排 N 份作业时建议在 Windows 端口里**取消勾选**「启用双向支持」。
2. **打印机内部死锁无法软件复位**。当打印机自身的固件 / FIFO 卡死（`sent=0`、`USB 传输失败 status=0`
   且面板报错），固件能做的只是快速失败保护 —— 端点 halt/flush/clear、SOFT_RESET、收尾序列
   对这类情况实测无效。**目前只能断电重启打印机。** 要根治需要 USB 设备句柄重枚举
   或可控 VBUS 的硬件兜底，尚未实现。
3. **ESP32-S3 只有 Full-Speed 12Mbps**，实测吞吐 0.6–1 MB/s。高速打印机 / 大幅面照片会很慢。
   升级路径是 ESP32-P4（内置 USB High-Speed PHY，480Mbps）。
4. **主机型打印机不支持手机免驱**。见[先看这个](#先看这个能不能用)。
5. **IPP-over-USB 只识别不代理**。能认出 `7/1/4` 接口，但不做 IPP 请求代理。

---

## 版本历史

### v2.7.0

**加 IPP / AirPrint 前端 —— 手机免驱打印**（产品定位：普通家庭，手机能打是核心）

- **独立 631 端口的 IPP 服务端**（`ipp_server.cpp`）。自己解析 HTTP + IPP，
  不用 Arduino 的 `WebServer` —— 后者会把 multipart 拆散、或者把整个 body
  缓冲进内存，而 IPP 必须区分「属性段」和「文件数据段」，
  且一张 A4 彩色光栅几 MB，S3 只有 2MB PSRAM，**必须边收边转 USB**。
- **mDNS AirPrint 免驱发现**（`airprint_mdns.cpp`）。关键在 **`_universal` 子类型** ——
  iOS/macOS 看到 `_ipp._tcp` 会先查这个子类型，有才认为是免驱 AirPrint 打印机，
  没有就退回去只当普通 IPP 打印机（客户端还得装驱动）。
  Arduino 的 `MDns.addService()` **不暴露子类型**，必须调 ESP-IDF 原生
  `mdns_service_subtype_add_for_host()`。
- 实现的 IPP 操作：`Get-Printer-Attributes` / `Print-Job` / `Create-Job` /
  `Get-Jobs` / `Get-Job-Attributes` / `Cancel-Job`（RFC 8010 / 8011）。
- ⚠️ **IPP 规范里 `Get-Printer-Attributes` 等查询用的是 HTTP GET**（无 body），
  只有 `Print-Job` / `Create-Job` / `Cancel-Job` 用 POST。iOS/macOS 正是用 GET
  拿打印机属性的 —— 只实现 POST 的话手机端根本发现不了设备。
  路径约定：`/ipp/printer`、`/ipp/jobs`、`/ipp/jobs/{id}`、`/ipp/print`。
- 通道互斥：IPP 作业占用期间 `:9100` 的新连接会被拒绝 —— 两条通路同时写 USB
  会把打印机的数据流交错成乱码。
- **定位文件数据起点的两个关键点**（踩过坑）：
  1. IPP 的 `end-of-attributes-tag` 是**单字节 `0x03`**（RFC 8010 §3.1），
     不是"整数 0x03 的 4 字节大端编码"。写成 4 字节就永远匹配不上。
  2. 匹配用**字节级 `findBytes()`**，不要用 Arduino 的 `String::indexOf` ——
     它对含 `\0` 的短标记不可靠（实测始终返回 -1，表现为"作业数据一个字节都没转发"，
     或每次只转发 5 字节 —— 因为 `keep = markLen + 4` 把缓冲截断到了 5 字节）。

**能力边界（重要，不要误解）**：

| 打印机 | 手机免驱 | 原因 |
|---|---|---|
| 支持 IPP-over-USB（接口 `7/1/4`） | ✅ 直接代理 | 打印机自己就是 IPP 端点，板子只转发 |
| 支持 PDF 直印（IPP 报 `application/pdf`） | ✅ 直接转发 | 渲染在打印机/客户端完成，照片质量最好 |
| 只认 ESC/P2 / PCLm 的老光栅机（如 1390） | ❌ | URF 是设备无关位图，要转成墨色/分辨率/抖动序列**仍是驱动的工作** |

第一期**不做渲染**（简化 RIP 是 v2.7.1 的事）。所以对 1390 这类 2004 年的老照片机，
手机免驱**仍然不可用** —— 它的 iPhone 端要用**支持 Raw 9100 的第三方打印 App**
（很多照片/文档 App 支持自定义 IP + 端口），或者走电脑打印。

### v2.6.8

- **OTA 不再断开打印机设备**。v2.6.6/2.6.7 为了让 IDF 内部 `usbHostTask` 也闲下来，
  会在写 flash 前 `usb_host_device_close()` 断开打印机。副作用是
  **设备重启后打印机不再自动重新枚举**（实测 Epson 1390 升级后
  `usb.attached=false`，得靠人拔插 USB 线；HP 则能自动回来）。
  现在只挂起我们自己的 USB Host 任务，**打印机全程保持连接**。
- 实测证明：只要打印机状态正常（没卡在半截作业上），写 flash 期间挂起我们自己的
  USB 任务就够了 —— OTA 本来就是**概率性成功**（HP 撞到第 4 次才成，1390 两次都成，
  早前 HP 连续失败 26 次是打印机卡在「打印中」状态导致的，不是固件问题）。

### v2.6.7

- **修网页「上传固件」按钮的鉴权**：`_reqToken()` 增加兜底 —— 从
  `_server.arg("PSID")` 取会话令牌。WebServer 会把请求里的 cookie 自动解析进
  `arg()` 集合，这条路**不受 `collectHeaders` 限制**；而 multipart upload 回调里
  `hasHeader("Cookie")` 偶尔取不到值 → 上传固件被判成「未登录」，
  网页上的固件上传按钮等于不能用（浏览器只带 Cookie，不带 Authorization）。
  同时明确 OTA 脚本要用 `Authorization: Bearer <token>`。

### v2.6.6

修「OTA 推不上去，版本号纹丝不动」：

- **OTA 写 flash 期间必须停掉所有碰 USB 的东西**。
  `Update.begin()` 会擦除整个 app 分区（1.9MB），`Update.write()` 逐块写入 ——
  这段时间 flash cache 被禁用，**任何从 flash 取指令的代码执行都会 panic**
  （Cache disabled but cached memory region accessed）。USB 驱动的代码就在 flash 上。
  实测症状非常迷惑：固件**上传完全部成功** → 设备立刻重启 → 分区写了一半 →
  bootloader 回退到另一分区 → **版本号还是旧的**，`rebootReason=0`（panic 特征）。
  三件事缺一不可：
  1. `g_otaBusy = true` → 打印任务**暂停业务并取消自己的 WDT 订阅**
     （⚠️ 之前是直接 `vTaskSuspend` 打印任务 —— 它订阅了 WDT，挂起就没人喂狗，
     写 flash 超过 60 秒照样 WDT panic）
  2. 挂起 USB Host 任务（`usbTaskHandle`）→ 我们的 USB 任务不碰 USB 控制器
  3. **断开打印机设备** → 让 IDF 内部的 `usbHostTask` 也无事可做
     （断开动作必须由 USB 任务自己在上下文里做：`requestCloseForOta()` +
     `otaCloseFinished()` 等待。不能先挂起再断 —— 它可能正持有 `_usbLock`，挂起即死锁）
- 修 IN 回读 transfer 泄漏：`_onInDone` 出错时**既没 free 也没把 `_inXfer` 置空** ——
  ① 每 STALL 一次泄漏一个 transfer 对象；② `_inXfer` 永远非空导致重试条件
  永远不成立，v2.6.4 加的重试机制实际从未生效。

### v2.6.5

**把「纯透传」从特殊模式提升为全局默认**（用户提问触发：为什么 D-Link 友讯 DPR-2000
这种老牌打印服务器就能通用支持所有打印机？）

- `JOB_APPEND_FORMFEED` 由 `1` 改为 **`0`** —— 未登记的机型**不再补 FF**。
  驱动发来的数据自带收尾标记（`%%EOF` / `UEL` / `@PJL EOJ` / `JE BC=1 00` /
  `ESC 00 00 00`），固件擅自补一个只会坏事的概率远大于「救回来」的概率。
  确实需要补 FF 的机型在 `printer_db.cpp` 里显式登记 `Q_WANT_FORMFEED` 或 `Q_TAIL_FF`。
- `_jobSelfTerminated()` 增加两个 Epson 收尾标志的识别：
  `ESC 00 00 00`（退出 ESC/P2 Remote Mode）与 `JE`（`*JE BC=1 00` flush 缓冲），
  这样显式登记了 `Q_WANT_FORMFEED` 的 Epson 机型也能正确判断「驱动已自收尾」。
- 设计哲学写进 README：**默认最保守地透传，例外才显式登记**。
  这与传统打印服务器（D-Link 友讯 DPR-2000 等）的做法一致 ——
  它们能「通用支持所有打印机」，靠的正是**一个字节都不加**。

### v2.6.4

修 Epson 1390「彩色相片直接出白纸」（用户实测报障）：

- **新增纯透传模式 `Q_PASSTHROUGH`**。开启后固件**一个字都不往打印机嘴里塞**：
  作业头不补 `ESC @`、作业尾不补 `FF`、异常路径不补 `ESC @`、自愈时绝不注入控制序列。
  只做「电脑发什么字节就原样转发什么字节」的字节级转发。
- **1390 改用纯透传**。依据实测字节流：
  - `job.headHex` = `00 00 00 1B 01 40 45 4A 4C 20 31 32 38 34 2E 34 0A 40 45 4A 4C 20 20 20`
    → `ESC 01 @EJL 1284.4`，Epson 官方的 **Exit Packet Mode**（magic command），
    让打印机退出 1284.4 封包模式、回到普通 ESC/P2 数据流。
  - `job.tailHex` = `... 52 45 4D 4F 54 45 31 ... 4A 45 01 00 00 1B 00 00 00`
    → `REMOTE1`（进入 ESC/P2 Remote Mode）+ `JE BC=1 00`（flush 打印机缓冲）+
      `ESC 00 00 00`（退出 Remote Mode）—— **驱动自己发的完整收尾序列**。
  - 旧策略的 `Q_TAIL_FF` 在驱动收尾之后又补一个裸 `FF`，等于**在作业结束后再开一页**，
    表现正是「打印纸直接出来了，上面没内容」；且裸 `FF` 落在命令流中间会被当成打印数据。
- **撤回 v2.6.2 的 force 软复位**（绕过 `Q_NO_CTRL` 硬发 EP0 class request）。
  实测证明这是错的：1390 对 EP0 **100% 不应答**，而本 IDF 版本 `timeout_ms` 无效、
  EP0 无取消接口 → 每发一次泄漏一个 transfer。实测 `leakedXfers=85` **恰好等于**
  `recover=85`，85 个野指针堆在 USB 驱动队列里把设备搞到反复重枚举
  （`enumCount` 1→7），彩色大作业大量失败。现在对 `Q_NO_CTRL` 机型彻底不碰 EP0。
- **IN 回读流改为可重试**（新增 `IN_STREAM_RETRY_MS`，默认 3000）。
  以前 STALL/CRC 后**永久禁用**回读，直到重新插拔。对 EJL 1284.4 机型这是致命的：
  1284.4 是双向协议，打印机每收一个包就回一个 ACK，主机不读 → 打印机 ACK 缓冲塞满
  → 直接 NAK 批量 OUT → 作业卡死。实测 1390 就是 `bidir=false` 却本该有回读。
  现在中断后延迟重试，重试前先清端点 HALT。
- API 新增 `printer.bidirStalled` 与 `printer.passthrough` 字段。

### v2.6.3

修「电脑不知道打印机已经打印完，队列里的任务全堆在一起」：

- **作业结束必须主动发 FIN**。Windows 打印后台判定「这份作业打印完了」靠的就是
  读到对端的 FIN（Raw 9100 没有作业结束握手）。旧代码只调 `WiFiClient::stop()`
  （`lwip_close`），客户端经常收不到 FIN —— 实测固件 **0.57 秒**就记账
  `已完成 35.9 KB`，客户端 **45~90 秒**仍在等。原因是 lwIP 在已经收到对端 FIN 的
  半关闭连接上直接释放、或在发送队列还有数据时 abort 成 RST，两种情况客户端都读不到
  「作业结束」。Spooler 于是把这份作业一直挂在「打印中」，后面几份全部排队等它。
- 改为**优雅关闭**：先 `lwip_shutdown(fd, SHUT_WR)` 发 FIN，隔 `FIN_RELEASE_MS`(200ms)
  再 `stop()` 释放 fd。紧跟着 `stop()` 会让 lwIP 在 FIN 还没刷出去时 abort 成 RST，
  所以这个间隔不能设 0。
- 收尾状态机新增 `FIN_SHUT_WR` / `FIN_CLOSE` 两个阶段；「数据已全量下发 + TCP 静默」
  现在**主动结束作业**（记账 + 补 FF + 发 FIN），不再靠「`stop()` 之后
  `connected()` 变 false」被动触发 —— 那个判据在半关闭连接上等不到，作业会一直 active 挂死。
- `_abortJob()` 失败路径同样走优雅关闭，让客户端立刻结束这次尝试而不是等 TCP 超时。
- 新增 `FIN_RELEASE_MS` 配置项。
- **⚠️ 关键陷阱（改动本项目时务必注意）**：`WiFiClient` 的 `operator bool()` 返回的是
  `connected()`，而 `connected()` 在对端发来 FIN（TCP 半关闭）之后**立刻变 false** ——
  但此时 socket 还在，而且**正是我们欠对端一个 FIN**。所以本文件里所有
  「连接还能不能用」的判断都走 `_clientAlive()` / `_clientFd()`（看 `fd()`），
  **绝不能写 `if (_client)`**。v2.6.3 首次实现时正是栽在这里：`_closeClient()` 开头
  一句 `if (!_client) return;` 就让 FIN 永远发不出去，症状与不修完全一样
  （固件 0.1 秒记账「已完成」，客户端 45~90 秒收不到 FIN）。

### v2.6.2

修 Epson 1390 多页大任务中断 + 自动重试导致重复内容（192.168.11.205 实测归因）：

- **1390 改 `Q_NO_ESC_RESET`**。`job.headHex` 实测解码为 `\x1B\x01@EJL 1284.4\n@EJL`
  —— 1390 走的是 **Epson EJL 1284.4 封包流**，不是裸 ESC/P 字节流。封包流里每个
  字节都有语义（通道号/长度/CRC），作业头尾硬塞裸控制字节会破坏帧同步，打印机
  解析错乱后直接停止接收后续数据。实测 `bytesReceived=2005982` /
  `bytesSent=2003484`，缺口 2498 字节被拒收 → Windows 判定失败 → 自动重试 →
  用户看到「中断 + 重复内容」。`Q_TAIL_FF` 保留（1390 是 ESC/P 系，尾部无 FF
  时补一个才出纸）。
- **自愈的 EP0 软复位绕过 `Q_NO_CTRL`**。1390 实测 `recover=5 / recoverOk=0` 的
  直接根因就是这一步被 `ctrlAllowed()` 永久挡死，自愈只剩「清端点 HALT」——
  等于什么都没做。软复位是 class request，不碰已堵死的批量管道、也不污染数据流，
  比往流里塞控制字节更正确。`softReset()`（网页按钮）同样放行。
- **自愈收尾序列加 `Q_NO_ESC_RESET` 守卫**。`UEL + @PJL EOJ + ESC E + ESC @`
  这套 PJL/PCL 字节对 HP `@PJL` 流、Epson EJL 封包流都是污染，只会让卡死的机器
  死得更透；这类机型完全靠软复位复位。
- **控制性小写入改用 2 秒短超时**（新增 `CTRL_WRITE_TIMEOUT_MS`，`sendData()`
  增加 `timeoutMs` 参数）。`ESC @` 和 `FF` 只有 1~2 字节，此前与业务数据共用
  `USB_XFER_TIMEOUT_MS`(30s)，于是「打印失败 → `_abortJob()` → `_resetPrinter()`
  发 `ESC @`」这一串能把 `task()` 堵住 30 秒以上，期间 `:9100` 既不 accept 也不
  读数据 —— 这正是「打印中断后要等很久才能再打」的机理。

### v2.6.1

- **作业收尾改为非阻塞状态机**（`FIN_PUMP → FIN_TAIL → FIN_DRAIN_TCP`）。原先
  「300ms 死等回读 + 同步补 FF + 关连接」三件事串行堵在 `task()` 里，而 `task()`
  还负责 `:9100` 的 accept。收尾期间新连接还会被当场 `rejected.stop()`，
  Windows 队列立刻提交下一份被拒 → 重试 → 用户感知「打完要等很久才能再打」。
- 收尾期间接受新连接；`_abortJob()` 会作废挂在旧作业上的收尾状态机。

### v2.6.0

- **打印队列秒级结束**。数据全量下发后 TCP 静默 1.5s 即主动断连，根治 Windows/macOS
  队列「打完半天不结束、下一份要等很久」（原先双方互等约 60 秒）
- 作业收尾策略按机型区分（`Q_TAIL_NONE` / `Q_TAIL_FF`）
- 异常路径不再发禁忌的 `ESC @`（`_resetPrinter()` 统一遵守机型库）
- 主机型打印机禁用纯文本测试页

### v2.5.9

- HP 加 `Q_NO_FAIL_RECOVER`，停止执行实测无效、反而污染打印机状态的恢复序列

### v2.5.7

- 作业收尾策略从「一刀切补 FF」改为按机型 + 作业尾部字节自描述判断

### v2.5.1

- **登录会话不再会把管理员关在门外**：槽位满时淘汰最久未活动的会话，登录永不被拒绝
- **打印通道看门狗**：120s 无成功下发 → 释放通道；单作业 10 分钟强制回收
- 打印任务独立成 FreeRTOS 任务后再无 WDT 重启

### v2.5.0

- **管理员登录**：所有写操作都要登录，SHA-256 加盐存 NVS，HttpOnly Cookie，会话令牌，指数退避锁定
- **打印服务搬出 `loop()`**，独立 FreeRTOS 任务 + 独立核心。打印机「叫不应」不再锁死管理页
- 网页触发的阻塞操作（测试页 / 软复位）改为异步
- WiFi 三级重连 + 指数退避 + 关闭 modem sleep
- 长时间离线自动开后备 AP
- 断链自愈：WiFi 重连后自动重建 mDNS 与 :9100
- 双向回读写保护、内存自愈

### v2.4.0

- **机型行为特征库**（`printer_db.cpp`）—— 本方案商业化的核心资产
- **USB 接口匹配改为多级打分**，放行惠普 MFP 非标准 `255/9/1`（依据 Linux `ipp-usb` 源码），
  显式排除 HID / MSC / Imaging 等搭车接口
- **双向回读**：常驻 Bulk IN + 环形缓冲 → 写回 TCP
- 打印机快速失败保护（`USB_STALL_*`）

### 更早

见 `git log`。

---

## 许可证

MIT License — 详见 [LICENSE](LICENSE)

## 致谢

基于 ESP-IDF USB Host 栈与 Arduino-ESP32 核心实现。
接口打分策略参考了 Linux `ipp-usb` 项目对惠普 MFP 非标准接口组合的处理。
