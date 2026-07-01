// =====================================================================
//  control.h  -  ライト/ファン/ヒーターの純粋制御ロジック
//  (Arduino 非依存。HAL は leds.cpp / fan.cpp / heater.cpp が担当)
//  モック v3 (app.js) の数式と一致させる。
// =====================================================================
#pragma once
#include "curve.h"
#include "config.h"

// ---- ライト: 各チャンネル曲線を分(0..1440)でサンプル → 0..255 ----
namespace leds {
inline uint8_t channelByte(const curve::Point* pts, int n, float minuteOfDay) {
  double y = curve::sample(pts, n, minuteOfDay);         // 0..100 (オーバーシュート有)
  double v = curve::clampd(y, 0.0, 100.0) / 100.0 * pwm::LED_MAX;
  int iv  = (int)(v + 0.5);                              // round-half-up (JS Math.round と一致)
  return (uint8_t)curve::clampd((double)iv, 0.0, (double)pwm::LED_MAX);
}
}  // namespace leds

// ---- ファン: 水温 → duty% / rpm / 風量 ----
namespace fan {
inline float dutyFromTemp(const curve::Point* pts, int n, float tempC) {
  return (float)curve::clampd(curve::sample(pts, n, tempC), 0.0, 100.0);
}
// rpm 推定 (モック gen_dummy と同方式)。乱数は載せず確定的に。
inline int rpmFromDuty(float duty) {
  if (duty < 1.0f) return 0;
  int r = (int)(ctrl::FAN_RPM_MAX * duty / 100.0f + 0.5f);
  return r < ctrl::FAN_RPM_MIN_ON ? ctrl::FAN_RPM_MIN_ON : r;
}
inline float airflowFromRpm(int rpm) { return rpm * ctrl::FAN_AIRFLOW_K; }
inline uint8_t dutyToByte(float duty) {
  return (uint8_t)curve::clampf(duty / 100.0f * pwm::FAN_MAX + 0.5f, 0.0f, (float)pwm::FAN_MAX);
}
}  // namespace fan

// ---- ヒーター: 目標キープ + ヒステリシス + 35°C ハードキャップ ----
namespace heater {
inline float capTarget(float t) {
  if (t < ctrl::HEATER_MIN_TARGET) return ctrl::HEATER_MIN_TARGET;
  if (t > ctrl::HEATER_MAX_TARGET) return ctrl::HEATER_MAX_TARGET;  // 生体保護: 絶対上限
  return t;
}
// ラッチ式: ON 中は water<target で維持、water>=target で OFF。
//           OFF 中は water<target-hyst で ON。 (モック app.js と一致)
inline bool decide(float water, float target, float hyst, bool prevOn) {
  target = capTarget(target);
  if (hyst < 0.0f) hyst = 0.0f;
  return prevOn ? (water < target) : (water < target - hyst);
}
}  // namespace heater
