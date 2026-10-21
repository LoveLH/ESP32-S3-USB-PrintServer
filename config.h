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

#define FW_VERSION "2.7.9"

//  兼容性说明（v2.4.0）：
//   打印数据全程透明转发，固件不理解任何页面描述语言，因此
//   PCL / PostScript / ESC-POS / PJL / GDI(主机型) 全部可用。
//   真正影响某台打印机"能不能用"的是它在 USB 层的脾气差异，
//   这些差异集中在 printer_db.cpp 的机型特征库里，按 VID:PID 命中。
//   下面的 JOB_* / ENABLE_* 只是**查不到该机型时的全局默认值**。

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
#define USB_XFER_TIMEOUT_MS      30000       // 单块批量传输等待上限（打印机忙/走纸时会 NAK，必须留足余量）
#define PRINT_CHUNK               16384       // 每次从 TCP 取多少字节交给 USB
#define PRINT_BUFFER_SIZE         (256 * 1024)// PSRAM 缓冲
#define STATUS_POLL_MS            2000        // IEEE1284 端口状态轮询间隔

// --- USB 故障自愈 ---
// 打印机被半截作业卡住时会一直 NAK 批量 OUT，以往只能人工断电。这里让固件
// 自己先尝试拉回来：清端点 HALT → EP0 上发打印机类 SOFT_RESET → 下发
// "UEL + @PJL EOJ + UEL + PCL 复位"收尾序列（详见 usb_printer.cpp 的注释）。
#define USB_RECOVER_ENABLED       1       // 1 = 启用自动自愈
// ⚠️ 必须保持 1！sendData() 里某一块写失败后会立刻 break 结束本次作业，
// 所以"连续 2 次"在同一份作业里永远凑不满 —— 阈值设成 2 的结果是
// 第一次失败的作业必然被牺牲，要等 Windows 重发下一个作业才自愈。
#define USB_RECOVER_AFTER         1       // 连续几次"传输无回调"就触发一次自愈
#define USB_RECOVER_MAX_PER_JOB   2       // 单份作业最多自愈几次（防止对已死的机器反复空等）
#define USB_RECOVER_MAX_WAIT_MS   1200    // 自愈各步骤的等待上限（要短，别再把任务堵死）
#define USB_RECOVER_RETRY         1       // 自愈后是否原地重试刚才失败的那块数据

// 控制性小写入（ESC @ 复位、换页符 FF）的单块等待上限。
// 这类写入只有 1~2 个字节，打印机若连它都不收，说明已经死了 —— 等 30 秒
// 毫无意义。v2.6.2 之前它们和业务数据共用 USB_XFER_TIMEOUT_MS(30s)，
// 于是"打印失败 → _abortJob() → _resetPrinter() 发 ESC @"这一串能把 task()
// 堵住 30 秒以上，期间 :9100 既不 accept 也不读数据 —— 用户感知就是
// "打印中断后要等很久才能再打"。
#define CTRL_WRITE_TIMEOUT_MS     2000

// --- 双向回读（bulk IN → TCP）---
// 打印机的状态应答（PJL/主机型机型查询、缺纸卡纸告警、墨量）是从 Bulk IN 端点
// 吐回来的。把它原样回写给 TCP，Windows 端口里就能真正勾上"启用双向支持"，
// 也能读到真实状态；这也是后续做 IPP-over-USB 透传的前置能力。
// 风险极低：Bulk 传输只用空闲带宽；打印机不吐数据时这条 transfer 一直 NAK 挂着，
// 不占 CPU。若某台机 verbose 回读导致异常，可在 printer_db 里给它打 Q_NO_BIDIR。
#define ENABLE_BIDIR_PASSTHROUGH  1
#define USB_RX_BUFFER_SIZE        (4 * 1024)  // IN 端点回读环形缓冲
// IN 回读流被 STALL/CRC 打断后，多久重试一次。
// 必须重试：对 EJL 1284.4 机型（Epson 1390），打印机每收一个包就回一个 ACK，
// 主机不读 → 打印机 ACK 缓冲塞满 → 直接 NAK 批量 OUT → 作业卡死。
// 实测 1390 彩色大作业就是这样失败的（回读通道曾被永久禁用）。
#define IN_STREAM_RETRY_MS        3000

// --- 打印机"叫不应"时的快速失败保护 ---
// 打印机拒收数据（缺粉、卡纸、固件卡死）时，一次批量传输会挂到
// USB_XFER_TIMEOUT_MS(30s) 才超时，而这段等待会把整个 loop() 堵死：
// 网页、API、OTA 全部无响应。Windows 后台又会不停重试失败的作业，
// 于是板子被永久锁死 —— 这正是"客户一出问题就打不开管理页"的场景。
// 连续超时达到上限后，进入冷却：期间新作业立刻失败返回，不傻等，
// 让网页和 API 保持可用；冷却结束自动恢复尝试。
#define USB_STALL_MAX_TIMEOUTS    3       // 连续几次传输无回调就判定打印机失联
#define USB_STALL_COOLDOWN_MS     15000   // 冷却时长（毫秒）

// 每个新打印作业开始前，先给打印机发 ESC @ 复位，清掉上一个（可能中断的）
// 作业残留的状态机，避免半截光栅数据污染下一次打印导致错位。
// 注意：对 HP 这类走 @PJL 流的打印机，作业开头硬塞 ESC @ 会打乱 PJL 解析，
// 导致 HP 收下数据却卡在“正在打印文档”不出纸。HP 用户请置 0。
// 这**只是查不到该机型时的全局默认值**；printer_db.cpp 里登记过的机型
// 以库中的 Q_WANT_ESC_RESET / Q_NO_ESC_RESET 为准（HP 已在库中置禁止）。
#define JOB_RESET_ON_CONNECT      0
// 每个作业成功转发完、关闭连接前，补发一个换页符(FF=0x0C)。
//
// ⚠️ v2.6.5 起**默认关闭**（0 = 不补）。
//   理由：市面上的老牌打印服务器（D-Link 友讯 DPR-2000 等）之所以能"通用支持
//   所有打印机"，靠的就是**一个字节都不加**。我们过去默认补 FF，理由是
//   "多数打印机补一个 FF 至多空走一张纸，无害"—— 但对 Epson 1390 这类
//   自带完整收尾协议（JE BC=1 00 + ESC 00 00 00）的机型，插在驱动收尾之后
//   的裸 FF 恰恰就是"作业结束后又开一页"，用户实测表现为
//   **彩色相片直接出白纸**。
//   驱动发来的数据自带收尾标记（%%EOF / UEL / @PJL EOJ / JE BC=1 00 / ESC 00 00 00），
//   固件擅自补一个只会坏事的概率远大于"救回来"的概率。
//   确实需要补 FF 的机型，在 printer_db.cpp 里显式登记 Q_WANT_FORMFEED 或 Q_TAIL_FF。
#define JOB_APPEND_FORMFEED       0

// --- 作业超时保护（修复"打印失灵、必须重启"）---
// 只要有一个连上来却既不发数据也不断开的连接（Windows 端口的双向探测、
// 被取消的打印作业留下的半开连接等），_currentJob 就会永远 active，
// 之后所有打印都被"打印机忙"拒绝，且不会自行恢复 —— 只能重启板子。
// 下面两个超时让固件自愈，无需重启。
#define JOB_NODATA_TIMEOUT_MS     15000   // 连上后一直没收到任何数据 → 判定空连接，快速断开
// RAW 9100 没有作业长度字段。Windows 发送完数据后会等服务端关闭，而旧固件又等 Windows
// 先关闭，双方互等到约 60 秒，队列一直显示“正在打印”。已有数据全部下发后，TCP 连续静默
// 1.5 秒即可认定本作业发送完毕，由服务端主动关闭连接。
#define JOB_DATA_IDLE_END_MS       1500
// 发出 FIN 之后，等多久才 stop() 真正释放 fd。
// 必须给 lwIP 留出把 FIN 放进发送队列并刷出去的时间 —— 紧跟着 stop() 会让
// lwIP 在"还有未发送数据"的情况下直接 abort（发 RST 而不是 FIN），
// 客户端就读不到作业结束的信号了（v2.6.3 实测：200ms 足够，别设 0）。
#define FIN_RELEASE_MS             200
#define JOB_IDLE_TIMEOUT_MS       60000   // 真正异常连接的最终兜底
#define JOB_TX_STALL_MS           120000  // 作业期内这么久没成功下发过任何字节 → 打印机失联，放弃本作业。
                                          // 作用：打印机 NAK 时我们把 job.abort 了，客户端（Windows 后台）
                                          // 会不停重试，每次重试都占着唯一的连接槽若干秒；有这道闸才能保证
                                          // 通道总能被释放给后来者，不会出现"某台电脑把打印服务器占死"。
#define JOB_MAX_DURATION_MS       600000  // 单个作业最长占用通道的时间（10 分钟）。
                                          // 正常打印无论如何都远小于此；超时的只可能是异常客户端，必须回收。
                                          // 必须大于 USB_XFER_TIMEOUT_MS(30000)：
                                          // 打印机忙时一次批量传输可能阻塞近 30s，
                                          // 超时设太小会把正常打印误判为空闲而掐掉。
// IEEE1284 端口状态查询（class request）纯属可选信息，raw 打印完全不需要。
// 不少 GDI / 主机型打印机（如 Epson 1390）对它长时间 NAK，而本 IDF 版本
// usb_transfer_t::timeout_ms 无效、EP0 又无取消接口，会留下永久悬挂的传输。故默认关闭，需要时再置 1。
#define ENABLE_PORT_STATUS_POLL   0

// --- 测试页 ---
// ESC/P 系列（爱普生、多数针式机）填 1：测试页前会先发 ESC @ 复位打印机
// PCL / PostScript / 热敏票据机请改成 0，否则可能打出乱码
// HP 用户置 0（避免测试页开头也带 ESC @）
#define TESTPAGE_PREPEND_ESC_RESET   0

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

// ============================================================
//  Web 管理后台鉴权（v2.5.0）
//  之前的版本任何人打开管理页都能改 WiFi、刷固件、重启设备 —— 放到客户
//  环境里就是"谁都能改"。现在所有写操作都要登录。
//  密码用 SHA-256(salt + 密码) 存 NVS，不存明文；salt 随机生成。
// ============================================================
#define AUTH_ENABLED             1
#define AUTH_DEFAULT_USER        "admin"
#define AUTH_DEFAULT_PASS        "admin"   // 出厂默认，登录后强制要求修改
#define AUTH_PASS_MIN_LEN        4
#define AUTH_PASS_MAX_LEN        32
#define AUTH_TOKEN_BYTES         16        // 会话令牌长度 -> 32 个 hex 字符
#define AUTH_SESSION_TIMEOUT_MS  (2 * 3600 * 1000UL)  // 多久无操作算过期
#define AUTH_MAX_SESSIONS        6         // 最多几个并发会话（网页/手机同时开也够）
#define AUTH_MAX_FAILS           5         // 连续错几次开始锁定
#define AUTH_LOCKOUT_BASE_MS     30000     // 首次锁定 30 秒
#define AUTH_LOCKOUT_MAX_MS      (30 * 60 * 1000UL)   // 锁定时长上限 30 分钟
#define AUTH_SESSION_COOKIE      "PSID"

// 忘记密码的救场手段：长按 BOOT 键（GPIO0）10 秒清空管理员密码。
// 必须连续按住，悬空引脚的偶发抖动触发不了。没有实体按键的板子请置 -1 关闭。
#define AUTH_FACTORY_RESET_PIN   0
#define AUTH_FACTORY_HOLD_MS     10000

// --- NVS 键名 ---
#define NVS_NAMESPACE     "printsvr"
#define NVS_KEY_SSID      "wifi_ssid"
#define NVS_KEY_PASS      "wifi_pass"
#define NVS_KEY_HOSTNAME  "hostname"
#define NVS_KEY_ADM_USER  "adm_user"
#define NVS_KEY_ADM_SALT  "adm_salt"
#define NVS_KEY_ADM_HASH  "adm_hash"
#define NVS_KEY_ADM_SET   "adm_set"   // 1 = 用户已改过密码（不再是出厂默认）

// ============================================================
//  v2.5.0：稳定性改造
// ============================================================

// --- 打印任务独立成 FreeRTOS 任务 ---
// 以前 PrintServer::task() 直接跑在 loop() 里。打印机拒收数据时一次 USB 批量
// 传输会阻塞 30 秒，这段时间 loop() 完全停摆 —— 网页打不开、API 无响应、
// OTA 推不进去，而 Windows 打印后台还在不停重试，板子等于被永久锁死。
// 现在打印服务跑在独立任务/独立核心上，就算卡住也不影响管理界面。
#define PRINT_TASK_CORE         0      // 与 loop()（跑在 core1）分开，真正并行
#define PRINT_TASK_PRIORITY     2
#define PRINT_TASK_STACK_SIZE   8192
#define PRINT_TASK_DELAY_MS     1

// --- WiFi 抗抖动（"网络抖动"专门处理）---
// 1) 关掉 modem sleep：这是打印服务器场景最重要的一条。
//    默认节能模式会让 WiFi 周期睡几十毫秒，TCP 延迟抖动可达数百毫秒，
//    打印作业出现明显卡顿、偶发断流，Windows 端表现为"打印到一半失败"。
#define WIFI_DISABLE_SLEEP        1
// 2) 三级重连：先 reconnect()；连续失败后整链路重新关联；长时间离线再重启 WiFi 驱动。
#define WIFI_RETRY_MIN_MS         5000    // 首次重连间隔
#define WIFI_RETRY_MAX_MS         60000   // 退避上限
#define WIFI_REASSOC_AFTER        3       // 连续失败几次后改走完整重新关联
#define WIFI_DRIVER_RESET_AFTER   8       // 再失败几次就 WiFi.mode 级复位
// 3) 长时间连不上 → 自动开后备 AP，避免设备彻底失联（这是"忘了路由器密码
//    / 路由器换了 SSID / 设备挪远了"时唯一的救场手段）。
#define WIFI_FALLBACK_AP_MS       180000  // 离线多久后开后备 AP
// 4) 链路质量采样
#define WIFI_RSSI_SAMPLE_MS       2000
#define WIFI_RSSI_HISTORY         30      // 保留多少个采样点（约 1 分钟）

// --- 内存自愈 ---
// 长时间运行的内存碎片化会让偶发的分配失败变成永久故障。
// 堆长时间低于阈值且当前没有作业在跑时，自动重启（比重置丝杆/打印机成本低得多）。
#define HEAP_GUARD_ENABLE         1
#define HEAP_GUARD_MIN_KB         24      // 低于此值开始计时
#define HEAP_GUARD_HOLD_MS        30000   // 持续这么久才真的重启

#endif // CONFIG_H
