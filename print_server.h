#ifndef PRINT_SERVER_H
#define PRINT_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "usb_printer.h"
#include "config.h"

// 注意 clientIP / 最后一笔作业描述都是定长 char[] 而不是 String：
// v2.5.0 起 PrintServer 跑在独立任务上，这些字段会被 Web 线程同时读，
// String 的并发拷贝会踩到正在重分配的缓冲区，直接把板子打挂。
// POD 字段并发读写最多导致读数偶尔不一致（状态页看得出来一点偏差），无 crash 风险。
struct PrintJob {
  bool         active;
  size_t       bytesReceived;
  size_t       bytesSent;
  size_t       bytesReturned;  // 打印机经 Bulk IN 回吐、已写回客户端的字节数
  unsigned long startTime;
  char         clientIP[16];
};

// 由网页触发、但必须放到打印任务里执行的操作（它们会阻塞，不能卡住 Web）
enum OpState {
  OP_IDLE    = 0,   // 没人请求过
  OP_PENDING = 1,   // 已提交，等待打印任务处理
  OP_OK      = 2,
  OP_FAIL    = 3
};

class PrintServer {
public:
  PrintServer(UsbPrinter& printer);

  bool begin();
  void task();

  // 打印一张纯文本测试页（任何打印机都能打）
  bool printTestPage();

  PrintJob  getCurrentJob() const { return _currentJob; }
  // 上一份作业的开头/结尾字节（十六进制，排障用）：
  // 开头能看出是什么语言（%PDF=PCLm、@PJL=PJL 系、%!PS=PostScript、ESC @=ESC/P），
  // 结尾能看出驱动有没有自己收尾。
  String    getJobTailHex() const;
  String    getJobHeadHex() const;
  uint32_t  getTotalJobs() const { return _totalJobs; }
  uint32_t  getTotalBytes() const { return _totalBytes; }
  uint32_t  getFailedJobs() const { return _failedJobs; }
  String    getLastJob() const { return String(_lastJob); }
  unsigned long getLastJobMs() const { return _lastJobMs; }
  uint32_t  getRejectedCount() const { return _rejectedCount; }

  // --- 网页触发的异步操作 ---
  // 测试页 / 软复位都会走usb 控制或批量传输，打印机不应答时能阻塞几十秒。
  // 以前直接在 Web handler 里同步调用，一次测试页就能把管理页卡死。
  // 现在只置个标志，真正的动作由打印任务执行，网页立刻返回。
  // 注意：这些 getter 会在 Web 线程被调用，而状态是在打印线程里改的，
  // 所以必须用锁保护（String 的并发拷贝会直接崩），见 print_server.cpp。
  void    requestTestPage();
  void    requestPrinterReset();
  OpState testPageState();
  String  testPageMsg();
  OpState resetState();
  String  resetMsg();

  // WiFi 重新连上时调用：重新起监听、丢掉半死的旧连接
  void onNetworkUp();

private:
  struct OpSlot {
    volatile OpState state;
    String           msg;
  };
  OpSlot           _testPageOp;
  OpSlot           _resetOp;
  SemaphoreHandle_t _opLock;   // 保护上面两个 slot 跨任务访问
  void _runPendingOps();

private:
  UsbPrinter& _printer;
  WiFiServer  _server;
  WiFiClient  _client;
  PrintJob    _currentJob;

  uint32_t _totalJobs;
  uint32_t _totalBytes;
  uint32_t _failedJobs;
  uint32_t  _rejectedCount;   // 因为已有作业在跑而被拒绝的连接数（排障用）
  // 定长而不是 String：见 PrintJob 上方的注释（跨线程读）以及 print_server.cpp 顶部说明
  char      _lastJob[192];
  unsigned long _lastJobMs;
  unsigned long _lastActivityMs;  // 最后一次"收到数据 / 成功下发数据"的时间，用于空闲超时判定
  unsigned long _lastSentMs;      // 最后一次"成功交给打印机"的时间，用于打印通道看门狗

  uint8_t* _buffer;
  size_t   _bufferSize;
  uint8_t* _rxScratch;      // 丢弃用临时缓冲（没有作业时要定期清掉打印机的自由回吐）

  // --- 作业收尾状态机（v2.6.1）---
  // 老代码把"补 FF + 300ms 回读等待"全塞在 task() 的同步路径里：
  // Epson 补 FF 时打印机走纸会 NAK 数秒，这段时间 :9100 既不 accept 也不读数据，
  // 表现为"打完一份要等很久才能打下一份"。
  // 现在收尾拆成 3 个非阻塞阶段，每轮 task() 只推进一小步。
  enum FinishStage {
    FIN_IDLE      = 0,   // 没有待收尾的作业
    FIN_PUMP      = 1,   // 把打印机收尾应答回写给客户端
    FIN_TAIL      = 2,   // 按机型策略补 FF
    FIN_DRAIN_TCP = 3    // 等客户端把最后的回读取走，再关连接
  };
  FinishStage  _finStage = FIN_IDLE;
  unsigned long _finStartMs = 0;
  bool _finNeedFf = false;

  void _handleClient();
  // 把打印机回吐的数据搬回 TCP（双向支持）：Windows 轮询状态、PJL 查询、缺纸卡纸告警都走这条路
  size_t _pumpBackToClient();

  // --- 作业尾部字节跟踪 ---
  // 驱动通常自己在作业末尾发 UEL（ESC % -12345X）收尾。这时候固件再补一个裸 FF，
  // 等于在作业结束之后又开了一页却永远不给数据 —— 打印机就卡在"正在打印中"。
  // 所以补 FF 之前必须先看驱动自己收尾了没。
  static const size_t JOB_TAIL_KEEP = 24;
  static const size_t JOB_HEAD_KEEP = 24;
  uint8_t      _jobTail[JOB_TAIL_KEEP];
  size_t       _jobTailLen;
  uint8_t      _jobHead[JOB_HEAD_KEEP];
  size_t       _jobHeadLen;
  void _noteTail(const uint8_t* d, size_t n);
  void _noteHead(const uint8_t* d, size_t n);
  bool _jobSelfTerminated() const;
  void _resetJobBytes() {
    _jobTailLen = 0; memset(_jobTail, 0, sizeof(_jobTail));
    _jobHeadLen = 0; memset(_jobHead, 0, sizeof(_jobHead));
  }
  // 异常终止当前作业：标记失败、复位打印机（清掉半截作业造成的状态机错位）、关闭连接
  void _abortJob(const String& reason);
  // 给打印机发 ESC @ 复位，清掉上一个（可能中断的）作业残留状态，避免下次打印错位
  void _resetPrinter();
  // 推进作业收尾状态机（非阻塞，详见 .cpp 中 _runFinish）
  void _startFinish(bool needFf);
  void _runFinish();
};

#endif // PRINT_SERVER_H
