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
#include "fan_tach.h"
#include "heater.h"
#include "history.h"
#include "net.h"
#include "web.h"
#include "display.h"
#include "control.h"
#include "cmd.h"
#include "ota.h"
#include "esp_task_wdt.h"

// ---- 安全域 + センサ無応答 判定 (水温タスクから 2秒毎) ----
static void evalSafety(float water, bool valid) {
  float lo, hi;
  state_lock(); lo = g_set.safe.lo; hi = g_set.safe.hi; state_unlock();
  int dir = 0;
  if (valid) {
    if (water > hi) dir = +1;
    else if (water < lo) dir = -1;
  }
  // センサ無応答: 規定時間 valid にならなければ異常。
  static uint32_t s_lastValid = 0; static bool s_inited = false;
  uint32_t now = millis();
  if (!s_inited) { s_lastValid = now; s_inited = true; }
  if (valid) s_lastValid = now;
  bool sensorFault = (now - s_lastValid) >= safety::SENSOR_TIMEOUT_MS;

  state_lock();
  g_live.alarmDir   = dir;
  g_live.sensorFault = sensorFault;
  // いずれかの異常で総合アラーム点灯 (heat/coolFault は制御タスクが設定)
  g_live.alarm = (dir != 0) || sensorFault || g_live.heatFault || g_live.coolFault || g_live.fanRpmFault;
  state_unlock();
}

// ---- ヒーター/ファン 効果監視 (制御タスクから 1秒毎) ----
//  連続稼働の判定窓内に温度が目標方向へ動かなければ「効いていない」= 機器故障を疑い異常。
static void evalEffectiveness(float water, bool valid) {
  float target; bool heaterOn; float duty;
  state_lock(); target = g_set.heater.target; heaterOn = g_live.heaterOn; duty = g_live.fanDuty; state_unlock();
  uint32_t now = millis();

  // ヒーター: ON 継続 かつ 目標未達 の間だけ監視
  static bool heatWin = false; static uint32_t heatT0 = 0; static float heatTemp0 = 0;
  if (valid && heaterOn && water < target) {
    if (!heatWin) { heatWin = true; heatT0 = now; heatTemp0 = water; }
    else if (now - heatT0 >= safety::HEAT_EVAL_MS) {
      bool rose = (water - heatTemp0) >= safety::HEAT_MIN_RISE_C;
      state_lock(); g_live.heatFault = !rose; state_unlock();
      heatT0 = now; heatTemp0 = water;                 // 窓を更新して継続監視
    }
  } else {
    heatWin = false;
    state_lock(); g_live.heatFault = false; state_unlock();
  }

  // ファン: ON(duty>0) 継続の間だけ監視 (温度が下がっているか)
  static bool coolWin = false; static uint32_t coolT0 = 0; static float coolTemp0 = 0;
  if (valid && duty > 0.0f) {
    if (!coolWin) { coolWin = true; coolT0 = now; coolTemp0 = water; }
    else if (now - coolT0 >= safety::COOL_EVAL_MS) {
      bool dropped = (coolTemp0 - water) >= safety::COOL_MIN_DROP_C;
      state_lock(); g_live.coolFault = !dropped; state_unlock();
      coolT0 = now; coolTemp0 = water;
    }
  } else {
    coolWin = false;
    state_lock(); g_live.coolFault = false; state_unlock();
  }
}

// ---- 水温 最優先タスク (2秒) ----
static void waterTask(void*) {
  esp_task_wdt_add(NULL);                // 自タスクをウォッチドッグ監視対象へ
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    sensors::readWater();
    float w; bool v;
    state_lock(); w = g_live.water; v = g_live.waterValid; state_unlock();
    evalSafety(w, v);
    if (!g_wdtPaused) esp_task_wdt_reset();   // 生存通知 (OTA 中は監視解除中のためスキップ)
    vTaskDelayUntil(&last, pdMS_TO_TICKS(timing::WATER_PERIOD_MS));
  }
}

// ---- 制御タスク (1秒): ライト/ファン/ヒーター + 気温 + 履歴 ----
static void controlTask(void*) {
  esp_task_wdt_add(NULL);                // 自タスクをウォッチドッグ監視対象へ
  TickType_t last = xTaskGetTickCount();
  uint32_t airAccum = 0, histAccum = 0, dispAccum = 0, safetyAccum = 0, btnHeld = 0;
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

    if (g_haltActuators) {
      // STA 移行/再起動前の安全停止: ヒーター/ファンを確実に OFF (ライトは継続)。
      leds::apply(ledMin);
      heater::force(false);
      fan::setDuty(0.0f);
    } else if (dir > 0) {
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

    // ヒーター/ファン 効果監視 (1秒)
    safetyAccum += timing::CONTROL_PERIOD_MS;
    if (safetyAccum >= 1000) { safetyAccum = 0; evalEffectiveness(w, valid); }

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
    if (!g_wdtPaused) esp_task_wdt_reset();   // 生存通知 (OTA 中は監視解除中のためスキップ)
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
  fan_tach::begin();
  heater::begin();       // 起動時 OFF
  history::begin();
  net::begin();          // STA 資格があれば接続、無ければ AP
  display::begin();      // OLED (SuperMini 72x40)
  web::begin();

  Serial.printf("[boot] mode=%s ip=%s ssid=%s reset_reason=%d build=%s %s\n",
                net::modeStr().c_str(), net::ip().c_str(), net::ssid().c_str(),
                (int)esp_reset_reason(),    // ESP_RST_TASK_WDT(7)/ESP_RST_WDT(8)=WDTリセット
                __DATE__, __TIME__);        // ビルド時刻 (OTA 反映確認用)

  cmd::begin();   // USB シリアル ファイルコマンド (ls/get/put) — デバッグ用
  ota::begin();   // 無線ファーム更新 (espota:3232 + POST /update) — USB 結合の排除

  // タスクウォッチドッグ: 水温/制御タスクの生存を監視。ハングで panic→リセット
  // (再起動で begin() がアクチュエータを OFF にするため安全側へ倒れる)。
  // idle_core_mask=0 で idle 監視は外し、明示的に登録した重要タスクのみ監視。
  esp_task_wdt_config_t twdt = { .timeout_ms = safety::WDT_TIMEOUT_MS,
                                 .idle_core_mask = 0, .trigger_panic = true };
  if (esp_task_wdt_init(&twdt) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&twdt);

  // 水温タスクを高優先 (3)、制御を中優先 (2)。Async サーバは内部タスク。
  // ハンドルは OTA 中の WDT 一時解除 (ota::wdtPause) に使う。
  xTaskCreate(waterTask,   "water",   4096, nullptr, 3, &g_waterTaskH);
  xTaskCreate(controlTask, "control", 6144, nullptr, 2, &g_controlTaskH);
}

void loop() {
  // 制御は専用タスク。loop は空転 (Arduino ループタスクは低優先)。
  vTaskDelay(pdMS_TO_TICKS(1000));
}
