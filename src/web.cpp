// =====================================================================
//  web.cpp  -  AsyncWebServer 実装
// =====================================================================
#include "web.h"
#include "state.h"
#include "store.h"
#include "history.h"
#include "net.h"
#include "control.h"
#include "web_ui_gz.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>

namespace web {

static AsyncWebServer server(80);

static void sendState(AsyncWebServerRequest* req) {
  JsonDocument d;
  state_lock();
  d["water"] = g_live.water; d["waterValid"] = g_live.waterValid;
  d["air"] = g_live.air; d["press"] = g_live.press;
  d["humidity"] = g_live.humidity; d["humidityValid"] = g_live.humidityValid;
  // 校正: 適用中オフセット(g_calib) + ライブ差分(基準-作業。書込前でも測定生存を確認)
  JsonObject cal = d["calib"].to<JsonObject>();
  cal["water"] = g_calib.water; cal["air"] = g_calib.air;
  cal["press"] = g_calib.press; cal["humid"] = g_calib.humid;
  cal["diffAir"] = g_live.calibDiffAir; cal["diffHumid"] = g_live.calibDiffHumid;
  cal["diffValid"] = g_live.calibDiffValid;
  JsonObject led = d["led"].to<JsonObject>();
  led["r"] = g_live.ledR; led["g"] = g_live.ledG; led["b"] = g_live.ledB; led["w"] = g_live.ledW;
  JsonObject fanO = d["fan"].to<JsonObject>();
  fanO["duty"] = g_live.fanDuty; fanO["rpm"] = g_live.fanRpm; fanO["airflow"] = g_live.airflow;
  JsonObject ht = d["heater"].to<JsonObject>();
  ht["on"] = g_live.heaterOn; ht["target"] = g_set.heater.target;
  d["alarm"] = g_live.alarm; d["alarmDir"] = g_live.alarmDir;
  d["sensorFault"] = g_live.sensorFault;    // 水温センサ無応答
  d["heatFault"]   = g_live.heatFault;      // ヒーターONでも上がらない
  d["coolFault"]   = g_live.coolFault;      // ファンONでも下がらない
  d["mode"] = (int)g_live.mode;
  state_unlock();
  d["time"]      = net::epoch();
  d["timeValid"] = net::timeValid();          // 実時刻確定済みか (未確定=手動設定を促す)
  d["ip"]   = net::ip();
  d["ssid"] = net::ssid();
  String out; serializeJson(d, out);
  req->send(200, "application/json", out);
}

static void sendSettings(AsyncWebServerRequest* req) {
  JsonDocument d;
  store::toJson(g_set, d.to<JsonObject>());   // toJson が内部で lock 不要 (呼出側で保証)
  String out; serializeJson(d, out);
  req->send(200, "application/json", out);
}

static void sendHistory(AsyncWebServerRequest* req) {
  char tier = 'f';
  if (req->hasParam("tier")) {
    String t = req->getParam("tier")->value();
    if (t.length()) tier = t[0];
  }
  AsyncResponseStream* res = req->beginResponseStream("application/json");
  if (!history::writeJson(tier, *res)) { req->send(400, "text/plain", "bad tier"); return; }
  req->send(res);
}

static void sendWifi(AsyncWebServerRequest* req) {
  JsonDocument d;
  d["mode"]       = (int)net::mode();      // 0=AP 1=STA 2=standalone
  d["modeStr"]    = net::modeStr();        // "ap"/"sta"
  d["configured"] = net::configured();     // wifi.ini があるか
  d["conn"]       = (int)net::connState(); // 0 idle 1 connecting 2 ok 3 fail
  d["ssid"]       = net::ssid();
  d["apSsid"]     = net::apSsid();
  d["staSsid"]    = net::staSsid();
  d["mdns"]       = net::mdnsName();
  d["ip"]         = net::ip();
  d["rssi"]       = net::rssi();
  String out; serializeJson(d, out);
  req->send(200, "application/json", out);
}

// 設定ダウンロード用 (パスワード含む。バックアップ/復元)
static void sendWifiConfig(AsyncWebServerRequest* req) {
  JsonDocument d;
  d["mode"]     = net::modeStr();
  d["ap_ssid"]  = net::apSsid();
  d["ap_pw"]    = net::apPw();
  d["sta_ssid"] = net::staSsid();
  d["sta_pw"]   = net::staPw();
  d["mdns"]     = net::mdnsName();
  String out; serializeJson(d, out);
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
  res->addHeader("Content-Disposition", "attachment; filename=wifi.ini.json");
  req->send(res);
}

void begin() {
  // ---- 設定 GET ----
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* r) {
    state_lock(); sendSettings(r); state_unlock();
  });
  server.on("/api/state",    HTTP_GET, sendState);
  server.on("/api/history",  HTTP_GET, sendHistory);
  // 【重要】ESPAsyncWebServer の canHandle は "/api/wifi" が "/api/wifi/" 配下の
  //   全パス (…/scan, …/config) にもマッチする (前方一致)。ハンドラは登録順評価のため、
  //   具体的な "/api/wifi/scan" "/api/wifi/config" を汎用の "/api/wifi" より必ず先に登録する。
  server.on("/api/wifi/config", HTTP_GET, sendWifiConfig);   // ダウンロード (/api/wifi より前)

  // ---- Wi-Fi スキャン (同期ブロッキング; 診断用に生の戻り値 n / mode も返す) ----
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    wifi_mode_t m0 = WiFi.getMode();
    if (!(m0 & WIFI_MODE_STA)) {
      WiFi.mode((m0 & WIFI_MODE_AP) ? WIFI_AP_STA : WIFI_STA);
      delay(100);                               // STA netif 起動待ち
    }
    WiFi.scanDelete();                          // 前回結果をクリア
    // 同期(blocking) / show_hidden=true / active / 各ch 300ms。
    //  n>=0: 件数、n<0: エラー(-1 RUNNING, -2 FAILED)。診断のため生値を返す。
    int n = WiFi.scanNetworks(false, true, false, 300);
    Serial.printf("[wifi/scan] sync n=%d mode=%d\n", n, (int)WiFi.getMode());
    JsonDocument d;
    d["scanning"] = false;
    d["n"]    = n;                              // 診断: scanNetworks の生戻り値
    d["mode"] = (int)WiFi.getMode();            // 診断: 現在の wifi mode
    JsonArray a = d["aps"].to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["lock"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    String out; serializeJson(d, out);
    r->send(200, "application/json", out);
  });

  // 汎用 "/api/wifi" は具体パス (config/scan) の後に登録 (前方一致で横取りしないため)
  server.on("/api/wifi",     HTTP_GET, sendWifi);

  // ---- 名前付きプリセット GET: LittleFS の /presets.json をそのまま返す (無ければ {}) ----
  server.on("/api/presets", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (LittleFS.exists(store::PRESETS_PATH))
      r->send(LittleFS, store::PRESETS_PATH, "application/json");
    else
      r->send(200, "application/json", "{}");
  });

  // ---- ファイルコマンド (デバッグ用): ls / get / put ----
  //  ※ wifi.ini 等の資格情報も読める。ローカル(AP/LAN)前提のデバッグ機能。
  server.on("/api/fs/ls", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    JsonArray a = d["files"].to<JsonArray>();
    File root = LittleFS.open("/");
    if (root) {
      File f = root.openNextFile();
      while (f) { JsonObject o = a.add<JsonObject>(); o["name"] = String(f.name()); o["size"] = (uint32_t)f.size(); f = root.openNextFile(); }
    }
    d["total"] = (uint32_t)LittleFS.totalBytes();
    d["used"]  = (uint32_t)LittleFS.usedBytes();
    String out; serializeJson(d, out);
    r->send(200, "application/json", out);
  });
  server.on("/api/fs/get", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("name")) { r->send(400, "text/plain", "name required"); return; }
    String name = r->getParam("name")->value();
    if (!name.startsWith("/")) name = "/" + name;
    if (!LittleFS.exists(name)) { r->send(404, "text/plain", "not found"); return; }
    r->send(LittleFS, name, "application/octet-stream");
  });
  // PUT: body をそのままファイルへ (index==0 で truncate, 以降 append)
  server.on("/api/fs/put", HTTP_POST,
    [](AsyncWebServerRequest* r) { r->send(200, "application/json", "{\"ok\":true}"); },
    nullptr,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
      if (!r->hasParam("name")) return;
      String name = r->getParam("name")->value();
      if (!name.startsWith("/")) name = "/" + name;
      File f = LittleFS.open(name, index == 0 ? "w" : "a");
      if (f) { f.write(data, len); f.close(); }
    });

  // ---- 設定 POST (部分更新 + 保存) ----
  auto* setPost = new AsyncCallbackJsonWebHandler("/api/settings",
    [](AsyncWebServerRequest* r, JsonVariant& json) {
      if (!json.is<JsonObject>()) { r->send(400, "text/plain", "bad json"); return; }
      state_lock();
      store::fromJson(json.as<JsonObjectConst>(), g_set);
      state_unlock();
      store::save();
      state_lock(); sendSettings(r); state_unlock();
    });
  server.addHandler(setPost);

  // ---- Wi-Fi 設定 保存/アップロード POST : {mode,ap_ssid,ap_pw,sta_ssid,sta_pw,mdns} → 保存+再起動 ----
  //   【重要】"/api/wifi/config" は汎用 "/api/wifi" POST より先に登録 (前方一致の横取り回避)。
  auto* wifiCfgPost = new AsyncCallbackJsonWebHandler("/api/wifi/config",
    [](AsyncWebServerRequest* r, JsonVariant& json) {
      bool ok = net::applyConfig(
        (const char*)(json["mode"]     | ""),
        (const char*)(json["ap_ssid"]  | ""),
        (const char*)(json["ap_pw"]    | ""),
        (const char*)(json["sta_ssid"] | ""),
        (const char*)(json["sta_pw"]   | ""),
        (const char*)(json["mdns"]     | ""));
      if (!ok) {
        r->send(400, "application/json",
                "{\"ok\":false,\"err\":\"ap_pw must be empty or 8+ chars (WPA2)\"}");
        return;
      }
      r->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");   // 約1.5秒後に再起動
    });
  server.addHandler(wifiCfgPost);

  // ---- Wi-Fi POST {ssid,pass} : 非ブロッキング (要求だけ受け即応答。実接続は net::loop) ----
  auto* wifiPost = new AsyncCallbackJsonWebHandler("/api/wifi",
    [](AsyncWebServerRequest* r, JsonVariant& json) {
      String ssid = (const char*)(json["ssid"] | "");
      String pass = (const char*)(json["pass"] | "");
      if (ssid.length() == 0) { r->send(400, "application/json", "{\"ok\":false,\"err\":\"no ssid\"}"); return; }
      net::requestConnect(ssid, pass);          // ブロックせずキュー
      r->send(200, "application/json", "{\"ok\":true,\"pending\":true}");
    });
  server.addHandler(wifiPost);

  // ---- プリセット POST : {type:"led"|"fan", name, data} 保存 / {type, delete:"名前"} 削除 → LittleFS ----
  auto* presetPost = new AsyncCallbackJsonWebHandler("/api/presets",
    [](AsyncWebServerRequest* r, JsonVariant& json) {
      String type = (const char*)(json["type"] | "led");   // 省略時は led (後方互換)
      if (json["delete"].is<const char*>()) {
        bool ok = store::presetDelete(type, (const char*)json["delete"]);
        r->send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        return;
      }
      String name = (const char*)(json["name"] | "");
      if (name.length() == 0 || !json["data"].is<JsonObjectConst>()) {
        r->send(400, "application/json", "{\"ok\":false,\"err\":\"type/name/data required\"}");
        return;
      }
      bool ok = store::presetSave(type, name, json["data"]);
      r->send(ok ? 200 : 507, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"full/invalid type/io error\"}");
    });
  server.addHandler(presetPost);

  // ---- 時刻 手動設定 POST {epoch:<UTC秒>} : AP モード等 NTP 無し環境用 ----
  auto* timePost = new AsyncCallbackJsonWebHandler("/api/time",
    [](AsyncWebServerRequest* r, JsonVariant& json) {
      uint32_t e = json["epoch"] | 0UL;
      if (e < 1700000000UL) {                   // 2023-11 以前は不正扱い
        r->send(400, "application/json", "{\"ok\":false,\"err\":\"bad epoch\"}");
        return;
      }
      net::setEpoch(e);
      r->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(timePost);

  // ---- モード POST {mode:"ap"|"standalone"} : AP スタンドアローン確定 ----
  auto* modePost = new AsyncCallbackJsonWebHandler("/api/mode",
    [](AsyncWebServerRequest* r, JsonVariant& json) {
      String m = (const char*)(json["mode"] | "");
      if (m == "standalone" || m == "ap") net::requestStandaloneAP();  // wifi.ini mode=ap 保存
      r->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(modePost);

  // ---- デバッグ・オーバーライド POST ----
  //  {led:{min:N}} 時刻固定 / {led:{off:true}} 解除
  //  {fan:{duty:N}} 手動運転 / {fan:{off:true}} 解除   TTL=4s で自動解除
  auto* testPost = new AsyncCallbackJsonWebHandler("/api/test",
    [](AsyncWebServerRequest* r, JsonVariant& json) {
      uint32_t now = millis();
      state_lock();
      if (json["led"].is<JsonObject>()) {
        if (json["led"]["off"] | false) { g_ovr.ledActive = false; }
        else if (!json["led"]["min"].isNull()) {
          g_ovr.ledActive = true;
          g_ovr.ledMinute = curve::clampf((float)json["led"]["min"], 0.0f, 1440.0f);
          g_ovr.ledExpireMs = now + 4000;
        }
      }
      if (json["fan"].is<JsonObject>()) {
        if (json["fan"]["off"] | false) { g_ovr.fanActive = false; }
        else if (!json["fan"]["duty"].isNull()) {
          g_ovr.fanActive = true;
          g_ovr.fanDuty = curve::clampf((float)json["fan"]["duty"], 0.0f, 100.0f);
          g_ovr.fanExpireMs = now + 4000;
        }
      }
      state_unlock();
      r->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(testPost);

  // ---- 完全版 UI を gzip で返す共通ハンドラ ----
  auto sendEmbedded = [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", WEBUI_GZ, WEBUI_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    r->send(res);
  };

  // ---- UI 配信: 常に埋め込みバンドル (web_ui_gz.h) を返す ----
  //  【重要】以前は LittleFS の /index.html を優先していたが、過去に uploadfs した
  //  古い UI が最新 (埋め込み) を隠す事故が起きるため廃止。バンドルは CSS/JS を内包する
  //  単一 HTML なので外部アセット要求は発生せず、LittleFS UI ファイルは不要。
  //  LittleFS は設定ファイル (wifi.ini / settings.json / presets.json) 専用とする。
  server.on("/", HTTP_GET, sendEmbedded);
  server.onNotFound(sendEmbedded);

  server.begin();
}

}  // namespace web
