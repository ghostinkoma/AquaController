// =====================================================================
//  fan.h / fan.cpp  -  ファン PWM (HAL)。水温→風量カーブで駆動。
//  タコ (fan_tach) 実測 rpm があれば PID で目標 rpm へ追従、無ければ duty から推定。
// =====================================================================
#pragma once
#include "state.h"

namespace fan {
void begin();
void applyForTemp(float water);   // カーブから duty 決定 → PWM → g_live 更新
void setDuty(float dutyPct);      // フェイルセーフ等の直接指定 (0..100)
}  // namespace fan
