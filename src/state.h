// =====================================================================
//  state.h  -  共有状態 (設定カーブ + ライブ値) と既定値
//  既定カーブはモック v3 の DEF_LIGHT / DEF_FAN と一致。
// =====================================================================
#pragma once
#include "curve.h"
#include "config.h"

struct LightCurves {
  curve::Point r[curve::MAX_POINTS]; int nr;
  curve::Point g[curve::MAX_POINTS]; int ng;
  curve::Point b[curve::MAX_POINTS]; int nb;
  curve::Point w[curve::MAX_POINTS]; int nw;
};
struct FanCurve  { curve::Point p[curve::MAX_POINTS]; int n; };
struct HeaterCfg { float target; float hyst; };
struct SafeCfg   { float lo; float hi; };

struct Settings {
  LightCurves light;
  FanCurve    fan;
  HeaterCfg   heater;
  SafeCfg     safe;
};

enum NetMode { MODE_AP = 0, MODE_STA = 1, MODE_STANDALONE = 2 };

struct Live {
  float water = 26.0f; bool waterValid = false;
  float air = 26.0f, press = 1013.0f;
  uint8_t ledR = 0, ledG = 0, ledB = 0, ledW = 0;
  float fanDuty = 0.0f; int fanRpm = 0; float airflow = 0.0f;
  bool  heaterOn = false;
  bool  alarm = false;              // 安全域逸脱
  int   alarmDir = 0;               // -1 低温, +1 高温, 0 正常
  NetMode mode = MODE_AP;
};

// 既定値 (モック一致) を埋める
void settings_defaults(Settings& s);

// デバッグ用オーバーライド (UI からの手動駆動)。安全のため TTL で自動解除。
struct Override {
  bool     ledActive = false; float ledMinute = 0;  uint32_t ledExpireMs = 0;
  bool     fanActive = false; float fanDuty = 0;     uint32_t fanExpireMs = 0;
};

#ifdef ARDUINO
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
// 単一インスタンス + 排他
extern Settings   g_set;
extern Live       g_live;
extern Override   g_ovr;
extern SemaphoreHandle_t g_mtx;
inline void state_lock()   { xSemaphoreTake(g_mtx, portMAX_DELAY); }
inline void state_unlock() { xSemaphoreGive(g_mtx); }
void state_init();
#endif
