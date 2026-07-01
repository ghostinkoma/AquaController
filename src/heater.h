// =====================================================================
//  heater.h / heater.cpp  -  ヒーターリレー (HAL)
//  目標キープ + ヒステリシス。35°C ハードキャップ (生体保護) は control.h。
// =====================================================================
#pragma once
#include "state.h"

namespace heater {
void begin();
void update(float water);     // ラッチ判定 → リレー → g_live 更新
void force(bool on);          // フェイルセーフ用 直接駆動
}  // namespace heater
