// =====================================================================
//  leds.h / leds.cpp  -  RGBW LEDC 調光 (HAL)
//  既存仕様の leds::setSchedule() に相当。v3 はチャンネル別曲線。
// =====================================================================
#pragma once
#include "state.h"

namespace leds {
void begin();
// 時刻(分, 0..1440)で各チャンネルをサンプルし PWM 出力。g_live へ反映。
void apply(float minuteOfDay);
}  // namespace leds
