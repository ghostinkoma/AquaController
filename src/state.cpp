// =====================================================================
//  state.cpp  -  既定カーブ (モック DEF_LIGHT / DEF_FAN と一致) と初期化
// =====================================================================
#include "state.h"

static void setPts(curve::Point* dst, int& n, const curve::Point* src, int cnt) {
  if (cnt > curve::MAX_POINTS) cnt = curve::MAX_POINTS;
  for (int i = 0; i < cnt; i++) dst[i] = src[i];
  n = cnt;
}

void settings_defaults(Settings& s) {
  static const curve::Point R[] = {{0,0},{360,24},{540,47},{1020,47},{1140,47},{1260,0},{1440,0}};
  static const curve::Point G[] = {{0,0},{360,16},{540,55},{1020,55},{1140,12},{1260,0},{1440,0}};
  static const curve::Point B[] = {{0,0},{360,4 },{540,71},{1020,71},{1140,4 },{1260,0},{1440,0}};
  static const curve::Point W[] = {{0,3},{300,3},{360,8},{540,24},{1020,24},{1140,4},{1260,3},{1440,3}};
  static const curve::Point F[] = {{24,0},{26,0},{27,40},{28.5f,70},{30,100},{35,100}};
  setPts(s.light.r, s.light.nr, R, 7);
  setPts(s.light.g, s.light.ng, G, 7);
  setPts(s.light.b, s.light.nb, B, 7);
  setPts(s.light.w, s.light.nw, W, 8);
  setPts(s.fan.p,   s.fan.n,    F, 6);
  s.heater.target = ctrl::HEATER_DEF_TARGET;
  s.heater.hyst   = ctrl::HEATER_DEF_HYST;
  s.safe.lo = ctrl::SAFE_LO;
  s.safe.hi = ctrl::SAFE_HI;
}

#ifdef ARDUINO
Settings   g_set;
Live       g_live;
Override   g_ovr;
volatile bool g_haltActuators = false;   // true=ヒーター/ファンを強制OFF (STA移行/再起動前の安全停止)
// 校正オフセットの実行時値 (既定は config.h)。store::calibLoad で /calib.json 反映。
CalibOffsets g_calib = { calib::WATER_OFFSET_C, calib::AIR_OFFSET_C,
                         calib::PRESS_OFFSET_HPA, calib::HUMID_OFFSET_PCT };
SemaphoreHandle_t g_mtx = nullptr;

void state_init() {
  g_mtx = xSemaphoreCreateMutex();
  settings_defaults(g_set);
}
#endif
