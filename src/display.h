// =====================================================================
//  display.h / display.cpp  -  SSD1306 72x40 ビルトイン OLED (SuperMini)
//  物理 128x64 の (27,23) 起点 72x40 窓に描画 (BadCodec 実測オフセット)。
//  表示: 1行目 モード+SSID/IP / 2行目 時刻(NTP) / 3行目 水温。
// =====================================================================
#pragma once

namespace display {
void begin();
void update();   // ~1Hz で呼ぶ。net / g_live から描画。
}  // namespace display
