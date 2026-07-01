// =====================================================================
//  curve.h  -  Catmull-Rom サンプラ (純粋ロジック / Arduino 非依存)
//  モック app.js の sampleCurve() を厳密移植。点列は x 昇順前提・端点クランプ。
//  ESP32-C3 は FPU 非搭載 (RV32IMC) のため float はソフトエミュレーション。
//  ただし本関数はホットループではなく制御/スケジュール更新時のみ呼ぶため実用上問題なし。
// =====================================================================
#pragma once
#include <stdint.h>

namespace curve {

struct Point { float x; float y; };

constexpr int MAX_POINTS = 16;   // 1 曲線あたり最大点数 (静的確保ポリシー)

inline float  clampf(float v, float lo, float hi)   { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 一様 (uniform) Catmull-Rom。pts は x 昇順、n>=1。x 範囲外は端点クランプ。
// 内部 double 演算でモック(JS) と bit 一致。返り値も double。
double sample(const Point* pts, int n, float x);

}  // namespace curve
