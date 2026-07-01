// =====================================================================
//  web.h / web.cpp  -  AsyncWebServer (REST API + LittleFS UI 配信)
//  API:
//   GET  /api/state              ライブ値
//   GET  /api/settings           設定カーブ一式 (= 既存 /api/settings 規約)
//   POST /api/settings           部分更新 + 保存 (heater target は 35°C キャップ)
//   GET  /api/history?tier=f|m|h 履歴 (ストリーム)
//   GET  /api/wifi               Wi-Fi 状態
//   POST /api/wifi  {ssid,pass}  STA 接続
//   POST /api/mode  {mode}       ap|sta|standalone
// =====================================================================
#pragma once

namespace web {
void begin();
}  // namespace web
