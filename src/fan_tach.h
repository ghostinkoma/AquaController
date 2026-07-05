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
// OTA のフラッシュ書込中は GPIO 割込み(ISR ディスパッチが flash 上)がキャッシュ無効の
// 瞬間に発火するとクラッシュするため、書込中は割込みを外す。失敗時は resume で復帰。
void pause();
void resume();
}  // namespace fan_tach
