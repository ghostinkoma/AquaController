// =====================================================================
//  history.cpp  -  多階層リングバッファ 実装
// =====================================================================
#include "history.h"
#include "histdb.h"       // 履歴永続化エンティティ (int16 固定小数点スキーマ)
#include "net.h"          // net::timeValid (実epochのみ永続化)
#include "control.h"     // fan::airflowFromRpm
#include <Arduino.h>

namespace history {

struct Sample { float water; float air; float press; float humid; int16_t rpm; uint8_t fanOn; };

struct Tier {
  uint32_t step;
  int      cap;
  Sample*  buf;
  int      head;        // 次の書込位置
  int      count;
  uint32_t lastPush;    // 直近 push の epoch
  uint32_t oldest;      // 最古サンプルの epoch
};

static Sample fBuf[hist::F_CAP], mBuf[hist::M_CAP], hBuf[hist::H_CAP];
static Tier T[3] = {
  { hist::F_STEP_S, hist::F_CAP, fBuf, 0, 0, 0, 0 },
  { hist::M_STEP_S, hist::M_CAP, mBuf, 0, 0, 0, 0 },
  { hist::H_STEP_S, hist::H_CAP, hBuf, 0, 0, 0, 0 },
};

void begin() {
  for (int k = 0; k < 3; k++) { T[k].head = T[k].count = 0; T[k].lastPush = 0; }
  histdb::begin();     // 永続 TSDB 初期化 (電源断をまたいで履歴を保持)
}

static void push(Tier& t, uint32_t epoch, const Sample& s) {
  t.buf[t.head] = s;
  t.head = (t.head + 1) % t.cap;
  if (t.count < t.cap) {
    if (t.count == 0) t.oldest = epoch;
    t.count++;
  } else {
    t.oldest += t.step;   // 最古を1つ押し出した
  }
  t.lastPush = epoch;
}

void tick(uint32_t epoch) {
  Sample s;
  state_lock();
  s.water = g_live.water; s.air = g_live.air; s.press = g_live.press;
  s.humid = g_live.humidity;
  s.rpm = (int16_t)g_live.fanRpm;
  state_unlock();
  for (int k = 0; k < 3; k++) {
    Tier& t = T[k];
    if (t.lastPush == 0 || epoch - t.lastPush >= t.step) {
      s.fanOn = (uint8_t)(s.rpm > 0 ? (t.step > 255 ? 255 : t.step) : 0);
      push(t, epoch, s);
    }
  }

  // 永続 TSDB: 実時刻(NTP同期済)のときのみ追記。未同期(起動経過秒)は epoch が
  // 巻き戻り DB を汚すため追記しない。解像度別(30s/10分/2h)の間引きは histdb 側。
  if (histdb::ready() && net::timeValid()) {
    uint8_t fanOn = s.rpm > 0 ? 1 : 0;
    histdb::append(histdb::encode(epoch, s.water, s.air, s.humid, s.press, s.rpm, fanOn));
  }
}

static Tier* pick(char tier) {
  if (tier == 'f') return &T[0];
  if (tier == 'm') return &T[1];
  if (tier == 'h') return &T[2];
  return nullptr;
}

// 配列を oldest→newest で out へ。stride で間引き。fld: 0water 1air 2press 3rpm 4airflow 5fanOn 6humid
static void writeArray(Tier& t, Print& out, int fld, int stride) {
  out.print('[');
  int base = (t.head - t.count + t.cap) % t.cap;
  bool first = true;
  for (int i = 0; i < t.count; i += stride) {
    const Sample& s = t.buf[(base + i) % t.cap];
    if (!first) out.print(','); first = false;
    switch (fld) {
      case 0: out.print(s.water, 2); break;
      case 1: out.print(s.air, 2);   break;
      case 2: out.print(s.press, 1); break;
      case 3: out.print((int)s.rpm); break;
      case 4: out.print(fan::airflowFromRpm(s.rpm), 2); break;
      case 5: out.print((int)s.fanOn); break;
      case 6: out.print(s.humid, 1); break;
    }
  }
  out.print(']');
}

bool writeJson(char tier, Print& out) {
  Tier* t = pick(tier);
  if (!t) return false;
  // 送信量削減: 最大 ~MAX_OUT 点に間引き (step を stride 倍にして時間軸は保つ)。
  constexpr int MAX_OUT = 240;
  int stride = 1;
  if (t->count > MAX_OUT) stride = (t->count + MAX_OUT - 1) / MAX_OUT;
  out.print("{\"tier\":\""); out.print(tier);
  out.print("\",\"step\":"); out.print(t->step * (uint32_t)stride);
  out.print(",\"base\":");   out.print(t->count ? t->oldest : 0);
  out.print(",\"water\":");   writeArray(*t, out, 0, stride);
  out.print(",\"air\":");     writeArray(*t, out, 1, stride);
  out.print(",\"press\":");   writeArray(*t, out, 2, stride);
  out.print(",\"humid\":");   writeArray(*t, out, 6, stride);
  out.print(",\"rpm\":");     writeArray(*t, out, 3, stride);
  out.print(",\"airflow\":"); writeArray(*t, out, 4, stride);
  out.print(",\"fanOn\":");   writeArray(*t, out, 5, stride);
  out.print('}');
  return true;
}

}  // namespace history
