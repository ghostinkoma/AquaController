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
  float humidity = 50.0f; bool humidityValid = false;   // 湿度 %RH (BME280/AHT が無ければ無効)
  // 校正デバッグ: 基準(SHT31/BME680) - 作業(AHT) の生の差分 (毎 readAir 更新)。オフセット適用値は g_calib。
  float calibDiffAir = 0.0f, calibDiffHumid = 0.0f; bool calibDiffValid = false;
  uint8_t ledR = 0, ledG = 0, ledB = 0, ledW = 0;
  float fanDuty = 0.0f; int fanRpm = 0; float airflow = 0.0f;
  bool  heaterOn = false;
  bool  alarm = false;              // 何らかの異常 (下記いずれか) で true
  int   alarmDir = 0;               // -1 低温, +1 高温, 0 正常 (安全域逸脱)
  // --- 生体保護: 追加の異常検知 ---
  bool  sensorFault = false;        // 水温センサ無応答 (規定時間 valid にならない)
  bool  heatFault   = false;        // ヒーター ON 継続でも水温が上がらない (故障/断線疑い)
  bool  coolFault   = false;        // ファン ON 継続でも水温が下がらない (故障/停止疑い)
  NetMode mode = MODE_AP;
};

// 実行時の校正オフセット (/calib.json 由来。基準センサ接続時は自動校正で更新)。
struct CalibOffsets { float water; float air; float press; float humid; };

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
extern volatile bool g_haltActuators;   // true=ヒーター/ファン強制OFF (STA移行/再起動前の安全停止)
extern CalibOffsets g_calib;            // 実行時 校正オフセット (config 既定 or /calib.json)
extern SemaphoreHandle_t g_mtx;
inline void state_lock()   { xSemaphoreTake(g_mtx, portMAX_DELAY); }
inline void state_unlock() { xSemaphoreGive(g_mtx); }
void state_init();
#endif
