#include "print_server.h"

static inline size_t szmin(size_t a, size_t b) { return a < b ? a : b; }

PrintServer::PrintServer(UsbPrinter& printer)
  : _printer(printer),
    _server(RAW_SOCKET_PORT),
    _totalJobs(0), _totalBytes(0), _failedJobs(0),
    _lastJobMs(0),
    _buffer(nullptr), _bufferSize(0) {
  _currentJob = {false, 0, 0, 0, ""};
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

  if (_buffer == nullptr) {
    Serial.println("[Print] 缓冲区分配失败");
    return false;
  }

  _server.begin();
  _server.setNoDelay(true);
  Serial.printf("[Print] Raw Socket 已监听 :%d\n", RAW_SOCKET_PORT);
  return true;
}

void PrintServer::task() {
  if (_server.hasClient()) {
    if (_client && _client.connected()) {
      WiFiClient rejected = _server.accept();
      Serial.printf("[Print] 打印机忙，拒绝来自 %s 的连接\n",
                    rejected.remoteIP().toString().c_str());
      rejected.stop();
    } else {
      _client = _server.accept();
      if (_client) {
        _currentJob = {true, 0, 0, millis(), _client.remoteIP().toString()};
#if JOB_RESET_ON_CONNECT
        // 新作业开始前先复位打印机，清掉上一个（可能中断的）作业残留状态，
        // 避免半截光栅数据污染本次打印导致错位。
        _resetPrinter();
#endif
        Serial.printf("[Print] 新作业，来自 %s\n", _currentJob.clientIP.c_str());
      }
    }
  }

  if (_client && _client.connected()) {
    _handleClient();
  } else if (_currentJob.active) {
    unsigned long ms = millis() - _currentJob.startTime;
    _lastJobMs = ms;
    // 收发的字节数不一致 = 连接中途断开（半截作业）。标记失败并复位打印机，
    // 否则残留状态会让下次打印从半截图续打，造成错位 / 重影。
    bool partial = (_currentJob.bytesSent < _currentJob.bytesReceived);
    if (partial) {
      _failedJobs++;
      _lastJob = "失败：连接中断（收 " + String(_currentJob.bytesReceived) +
                 " / 发 " + String(_currentJob.bytesSent) + " 字节）";
      _resetPrinter();
    } else {
      _lastJob = "已完成 " + String(_currentJob.bytesReceived / 1024.0, 1) +
                 " KB，用时 " + String(ms / 1000.0, 1) +
                 " 秒，来自 " + _currentJob.clientIP;
      _totalJobs++;
      _totalBytes += _currentJob.bytesReceived;
    }
    Serial.printf("[Print] 作业结束: %s\n", _lastJob.c_str());
    _currentJob.active = false;
    _client.stop();
  }
}

void PrintServer::_handleClient() {
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

    int sent = _printer.sendData(_buffer, n);
    if (sent > 0) {
      _currentJob.bytesSent += (size_t)sent;
    } else {
      // 发送失败：终止本作业并复位打印机，绝不能把后半截残流继续塞给打印机，
      // 否则打印机状态机错位，重试时新作业被当成旧像素吞掉 → 错位 / 重影。
      _abortJob("USB 发送错误 - " + _printer.getLastError());
      return;
    }

    yield();   // 让 Web 管理界面有机会响应
  }
}

void PrintServer::_resetPrinter() {
  if (_printer.getState() != PRINTER_READY) return;
  static const uint8_t resetSeq[] = {0x1B, '@'};  // ESC @ 打印机初始化
  _printer.sendData(resetSeq, sizeof(resetSeq));
}

void PrintServer::_abortJob(const String& reason) {
  if (_currentJob.active) {
    _failedJobs++;
    _lastJob = "失败：" + reason;
  }
  _resetPrinter();
  if (_client) _client.stop();
  _currentJob.active = false;
}

bool PrintServer::printTestPage() {
  if (_printer.getState() == PRINTER_DISCONNECTED) {
    _lastJob = "失败：打印机未连接";
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
    _lastJob = "已打印测试页";
    return true;
  }
  _failedJobs++;
  _lastJob = "测试页发送失败 - " + _printer.getLastError();
  return false;
}
