// =====================================================================
//  store.cpp  -  設定永続化 + JSON 変換 実装
// =====================================================================
#include "store.h"
#include "control.h"      // heater::capTarget
#include <Arduino.h>
#include <LittleFS.h>

namespace store {

static const char* PATH = "/settings.json";

// ---- JSON -> Point 配列 (x 昇順前提・件数クランプ) ----
static int readPts(JsonArrayConst arr, curve::Point* dst) {
  int n = 0;
  for (JsonObjectConst o : arr) {
    if (n >= curve::MAX_POINTS) break;
    dst[n].x = o["x"] | 0.0f;
    dst[n].y = o["y"] | 0.0f;
    n++;
  }
  return n;
}
static void writePts(JsonArray arr, const curve::Point* src, int n) {
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["x"] = src[i].x;
    o["y"] = src[i].y;
  }
}

void toJson(const Settings& s, JsonObject root) {
  JsonObject L = root["light"].to<JsonObject>();
  writePts(L["r"].to<JsonArray>(), s.light.r, s.light.nr);
  writePts(L["g"].to<JsonArray>(), s.light.g, s.light.ng);
  writePts(L["b"].to<JsonArray>(), s.light.b, s.light.nb);
  writePts(L["w"].to<JsonArray>(), s.light.w, s.light.nw);
  JsonObject F = root["fan"].to<JsonObject>();
  writePts(F["points"].to<JsonArray>(), s.fan.p, s.fan.n);
  JsonObject H = root["heater"].to<JsonObject>();
  H["target"] = s.heater.target;
  H["hyst"]   = s.heater.hyst;
  JsonObject S = root["safe"].to<JsonObject>();
  S["lo"] = s.safe.lo;
  S["hi"] = s.safe.hi;
}

bool fromJson(JsonObjectConst root, Settings& s) {
  if (root["light"].is<JsonObjectConst>()) {
    JsonObjectConst L = root["light"];
    if (L["r"].is<JsonArrayConst>()) s.light.nr = readPts(L["r"], s.light.r);
    if (L["g"].is<JsonArrayConst>()) s.light.ng = readPts(L["g"], s.light.g);
    if (L["b"].is<JsonArrayConst>()) s.light.nb = readPts(L["b"], s.light.b);
    if (L["w"].is<JsonArrayConst>()) s.light.nw = readPts(L["w"], s.light.w);
  }
  if (root["fan"]["points"].is<JsonArrayConst>())
    s.fan.n = readPts(root["fan"]["points"], s.fan.p);
  if (root["heater"].is<JsonObjectConst>()) {
    if (!root["heater"]["target"].isNull())
      s.heater.target = heater::capTarget((float)root["heater"]["target"]);   // 35°C キャップ
    if (!root["heater"]["hyst"].isNull()) {
      float h = root["heater"]["hyst"]; s.heater.hyst = h < 0 ? 0 : h;
    }
  }
  if (root["safe"].is<JsonObjectConst>()) {
    if (!root["safe"]["lo"].isNull()) s.safe.lo = (float)root["safe"]["lo"];
    if (!root["safe"]["hi"].isNull()) s.safe.hi = (float)root["safe"]["hi"];
  }
  return true;
}

bool load() {
  if (!LittleFS.exists(PATH)) return false;
  File f = LittleFS.open(PATH, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return false;
  state_lock();
  fromJson(doc.as<JsonObjectConst>(), g_set);
  state_unlock();
  return true;
}

bool save() {
  File f = LittleFS.open(PATH, "w");
  if (!f) return false;
  JsonDocument doc;
  state_lock();
  toJson(g_set, doc.to<JsonObject>());
  state_unlock();
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

// ---- 名前付きプリセット (LittleFS /presets.json) ----
const char* PRESETS_PATH = "/presets.json";

static bool validType(const String& t) { return t == "led" || t == "fan"; }

bool presetSave(const String& type, const String& name, JsonVariantConst data) {
  if (name.length() == 0 || !validType(type)) return false;
  JsonDocument doc;
  if (LittleFS.exists(PRESETS_PATH)) {
    File f = LittleFS.open(PRESETS_PATH, "r");
    if (f) { deserializeJson(doc, f); f.close(); }
  }
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();   // 壊れ/空なら新規オブジェクト
  JsonObject root = doc.as<JsonObject>();
  // type グループ (既存なら流用、無ければ作成。既存を to<> でクリアしないよう分岐)
  JsonObject grp = root[type].is<JsonObject>() ? root[type].as<JsonObject>()
                                               : root[type].to<JsonObject>();
  // 新規名かつ上限到達なら拒否 (無制限な FS 肥大を防ぐ)
  if (grp[name].isNull() && (int)grp.size() >= MAX_PRESETS) return false;
  grp[name] = data;                                  // deep copy (別ドキュメントへ複製)
  File w = LittleFS.open(PRESETS_PATH, "w");
  if (!w) return false;
  bool ok = serializeJson(doc, w) > 0;
  w.close();
  return ok;
}

bool presetDelete(const String& type, const String& name) {
  if (!validType(type)) return false;
  if (!LittleFS.exists(PRESETS_PATH)) return true;   // 既に無い = 成功扱い
  JsonDocument doc;
  File f = LittleFS.open(PRESETS_PATH, "r");
  if (!f) return false;
  deserializeJson(doc, f); f.close();
  if (!doc.is<JsonObject>()) return true;
  JsonObject root = doc.as<JsonObject>();
  if (root[type].is<JsonObject>()) root[type].as<JsonObject>().remove(name);
  File w = LittleFS.open(PRESETS_PATH, "w");
  if (!w) return false;
  bool ok = serializeJson(doc, w) > 0;
  w.close();
  return ok;
}

void begin() {
  if (!LittleFS.begin(true)) {     // true: 失敗時フォーマット
    Serial.println("[store] LittleFS mount FAILED (パーティション/ラベル不一致を疑う)");
    return;
  }
  Serial.printf("[store] LittleFS mounted: total=%u used=%u\n",
                (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
  if (!load()) {                   // 無ければ既定で保存
    state_lock(); settings_defaults(g_set); state_unlock();
    save();
  }
}

}  // namespace store
