// =====================================================================
//  sensors.h / sensors.cpp  -  DS18B20 水温 (ノンブロッキング) + BME280
//  水温は最優先 2秒タスクから読む。DS18B20 の変換待ち(~750ms)で
//  ブロックしないよう「前回値を読む→次の変換を要求」の非同期方式。
// =====================================================================
#pragma once
#include "state.h"

namespace sensors {
void begin();
void readWater();   // 2秒タスク: g_live.water / waterValid を更新
void readAir();     // g_live.air / press を更新 (BME280)
}  // namespace sensors
