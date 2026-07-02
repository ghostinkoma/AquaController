// =====================================================================
//  store.cpp  -  設定永続化 + JSON 変換 実装
// =====================================================================
#include "store.h"
#include "control.h"      // heater::capTarget
#include <Arduino.h>
#include <LittleFS.h>

namespace store {

static const char* PATH = "/settings.json";

// =====================================================================
//  フェイルセーフ書き込み (電源急断による JSON 破損対策)
//  方式: 本体(.json) + CRC(.crc) + 直近正常のバックアップ(.old/.old.crc)。
//   - 書込前に「検証OKの現行値」を .old へ退避 → 本体書込 → CRC 記録。
//   - 読込時に本体の CRC が不一致(破損)なら .old を本体へ昇格して採用。
//   CRC32 はソフトで十分軽量 (SHA は C3 の負荷/水温2秒タスクを考え不採用)。
// =====================================================================
static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return c ^ 0xFFFFFFFFu;
}
static String readAll(const char* path) {
  File f = LittleFS.open(path, "r"); if (!f) return String();
  String s = f.readString(); f.close(); return s;
}
static bool writeAll(const char* path, const String& s) {
  File f = LittleFS.open(path, "w"); if (!f) return false;
  size_t w = f.print(s); f.close(); return w == s.length();
}
static bool verifyFile(const char* path, const char* crcPath) {
  if (!LittleFS.exists(path) || !LittleFS.exists(crcPath)) return false;
  String body = readAll(path); if (body.length() == 0) return false;
  String cs = readAll(crcPath); cs.trim();
  uint32_t stored = (uint32_t)strtoul(cs.c_str(), nullptr, 16);
  uint32_t actual = crc32((const uint8_t*)body.c_str(), body.length());
  return stored == actual;
}
// 本体を安全に書く。crc/old のパスは規約: <base> / <base>.crc / <base>.old / <base>.old.crc
static bool writeVerified(const char* path, const char* crcPath,
                          const char* oldPath, const char* oldCrcPath, const String& body) {
  if (verifyFile(path, crcPath)) {              // 直近の正常値を .old へ退避
    writeAll(oldPath, readAll(path));
    writeAll(oldCrcPath, readAll(crcPath));
  }
  if (!writeAll(path, body)) return false;
  char hex[9]; snprintf(hex, sizeof(hex), "%08x",
                        crc32((const uint8_t*)body.c_str(), body.length()));
  return writeAll(crcPath, String(hex));
}
// 検証付き読込。本体が破損していれば .old を本体へ昇格して返す。戻り値="" は両方無効。
static String loadVerified(const char* path, const char* crcPath,
                           const char* oldPath, const char* oldCrcPath) {
  if (verifyFile(path, crcPath)) return readAll(path);
  if (verifyFile(oldPath, oldCrcPath)) {        // 本体破損 → .old を昇格 (フェイルセーフ)
    String good = readAll(oldPath);
    writeAll(path, good);
    writeAll(crcPath, readAll(oldCrcPath));
    return good;
  }
  return String();
}

// 設定/プリセットのファイル群パス
static const char* SET_CRC     = "/settings.crc";
static const char* SET_OLD     = "/settings.old";
static const char* SET_OLD_CRC = "/settings.old.crc";
static const char* PR_CRC      = "/presets.crc";
static const char* PR_OLD      = "/presets.old";
static const char* PR_OLD_CRC  = "/presets.old.crc";

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
  // CRC 検証付き読込。本体破損時は .old を昇格 (電源断フェイルセーフ)。
  String body = loadVerified(PATH, SET_CRC, SET_OLD, SET_OLD_CRC);
  if (body.length() == 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  state_lock();
  fromJson(doc.as<JsonObjectConst>(), g_set);
  state_unlock();
  return true;
}

bool save() {
  JsonDocument doc;
  state_lock();
  toJson(g_set, doc.to<JsonObject>());
  state_unlock();
  String body; serializeJson(doc, body);
  return writeVerified(PATH, SET_CRC, SET_OLD, SET_OLD_CRC, body);
}

// ---- 名前付きプリセット (LittleFS /presets.json) ----
const char* PRESETS_PATH = "/presets.json";

static bool validType(const String& t) { return t == "led" || t == "fan"; }

// 検証付きで presets.json を読み込む (破損時は .old 昇格)。返り値は JsonObject を保持する doc。
static void presetsLoadDoc(JsonDocument& doc) {
  String body = loadVerified(PRESETS_PATH, PR_CRC, PR_OLD, PR_OLD_CRC);
  if (body.length() == 0 || deserializeJson(doc, body)) doc.to<JsonObject>();
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();
}

bool presetSave(const String& type, const String& name, JsonVariantConst data) {
  if (name.length() == 0 || !validType(type)) return false;
  JsonDocument doc; presetsLoadDoc(doc);
  JsonObject root = doc.as<JsonObject>();
  // type グループ (既存なら流用、無ければ作成。既存を to<> でクリアしないよう分岐)
  JsonObject grp = root[type].is<JsonObject>() ? root[type].as<JsonObject>()
                                               : root[type].to<JsonObject>();
  // 新規名かつ上限到達なら拒否 (無制限な FS 肥大を防ぐ)
  if (grp[name].isNull() && (int)grp.size() >= MAX_PRESETS) return false;
  grp[name] = data;                                  // deep copy (別ドキュメントへ複製)
  String body; serializeJson(doc, body);
  return writeVerified(PRESETS_PATH, PR_CRC, PR_OLD, PR_OLD_CRC, body);
}

bool presetDelete(const String& type, const String& name) {
  if (!validType(type)) return false;
  JsonDocument doc; presetsLoadDoc(doc);
  JsonObject root = doc.as<JsonObject>();
  if (root[type].is<JsonObject>()) root[type].as<JsonObject>().remove(name);
  String body; serializeJson(doc, body);
  return writeVerified(PRESETS_PATH, PR_CRC, PR_OLD, PR_OLD_CRC, body);
}

void begin() {
  if (!LittleFS.begin(true)) {     // true: 失敗時フォーマット
    Serial.println("[store] LittleFS mount FAILED (パーティション/ラベル不一致を疑う)");
    return;
  }
  Serial.printf("[store] LittleFS mounted: total=%u used=%u\n",
                (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
  if (!load()) {                   // 無ければ既定で保存 (load 内で CRC 検証/.old 昇格)
    state_lock(); settings_defaults(g_set); state_unlock();
    save();
  }
  // プリセットも起動時に検証・修復 (破損していれば .old を本体へ昇格させる)
  if (LittleFS.exists(PRESETS_PATH)) { JsonDocument d; presetsLoadDoc(d); }
}

}  // namespace store
