// =====================================================================
//  store.h / store.cpp  -  設定の永続化 (LittleFS) と JSON 変換
//  JSON スキーマはダッシュボードのモデルと一致 ({x,y} オブジェクト配列)。
// =====================================================================
#pragma once
#include "state.h"
#include <ArduinoJson.h>

namespace store {
void begin();                                   // LittleFS マウント + 読込 (無ければ既定)
bool load();                                    // /settings.json -> g_set
bool save();                                    // g_set -> /settings.json

// 校正オフセット (/calib.json)。無ければ config.h 既定のまま。
bool calibLoad();                               // /calib.json -> g_calib
bool calibSave();                               // g_calib -> /calib.json

// 共有 JSON 変換 (web ハンドラからも使用)
void toJson(const Settings& s, JsonObject root);
bool fromJson(JsonObjectConst root, Settings& s);   // 部分更新可 (存在キーのみ反映)

// ---- 名前付きプリセット (LittleFS /presets.json に保存。全端末・全ブラウザ・再起動で共有) ----
//  構造: {"led":{"名前":{r,g,b,w}}, "fan":{"名前":{points:[...]}}}。type は "led" / "fan"。
constexpr int   MAX_PRESETS = 12;                   // type ごとの上限
extern const char* PRESETS_PATH;                    // "/presets.json"
bool presetSave(const String& type, const String& name, JsonVariantConst data);  // 追加/更新 (上限超過で false)
bool presetDelete(const String& type, const String& name);                       // 削除
}  // namespace store
