// =====================================================================
//  ota.h / ota.cpp  -  無線ファーム更新 (ArduinoOTA / espota)
//  PC とターゲット (5V ファン / AC ヒーター側) を USB で結合せずに書き込む。
//    PlatformIO: pio run -e esp32-c3-ota -t upload  (WiFi 経由, ポート3232)
//  生体安全: 書換中は g_haltActuators でヒーター/ファンを強制 OFF。
//  ブラウザからの POST /update は web.cpp 側 (Update.h) で提供。
// =====================================================================
#pragma once

namespace ota {
void begin();       // ArduinoOTA 初期化 + 監視タスク起動 (WiFi 起動後に呼ぶ)
// フラッシュ書込中はキャッシュ無効化で水温/制御タスクが停止し WDT が誤発報するため、
// OTA 開始時に両タスクを WDT 監視から外し、失敗時に復帰させる (成功時は再起動)。
void wdtPause();
void wdtResume();
}  // namespace ota
