// =====================================================================
//  main.cpp  -  起動・タスク構成
//  ESP32-C3 は単コア。水温タスクを高優先で常時走らせ、Web/制御に
//  ブロックされず 2秒以内の水温取得を保証 (既存仕様)。
// =====================================================================
#include <Arduino.h>
#include "state.h"
#include "store.h"
#include "sensors.h"
#include "leds.h"
#include "fan.h"
#include "heater.h"
#include "history.h"
#include "net.h"
#include "web.h"
#include "display.h"
#include "control.h"

// ---- 安全域判定 (水温タスクから) ----
static void evalSafety(float water, bool valid) {
  float lo, hi;
  state_lock(); lo = g_set.safe.lo; hi = g_set.safe.hi; state_unlock();
  int dir = 0;
  if (valid) {
    if (water > hi) dir = +1;
    else if (water < lo) dir = -1;
  }
  state_lock();
  g_live.alarm = (dir != 0);
  g_live.alarmDir = dir;
  state_unlock();
}

// ---- 水温 最優先タスク (2秒) ----
static void waterTask(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    sensors::readWater();
    float w; bool v;
    state_lock(); w = g_live.water; v = g_live.waterValid; state_unlock();
    evalSafety(w, v);
    vTaskDelayUntil(&last, pdMS_TO_TICKS(timing::WATER_PERIOD_MS));
  }
}

// ---- 制御タスク (1秒): ライト/ファン/ヒーター + 気温 + 履歴 ----
static void controlTask(void*) {
  TickType_t last = xTaskGetTickCount();
  uint32_t airAccum = 0, histAccum = 0, dispAccum = 0, btnHeld = 0;
  for (;;) {
    float w; bool valid; int dir;
    state_lock(); w = g_live.water; valid = g_live.waterValid; dir = g_live.alarmDir; state_unlock();

    // 時刻。ep は UTC (履歴タイムスタンプ用)、LED スケジュールの「時刻(分)」は
    // ローカル (JST) 基準にする (UTC のままだと 9h ズレる)。
    uint32_t ep = net::epoch();
    float minuteOfDay = (float)((net::epochLocal() % 86400UL) / 60UL);

    // デバッグ・オーバーライド (TTL 自動解除)
    uint32_t nowMs = millis();
    bool ledOvr, fanOvr; float ovrMin, ovrDuty;
    state_lock();
    ledOvr  = g_ovr.ledActive && (int32_t)(g_ovr.ledExpireMs - nowMs) > 0;
    fanOvr  = g_ovr.fanActive && (int32_t)(g_ovr.fanExpireMs - nowMs) > 0;
    ovrMin  = g_ovr.ledMinute; ovrDuty = g_ovr.fanDuty;
    if (g_ovr.ledActive && !ledOvr) g_ovr.ledActive = false;   // 期限切れ掃除
    if (g_ovr.fanActive && !fanOvr) g_ovr.fanActive = false;
    state_unlock();

    float ledMin = ledOvr ? ovrMin : minuteOfDay;   // LED は override 時刻を優先

    if (dir > 0) {
      // 高温フェイルセーフ: ファン全開 + ヒーター OFF (安全優先, fan override 無視)
      leds::apply(ledMin);
      fan::setDuty(100.0f);
      heater::force(false);
    } else if (dir < 0) {
      // 低温フェイルセーフ: ヒーター強制 ON
      leds::apply(ledMin);
      if (fanOvr)      fan::setDuty(ovrDuty);
      else if (valid)  fan::applyForTemp(w);
      heater::force(true);
    } else {
      // 通常制御 (+ override)
      leds::apply(ledMin);
      if (fanOvr)      fan::setDuty(ovrDuty);
      else if (valid)  fan::applyForTemp(w);
      if (valid)       heater::update(w);
    }

    // 気温・気圧 (5秒)
    airAccum += timing::CONTROL_PERIOD_MS;
    if (airAccum >= timing::AIR_PERIOD_MS) { airAccum = 0; sensors::readAir(); }

    // 履歴 tick (1秒)
    histAccum += timing::CONTROL_PERIOD_MS;
    if (histAccum >= timing::HIST_PERIOD_MS) { histAccum = 0; history::tick(ep); }

    // OLED 更新 (1秒。LED ループは高速だが I2C 描画は 1Hz で十分)
    dispAccum += timing::CONTROL_PERIOD_MS;
    if (dispAccum >= 1000) { dispAccum = 0; display::update(); }

    // ファクトリーリセット: ボタン長押しで wifi 設定のみ消去 (プリセットは保持)
    if (!pin::isDummy(pin::RESET_BTN)) {
      if (digitalRead(pin::RESET_BTN) == LOW) {
        btnHeld += timing::CONTROL_PERIOD_MS;
        if (btnHeld >= timing::RESET_HOLD_MS) {
          net::eraseCreds();        // /wifi.json のみ削除 → 次回起動は AP セットアップ
          delay(150);
          ESP.restart();
        }
      } else btnHeld = 0;
    }

    net::loop();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(timing::CONTROL_PERIOD_MS));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  state_init();
  if (!pin::isDummy(pin::RESET_BTN)) pinMode(pin::RESET_BTN, INPUT_PULLUP);
  store::begin();        // LittleFS マウント + 設定読込 (無ければ既定)
  sensors::begin();
  leds::begin();
  fan::begin();
  heater::begin();       // 起動時 OFF
  history::begin();
  net::begin();          // STA 資格があれば接続、無ければ AP
  display::begin();      // OLED (SuperMini 72x40)
  web::begin();

  Serial.printf("[boot] mode=%s ip=%s ssid=%s\n",
                net::modeStr().c_str(), net::ip().c_str(), net::ssid().c_str());

  // 水温タスクを高優先 (3)、制御を中優先 (2)。Async サーバは内部タスク。
  xTaskCreate(waterTask,   "water",   4096, nullptr, 3, nullptr);
  xTaskCreate(controlTask, "control", 6144, nullptr, 2, nullptr);
}

void loop() {
  // 制御は専用タスク。loop は空転 (Arduino ループタスクは低優先)。
  vTaskDelay(pdMS_TO_TICKS(1000));
}
