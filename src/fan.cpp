// =====================================================================
//  fan.cpp  -  ファン PWM 実装
// =====================================================================
#include "fan.h"
#include "control.h"
#include <Arduino.h>

namespace fan {

static bool s_dummy = false;

static void writeDuty(float dutyPct) {
  if (!s_dummy) ledcWrite(pin::FAN_PWM, dutyToByte(dutyPct));
  int rpm = rpmFromDuty(dutyPct);
  state_lock();
  g_live.fanDuty = dutyPct;
  g_live.fanRpm  = rpm;
  g_live.airflow = airflowFromRpm(rpm);
  state_unlock();
}

void begin() {
  s_dummy = pin::isDummy(pin::FAN_PWM);
  if (s_dummy) return;
  ledcAttach(pin::FAN_PWM, pwm::FAN_FREQ_HZ, pwm::FAN_RES_BITS);
  ledcWrite(pin::FAN_PWM, 0);
}

void applyForTemp(float water) {
  curve::Point P[curve::MAX_POINTS]; int n;
  state_lock();
  n = g_set.fan.n;
  for (int i = 0; i < n; i++) P[i] = g_set.fan.p[i];
  state_unlock();
  float duty = dutyFromTemp(P, n, water);
  writeDuty(duty);
}

void setDuty(float dutyPct) {
  writeDuty(curve::clampf(dutyPct, 0.0f, 100.0f));
}

}  // namespace fan
