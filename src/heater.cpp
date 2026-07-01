// =====================================================================
//  heater.cpp  -  ヒーターリレー 実装
// =====================================================================
#include "heater.h"
#include "control.h"      // heater::decide / capTarget (純粋ロジック, 同名前空間)
#include <Arduino.h>

namespace heater {

static bool s_dummy = false;

static void drive(bool on) {
  if (s_dummy) return;
  int level = (on == pwm::HEATER_ACTIVE_HIGH) ? HIGH : LOW;
  digitalWrite(pin::HEATER, level);
}

void begin() {
  s_dummy = pin::isDummy(pin::HEATER);
  if (!s_dummy) { pinMode(pin::HEATER, OUTPUT); drive(false); }   // 起動時 OFF
}

void update(float water) {
  float target, hyst;
  bool prev;
  state_lock();
  target = g_set.heater.target;
  hyst   = g_set.heater.hyst;
  prev   = g_live.heaterOn;
  state_unlock();

  bool on = decide(water, target, hyst, prev);   // 35°C キャップ込み
  drive(on);
  state_lock();
  g_live.heaterOn = on;
  state_unlock();
}

void force(bool on) {
  drive(on);
  state_lock();
  g_live.heaterOn = on;
  state_unlock();
}

}  // namespace heater
