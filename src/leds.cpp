// =====================================================================
//  leds.cpp  -  WS2812 / SK6812 RGBW (アドレサブル) 実装
//  モデルは「時刻ごとの全体 RGBW 色」なので、全ピクセルを同色で塗る。
//  pin::LED_DATA == DUMMY のときは HW を触らず g_live のみ更新。
// =====================================================================
#include "leds.h"
#include "control.h"
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

namespace leds {

// 色順/チャンネル数/速度は config.h の AQ_LED_TYPE で指定 (自作モジュール対応)。
static Adafruit_NeoPixel strip(pin::LED_COUNT, pin::LED_DATA, AQ_LED_TYPE);
static bool s_dummy = false;
static bool s_warnDummy = false;      // 専用 警告 LED (GPIO)

void begin() {
  s_warnDummy = pin::isDummy(pin::WARN_LED);
  if (!s_warnDummy) { pinMode(pin::WARN_LED, OUTPUT); digitalWrite(pin::WARN_LED, LOW); }
  s_dummy = pin::isDummy(pin::LED_DATA);
  if (s_dummy) return;
  strip.begin();
  strip.setBrightness(pwm::LED_BRIGHT);
  strip.clear();
  strip.show();
}

void apply(float minuteOfDay) {
  curve::Point R[curve::MAX_POINTS], G[curve::MAX_POINTS],
               B[curve::MAX_POINTS], W[curve::MAX_POINTS];
  int nr, ng, nb, nw;
  state_lock();
  nr = g_set.light.nr; ng = g_set.light.ng; nb = g_set.light.nb; nw = g_set.light.nw;
  for (int i = 0; i < nr; i++) R[i] = g_set.light.r[i];
  for (int i = 0; i < ng; i++) G[i] = g_set.light.g[i];
  for (int i = 0; i < nb; i++) B[i] = g_set.light.b[i];
  for (int i = 0; i < nw; i++) W[i] = g_set.light.w[i];
  state_unlock();

  uint8_t r = channelByte(R, nr, minuteOfDay);
  uint8_t g = channelByte(G, ng, minuteOfDay);
  uint8_t b = channelByte(B, nb, minuteOfDay);
  uint8_t w = channelByte(W, nw, minuteOfDay);

  if (!s_dummy) {
    uint32_t c = strip.Color(r, g, b, w);    // NEO_GRBW 指定により並びは自動 (照明は常に通常色)
    for (int i = 0; i < pin::LED_COUNT; i++) strip.setPixelColor(i, c);
    strip.show();                            // ESP32 は RMT 駆動 (割込み長時間停止なし)
  }

  // 専用 警告 LED (GPIO): 何らかの異常(alarm)時に点滅。照明ストリップには一切干渉しない。
  if (!s_warnDummy) {
    bool alarm; state_lock(); alarm = g_live.alarm; state_unlock();
    bool on = alarm && (millis() % warn::PERIOD_MS) < warn::FLASH_MS;
    digitalWrite(pin::WARN_LED, on ? HIGH : LOW);
  }

  state_lock();
  g_live.ledR = r; g_live.ledG = g; g_live.ledB = b; g_live.ledW = w;
  state_unlock();
}

}  // namespace leds
