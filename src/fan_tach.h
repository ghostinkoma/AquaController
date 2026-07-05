// =====================================================================
//  fan_tach.h / fan_tach.cpp  -  ファン タコ (回転数) 実測
//  GPIO 割込みでパルスをカウントし、1秒周期で rpm へ換算する。
//  pin::FAN_TACH == DUMMY のときは常に rpm=0 / valid=false を返す。
// =====================================================================
#pragma once
#include "config.h"

namespace fan_tach {
void begin();
// FAN_TACH_SAMPLE_MS 周期で呼ぶ。呼ぶたびにカウンタを消費して rpm を返す。
// tach 未接続 (dummy) の場合は valid=false, rpm=0。
int  sampleRpm(bool* valid);
}  // namespace fan_tach
