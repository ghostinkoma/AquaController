// =====================================================================
//  histdb.h / histdb.cpp  -  履歴の永続化エンティティ (FlashDB TSDB)
//
//  センサ履歴を int16 固定小数点(×100, 小数第2位)で 1 レコード 11 バイトに格納する。
//  FlashDB の TSDB(時系列DB) に {timestamp(epoch秒), HistRec(11B)} として追記する想定。
//  ※ 本ヘッダのエンコード規則は純粋 C++ でホストテスト可能（FlashDB 非依存）。
//    FlashDB を用いた実体(begin/append/query)は histdb.cpp（HISTDB_FLASHDB 有効時）。
//
//  ---- 固定小数点エンコード規則（各値 小数第2位）----
//    water °C : v×100                 例 26.55 → 2655   （範囲 ±327.67°C 十分）
//    air   °C : v×100                 例 28.10 → 2810
//    humid %RH: v×100                 例 67.35 → 6735   （0..100% → 0..10000）
//    press hPa: (v-1000)×100          例 1005.46 → 546  （★1000hPaオフセットで int16 に収める）
//                                       800..1085hPa → -20000..+8500（int16 内）
//    rpm      : v（整数, ×1）          例 1500 → 1500   （0..2200）
//    fanOn    : 0/非0（uint8）
//  値が int16 範囲外なら飽和(クランプ)して格納する。
// =====================================================================
#pragma once
#include <stdint.h>
#include <math.h>

namespace histdb {

// ---- 1 サンプル = 15 バイト（パック）----
//  epoch(datetime) を含む自己完結レコード。FlashDB TSDB のタイムスタンプにも同値を使う。
struct __attribute__((packed)) HistRec {
  uint32_t epoch;  // Unix秒 (UTC) — datetime
  int16_t water;   // °C  ×100
  int16_t air;     // °C  ×100
  int16_t humid;   // %RH ×100
  int16_t press;   // (hPa - 1000) ×100
  int16_t rpm;     // rpm（整数）
  uint8_t fanOn;   // 稼働フラグ
};
static_assert(sizeof(HistRec) == 15, "HistRec は 15 バイトであること");

// 気圧オフセット基準（この値を引いてから ×100 する）
constexpr float PRESS_BASE_HPA = 1000.0f;

// ---- 飽和付き固定小数点 変換 ----
inline int16_t clamp16(long v) {
  if (v >  32767) return  32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}
inline int16_t enc2(float v)   { return clamp16(lroundf(v * 100.0f)); }        // ×100
inline float   dec2(int16_t x) { return x / 100.0f; }
inline int16_t encP(float hpa) { return clamp16(lroundf((hpa - PRESS_BASE_HPA) * 100.0f)); }
inline float   decP(int16_t x) { return PRESS_BASE_HPA + x / 100.0f; }
inline int16_t encRpm(float r) { return clamp16(lroundf(r)); }

// ---- ライブ値 → レコード ----
inline HistRec encode(uint32_t epoch, float water, float air, float humid, float press, int rpm, uint8_t fanOn) {
  HistRec r;
  r.epoch = epoch;
  r.water = enc2(water); r.air = enc2(air); r.humid = enc2(humid);
  r.press = encP(press); r.rpm = clamp16(rpm); r.fanOn = fanOn;
  return r;
}
// ---- レコード → 物理値（デコード）----
struct HistVal { uint32_t epoch; float water, air, humid, press; int rpm; uint8_t fanOn; };
inline HistVal decode(const HistRec& r) {
  return HistVal{ r.epoch, dec2(r.water), dec2(r.air), dec2(r.humid), decP(r.press), (int)r.rpm, r.fanOn };
}

#ifdef ARDUINO
// ---- FlashDB TSDB 実体 API（histdb.cpp）----
//  格納先は LittleFS 上のディレクトリ（FlashDB file モード）。パーティション追加不要。
bool begin();                                   // TSDB 初期化（失敗時 false → RAM 履歴にフォールバック）
bool append(const HistRec& r);                  // 1 サンプル追記（epoch は r.epoch）
// 期間 [from,to] を最大 maxPoints 点に間引いて cb へ（oldest→newest）。返り値=出力点数。
typedef void (*IterCb)(uint32_t epoch, const HistRec& r, void* arg);
int  query(uint32_t from, uint32_t to, int maxPoints, IterCb cb, void* arg);
uint32_t count();                               // 保存件数
bool ready();                                   // 初期化成功済みか
#endif

}  // namespace histdb
