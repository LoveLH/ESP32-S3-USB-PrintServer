#include "auth.h"
#include "config.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

// ---------------------------------------------------------------
static void toHex(const uint8_t* in, size_t len, String& out) {
  static const char* HX = "0123456789abcdef";
  out.reserve(out.length() + len * 2);
  for (size_t i = 0; i < len; i++) {
    out += (char)HX[(in[i] >> 4) & 0x0F];
    out += (char)HX[in[i] & 0x0F];
  }
}

String Auth::_randomHex(size_t bytes) {
  String s;
  for (size_t i = 0; i < bytes; i++) {
    uint8_t v = (uint8_t)(esp_random() & 0xFF);
    toHex(&v, 1, s);
  }
  return s;
}

String Auth::_hashPassword(const String& plain) const {
  // SHA-256(salt + "|" + 明文)。加盐是为了：即使两台设备密码一样，
  // NVS 里的 hash 也不同，避免"拿别台的 hash 覆盖过来"这种攻击。
  String in = _saltHex;
  in += "|";
  in += plain;

  uint8_t out[32];
  mbedtls_sha256((const unsigned char*)in.c_str(), in.length(), out, 0);

  String h;
  toHex(out, 32, h);
  return h;
}

// ---------------------------------------------------------------
void Auth::begin() {
  memset(_sessions, 0, sizeof(_sessions));
  _activeSessions = 0;
  _failCount      = 0;
  _lockUntilMs    = 0;
  _lockDurationMs = AUTH_LOCKOUT_BASE_MS;
  _holdStartMs    = 0;
  _customPass     = false;

  _loadOrCreate();

#if AUTH_FACTORY_RESET_PIN >= 0
  pinMode(AUTH_FACTORY_RESET_PIN, INPUT_PULLUP);
#endif

  Serial.printf("[Auth] 管理员账号 '%s'，密码%s\n",
                _user.c_str(), _customPass ? "已自定义" : "仍是出厂默认（请尽快修改）");
}

void Auth::_loadOrCreate() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);   // 只读
  _user       = p.getString(NVS_KEY_ADM_USER, AUTH_DEFAULT_USER);
  _saltHex    = p.getString(NVS_KEY_ADM_SALT, "");
  _hashHex    = p.getString(NVS_KEY_ADM_HASH, "");
  _customPass = p.getUInt(NVS_KEY_ADM_SET, 0) != 0;
  p.end();

  if (_user.length() == 0) _user = AUTH_DEFAULT_USER;

  if (_saltHex.length() == 0 || _hashHex.length() == 0) {
    _saltHex    = _randomHex(16);
    _hashHex    = _hashPassword(AUTH_DEFAULT_PASS);
    _customPass = false;
    _save();
    Serial.println("[Auth] 未找到管理员凭据，已写入出厂默认账号/密码");
  }
}

void Auth::_save() {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putString(NVS_KEY_ADM_USER, _user);
  p.putString(NVS_KEY_ADM_SALT, _saltHex);
  p.putString(NVS_KEY_ADM_HASH, _hashHex);
  p.putUInt(NVS_KEY_ADM_SET, _customPass ? 1 : 0);
  p.end();
}

// ---------------------------------------------------------------
bool Auth::isLockedOut() const { return lockoutRemainingMs() > 0; }

unsigned long Auth::lockoutRemainingMs() const {
  if (_lockUntilMs == 0) return 0;
  long rem = (long)(_lockUntilMs - millis());
  return rem > 0 ? (unsigned long)rem : 0;
}

void Auth::_registerFail(String& errOut) {
  _failCount++;
  unsigned long remain = 0;
  if (_failCount >= AUTH_MAX_FAILS) {
    _lockUntilMs = millis() + _lockDurationMs;
    remain = _lockDurationMs;
    // 锁定时间翻倍，最多到上限
    if (_lockDurationMs < AUTH_LOCKOUT_MAX_MS) {
      _lockDurationMs = _lockDurationMs * 2;
      if (_lockDurationMs > AUTH_LOCKOUT_MAX_MS) _lockDurationMs = AUTH_LOCKOUT_MAX_MS;
    }
  }
  Serial.printf("[Auth] 登录失败 %u 次%s\n", (unsigned)_failCount,
                remain ? "（已进入锁定）" : "");
  if (remain) {
    errOut = "连续输错多次，请 " + String(remain / 1000) + " 秒后再试";
  } else {
    errOut = "账号或密码错误（还剩 " + String(AUTH_MAX_FAILS - _failCount) + " 次机会）";
  }
}

String Auth::_issueToken() {
  _expireSessions();

  int slot = -1;
  for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
    if (!_sessions[i].used) { slot = i; break; }
  }

  if (slot < 0) {
    // 槽位满了：淘汰"最久没活动"的那个会话，而不是拒绝登录。
    //
    // 为什么必须这样：会话只能用"持有令牌"的方式主动登出，没有令牌就腾不出空位。
    // 一旦累积到上限（多设备登录 / 反复刷新 / 浏览器页面忘关），管理员会
    // 连正确密码都登不进去，且没有任何自救手段 —— 只能干等会话超时或物理断电。
    // 对一台要卖给用户放在弱电箱里的设备来说，这是致命的。宁可挤掉别人的会话，
    // 也不能把主人锁在门外。
    unsigned long now    = millis();
    unsigned long oldest = 0;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
      unsigned long age = now - _sessions[i].lastSeen;
      if (age >= oldest) { oldest = age; slot = i; }
    }
    Serial.printf("[Auth] 会话已满，淘汰最旧会话（已闲置 %lus）\n", oldest / 1000);
    if (_activeSessions) _activeSessions--;
  }

  String t = _randomHex(AUTH_TOKEN_BYTES);
  strncpy(_sessions[slot].token, t.c_str(), sizeof(_sessions[slot].token) - 1);
  _sessions[slot].token[sizeof(_sessions[slot].token) - 1] = 0;
  _sessions[slot].used     = true;
  _sessions[slot].lastSeen = millis();
  _activeSessions++;
  return t;
}

bool Auth::validate(const String& token) {
  if (token.length() == 0) return false;
  for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
    if (!_sessions[i].used) continue;
    if (strcmp(_sessions[i].token, token.c_str()) != 0) continue;
    if (millis() - _sessions[i].lastSeen > AUTH_SESSION_TIMEOUT_MS) {
      _sessions[i].used = false;
      if (_activeSessions) _activeSessions--;
      return false;
    }
    _sessions[i].lastSeen = millis();
    return true;
  }
  return false;
}

void Auth::logout(const String& token) {
  if (token.length() == 0) return;
  for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
    if (_sessions[i].used && strcmp(_sessions[i].token, token.c_str()) == 0) {
      _sessions[i].used = false;
      if (_activeSessions) _activeSessions--;
      return;
    }
  }
}

void Auth::_dropAllSessions() {
  for (int i = 0; i < AUTH_MAX_SESSIONS; i++) _sessions[i].used = false;
  _activeSessions = 0;
}

void Auth::_expireSessions() {
  unsigned long now = millis();
  for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
    if (!_sessions[i].used) continue;
    if (now - _sessions[i].lastSeen > AUTH_SESSION_TIMEOUT_MS) {
      _sessions[i].used = false;
      if (_activeSessions) _activeSessions--;
    }
  }
}

// ---------------------------------------------------------------
bool Auth::login(const String& username, const String& password,
                 String& tokenOut, String& errOut) {
  if (isLockedOut()) {
    errOut = "连续输错太多次，请 " + String(lockoutRemainingMs() / 1000 + 1) + " 秒后再试";
    return false;
  }
  if (username != _user) { _registerFail(errOut); return false; }

  String h = _hashPassword(password);
  unsigned diff = h.length() ^ _hashHex.length();
  size_t n = h.length() < _hashHex.length() ? h.length() : _hashHex.length();
  for (size_t i = 0; i < n; i++) diff |= (unsigned)((uint8_t)h[i] ^ (uint8_t)_hashHex[i]);
  if (diff != 0) { _registerFail(errOut); return false; }

  // 成功：清零失败计数
  _failCount      = 0;
  _lockUntilMs    = 0;
  _lockDurationMs = AUTH_LOCKOUT_BASE_MS;

  tokenOut = _issueToken();
  if (tokenOut.length() == 0) {
    errOut = "登录会话已满，请稍后再试";
    return false;
  }
  Serial.printf("[Auth] 登录成功（当前会话 %u 个）\n", (unsigned)_activeSessions);
  return true;
}

bool Auth::changePassword(const String& oldPass, const String& newPass, String& errOut) {
  if (newPass.length() < AUTH_PASS_MIN_LEN) {
    errOut = "密码至少 " + String(AUTH_PASS_MIN_LEN) + " 位";
    return false;
  }
  if (newPass.length() > AUTH_PASS_MAX_LEN) {
    errOut = "密码最多 " + String(AUTH_PASS_MAX_LEN) + " 位";
    return false;
  }
  if (_hashPassword(oldPass) != _hashHex) {
    errOut = "原密码不正确";
    return false;
  }
  if (oldPass == newPass) {
    errOut = "新密码不能与原密码相同";
    return false;
  }

  _saltHex    = _randomHex(16);
  _hashHex    = _hashPassword(newPass);
  _customPass = true;
  _save();

  // 换密码后踢掉所有会话：万一旧会话已经泄露，改密后立刻失效
  _dropAllSessions();
  _failCount   = 0;
  _lockUntilMs = 0;
  Serial.println("[Auth] 管理员密码已更新，所有会话已登出");
  return true;
}

bool Auth::clearPassword() {
  _saltHex     = _randomHex(16);
  _hashHex     = _hashPassword(AUTH_DEFAULT_PASS);
  _customPass  = false;
  _save();
  _dropAllSessions();
  _failCount      = 0;
  _lockUntilMs    = 0;
  _lockDurationMs = AUTH_LOCKOUT_BASE_MS;
  Serial.printf("[Auth] 已恢复出厂默认密码（%s / %s）\n", AUTH_DEFAULT_USER, AUTH_DEFAULT_PASS);
  return true;
}

void Auth::task() {
  static unsigned long lastSweep = 0;
  if (millis() - lastSweep > 30000) {
    lastSweep = millis();
    _expireSessions();
  }

#if AUTH_FACTORY_RESET_PIN >= 0
  // 长按 BOOT 键恢复出厂密码 —— 忘密码时唯一的救场手段。
  // 内部上拉 + 要求"连续按住"，松开立刻清零，悬空引脚的偶发抖动触发不了。
  if (digitalRead(AUTH_FACTORY_RESET_PIN) == LOW) {
    if (_holdStartMs == 0) {
      _holdStartMs = millis();
      Serial.printf("[Auth] BOOT 键按下，继续按住 %u 秒可清空管理员密码\n",
                    (unsigned)(AUTH_FACTORY_HOLD_MS / 1000));
    } else if (millis() - _holdStartMs >= AUTH_FACTORY_HOLD_MS) {
      Serial.println("[Auth] BOOT 长按到位 → 清空管理员密码");
      clearPassword();
      _holdStartMs = 0;
      unsigned long t0 = millis();
      while (digitalRead(AUTH_FACTORY_RESET_PIN) == LOW && millis() - t0 < 30000) {
        delay(100);
      }
    }
  } else {
    _holdStartMs = 0;
  }
#endif
}
