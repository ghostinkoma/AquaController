// =====================================================================
//  history.h / history.cpp  -  多階層リングバッファ履歴
//  出力 JSON はダッシュボードの sliceDummy 形状と一致:
//  { tier, step, base, water[], air[], press[], rpm[], airflow[], fanOn[] }
//  巨大配列は JsonDocument を作らず Print へ直接ストリーム。
// =====================================================================
#pragma once
#include "state.h"
#include <Print.h>

namespace history {
void begin();
// 現在時刻(epoch秒)とライブ値で各階層を周期push。loop から毎秒呼ぶ。
void tick(uint32_t epoch);
// tier: 'f' | 'm' | 'h' を Print へ JSON ストリーム。
bool writeJson(char tier, Print& out);
}  // namespace history
