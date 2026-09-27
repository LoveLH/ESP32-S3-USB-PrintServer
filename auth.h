#ifndef AUTH_H
#define AUTH_H

// ============================================================
//  管理后台鉴权
//
//  · 账号密码存 NVS，密码本身不落盘：落盘的是 SHA-256(salt + 明文)，
//    salt 是设备上电时随机生成的 16 字节随机数。
//  · 登录成功发一个随机会话令牌，写进 HttpOnly Cookie（浏览器 JS 读不到，
//    XSS 也偷不走），令牌只在内存里，重启即失效。
//  · 连续输错会指数退避锁定，避免被暴力猜密码。
//  · 出厂默认密码登录后会被强制要求修改，不允许带着默认口令上线。
// ============================================================

#include <Arduino.h>
#include "config.h"

class Auth {
public:
  void begin();
  // 主循环里跑：会话过期清理、BOOT 键恢复出厂检测
  void task();

  bool hasCustomPassword() const { return _customPass; }
  String getUser() const { return _user; }

  // 登录成功返回令牌；失败时 errOut 里给人类可读的原因
  bool login(const String& username, const String& password, String& tokenOut, String& errOut);
  void logout(const String& token);
  bool validate(const String& token);          // 命中就顺带刷新最后活跃时间
  bool changePassword(const String& oldPass, const String& newPass, String& errOut);
  bool clearPassword();                        // 恢复出厂默认（BOOT 键长按触发）

  uint32_t      activeSessions() const { return _activeSessions; }
  uint32_t      failCount() const { return _failCount; }
  bool          isLockedOut() const;
  unsigned long lockoutRemainingMs() const;

private:
  String  _user;
  String  _saltHex;
  String  _hashHex;
  bool    _customPass;

  struct Session {
    bool     used;
    char     token[AUTH_TOKEN_BYTES * 2 + 1];
    unsigned long lastSeen;
  };
  Session _sessions[AUTH_MAX_SESSIONS];
  uint32_t _activeSessions;

  uint32_t      _failCount;
  unsigned long _lockUntilMs;
  unsigned long _lockDurationMs;

  // BOOT 键长按恢复出厂
  unsigned long _holdStartMs;

  void   _loadOrCreate();
  void   _save();
  String _hashPassword(const String& plain) const;
  String _issueToken();
  void   _registerFail(String& errOut);
  void   _dropAllSessions();
  void   _expireSessions();
  static String _randomHex(size_t bytes);
};

#endif // AUTH_H
