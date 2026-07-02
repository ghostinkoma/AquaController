// =====================================================================
//  display.cpp  -  OLED 描画 (U8g2 / SSD1306 128x64, 72x40 窓)
//  4 段情報表示 (SSID / IP / 時刻 / 水温)。QR は画面が小さすぎるため廃止し
//  アクセスは mDNS (aquacontroller.local) / IP で行う。
// =====================================================================
#include "display.h"
#include "state.h"
#include "net.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>

namespace display {

// C3 は HW I2C が1つのみ (センサが Wire=GPIO8/9 を使用) のため、OLED は
// ソフトウェア I2C (ビットバン) で内蔵ピン GPIO5/6 を独立に駆動する。1Hz 更新なら十分。
static U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, pin::OLED_SCL, pin::OLED_SDA, U8X8_PIN_NONE);
static bool s_on = false;

void begin() {
  if (!oled::ENABLE || pin::isDummy(pin::OLED_SDA)) return;
  u8g2.setI2CAddress(oled::ADDR << 1);
  u8g2.begin();                                 // SW I2C は GPIO5/6 を直接駆動 (Wire 不要)
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  s_on = true;
}

void update() {
  if (!s_on) return;
  const int ox = oled::X_OFF, oy = oled::Y_OFF;

  float water, air, hum, press; bool wValid, hValid;
  state_lock();
  water = g_live.water; wValid = g_live.waterValid;
  air = g_live.air; hum = g_live.humidity; hValid = g_live.humidityValid; press = g_live.press;
  state_unlock();

  char l1[16], l2[16], l3[16], l4[16];
  if (wValid) snprintf(l1, sizeof(l1), "Water %.1fC", water);
  else        snprintf(l1, sizeof(l1), "Water --.-C");
  snprintf(l2, sizeof(l2), "Air   %.1fC", air);
  if (hValid) snprintf(l3, sizeof(l3), "Humi  %.1f%%", hum);
  else        snprintf(l3, sizeof(l3), "Humi  --.-%%");
  snprintf(l4, sizeof(l4), "%.1f hPa", press);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);                 // 約9px, 4 段 (40px)
  u8g2.drawStr(ox, oy + 8,  l1);                  // 1: 水温
  u8g2.drawStr(ox, oy + 18, l2);                  // 2: 気温
  u8g2.drawStr(ox, oy + 28, l3);                  // 3: 湿度
  u8g2.drawStr(ox, oy + 38, l4);                  // 4: 気圧
  u8g2.sendBuffer();
}

}  // namespace display
