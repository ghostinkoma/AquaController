// =====================================================================
//  history.cpp  -  多階層リングバッファ 実装
// =====================================================================
#include "history.h"
#include "control.h"     // fan::airflowFromRpm
#include <Arduino.h>

namespace history {

struct Sample { float water; float air; float press; int16_t rpm; uint8_t fanOn; };

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
  s.rpm = (int16_t)g_live.fanRpm;
  state_unlock();
  for (int k = 0; k < 3; k++) {
    Tier& t = T[k];
    if (t.lastPush == 0 || epoch - t.lastPush >= t.step) {
      s.fanOn = (uint8_t)(s.rpm > 0 ? (t.step > 255 ? 255 : t.step) : 0);
      push(t, epoch, s);
    }
  }
}

static Tier* pick(char tier) {
  if (tier == 'f') return &T[0];
  if (tier == 'm') return &T[1];
  if (tier == 'h') return &T[2];
  return nullptr;
}

// 配列を oldest→newest で out へ。fld: 0water 1air 2press 3rpm 4airflow 5fanOn
static void writeArray(Tier& t, Print& out, int fld) {
  out.print('[');
  int idx = (t.head - t.count + t.cap) % t.cap;
  for (int i = 0; i < t.count; i++) {
    const Sample& s = t.buf[idx];
    if (i) out.print(',');
    switch (fld) {
      case 0: out.print(s.water, 2); break;
      case 1: out.print(s.air, 2);   break;
      case 2: out.print(s.press, 1); break;
      case 3: out.print((int)s.rpm); break;
      case 4: out.print(fan::airflowFromRpm(s.rpm), 2); break;
      case 5: out.print((int)s.fanOn); break;
    }
    idx = (idx + 1) % t.cap;
  }
  out.print(']');
}

bool writeJson(char tier, Print& out) {
  Tier* t = pick(tier);
  if (!t) return false;
  out.print("{\"tier\":\""); out.print(tier);
  out.print("\",\"step\":"); out.print(t->step);
  out.print(",\"base\":");   out.print(t->count ? t->oldest : 0);
  out.print(",\"water\":");   writeArray(*t, out, 0);
  out.print(",\"air\":");     writeArray(*t, out, 1);
  out.print(",\"press\":");   writeArray(*t, out, 2);
  out.print(",\"rpm\":");     writeArray(*t, out, 3);
  out.print(",\"airflow\":"); writeArray(*t, out, 4);
  out.print(",\"fanOn\":");   writeArray(*t, out, 5);
  out.print('}');
  return true;
}

}  // namespace history
