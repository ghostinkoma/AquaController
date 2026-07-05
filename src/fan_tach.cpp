// =====================================================================
//  fan_tach.cpp  -  ファン タコ (回転数) 実測
// =====================================================================
#include "fan_tach.h"
#include <Arduino.h>

namespace fan_tach {

static volatile uint32_t s_pulses = 0;
static volatile uint32_t s_lastEdgeUs = 0;
static bool s_dummy = false;

// ISR デバウンス: FAN_TACH_MIN_PULSE_US 未満で来た連続エッジはノイズとして無視。
static void IRAM_ATTR onPulse() {
  uint32_t now = micros();
  if (now - s_lastEdgeUs >= ctrl::FAN_TACH_MIN_PULSE_US) {
    s_pulses++;
    s_lastEdgeUs = now;
  }
}

void begin() {
  s_dummy = pin::isDummy(pin::FAN_TACH);
  if (s_dummy) return;
  pinMode(pin::FAN_TACH, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pin::FAN_TACH), onPulse, FALLING);
}

int sampleRpm(bool* valid) {
  if (s_dummy) { if (valid) *valid = false; return 0; }
  noInterrupts();
  uint32_t pulses = s_pulses;
  s_pulses = 0;
  interrupts();
  // rpm = パルス数 / (回転あたりパルス数) / (サンプル秒) * 60
  float secs = ctrl::FAN_TACH_PULSES_PER_REV > 0 ? (timing::FAN_TACH_SAMPLE_MS / 1000.0f) : 1.0f;
  int rpm = (int)(pulses / (float)ctrl::FAN_TACH_PULSES_PER_REV / secs * 60.0f + 0.5f);
  // 妥当性チェック: 定格上限の 1.2 倍を超える値は残留ノイズ由来のグリッチ。
  //  無効を返し、呼出側 (fan.cpp) は前回の実測値を保持し PID を乱さない。
  //  ※生値(デバウンス+クランプ済)を返す。表示用の平滑(EMA)は fan.cpp 側で行い、
  //    PID フィードバックには遅れの無いこの生値を使う (LPF をループに入れると不安定化)。
  if (rpm > (int)(ctrl::FAN_RPM_MAX * 1.2f)) { if (valid) *valid = false; return 0; }
  if (valid) *valid = true;
  return rpm;
}

}  // namespace fan_tach
