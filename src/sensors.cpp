// =====================================================================
//  sensors.cpp  -  DS18B20 水温 + 気温/気圧 (BME280/BMP280/C3ダイ/ダミー)
//  - 水温: pin::DS18B20==DUMMY でダミー波形
//  - 気温: air::SOURCE で選択。I2C が DUMMY のとき BME/BMP は DIE へ降格。
//          DIE = ESP32-C3 内蔵ダイ温度 (temperatureRead)。
// =====================================================================
#include "sensors.h"
#include <Arduino.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

namespace sensors {

static OneWire           oneWire(pin::isDummy(pin::DS18B20) ? 0 : pin::DS18B20);
static DallasTemperature ds(&oneWire);
static Adafruit_BME280   bme;
static Adafruit_BMP280   bmp;
static bool   waterDummy = false;
static bool   firstReq   = true;
static int    airSrc     = air::DUMMY;     // 実効ソース (降格後)
static bool   baroOk     = false;          // 気圧センサ利用可

static constexpr float DEMO_PERIOD = 600.0f;     // ダミー 10 分周期
static inline float demoPhase() { return (millis() / 1000.0f) / DEMO_PERIOD * 2.0f * (float)M_PI; }

void begin() {
  waterDummy = pin::isDummy(pin::DS18B20);
  if (!waterDummy) {
    ds.begin();
    ds.setResolution(12);
    ds.setWaitForConversion(false);
    ds.requestTemperatures();
    firstReq = true;
  }

  // 気温ソース決定 (I2C 無効なら BME/BMP は DIE へ降格)
  airSrc = air::SOURCE;
  bool i2cOff = pin::isDummy(pin::I2C_SDA) || pin::isDummy(pin::I2C_SCL);
  if ((airSrc == air::BME280 || airSrc == air::BMP280) && i2cOff) airSrc = air::DIE;

  if (airSrc == air::BME280 || airSrc == air::BMP280) {
    Wire.begin(pin::I2C_SDA, pin::I2C_SCL);     // OLED と共有 (二重 begin は無害)
    if (airSrc == air::BME280) baroOk = bme.begin((uint8_t)air::ADDR, &Wire);
    else                       baroOk = bmp.begin((uint8_t)air::ADDR);
    if (!baroOk) airSrc = air::DIE;             // 検出失敗も DIE へ降格
  }
}

void readWater() {
  if (waterDummy) {
    float w = 26.0f + 1.5f * sinf(demoPhase());    // 24.5..27.5°C
    state_lock(); g_live.water = w; g_live.waterValid = true; state_unlock();
    return;
  }
  if (!firstReq) {
    float t = ds.getTempCByIndex(0);
    if (t > -55.0f && t < 125.0f && t != DEVICE_DISCONNECTED_C) {
      t += calib::WATER_OFFSET_C;
      state_lock(); g_live.water = t; g_live.waterValid = true; state_unlock();
    } else {
      state_lock(); g_live.waterValid = false; state_unlock();
    }
  }
  firstReq = false;
  ds.requestTemperatures();
}

void readAir() {
  float a, pr;
  switch (airSrc) {
    case air::BME280:
      a = bme.readTemperature(); pr = bme.readPressure() / 100.0f;
      if (isnan(a) || isnan(pr)) return;
      break;
    case air::BMP280:
      a = bmp.readTemperature(); pr = bmp.readPressure() / 100.0f;
      if (isnan(a) || isnan(pr)) return;
      break;
    case air::DIE:
      a  = temperatureRead();                            // C3 ダイ温度 (°C)
      pr = 1013.0f + 6.0f * sinf(demoPhase() * 0.66f);   // 気圧センサ無し → 合成
      break;
    default: // DUMMY
      a  = 26.0f + 2.5f * sinf(demoPhase() + 1.0f);
      pr = 1013.0f + 6.0f * sinf(demoPhase() * 0.66f);
      break;
  }
  a  += calib::AIR_OFFSET_C;
  pr += calib::PRESS_OFFSET_HPA;
  state_lock(); g_live.air = a; g_live.press = pr; state_unlock();
}

}  // namespace sensors
