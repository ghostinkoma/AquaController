// =====================================================================
//  sensors.cpp  -  DS18B20 水温 + 気温/気圧 (BME280/BMP280/C3ダイ/ダミー)
//  - 水温: pin::DS18B20==DUMMY でダミー波形
//  - 気温/気圧/湿度: air::SENSOR1/2 で選択 (最大2個併用)。I2C DUMMY 時は DIE/ダミーへ降格。
//          DIE = ESP32-C3 内蔵ダイ温度 (temperatureRead)。AHT20/25=温湿, BME=温湿圧, BMP=温圧。
// =====================================================================
#include "sensors.h"
#include "store.h"        // store::calibSave (自動校正の保存)
#include <Arduino.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_BME680.h>

namespace sensors {

static OneWire           oneWire(pin::isDummy(pin::DS18B20) ? 0 : pin::DS18B20);
static DallasTemperature ds(&oneWire);
// C3 は HW I2C が1つ (Wire) のみ。センサは Wire(GPIO8/9)、OLED は SW I2C(GPIO5/6) で分離。
static Adafruit_BME280   bme;
static Adafruit_BMP280   bmp;
static Adafruit_AHTX0    aht;
static Adafruit_SHT31    sht31;                  // 校正基準 (温湿)
static Adafruit_BME680   bme680;                 // 校正基準 (温湿圧)
static bool   waterDummy = false;
static bool   firstReq   = true;
// 検出済みセンサ (最大2個の併用結果として、どのデバイスが使えるか)
static bool   hasBme = false, hasBmp = false, hasAht = false, hasDie = false;
static bool   hasSht = false, hasBme680 = false; // 校正基準センサ
static SemaphoreHandle_t s_i2c = nullptr;        // Wire バスの排他 (readAir と校正タスク)
static inline void i2cLock()   { if (s_i2c) xSemaphoreTake(s_i2c, portMAX_DELAY); }
static inline void i2cUnlock() { if (s_i2c) xSemaphoreGive(s_i2c); }

static constexpr float DEMO_PERIOD = 600.0f;     // ダミー 10 分周期
static inline float demoPhase() { return (millis() / 1000.0f) / DEMO_PERIOD * 2.0f * (float)M_PI; }

static void calibTask(void*);                    // 自動校正タスク (前方宣言)

static bool usesI2C(air::Type t) {
  return t == air::BME280 || t == air::BMP280 || t == air::AHT20 || t == air::AHT25;
}
static void initSensor(air::Type t) {
  switch (t) {
    case air::BME280: if (bme.begin(air::BARO_ADDR, &Wire)) hasBme = true; break;
    case air::BMP280: {
      bool ok = bmp.begin(air::BARO_ADDR);        // 正規 (chipID 0x58) をまず試行
      if (!ok) {                                  // 中華クローン対応: 実チップIDを読んで再試行
        Wire.beginTransmission(air::BARO_ADDR); Wire.write(0xD0); Wire.endTransmission(false);
        Wire.requestFrom((int)air::BARO_ADDR, 1);
        int id = Wire.available() ? Wire.read() : -1;
        Serial.printf("[sensors] BMP280 @0x%02X chipID=0x%02X (0x58以外→そのID で再試行)\n",
                      air::BARO_ADDR, id);
        if (id > 0) ok = bmp.begin(air::BARO_ADDR, (uint8_t)id);
      }
      if (ok) {
        // データシート既定 (SunFounder 参考): 温度x2/気圧x16/フィルタx16/待機500ms
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
                        Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
                        Adafruit_BMP280::STANDBY_MS_500);
        hasBmp = true;
      }
      break;
    }
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
  if (!i2cOff) {                                 // I2C スキャン (診断: 実アドレス確認)
    Serial.print("[i2c] scan:");
    for (uint8_t a = 0x08; a < 0x78; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
    }
    Serial.println();
  }
  for (air::Type t : slots) {
    if (usesI2C(t) && i2cOff) continue;         // I2C 無効 → I2C センサはスキップ
    initSensor(t);
  }
  Serial.printf("[sensors] air: bme=%d bmp=%d aht=%d die=%d (SDA=%d SCL=%d)\n",
                hasBme, hasBmp, hasAht, hasDie, pin::I2C_SDA, pin::I2C_SCL);

  // 校正基準センサ (SHT31 + BME680) の検出と自動校正タスク起動
  s_i2c = xSemaphoreCreateMutex();
  if (calibauto::ENABLE && !i2cOff) {
    hasSht    = sht31.begin(calibauto::SHT31_ADDR);
    hasBme680 = bme680.begin(calibauto::BME680_ADDR);
    if (!hasBme680) {                            // 0x76 の実チップ ID を確認 (0x61=BME680/0x60=BME280/0x58=BMP280)
      Wire.beginTransmission(calibauto::BME680_ADDR); Wire.write(0xD0); Wire.endTransmission(false);
      Wire.requestFrom((int)calibauto::BME680_ADDR, 1);
      int id = Wire.available() ? Wire.read() : -1;
      Serial.printf("[calib] bme680 begin fail; chipID@0x%02X=0x%02X\n", calibauto::BME680_ADDR, id);
    }
    Serial.printf("[calib] ref: sht31=%d bme680=%d work(aht)=%d\n", hasSht, hasBme680, hasAht);
    // 基準 (SHT31 / BME680) が1つ以上 + 作業センサ(AHT) が有れば校正。基準ゼロなら校正しない。
    if ((hasSht || hasBme680) && hasAht) {
      xTaskCreate(calibTask, "calib", 4096, nullptr, 1, nullptr);
    }
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
      t += g_calib.water;                        // 実行時オフセット (config 既定 or calib.json)
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

  i2cLock();                             // 校正タスクと Wire バスを排他
  if (hasAht) {                          // AHT20/AHT25: 温度・湿度
    sensors_event_t he, te;
    aht.getEvent(&he, &te);
    if (!isnan(te.temperature))       a  = te.temperature;
    if (!isnan(he.relative_humidity)) hu = he.relative_humidity;
  }
  // 気圧の妥当性: 地表実在範囲外 (中華クローンの係数不整合でガベージ 1126hPa 等) は棄却。
  auto plausP = [](float p) { return !isnan(p) && p > 800.0f && p < 1085.0f; };
  if (hasBme) {                          // BME280: 温度・気圧・湿度
    float t = bme.readTemperature(), p = bme.readPressure() / 100.0f, h = bme.readHumidity();
    if (isnan(a)  && !isnan(t)) a  = t;
    if (isnan(pr) && plausP(p)) pr = p;
    if (isnan(hu) && !isnan(h)) hu = h;
  }
  if (hasBmp) {                          // BMP280: 温度・気圧
    float t = bmp.readTemperature(), p = bmp.readPressure() / 100.0f;
    if (isnan(a)  && !isnan(t)) a  = t;
    if (isnan(pr) && plausP(p)) pr = p;
  }
  i2cUnlock();
  if (isnan(a) && hasDie) a = temperatureRead();               // 温度フォールバック: C3 ダイ
  if (isnan(a))  a  = 26.0f   + 2.5f * sinf(demoPhase() + 1.0f);        // 最終フォールバック
  if (isnan(pr)) pr = 1013.0f + 6.0f * sinf(demoPhase() * 0.66f);      // 気圧センサ無し → 合成

  a  += g_calib.air;                     // 実行時オフセット (自動校正 or config 既定)
  pr += g_calib.press;
  bool huValid = !isnan(hu);
  if (huValid) hu += g_calib.humid;

  state_lock();
  g_live.air = a; g_live.press = pr;
  g_live.humidityValid = huValid;
  if (huValid) g_live.humidity = hu;     // 湿度センサ無しなら前回値を保持 (Valid=false)
  state_unlock();
}

// ---- 自動校正 (SHT31+BME680 基準 と 作業センサ AHT の差分学習) ----
// 最小最大を1つずつ捨てて平均。n<3 なら単純平均。
static float trimmedMean1(const float* a, int n) {
  if (n <= 0) return 0.0f;
  if (n < 3)  { float s = 0; for (int i = 0; i < n; i++) s += a[i]; return s / n; }
  float s = 0, mn = a[0], mx = a[0];
  for (int i = 0; i < n; i++) { s += a[i]; if (a[i] < mn) mn = a[i]; if (a[i] > mx) mx = a[i]; }
  return (s - mn - mx) / (n - 2);
}
// 1回の測定: 利用可能な基準(SHT31/BME680)の平均 - 作業(AHT) の差分 (温度/湿度)。
// 基準が1つも読めない or 作業が無効なら ok=false。
static void calibReadOnce(float& dTemp, float& dHumid, bool& ok) {
  ok = false;
  float rt = 0, rh = 0; int nr = 0;
  i2cLock();
  if (hasSht) {
    float t = sht31.readTemperature(), h = sht31.readHumidity();
    if (!isnan(t) && !isnan(h)) { rt += t; rh += h; nr++; }
  }
  if (hasBme680) {
    if (bme680.performReading() && !isnan(bme680.temperature) && !isnan(bme680.humidity)) {
      rt += bme680.temperature; rh += bme680.humidity; nr++;
    }
  }
  sensors_event_t he, te; aht.getEvent(&he, &te);
  i2cUnlock();
  float wT = te.temperature, wH = he.relative_humidity;
  if (nr == 0 || isnan(wT) || isnan(wH)) return;
  dTemp  = rt / nr - wT;                  // 基準平均 - 作業 (これを作業値に足すと基準へ近づく)
  dHumid = rh / nr - wH;
  ok = true;
}
// 校正タスク: 12分ごとに [1秒×12回, 最小最大除外の平均] を1サンプル取得。
// 5サンプル (=1時間) ごとに最小最大除外平均を g_calib へ反映し calib.json 保存。
static void calibTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  { float t, h; bool ok; calibReadOnce(t, h, ok);   // 起動診断: 即時に差分を1回表示
    if (ok) Serial.printf("[calib] init diff: air=%.3f humid=%.2f\n", t, h);
    else    Serial.println("[calib] init read fail"); }
  const int SPW = calibauto::SAMPLES_PER_WRITE;
  float sT[16], sH[16]; int nS = 0;      // 時間集約バッファ (SPW<=16 前提)
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(calibauto::SAMPLE_PERIOD_MS));   // 12分待つ
    float dT[calibauto::INNER_N], dH[calibauto::INNER_N]; int m = 0;
    for (int i = 0; i < calibauto::INNER_N; i++) {
      float t, h; bool ok; calibReadOnce(t, h, ok);
      if (ok) { dT[m] = t; dH[m] = h; m++; }
      vTaskDelay(pdMS_TO_TICKS(calibauto::INNER_STEP_MS));    // 1秒間隔
    }
    if (m < 3) continue;                                       // 読み取り不足はスキップ
    sT[nS] = trimmedMean1(dT, m); sH[nS] = trimmedMean1(dH, m); nS++;
    if (nS >= SPW) {
      float oT = trimmedMean1(sT, nS), oH = trimmedMean1(sH, nS);
      state_lock(); g_calib.air = oT; g_calib.humid = oH; state_unlock();
      store::calibSave();                                      // 1時間に1回書き込み
      Serial.printf("[calib] write: air_off=%.3f humid_off=%.2f\n", oT, oH);
      nS = 0;
    }
  }
}

}  // namespace sensors
