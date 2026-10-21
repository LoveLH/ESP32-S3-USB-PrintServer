#include "print_server.h"
// v2.6.3：优雅关闭 TCP 连接需要 shutdown(SHUT_WR)。NetworkClient 没暴露 shutdown()，
// 但 fd() 是 public 的，直接对底层 socket 调用 lwIP 即可。
#include <lwip/sockets.h>

static inline size_t szmin(size_t a, size_t b) { return a < b ? a : b; }

static const size_t RX_SCRATCH = 512;

PrintServer::PrintServer(UsbPrinter& printer)
  : _printer(printer),
    _server(RAW_SOCKET_PORT),
    _totalJobs(0), _totalBytes(0), _failedJobs(0), _rejectedCount(0),
    _lastJobMs(0),
    _lastActivityMs(0),
    _lastSentMs(0),
    _buffer(nullptr), _bufferSize(0), _rxScratch(nullptr),
    _jobTailLen(0) {
  memset(&_currentJob, 0, sizeof(_currentJob));
  memset(_jobTail, 0, sizeof(_jobTail));
  _lastJob[0] = 0;
  _testPageOp.state = OP_IDLE;
  _resetOp.state    = OP_IDLE;
  _opLock = xSemaphoreCreateMutex();
}

bool PrintServer::begin() {
  _bufferSize = PRINT_BUFFER_SIZE;
  _buffer = (uint8_t*)ps_malloc(_bufferSize);

  if (_buffer == nullptr) {
    _bufferSize = 8192;
    _buffer = (uint8_t*)malloc(_bufferSize);
    Serial.println("[Print] PSRAM 不可用，改用 8KB 缓冲");
  } else {
    Serial.printf("[Print] PSRAM 缓冲 %u KB\n", (unsigned)(_bufferSize / 1024));
  }
  if (_rxScratch == nullptr) _rxScratch = (uint8_t*)malloc(RX_SCRATCH);

  if (_buffer == nullptr) {
    Serial.println("[Print] 缓冲区分配失败");
    return false;
  }

  _server.begin();
  _server.setNoDelay(true);
  Serial.printf("[Print] Raw Socket 已监听 :%d\n", RAW_SOCKET_PORT);
  return true;
}

// ---------------------------------------------------------------
//  网页触发的异步操作
// ---------------------------------------------------------------
// 以下 getter 在 Web 线程被调用、写入在打印线程，所以用同一把锁串行化：
// String 的并发拷贝会踩到已经释放的缓冲区，直接把板子打挂。

void PrintServer::requestTestPage() {
  if (_opLock && xSemaphoreTake(_opLock, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (_testPageOp.state != OP_PENDING) {
      _testPageOp.msg   = "";
      _testPageOp.state = OP_PENDING;
    }
    xSemaphoreGive(_opLock);
  }
}

void PrintServer::requestPrinterReset() {
  if (_opLock && xSemaphoreTake(_opLock, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (_resetOp.state != OP_PENDING) {
      _resetOp.msg   = "";
      _resetOp.state = OP_PENDING;
    }
    xSemaphoreGive(_opLock);
  }
}

OpState PrintServer::testPageState() {
  if (!_opLock || xSemaphoreTake(_opLock, pdMS_TO_TICKS(20)) != pdTRUE) return OP_PENDING;
  OpState s = (OpState)_testPageOp.state;
  xSemaphoreGive(_opLock);
  return s;
}

String PrintServer::testPageMsg() {
  if (!_opLock || xSemaphoreTake(_opLock, pdMS_TO_TICKS(20)) != pdTRUE) return String();
  String m = _testPageOp.msg;
  xSemaphoreGive(_opLock);
  return m;
}

OpState PrintServer::resetState() {
  if (!_opLock || xSemaphoreTake(_opLock, pdMS_TO_TICKS(20)) != pdTRUE) return OP_PENDING;
  OpState s = (OpState)_resetOp.state;
  xSemaphoreGive(_opLock);
  return s;
}

String PrintServer::resetMsg() {
  if (!_opLock || xSemaphoreTake(_opLock, pdMS_TO_TICKS(20)) != pdTRUE) return String();
  String m = _resetOp.msg;
  xSemaphoreGive(_opLock);
  return m;
}

// ---------------------------------------------------------------
//  IPP 作业通道互斥（v2.7.0）
//  IPP 的 Print-Job 走"边收边转"，数据由 IPP 侧直接写 USB。
//  这里负责保证同一时刻只有一条通路在写打印机 —— 两条同时写会把
//  打印机的数据流交错成乱码（比"打印失败"更难查）。
// ---------------------------------------------------------------
bool PrintServer::beginExternalJob(const char* clientIP) {
  if (_currentJob.active || _extJobActive) return false;
  _extJobActive = true;
  _currentJob.active        = true;
  _currentJob.bytesReceived = 0;
  _currentJob.bytesSent     = 0;
  _currentJob.bytesReturned = 0;
  _currentJob.startTime     = millis();
  _resetJobBytes();
  strncpy(_currentJob.clientIP, clientIP ? clientIP : "ipp",
          sizeof(_currentJob.clientIP) - 1);
  _currentJob.clientIP[sizeof(_currentJob.clientIP) - 1] = 0;
  _lastActivityMs = millis();
  _lastSentMs     = millis();
  Serial.printf("[Print] IPP 作业占用通道，来自 %s\n", _currentJob.clientIP);
  return true;
}

void PrintServer::endExternalJob(bool success, size_t bytes) {
  if (!_extJobActive) return;
  _extJobActive = false;
  unsigned long ms = millis() - _currentJob.startTime;
  _lastJobMs = ms;
  _currentJob.bytesSent = bytes;
  if (success) {
    _totalJobs++;
    _totalBytes += bytes;
    snprintf(_lastJob, sizeof(_lastJob),
             "已完成 %.1f KB，用时 %.1f 秒，来自 %s（IPP）",
             bytes / 1024.0, ms / 1000.0, _currentJob.clientIP);
  } else {
    _failedJobs++;
    snprintf(_lastJob, sizeof(_lastJob), "失败：IPP 作业中断（%s）",
             _currentJob.clientIP);
  }
  _currentJob.active = false;
  Serial.printf("[Print] IPP 作业结束: %s\n", _lastJob);
}

// ---------------------------------------------------------------
void PrintServer::_runPendingOps() {
  // OTA 写 flash 期间绝对不能碰打印机：打印任务已暂停、USB 已断开，
  // 此时发测试页只会写进一台半死的机器（实测表现为 txErrors 累加）。
  extern volatile bool g_otaBusy;
  if (g_otaBusy) return;
  // 有正式作业在跑时不抢打印通道，等操作窗口
  if (_currentJob.active || _clientAlive()) return;

  bool doTest = false, doReset = false;
  if (_opLock && xSemaphoreTake(_opLock, pdMS_TO_TICKS(20)) == pdTRUE) {
    doTest  = (_testPageOp.state == OP_PENDING);
    doReset = (_resetOp.state    == OP_PENDING);
    xSemaphoreGive(_opLock);
  }
  if (!doTest && !doReset) return;

  if (doTest) {
    bool ok = printTestPage();
    String m = _lastJob;
    if (_opLock && xSemaphoreTake(_opLock, pdMS_TO_TICKS(50)) == pdTRUE) {
      _testPageOp.msg   = m;
      _testPageOp.state = ok ? OP_OK : OP_FAIL;
      xSemaphoreGive(_opLock);
    }
    Serial.printf("[Print] 异步测试页：%s\n", m.c_str());
  }

  if (doReset) {
    bool ok = _printer.softReset();
    String m = ok ? String("打印机已软复位")
                  : (_printer.getLastError().length() ? _printer.getLastError()
                                                      : String("复位失败"));
    if (_opLock && xSemaphoreTake(_opLock, pdMS_TO_TICKS(50)) == pdTRUE) {
      _resetOp.msg   = m;
      _resetOp.state = ok ? OP_OK : OP_FAIL;
      xSemaphoreGive(_opLock);
    }
    Serial.printf("[Print] 异步复位：%s\n", m.c_str());
  }
}

// WiFi 断线重连后：TCP 监听有时会跟着接口一起失效，这里重新起一次，
// 并把上一轮可能已经半死的连接清掉，避免"能 ping 通但打印机连不上"。
void PrintServer::onNetworkUp() {
  if (_clientAlive()) { _client.stop(); _closeSentMs = 0; }   // 链路已断，发 FIN 没意义
  if (_currentJob.active) _abortJob("WiFi 重新连接");
  _server.begin();
  _server.setNoDelay(true);
  Serial.println("[Print] 网络恢复，已重启 :9100 监听");
}

void PrintServer::task() {
  // 网页请求的测试页 / 复位在空闲时执行（会阻塞，绝不能在 Web 线程里做）
  _runPendingOps();

  // v2.6.3：已发 FIN 但 fd 还没到释放时刻的连接，在这里收尾。
  // 顺序很重要：必须在收尾状态机之前 —— 状态机要用 _client 做最后的回读搬运。
  _releaseClientIfDue();

  // 上一份作业的收尾状态机放在最前面：它必须能连续推进，
  // 否则补 FF（Epson 走纸 NAK 数秒）会把整个 task() 堵住，
  // :9100 期间既不 accept 也不读数据 —— 就是"打完一份要等很久才能打下一份"。
  if (_finStage != FIN_IDLE) _runFinish();

  // --- 作业超时自愈 ---
  // 连上却不发数据、或长时间无收发的连接会让 _currentJob 永远 active，
  // 从而拒绝之后所有打印（表现为"打印失灵，重启才好"）。这里强制回收，无需重启。
  // ⚠️ IPP 作业占用通道时（_extJobActive），这些判定要跳过：
  //    IPP 的进度由 IPP 侧自己管，而且大作业本来就可能长时间没有新数据。
  if (_currentJob.active && !_extJobActive) {
    unsigned long now    = millis();
    unsigned long idle   = now - _lastActivityMs;
    unsigned long noTx   = now - _lastSentMs;
    unsigned long durat  = now - _currentJob.startTime;
    if (_currentJob.bytesReceived == 0 && idle > JOB_NODATA_TIMEOUT_MS) {
      Serial.printf("[Print] 空连接超时（%lus 未收到数据），强制断开\n", idle / 1000);
      _abortJob("连接后未收到数据（空连接超时）");
    } else if (_currentJob.bytesReceived > 0 &&
               _currentJob.bytesSent == _currentJob.bytesReceived &&
               !_client.available() && idle > JOB_DATA_IDLE_END_MS) {
      // RAW 9100 没有长度/EOJ 握手。Windows 发完会等服务端关闭，服务端若也等客户端
      // 关闭，就会双方互等到系统超时（实测约 60 秒），后续队列全部堵住。
      // 已收字节全部成功交给 USB 且 TCP 静默一小段时间，判定本作业正常结束。
      //
      // v2.6.3：这里**主动结束作业**（记账 + 补 FF + 发 FIN），不再只 stop() 连接。
      // 老写法靠 "stop() 之后 connected() 变 false" 来触发下面的作业结束分支，
      // 在对端已 shutdown(SHUT_WR) 的半关闭连接上等不到那个 false，
      // 作业就会一直 active 挂死 —— 实测固件 0.57 秒就记账完成、客户端 45 秒收不到 FIN，
      // Windows 因此认为作业未完成，后续任务全部堆在队列里。
      Serial.printf("[Print] 数据已全量下发，TCP 静默 %lums，正常结束作业\n", idle);
      _finishJobSuccess();
    } else if (idle > JOB_IDLE_TIMEOUT_MS) {
      Serial.printf("[Print] 作业空闲超时（%lus 无收发），强制结束\n", idle / 1000);
      _abortJob("作业空闲超时");
    } else if (noTx > JOB_TX_STALL_MS) {
      // 通道看门狗：作业还挂着，但迟迟送不进打印机 —— 打印机多半已经"叫不应"。
      // 必须放弃并释放连接槽，否则这台机器会被反复重试的客户端持续占用，
      // 其他人发过来的打印永远排不上队。
      Serial.printf("[Print] 下发停滞 %lus（收 %u / 发 %u），判定打印机失联，放弃本作业\n",
                    noTx / 1000, (unsigned)_currentJob.bytesReceived,
                    (unsigned)_currentJob.bytesSent);
      _abortJob("打印机长时间未接收数据");
    } else if (durat > JOB_MAX_DURATION_MS) {
      Serial.printf("[Print] 作业超过 %lus 仍未结束，强制回收\n", durat / 1000);
      _abortJob("单作业超时（可能被异常客户端占用）");
    }
  }

  if (_server.hasClient()) {
    // 收尾进行中也接受新连接 —— 这是 v2.6.1 的关键改动。
    // 收尾只是对"已经打完的作业"做善后（补 FF、回读应答），不该拦住下一份作业：
    // Windows 队列在上一份完成后会立刻提交下一份，若此刻拒绝，Windows 要重试，
    // 用户感知就是"打完要等很久才能再打"。
    // 收尾阶段 _currentJob.active 已是 false，新连接可以安全接管通道。
    //
    // v2.6.3：判据还要排除"已发 FIN、fd 等待释放"的状态 —— shutdown(SHUT_WR)
    // 只关写方向，connected() 会继续返回 true，这些连接同样是"已经干完活"的，
    // 不该再拦住下一份作业。
    // v2.7.0：IPP 作业占用通道时也要拒绝 —— 两条通路同时写 USB 会把数据交错成乱码。
    if (_clientAlive() && _finStage == FIN_IDLE && _closeSentMs == 0 && !_extJobActive) {
      WiFiClient rejected = _server.accept();
      _rejectedCount++;
      Serial.printf("[Print] 打印机忙，拒绝来自 %s 的连接（累计拒绝 %u 次）\n",
                    rejected.remoteIP().toString().c_str(), (unsigned)_rejectedCount);
      rejected.stop();
    } else {
      // 收尾还没跑完就来了新作业：先把上一份的善后收掉（不阻塞），再接管。
      // ⚠️ v2.7.3：IPP 作业占用通道时**不能接管** ——
      //    _client 一旦被 accept 覆盖，正在跑的 :9100 作业的连接就丢了；
      //    而且 _currentJob 会被重置（bytesReceived 清零），IPP 作业的状态也就乱了。
      if (_extJobActive) {
        // 直接拒绝这次连接，别抢通道
        WiFiClient rejected = _server.accept();
        _rejectedCount++;
        Serial.printf("[Print] IPP 作业进行中，拒绝来自 %s 的连接（累计拒绝 %u 次）\n",
                      rejected.remoteIP().toString().c_str(), (unsigned)_rejectedCount);
        rejected.stop();
        return;
      }
      if (_finStage != FIN_IDLE) {
        if (_finNeedFf) {
          static const uint8_t ff = 0x0C;
          _printer.sendData(&ff, 1, CTRL_WRITE_TIMEOUT_MS);
        }
        // 新作业要接管 _client 了，这里必须把旧 fd 彻底释放 ——
        // 否则 stop() 之后紧接着 accept() 会复用到同一个 fd 编号，
        // 上一份作业的连接状态和新作业混在一起，排障时完全看不出问题在哪。
        if (_clientAlive()) _client.stop();
        _closeSentMs = 0;
        _finStage = FIN_IDLE;
        Serial.println("[Print] 新作业到达，收尾提前结束");
      }
      _client = _server.accept();
      if (_client) {
        _currentJob.active        = true;
        _currentJob.bytesReceived = 0;
        _currentJob.bytesSent     = 0;
        _currentJob.bytesReturned = 0;
        _currentJob.startTime     = millis();
        _resetJobBytes();   // 上一份作业的头/尾字节不能带到这一份来
        strncpy(_currentJob.clientIP, _client.remoteIP().toString().c_str(),
                sizeof(_currentJob.clientIP) - 1);
        _currentJob.clientIP[sizeof(_currentJob.clientIP) - 1] = 0;
        _lastActivityMs = millis();
        _lastSentMs     = millis();
        // 是否发 ESC @ 复位由机型特征库决定（库里 HP 一类 @PJL 流机型被禁止，
        // 因为作业开头塞 ESC @ 会让它们卡在"正在打印文档"不出纸）。
        if (_printer.wantsEscReset()) {
          // 清掉上一个（可能中断的）作业残留状态，避免半截光栅数据污染本次打印导致错位。
          _resetPrinter();
        }
        Serial.printf("[Print] 新作业，来自 %s\n", _currentJob.clientIP);
      }
    }
  }

  // 收尾进行中就不要再去读客户端了：
  // 此时 _currentJob.active 已置 false，_handleClient() 会把连接上残留的数据
  // 当成"新作业"记进 bytesReceived，进而触发 _abortJob 打断正在进行的收尾
  // （表现为打印明明成功，日志却报"连接中断"）。收尾阶段只搬运回读数据。
  // 同理，已发 FIN 等待释放的连接（_closeSentMs != 0）也不能再读 —— 它已经收工了。
  if (_finStage != FIN_IDLE || _closeSentMs != 0) {
    _pumpBackToClient();
  } else if (_clientAlive() && _client.connected()) {
    _handleClient();
  } else {
    // 没有作业在跑：打印机若自由回吐状态数据也不能让它把缓冲塞满，定期清掉。
    if (_rxScratch) {
      while (_printer.readData(_rxScratch, RX_SCRATCH) > 0) { /* 丢弃 */ }
    }
  }

  // ⚠️ v2.7.3：IPP 作业占用通道时（_extJobActive）**必须跳过**这段。
  //    IPP 作业会把 _currentJob.active 置 true（用来占住通道、拒绝 :9100 新连接），
  //    但它的进度由 IPP 侧自己管、_currentJob 里的字节数一直是 0。
  //    如果这里照常结算：partial = (0 < 0) = false → 走"成功"分支
  //    → 既凭空记一次成功作业，又按机型策略**往打印机补一个 FF**
  //    → 打印机会莫名走一张白纸，而且 IPP 作业的状态被反复清掉。
  if (!_clientAlive() && _currentJob.active && !_extJobActive) {
    // 收发的字节数不一致 = 连接中途断开（半截作业）。标记失败并复位打印机，
    // 否则残留状态会让下次打印从半截图续打，造成错位 / 重影。
    bool partial = (_currentJob.bytesSent < _currentJob.bytesReceived);
    if (partial) {
      unsigned long ms = millis() - _currentJob.startTime;
      _lastJobMs = ms;
      _failedJobs++;
      snprintf(_lastJob, sizeof(_lastJob),
               "失败：连接中断（收 %u / 发 %u 字节）",
               (unsigned)_currentJob.bytesReceived, (unsigned)_currentJob.bytesSent);
      Serial.printf("[Print] 作业结束: %s\n", _lastJob);
      _currentJob.active = false;
      // 同理：打印机里留了半截数据，先自愈再复位，别让它卡到要人工断电。
      _printer.recover();
      _resetPrinter();
    } else {
      _finishJobSuccess();
    }
  }
}

// 作业正常结束的统一入口（v2.6.3）。
// 两个调用点：① 数据全量下发 + TCP 静默；② 客户端主动断开且收发字节数一致。
// 职责：记账 → 决定补不补 FF → 补 FF → 给客户端发 FIN 关连接。
//
// 为什么必须在这里发 FIN（而不是像 v2.6.2 之前那样只 _client.stop()）：
// Windows 打印后台判定"这份作业打完"靠的就是读到对端的 FIN。
// 实测（1390 / 192.168.11.205）只用 stop() 的后果：
//   固件侧 0.57 秒就记账 "已完成 35.9 KB"，客户端却 45~90 秒收不到 FIN；
//   Spooler 认为作业仍在打印，后续任务全堆在队列里等 TCP 超时。
void PrintServer::_finishJobSuccess() {
  unsigned long ms = millis() - _currentJob.startTime;
  _lastJobMs = ms;
  snprintf(_lastJob, sizeof(_lastJob),
           "已完成 %.1f KB，用时 %.1f 秒，来自 %s",
           _currentJob.bytesReceived / 1024.0, ms / 1000.0, _currentJob.clientIP);
  _totalJobs++;
  _totalBytes += _currentJob.bytesReceived;
  _currentJob.active = false;

  // 只做"要不要补 FF"的决策。真正补发 + 回读等待 + 发 FIN + 关连接
  // 全部交给 _runFinish() 状态机分轮完成（v2.6.1 起）。
  // 原因：补 FF 是同步 USB 写，Epson 走纸时会 NAK 数秒；老代码把这段连同
  // 回读等待一起塞在 task() 里，期间 :9100 既不 accept 也不读数据，
  // 实测表现就是"打完一份要等很久才能打下一份"。
  bool needFf = false;
  if (_currentJob.bytesSent > 0) {
    PrinterTailPolicy tp = _printer.tailPolicy();
    if (tp == TAIL_FF) {
      needFf = true;                                     // 机型库明确要求（ESC/P 等）
    } else if (tp == TAIL_AUTO) {
      needFf = _printer.wantsFormfeed() && !_jobSelfTerminated();
    }
    if (!needFf) {
      Serial.println(tp == TAIL_NONE ? "[Print] 跳过收尾补发：机型库策略 TAIL_NONE"
                                     : "[Print] 跳过收尾补发：驱动已自收尾");
    }
  }

  Serial.printf("[Print] 作业结束: %s\n", _lastJob);

  if (_clientAlive()) {
    _startFinish(needFf);          // 走状态机：回读 → 补 FF → 发 FIN → 释放
  } else {
    // 客户端已经不在了：没有回读对象，同步补完 FF 就收工。
    // 仍然交给状态机去发 FIN —— shutdown(SHUT_WR) 之后 connected() 会继续返回
    // true（只关了写方向），不推进状态机的话连接会一直占着通道，
    // 下一份作业进不来。
    if (needFf) _printer.sendData((const uint8_t*)"\x0C", 1, CTRL_WRITE_TIMEOUT_MS);
    _startFinish(false);
  }
}

// 把打印机经 Bulk IN 回吐的数据写回 TCP。这就是"双向支持"的实现：
// Windows 端口查 PJL 状态、macOS 问缺纸/卡纸、票据机回 ACK，全走这条反向通道。
size_t PrintServer::_pumpBackToClient() {
  // 用 fd() 而不是 connected() 判断：对端 shutdown(SHUT_WR) 之后 connected() 变 false，
  // 但它的**读方向还开着** —— 这正是 Windows 双向支持等待收尾应答的场景，
  // 这时候把回读丢掉就等于告诉电脑"打印机没反应"。
  if (!_clientAlive() || _rxScratch == nullptr) return 0;
  size_t total = 0;
  for (int i = 0; i < 8; i++) {          // 每轮最多搬 8 次，避免独占任务
    int n = _printer.readData(_rxScratch, RX_SCRATCH);
    if (n <= 0) break;
    // 客户端不读数据时不硬写：lwIP 发送缓冲塞满后 write() 会一直阻塞，
    // 网络抖动期间这足以把整个打印任务卡死（进而触发看门狗重启）。
    // 状态回读丢几个字节无所谓，卡住才是灾难。
    size_t space = (size_t)_client.availableForWrite();
    if (space == 0) break;
    if (space < (size_t)n) n = (int)space;
    size_t wrote = _client.write(_rxScratch, (size_t)n);
    if (wrote == 0) break;
    _currentJob.bytesReturned += wrote;
    total += wrote;
    if (wrote < (size_t)n) break;
  }
  return total;
}

void PrintServer::_handleClient() {
  // 连接一进来就先把打印机已回吐的数据搬回去，哪怕客户端这会儿还没发任何东西
  // （Windows 的双向状态查询经常就是这样：它发一小段查询，等着听打印机的回答）。
  _pumpBackToClient();

  if (!_client.available()) return;

  // 打印机没接上：直接终止本作业，不要空转读取（否则 _currentJob 一直 active、
  // 连接也不关，会卡住后续所有打印，表现为“收不到”）
  if (_printer.getState() == PRINTER_DISCONNECTED) {
    size_t dropped = 0;
    while (_client.available()) {
      dropped += _client.read(_buffer, szmin((size_t)_client.available(), _bufferSize));
    }
    if (dropped) {
      Serial.printf("[Print] 打印机未连接，丢弃 %u 字节\n", (unsigned)dropped);
    }
    _abortJob("打印机未连接");
    return;
  }

  while (_client.available()) {
    size_t want = szmin((size_t)_client.available(), (size_t)PRINT_CHUNK);
    size_t n = _client.read(_buffer, want);
    if (n == 0) break;

    _currentJob.bytesReceived += n;
    _lastActivityMs = millis();
    _noteTail(_buffer, n);
    _noteHead(_buffer, n);

    int sent = _printer.sendData(_buffer, n);
    if (sent > 0) {
      _currentJob.bytesSent += (size_t)sent;
      // 一次 sendData 可能阻塞到 30 秒（打印机忙时 NAK），回来后立刻 Activity
      // 刷新 + 把这段时间里打印机回吐的数据搬回客户端。
      _lastActivityMs = millis();
      _lastSentMs     = millis();
      _pumpBackToClient();
    } else {
      // 发送失败：终止本作业并复位打印机，绝不能把后半截残流继续塞给打印机，
      // 否则打印机状态机错位，重试时新作业被当成旧像素吞掉 → 错位 / 重影。
      _abortJob("USB 发送错误 - " + _printer.getLastError());
      return;
    }

    yield();   // 让 Web 管理界面有机会响应
  }
}

// --- 作业头/尾字节跟踪 ------------------------------------------
// 只留开头/结尾各 24 字节，够判断语言与收尾方式，又不占内存。
void PrintServer::_noteHead(const uint8_t* d, size_t n) {
  if (n == 0 || _jobHeadLen >= JOB_HEAD_KEEP) return;   // 只记最前面那一段
  size_t take = szmin(n, JOB_HEAD_KEEP - _jobHeadLen);
  memcpy(_jobHead + _jobHeadLen, d, take);
  _jobHeadLen += take;
}

void PrintServer::_noteTail(const uint8_t* d, size_t n) {
  if (n == 0) return;
  if (n >= JOB_TAIL_KEEP) {
    memcpy(_jobTail, d + (n - JOB_TAIL_KEEP), JOB_TAIL_KEEP);
    _jobTailLen = JOB_TAIL_KEEP;
    return;
  }
  size_t overflow = (n > (JOB_TAIL_KEEP - _jobTailLen))
                      ? (n - (JOB_TAIL_KEEP - _jobTailLen)) : 0;
  if (overflow) {
    memmove(_jobTail, _jobTail + overflow, _jobTailLen - overflow);
    _jobTailLen -= overflow;
  }
  memcpy(_jobTail + _jobTailLen, d, n);
  _jobTailLen += n;
}

// 驱动自己在末尾收尾了吗？
// 实测各家的收尾花样（03F0:622A 装的 HP LaserJet MFP M129-M134 PCLm-S 驱动，
// 179.4 KB 作业尾部实测为 "ef\n181436\n%%EOF\n" —— 是 PDF 系收尾，既不是 UEL 也不是 FF）：
//   - 以 FF(0x0C) 结尾             —— PCL/ESC-P 系
//   - 尾部含 UEL ESC%-12345X       —— PJL 系（PCL5/PCL6）
//   - 尾部含 "%%EOF"               —— PCLm / PDF 系
//   - 尾部含 "@PJL EOJ"            —— PJL 作业结束命令
//   - 尾部含 "JE" + BC=1          —— Epson ESC/P2 Remote Mode 的 "*JE BC=1 00"
//                                     （flush 打印机缓冲；Epson 1390 实测尾部 =
//                                      "REMOTE1" + "JE BC=1 00" + ESC 00 00 00）
//   - 尾部含 ESC 00 00 00          —— Epson 退出 Remote Mode 的终止符
bool PrintServer::_jobSelfTerminated() const {
  if (_jobTailLen == 0) return false;
  if (_jobTail[_jobTailLen - 1] == 0x0C) return true;          // 换页符

  struct Sig { const char* s; size_t n; };
  static const Sig sigs[] = {
    {"\x1B" "%-12345X", 9},   // UEL（PJL 系）
    {"%%EOF",          5},   // PCLm / PDF
    {"@PJL EOJ",       8},   // PJL 作业结束命令
    {"\x04",           1},   // PostScript 的作业结束符 Ctrl-D
    {"\x1B" "\x00\x00\x00", 4},   // Epson 退出 ESC/P2 Remote Mode
    {"JE",             2},   // Epson "*JE BC=1 00"（flush 缓冲）
  };
  for (size_t k = 0; k < sizeof(sigs) / sizeof(sigs[0]); k++) {
    const uint8_t* p = (const uint8_t*)sigs[k].s;
    size_t n = sigs[k].n;
    if (n > _jobTailLen) continue;
    for (size_t i = 0; i + n <= _jobTailLen; i++) {
      if (memcmp(_jobTail + i, p, n) == 0) return true;
    }
  }
  return false;
}

static String hexOf(const uint8_t* d, size_t n) {
  String s;
  s.reserve(n * 3 + 1);
  for (size_t i = 0; i < n; i++) {
    char b[4];
    snprintf(b, sizeof(b), "%02X ", d[i]);
    s += b;
  }
  return s;
}
String PrintServer::getJobTailHex() const { return hexOf(_jobTail, _jobTailLen); }
String PrintServer::getJobHeadHex() const { return hexOf(_jobHead, _jobHeadLen); }

void PrintServer::_resetPrinter() {
  // 机型库是最终裁决。HP PCLm/GDI 等主机型收到 ESC @ 会污染当前语言状态，
  // 异常路径也绝不能绕过 Q_NO_ESC_RESET。
  if (!_printer.wantsEscReset() || _printer.getState() != PRINTER_READY) return;
  static const uint8_t resetSeq[] = {0x1B, '@'};  // ESC @ 打印机初始化
  // 短超时：这是控制性写入，不是业务数据。打印机若已死，2 秒足够判定，
  // 绝不能让这 2 个字节把 task() 堵住 30 秒（见 config.h 的 CTRL_WRITE_TIMEOUT_MS）。
  _printer.sendData(resetSeq, sizeof(resetSeq), CTRL_WRITE_TIMEOUT_MS);
}

void PrintServer::_abortJob(const String& reason) {
  if (_currentJob.active) {
    _failedJobs++;
    snprintf(_lastJob, sizeof(_lastJob), "失败：%s", reason.c_str());
  }
  // 作业被掐断 = 打印机里还留着半截数据流。不主动收尸的话它会一直 NAK 批量 OUT，
  // 下一个作业照样打不进去 —— 过去只能让用户拔电。先自愈，再补复位序列。
  _printer.recover();
  _resetPrinter();
  // v2.6.3：失败也走优雅关闭。让客户端立刻读到 FIN，才能马上结束这次尝试
  // 而不是挂在 TCP 超时上 —— 否则 Windows 会把它算成"作业还在跑"，
  // 队列里的下一份迟迟提交不上来。
  _closeClient();
  _currentJob.active = false;
  // 收尾状态机若还挂在这一份作业上，直接作废，别让它继续操作已经关掉的连接
  _finStage = FIN_IDLE;
}

// ---------------------------------------------------------------
//  作业收尾状态机（v2.6.1 起，v2.6.3 加入 FIN 阶段）
//
//  为什么需要它：老代码在 task() 里同步做三件事 ——
//    ① 300ms 死等打印机回读应答、② 补 FF（Epson 走纸时 NAK 数秒）、③ 关连接。
//  这三件事串起来能把 task() 堵住好几秒，而 task() 同时还要负责 :9100 的
//  accept 与读数据。结果就是"打完一份，下一份要等很久才进得来"。
//
//  阶段：FIN_PUMP(搬回读应答) → FIN_TAIL(按机型补 FF) → FIN_SHUT_WR(发 FIN) → FIN_CLOSE(释放 fd)
//  每轮 task() 只推进一步，中间照常 accept 新连接、读数据。
//
//  v2.6.3 的关键改动：最后一定要**显式发 FIN**。Windows 打印后台判断
//  "这份作业打印完了"靠的就是读到对端的 FIN；只 lwip_close 的话客户端
//  收不到（实测固件 0.57 秒就记账完成，客户端 45~90 秒仍在等），
//  Spooler 就一直把这份作业挂在"打印中"，后面的任务全部堆在队列里。
//
//  注意：状态机运行期间 _currentJob.active 已经是 false，所以超时判定不会
//  误伤它；但通道仍被占用（_client 还连着），新连接会被拒绝 —— 这是有意的，
//  同一时刻只允许一份作业，避免两个数据流交错打乱打印机状态机。
// ---------------------------------------------------------------
void PrintServer::_startFinish(bool needFf) {
  _finNeedFf  = needFf;
  _finStartMs = millis();
  _finStage   = FIN_PUMP;
}

void PrintServer::_runFinish() {
  unsigned long elapsed = millis() - _finStartMs;

  switch (_finStage) {
    case FIN_PUMP: {
      // 把打印机的收尾应答（"作业结束"/缺纸告警）回写给客户端，
      // 否则 Windows 的"双向支持"永远拿不到最后一段回应。
      // 老代码死等 300ms；这里改成"搬空后静默 300ms 就走"，每轮最多搬 8 次。
      // 另加 2 秒硬上限：打印机若一直吐数据，不能把收尾卡在这里。
      size_t moved = _pumpBackToClient();
      if (moved > 0 && elapsed < 2000) _finStartMs = millis();
      else if (elapsed >= 300) {
        _finStartMs = millis();
        _finStage   = FIN_TAIL;
      }
      break;
    }

    case FIN_TAIL: {
      // 补不补 FF 由机型库策略决定（TAIL_NONE 的 PCLm/PJL 系补了反而卡死，
      // 见 printer_db.cpp 里 03F0:622A 的注释）。这里只执行决策结果。
      if (_finNeedFf) {
        static const uint8_t ff = 0x0C;
        // 短超时：FF 是控制性写入，走纸慢时打印机可能 NAK 数秒，但绝不能
        // 为此把 task() 堵住 30 秒 —— 那会让 :9100 停摆（v2.6.1 的教训）。
        _printer.sendData(&ff, 1, CTRL_WRITE_TIMEOUT_MS);
        Serial.println("[Print] 补发换页符（作业未自收尾 / 机型库要求）");
      }
      _finStartMs = millis();
      _finStage   = FIN_SHUT_WR;
      break;
    }

    case FIN_SHUT_WR: {
      // 核心（v2.6.3）：主动 shutdown(SHUT_WR) 发 FIN。
      // 对端读到 EOF 就知道"这份作业结束了"，Windows 打印后台据此结束该作业、
      // 立刻提交队列里的下一份 —— 不再挂在 TCP 超时上（实测 60~90 秒）。
      // 之前只调 _client.stop()（lwip_close）收不到 FIN：lwIP 在已经收到对端
      // FIN 的半关闭连接上直接释放，或在发送队列还有数据时 abort 成 RST，
      // 客户端两种情况都读不到"作业结束"。
      _closeClient();
      _finStartMs = millis();
      _finStage   = FIN_CLOSE;
      break;
    }

    case FIN_CLOSE: {
      // 等 FIN 真正发出去（FIN_RELEASE_MS），同时给客户端把最后的回读取走。
      // 到期后由 task() 开头的 _releaseClientIfDue() 统一 stop() 释放 fd。
      _pumpBackToClient();
      if (elapsed >= FIN_RELEASE_MS + 150) {
        _releaseClientIfDue();
        _finStage = FIN_IDLE;
        Serial.println("[Print] 收尾完成，连接已释放");
      }
      break;
    }

    default:
      _finStage = FIN_IDLE;
      break;
  }
}

// ---------------------------------------------------------------
//  优雅关闭连接（v2.6.3）
//
//  必须分两步：先 shutdown(SHUT_WR) 发 FIN，隔 FIN_RELEASE_MS 再 stop() 释放 fd。
//  一次性 stop() 是不行的 —— lwIP 在发送队列里还有东西没刷出去时直接 abort，
//  发出去的是 RST 而不是 FIN，客户端照样读不到"作业结束"。
//
//  对端已经先断开（fd 已失效）时，直接释放。
// ---------------------------------------------------------------
void PrintServer::_closeClient() {
  int fd = _clientFd();
  if (fd < 0) { _closeSentMs = 0; return; }
  if (_closeSentMs == 0) {
    lwip_shutdown(fd, SHUT_WR);       // 发 FIN
    Serial.printf("[Print] 已发 FIN（fd=%d），%lums 后释放\n", fd,
                  (unsigned long)FIN_RELEASE_MS);
    _closeSentMs = millis();
  }
  // 延后释放：紧跟着 stop() 会让 lwIP 在 FIN 还没刷出去时 abort 成 RST
}

void PrintServer::_releaseClientIfDue() {
  if (_closeSentMs == 0) return;
  if (millis() - _closeSentMs < FIN_RELEASE_MS) return;
  if (_clientAlive()) _client.stop();
  _closeSentMs = 0;
}

bool PrintServer::printTestPage() {
  if (_printer.getState() == PRINTER_DISCONNECTED) {
    snprintf(_lastJob, sizeof(_lastJob), "失败：打印机未连接");
    return false;
  }

  // 主机型（PCLm / PCL3GUI / GDI）只认官方驱动产生的页面描述流。
  // 纯文本测试页发过去它既不渲染也不结束作业 —— 面板永久卡在"正在打印"，
  // 后面所有作业都进不来，只能人工断电。所以这类机器直接拒绝，一个字节都不发。
  if (!_printer.textTestPageSupported()) {
    snprintf(_lastJob, sizeof(_lastJob),
             "已拒绝：本机是主机型打印机，不认纯文本测试页（请用电脑打印来验证链路）");
    return false;
  }

  const PrinterInfo& pi = _printer.getPrinterInfo();

  String body;
#if TESTPAGE_PREPEND_ESC_RESET
  body += "\x1B" "@";   // ESC @ —— ESC/P 打印机初始化
#endif
  body += "\r\n";
  body += "     ESP32-S3 WiFi Print Server\r\n";
  body += "     ==========================\r\n\r\n";
  body += "  固件版本 : " + String(FW_VERSION) + "\r\n";
  body += "  IP 地址  : " + WiFi.localIP().toString() + "\r\n";
  body += "  运行时长 : " + String(millis() / 1000) + " 秒\r\n";
  body += "  打印机   : " + pi.mfg + " " + pi.mdl + "\r\n";
  body += "  命令语言 : " + (pi.cmd.length() ? pi.cmd : String("(未知)")) + "\r\n";
  body += "  端口状态 : " + _printer.getStatusText() + "\r\n";
  body += "  设备 ID  : " + (pi.deviceId.length() ? pi.deviceId : String("(无)")) + "\r\n";
  body += "\r\n  如果你能看到这段文字，说明整条链路完全正常。\r\n";
  body += "\r\n\r\n\r\n";
  body += "\x0C";   // 走纸换页

  int sent = _printer.sendData((const uint8_t*)body.c_str(), body.length());
  if (sent > 0) {
    snprintf(_lastJob, sizeof(_lastJob), "已打印测试页（%u 字节）", (unsigned)sent);
    return true;
  }
  _failedJobs++;
  snprintf(_lastJob, sizeof(_lastJob), "测试页发送失败 - %s", _printer.getLastError().c_str());
  return false;
}
