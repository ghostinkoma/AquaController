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

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
static bool s_on = false;

void begin() {
  if (!oled::ENABLE || pin::isDummy(pin::I2C_SDA)) return;
  Wire.begin(pin::I2C_SDA, pin::I2C_SCL);
  u8g2.setI2CAddress(oled::ADDR << 1);
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  s_on = true;
}

void update() {
  if (!s_on) return;
  const int ox = oled::X_OFF, oy = oled::Y_OFF;

  float water; bool wValid; NetMode mode;
  state_lock(); water = g_live.water; wValid = g_live.waterValid; mode = g_live.mode; state_unlock();
  String ssid = net::ssid(), ip = net::ip();

  char ts[16];
  if (net::timeValid()) {
    time_t now = time(nullptr); struct tm tmv; localtime_r(&now, &tmv);
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    uint32_t up = net::epoch();
    snprintf(ts, sizeof(ts), "up %02u:%02u:%02u", (up/3600)%24, (up/60)%60, up%60);
  }
  char ws[16];
  if (wValid) snprintf(ws, sizeof(ws), "W:%.1fC", water);
  else        snprintf(ws, sizeof(ws), "W:--.-C");

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);                 // 約9px, 4 段 (40px)
  u8g2.drawStr(ox, oy + 8,  ssid.c_str());        // 1: SSID
  u8g2.drawStr(ox, oy + 18, ip.c_str());          // 2: IP (アクセス先)
  u8g2.drawStr(ox, oy + 28, ts);                  // 3: 時刻 (NTP)
  u8g2.drawStr(ox, oy + 38, ws);                  // 4: 水温
  u8g2.sendBuffer();
}

}  // namespace display
