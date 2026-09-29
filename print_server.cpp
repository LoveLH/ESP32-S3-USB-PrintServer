#include "print_server.h"

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

void PrintServer::_runPendingOps() {
  // 有正式作业在跑时不抢打印通道，等操作窗口
  if (_currentJob.active || (_client && _client.connected())) return;

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
  if (_client) { _client.stop(); }
  if (_currentJob.active) _abortJob("WiFi 重新连接");
  _server.begin();
  _server.setNoDelay(true);
  Serial.println("[Print] 网络恢复，已重启 :9100 监听");
}

void PrintServer::task() {
  // 网页请求的测试页 / 复位在空闲时执行（会阻塞，绝不能在 Web 线程里做）
  _runPendingOps();

  // --- 作业超时自愈 ---
  // 连上却不发数据、或长时间无收发的连接会让 _currentJob 永远 active，
  // 从而拒绝之后所有打印（表现为"打印失灵，重启才好"）。这里强制回收，无需重启。
  if (_currentJob.active) {
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
      // 已收字节全部成功交给 USB 且 TCP 静默一小段时间，主动 FIN，触发现有成功收尾分支。
      Serial.printf("[Print] 数据已全量下发，TCP 静默 %lums，主动结束作业\n", idle);
      _client.stop();
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
    if (_client && _client.connected()) {
      WiFiClient rejected = _server.accept();
      _rejectedCount++;
      Serial.printf("[Print] 打印机忙，拒绝来自 %s 的连接（累计拒绝 %u 次）\n",
                    rejected.remoteIP().toString().c_str(), (unsigned)_rejectedCount);
      rejected.stop();
    } else {
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

  if (_client && _client.connected()) {
    _handleClient();
  } else {
    // 没有作业在跑：打印机若自由回吐状态数据也不能让它把缓冲塞满，定期清掉。
    if (_rxScratch) {
      while (_printer.readData(_rxScratch, RX_SCRATCH) > 0) { /* 丢弃 */ }
    }
  }

  if (!(_client && _client.connected()) && _currentJob.active) {
    unsigned long ms = millis() - _currentJob.startTime;
    _lastJobMs = ms;
    // 收发的字节数不一致 = 连接中途断开（半截作业）。标记失败并复位打印机，
    // 否则残留状态会让下次打印从半截图续打，造成错位 / 重影。
    bool partial = (_currentJob.bytesSent < _currentJob.bytesReceived);
    if (partial) {
      _failedJobs++;
      snprintf(_lastJob, sizeof(_lastJob),
               "失败：连接中断（收 %u / 发 %u 字节）",
               (unsigned)_currentJob.bytesReceived, (unsigned)_currentJob.bytesSent);
      // 同理：打印机里留了半截数据，先自愈再复位，别让它卡到要人工断电。
      _printer.recover();
      _resetPrinter();
    } else {
      snprintf(_lastJob, sizeof(_lastJob),
               "已完成 %.1f KB，用时 %.1f 秒，来自 %s",
               _currentJob.bytesReceived / 1024.0, ms / 1000.0, _currentJob.clientIP);
      _totalJobs++;
      _totalBytes += _currentJob.bytesReceived;

      // 作业收尾后稍等片刻，把打印机的收尾应答（"作业结束/缺纸"之类）回写给客户端，
      // 否则 Windows 的"双向支持"始终拿不到最后一段回应。
      unsigned long t0 = millis();
      while (millis() - t0 < 300) {
        if (_pumpBackToClient() == 0) delay(10);
      }

      // 作业收尾：补不补东西由机型库的策略位决定（见 printer_db.h 的 Q_TAIL_*）。
      // 背景（HP LaserJet MFP M129-M134 / PCLm-S 实测，2.5.5~2.5.6 修的核心问题）：
      // 驱动自己已收尾（PCLm 是 %%EOF、PJL 系是 UEL、PostScript 是 0x04）时再补一个裸 FF，
      // 等于在作业结束之后又开了一页却永远不给它数据 —— 打印机一直卡在"正在打印中"，
      // 后面的作业全进不来，只能人工断电。
      if (_currentJob.bytesSent > 0) {
        PrinterTailPolicy tp = _printer.tailPolicy();
        bool appendFf = false;
        if (tp == TAIL_FF) {
          appendFf = true;                                   // 机型库明确要求（ESC/P 等）
        } else if (tp == TAIL_AUTO) {
          appendFf = _printer.wantsFormfeed() && !_jobSelfTerminated();
        }
        if (appendFf) {
          static const uint8_t ff = 0x0C;
          _printer.sendData(&ff, 1);
          Serial.println("[Print] 补发换页符（作业未自收尾 / 机型库要求）");
        } else if (tp == TAIL_NONE) {
          Serial.println("[Print] 跳过收尾补发：机型库策略 TAIL_NONE");
        } else {
          Serial.println("[Print] 跳过收尾补发：驱动已自收尾");
        }
      }
    }
    Serial.printf("[Print] 作业结束: %s\n", _lastJob);
    _currentJob.active = false;
    if (_client) _client.stop();   // 等上面的回读窗口结束后再关，免得丢掉打印机最后的应答
  }
}

// 把打印机经 Bulk IN 回吐的数据写回 TCP。这就是"双向支持"的实现：
// Windows 端口查 PJL 状态、macOS 问缺纸/卡纸、票据机回 ACK，全走这条反向通道。
size_t PrintServer::_pumpBackToClient() {
  if (!_client || !_client.connected() || _rxScratch == nullptr) return 0;
  size_t total = 0;
  for (int i = 0; i < 8; i++) {          // 每轮最多搬 8 次，避免独占任务
    int n = _printer.readData(_rxScratch, RX_SCRATCH);
    if (n <= 0) break;
    // 客户端不读数据时不硬写：lwIP 发送缓冲塞满后 write() 会一直阻塞，
    // 网络抖动期间这足以把整个打印任务卡死（进而触发看门狗重启）。
    // 状态回读丢几个字节无所谓，卡住才是灾难。
    size_t space = (size_t)_client.availableForWrite();
    if (space < (size_t)n) break;
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
bool PrintServer::_jobSelfTerminated() const {
  if (_jobTailLen == 0) return false;
  if (_jobTail[_jobTailLen - 1] == 0x0C) return true;          // 换页符

  struct Sig { const char* s; size_t n; };
  static const Sig sigs[] = {
    {"\x1B" "%-12345X", 9},   // UEL（PJL 系）
    {"%%EOF",          5},   // PCLm / PDF
    {"@PJL EOJ",       8},   // PJL 作业结束命令
    {"\x04",           1},   // PostScript 的作业结束符 Ctrl-D
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
  _printer.sendData(resetSeq, sizeof(resetSeq));
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
  if (_client) _client.stop();
  _currentJob.active = false;
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
