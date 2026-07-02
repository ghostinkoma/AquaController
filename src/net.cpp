// =====================================================================
//  net.cpp  -  Wi-Fi 管理 (wifi.ini) + NTP + mDNS
// =====================================================================
#include "net.h"
#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>

namespace net {

static const char* WIFI_INI = "/wifi.ini";

struct Cfg {
  String mode    = "ap";              // "ap" / "sta"
  String apSsid  = AQ_AP_SSID;
  String apPw    = AQ_AP_PW_DEFAULT;  // <8文字は開放 (WPA2 制約)
  String staSsid = "";
  String staPw   = "";
  String mdns    = AQ_HOSTNAME;
};
static Cfg      s_cfg;
static bool     s_configured = false;
static ConnState s_conn = CS_IDLE;

static NetMode  s_mode = MODE_AP;
static bool     s_ntpStarted = false;
static uint32_t s_bootBase = 0;
static uint32_t s_lastTry  = 0;
static uint32_t s_rebootAt = 0;      // 設定適用/STA移行のための再起動時刻
static bool     s_mdns     = false;

// --- 要求 (loop で実行) ---
static bool   s_connReq = false, s_standaloneReq = false;
static String s_reqSsid, s_reqPass;

static constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 12000;
static constexpr int      STA_BOOT_RETRIES       = 3;    // 起動時 STA リトライ回数 (弱電波対策)

static void setMode(NetMode m) { s_mode = m; state_lock(); g_live.mode = m; state_unlock(); }

// wifi.ini は "key=value\n" 形式の単純パーサのため、値に改行/復帰が混じると
// 別キーとして誤解釈されうる (設定注入)。保存前に必ず除去する。
static String sanitize(const String& in) {
  String out; out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\r' || c == '\n') continue;
    out += c;
  }
  return out;
}

// ---- wifi.ini 読み書き (key=value) ----
static void loadCfg() {
  s_configured = false;
  if (!LittleFS.exists(WIFI_INI)) return;
  File f = LittleFS.open(WIFI_INI, "r"); if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    int eq = line.indexOf('='); if (eq < 1) continue;
    String k = line.substring(0, eq); k.trim();
    String v = line.substring(eq + 1); v.trim();
    if      (k == "mode")     s_cfg.mode    = v;
    else if (k == "ap_ssid")  s_cfg.apSsid  = v;
    else if (k == "ap_pw")    s_cfg.apPw    = v;
    else if (k == "sta_ssid") s_cfg.staSsid = v;
    else if (k == "sta_pw")   s_cfg.staPw   = v;
    else if (k == "mdns")     s_cfg.mdns    = v;
  }
  f.close();
  s_configured = true;
}

static void saveCfg() {
  File f = LittleFS.open(WIFI_INI, "w"); if (!f) return;
  f.printf("mode=%s\n",     s_cfg.mode.c_str());
  f.printf("ap_ssid=%s\n",  s_cfg.apSsid.c_str());
  f.printf("ap_pw=%s\n",    s_cfg.apPw.c_str());
  f.printf("sta_ssid=%s\n", s_cfg.staSsid.c_str());
  f.printf("sta_pw=%s\n",   s_cfg.staPw.c_str());
  f.printf("mdns=%s\n",     s_cfg.mdns.c_str());
  f.close();
  s_configured = true;
}

static void startMDNS() {
  MDNS.end();
  if (MDNS.begin(s_cfg.mdns.c_str())) { MDNS.addService("http", "tcp", 80); s_mdns = true; }
}

static void startNTP() {
  if (s_ntpStarted) return;
  configTime(tz::OFFSET_SEC, 0, "ntp.nict.jp", "pool.ntp.org");
  s_ntpStarted = true;
}

static void softAPup() {
  IPAddress ip; ip.fromString(AQ_AP_IP);
  IPAddress gw = ip, mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);
  if (s_cfg.apPw.length() >= 8) WiFi.softAP(s_cfg.apSsid.c_str(), s_cfg.apPw.c_str());
  else                          WiFi.softAP(s_cfg.apSsid.c_str());   // 開放 (WPA2 は8文字以上)
}

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);   // モデムスリープ無効 (USB-Serial-JTAG 断/スキャン取りこぼし対策)
  softAPup();
  setMode(MODE_AP);
  startMDNS();
}

// ブロッキング版。起動シーケンス (begin) 専用 — この時点では他タスク未起動で
// ヒーター/ファンも未駆動のため安全。STA 単独で接続し AP は作らない (併存させない)。
// keepAp は互換のため残すが常に false 扱い (AP と STA を同時に上げない方針)。
// 1回の STA 接続試行 (失敗しても AP は立てない。フォールバックは呼び出し側)。
bool connectSTA(const String& ssid, const String& pass, bool /*keepAp*/) {
  if (ssid.length() == 0) { s_conn = CS_FAIL; return false; }
  s_conn = CS_CONNECTING;
  WiFi.mode(WIFI_STA);                     // STA 単独 (AP は上げない)
  WiFi.setSleep(false);                    // モデムスリープ無効 (USB-Serial-JTAG 断/取りこぼし対策)
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_CONNECT_TIMEOUT_MS) delay(200);

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setHostname(s_cfg.mdns.c_str());
    s_cfg.mode = "sta"; s_cfg.staSsid = sanitize(ssid); s_cfg.staPw = sanitize(pass); saveCfg();
    setMode(MODE_STA); startNTP(); startMDNS();
    s_conn = CS_OK;
    return true;
  }
  s_conn = CS_FAIL;
  return false;
}

// 起動時 STA 接続 (弱電波の間欠ハンドシェイク失敗に備え複数回リトライ)。失敗で AP へ。
static void connectSTAWithRetry(const String& ssid, const String& pass) {
  for (int i = 0; i < STA_BOOT_RETRIES; i++) {
    if (i) { WiFi.disconnect(true, true); delay(400); }   // 状態リセットして再試行
    Serial.printf("[net] STA attempt %d/%d to %s\n", i + 1, STA_BOOT_RETRIES, ssid.c_str());
    if (connectSTA(ssid, pass, false)) { Serial.println("[net] STA connected"); return; }
  }
  Serial.println("[net] STA failed after retries -> AP");
  startAP();                               // 全滅 → クリーンな AP フォールバック
}

void begin() {
  s_bootBase = millis();
  loadCfg();
  if (!s_configured) { s_cfg.mode = "ap"; startAP(); return; }   // 初回: AP セットアップ
  if (s_cfg.mode == "sta" && s_cfg.staSsid.length()) {
    connectSTAWithRetry(s_cfg.staSsid, s_cfg.staPw);             // 複数回試行 → 失敗で AP
  } else {
    startAP();                            // AP スタンドアローン
  }
}

// ---- 要求 API (async ハンドラから。実処理は loop) ----
void requestConnect(const String& ssid, const String& pass) {
  s_reqSsid = ssid; s_reqPass = pass; s_connReq = true; s_conn = CS_CONNECTING;
}
void requestStandaloneAP() { s_standaloneReq = true; }

bool applyConfig(const String& mode, const String& apSsid, const String& apPw,
                 const String& staSsid, const String& staPw, const String& mdns) {
  // WPA2 は8文字以上必須。1〜7文字を許すと「保護したつもり」で無条件に開放APへ
  // 降格してしまう (softAPup 参照) ため、ここで弾く。0文字 (開放) は意図的として許容。
  String pw = sanitize(apPw);
  if (pw.length() > 0 && pw.length() < 8) return false;

  if (mode.length())    s_cfg.mode    = sanitize(mode);
  if (apSsid.length())  s_cfg.apSsid  = sanitize(apSsid);
  s_cfg.apPw   = pw;                       // 空許容 (開放 AP)
  if (staSsid.length()) s_cfg.staSsid = sanitize(staSsid);
  if (staPw.length())   s_cfg.staPw   = sanitize(staPw);
  if (mdns.length())    s_cfg.mdns    = sanitize(mdns);
  saveCfg();
  g_haltActuators = true;                  // 再起動までヒーター/ファンを安全停止
  s_rebootAt = millis() + 1500;            // 保存後、確実に反映するため再起動
  return true;
}

void eraseCreds() { if (LittleFS.exists(WIFI_INI)) LittleFS.remove(WIFI_INI); }

void loop() {
  // STA 接続要求: AP と STA を同時に上げない (セキュリティ)。資格情報を保存し、
  // アクチュエータを安全停止してから再起動 → 起動時に STA 単独で接続 (AP を作らない)。
  // 起動時の STA 接続中は制御タスク未起動のためファン/ヒーターは自然に OFF。
  if (s_connReq) {
    s_connReq = false;
    s_cfg.mode = "sta";
    s_cfg.staSsid = sanitize(s_reqSsid);
    s_cfg.staPw   = sanitize(s_reqPass);
    saveCfg();
    g_haltActuators = true;                 // 再起動までの猶予中もヒーター/ファンを止める
    s_conn = CS_CONNECTING;
    s_rebootAt = millis() + 1500;           // 応答返却後に再起動 → STA 単独へ
  }
  if (s_standaloneReq){ s_standaloneReq = false; s_cfg.mode = "ap"; saveCfg(); s_conn = CS_OK; }

  if (s_rebootAt && (int32_t)(millis() - s_rebootAt) >= 0) { delay(50); ESP.restart(); }

  // STA 単独運用中に切断されたら再接続 (AP は作らない)
  if (s_mode == MODE_STA && WiFi.status() != WL_CONNECTED) {
    if (millis() - s_lastTry > 30000) { s_lastTry = millis(); WiFi.reconnect(); }
  }
}

// ---- 状態取得 ----
NetMode  mode()        { return s_mode; }
bool     configured()  { return s_configured; }
ConnState connState()  { return s_conn; }
String   ip()          { return s_mode == MODE_STA ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }
String   ssid()        { return s_mode == MODE_STA ? WiFi.SSID() : s_cfg.apSsid; }
String   apSsid()      { return s_cfg.apSsid; }
String   apPw()        { return s_cfg.apPw; }
String   staSsid()     { return s_cfg.staSsid; }
String   staPw()       { return s_cfg.staPw; }
String   mdnsName()    { return s_cfg.mdns; }
String   modeStr()     { return s_mode == MODE_STA ? "sta" : "ap"; }   // 実モード (config値でなく実状態)
int      rssi()        { return s_mode == MODE_STA ? WiFi.RSSI() : 0; }

uint32_t epoch() {
  time_t now = time(nullptr);
  if (now > 1700000000) return (uint32_t)now;
  return (millis() - s_bootBase) / 1000;
}
uint32_t epochLocal() { return epoch() + (uint32_t)tz::OFFSET_SEC; }
bool timeValid() { return time(nullptr) > 1700000000; }

// 手動で時刻設定 (AP モード等 NTP が無い環境用)。UTC epoch を渡す。
void setEpoch(uint32_t utc) {
  struct timeval tv;
  tv.tv_sec  = (time_t)utc;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

}  // namespace net
