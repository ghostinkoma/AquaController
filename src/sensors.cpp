// =====================================================================
//  sensors.cpp  -  DS18B20 水温 + 気温/気圧 (BME280/BMP280/C3ダイ/ダミー)
//  - 水温: pin::DS18B20==DUMMY でダミー波形
//  - 気温/気圧/湿度: air::SENSOR1/2 で選択 (最大2個併用)。I2C DUMMY 時は DIE/ダミーへ降格。
//          DIE = ESP32-C3 内蔵ダイ温度 (temperatureRead)。AHT20/25=温湿, BME=温湿圧, BMP=温圧。
// =====================================================================
#include "sensors.h"
#include <Arduino.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>

namespace sensors {

static OneWire           oneWire(pin::isDummy(pin::DS18B20) ? 0 : pin::DS18B20);
static DallasTemperature ds(&oneWire);
// C3 は HW I2C が1つ (Wire) のみ。センサは Wire(GPIO8/9)、OLED は SW I2C(GPIO5/6) で分離。
static Adafruit_BME280   bme;
static Adafruit_BMP280   bmp;
static Adafruit_AHTX0    aht;
static bool   waterDummy = false;
static bool   firstReq   = true;
// 検出済みセンサ (最大2個の併用結果として、どのデバイスが使えるか)
static bool   hasBme = false, hasBmp = false, hasAht = false, hasDie = false;

static constexpr float DEMO_PERIOD = 600.0f;     // ダミー 10 分周期
static inline float demoPhase() { return (millis() / 1000.0f) / DEMO_PERIOD * 2.0f * (float)M_PI; }

static bool usesI2C(air::Type t) {
  return t == air::BME280 || t == air::BMP280 || t == air::AHT20 || t == air::AHT25;
}
static void initSensor(air::Type t) {
  switch (t) {
    case air::BME280: if (bme.begin(air::BARO_ADDR, &Wire)) hasBme = true; break;
    case air::BMP280: if (bmp.begin(air::BARO_ADDR))        hasBmp = true; break;
    case air::AHT20:
    case air::AHT25:  if (aht.begin(&Wire))                 hasAht = true; break;
    case air::DIE:    hasDie = true; break;
    default: break;                                          // NONE
  }
}

void begin() {
  waterDummy = pin::isDummy(pin::DS18B20);
  if (!waterDummy) {
    ds.begin();
    ds.setResolution(12);
    ds.setWaitForConversion(false);
    ds.requestTemperatures();
    firstReq = true;
  }

  // 気温/気圧/湿度センサ (最大2個併用)。I2C 無効なら I2C センサは初期化しない。
  bool i2cOff = pin::isDummy(pin::I2C_SDA) || pin::isDummy(pin::I2C_SCL);
  const air::Type slots[2] = { air::SENSOR1, air::SENSOR2 };
  bool needI2C = usesI2C(air::SENSOR1) || usesI2C(air::SENSOR2);
  if (needI2C && !i2cOff) Wire.begin(pin::I2C_SDA, pin::I2C_SCL); // センサ用 HW I2C (Wire, GPIO8/9)
  for (air::Type t : slots) {
    if (usesI2C(t) && i2cOff) continue;         // I2C 無効 → I2C センサはスキップ
    initSensor(t);
  }
  Serial.printf("[sensors] air: bme=%d bmp=%d aht=%d die=%d (SDA=%d SCL=%d)\n",
                hasBme, hasBmp, hasAht, hasDie, pin::I2C_SDA, pin::I2C_SCL);
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
  float a = NAN, pr = NAN, hu = NAN;    // 温度 / 気圧 / 湿度 (提供センサから収集)

  if (hasAht) {                          // AHT20/AHT25: 温度・湿度
    sensors_event_t he, te;
    aht.getEvent(&he, &te);
    if (!isnan(te.temperature))       a  = te.temperature;
    if (!isnan(he.relative_humidity)) hu = he.relative_humidity;
  }
  if (hasBme) {                          // BME280: 温度・気圧・湿度
    float t = bme.readTemperature(), p = bme.readPressure() / 100.0f, h = bme.readHumidity();
    if (isnan(a)  && !isnan(t)) a  = t;
    if (isnan(pr) && !isnan(p)) pr = p;
    if (isnan(hu) && !isnan(h)) hu = h;
  }
  if (hasBmp) {                          // BMP280: 温度・気圧
    float t = bmp.readTemperature(), p = bmp.readPressure() / 100.0f;
    if (isnan(a)  && !isnan(t)) a  = t;
    if (isnan(pr) && !isnan(p)) pr = p;
  }
  if (isnan(a) && hasDie) a = temperatureRead();               // 温度フォールバック: C3 ダイ
  if (isnan(a))  a  = 26.0f   + 2.5f * sinf(demoPhase() + 1.0f);        // 最終フォールバック
  if (isnan(pr)) pr = 1013.0f + 6.0f * sinf(demoPhase() * 0.66f);      // 気圧センサ無し → 合成

  a  += calib::AIR_OFFSET_C;
  pr += calib::PRESS_OFFSET_HPA;
  bool huValid = !isnan(hu);
  if (huValid) hu += calib::HUMID_OFFSET_PCT;

  state_lock();
  g_live.air = a; g_live.press = pr;
  g_live.humidityValid = huValid;
  if (huValid) g_live.humidity = hu;     // 湿度センサ無しなら前回値を保持 (Valid=false)
  state_unlock();
}

}  // namespace sensors
