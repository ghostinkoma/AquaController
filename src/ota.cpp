// =====================================================================
//  ota.cpp  -  ArduinoOTA (espota) 実装
// =====================================================================
#include "ota.h"
#include "state.h"
#include "config.h"
#include "fan_tach.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include "esp_task_wdt.h"

namespace ota {

// OTA のフラッシュ書込中はキャッシュ無効化で水温/制御タスクが数十秒停止する。
// WDT(8s) が誤発報して書込を中断するため、OTA 中のみ両タスクを監視から外す。
// (この間 g_haltActuators でヒーター/ファンは OFF = 安全側で待機)
void wdtPause() {
  g_wdtPaused = true;                    // 先にフラグ (タスク側の wdt_reset 呼出を止める)
  if (g_waterTaskH)   esp_task_wdt_delete(g_waterTaskH);
  if (g_controlTaskH) esp_task_wdt_delete(g_controlTaskH);
  Serial.println("[ota] wdt paused");
}
void wdtResume() {
  if (g_waterTaskH)   esp_task_wdt_add(g_waterTaskH);
  if (g_controlTaskH) esp_task_wdt_add(g_controlTaskH);
  g_wdtPaused = false;
  Serial.println("[ota] wdt resumed");
}

// ArduinoOTA.handle() は更新中ブロックするため、WDT 非監視の専用タスクで回す
// (水温/制御タスクは並走を続け、g_haltActuators で安全出力を維持する)。
static void task(void*) {
  for (;;) {
    ArduinoOTA.handle();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void begin() {
  if (!ENABLE) return;
  ArduinoOTA.setHostname(AQ_HOSTNAME);
  ArduinoOTA.setPassword(AQ_OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    g_haltActuators = true;              // 生体安全: 書換中はヒーター/ファン OFF
    wdtPause();                          // フラッシュ書込中の WDT 誤発報を防止
    fan_tach::pause();                   // タコ割込みを外す (書込中の ISR クラッシュ防止)
    Serial.println("[ota] start (actuators halted)");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    wdtResume();                         // 失敗 → 監視・通常制御へ復帰
    fan_tach::resume();                  // タコ割込み復帰
    g_haltActuators = false;
    Serial.printf("[ota] error %d -> resume control\n", (int)e);
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] end -> reboot"); });
  ArduinoOTA.begin();
  xTaskCreate(task, "ota", 4096, nullptr, 1, nullptr);
  Serial.println("[ota] ready (espota port 3232)");
}

}  // namespace ota
