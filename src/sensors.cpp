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

static void calibTask(void*);                          // 自動校正タスク (前方宣言)
static bool readRefAvg(float& rt, float& rh, float& rp); // 基準平均+BME680気圧 (前方宣言)

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
  for (air::Type t : slots) {
    if (usesI2C(t) && i2cOff) continue;         // I2C 無効 → I2C センサはスキップ
    initSensor(t);
  }

  // 校正基準センサ (SHT31 + BME680) の検出と自動校正タスク起動
  s_i2c = xSemaphoreCreateMutex();
  if (calibauto::ENABLE && !i2cOff) {
    hasSht    = sht31.begin(calibauto::SHT31_ADDR);
    hasBme680 = bme680.begin(calibauto::BME680_ADDR);
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
  float a = NAN, hu = NAN;              // 温度 / 湿度 (AHT 優先)
  float ahtT = NAN, ahtH = NAN;         // 作業センサ(AHT)の生値 (校正差分用)
  float workP = NAN;                    // 作業気圧 (BMP280。BME280 併用時はそちら)
  float refT = NAN, refH = NAN, refP = NAN; bool haveRef = false;  // 基準(SHT31/BME680)
  // 気圧の妥当性: 地表実在範囲外 (中華クローンの係数不整合でガベージ 1126hPa 等) は棄却。
  auto plausP = [](float p) { return !isnan(p) && p > 800.0f && p < 1085.0f; };

  i2cLock();                             // 校正タスクと Wire バスを排他
  if (hasAht) {                          // AHT20/AHT25: 温度・湿度
    sensors_event_t he, te;
    aht.getEvent(&he, &te);
    ahtT = te.temperature; ahtH = he.relative_humidity;
    if (!isnan(ahtT)) a  = ahtT;
    if (!isnan(ahtH)) hu = ahtH;
  }
  if (hasBme) {                          // BME280(現構成では未使用): 温度・気圧・湿度
    float t = bme.readTemperature(), p = bme.readPressure() / 100.0f, h = bme.readHumidity();
    if (isnan(a)  && !isnan(t)) a  = t;
    if (plausP(p)) workP = p;
    if (isnan(hu) && !isnan(h)) hu = h;
  }
  if (hasBmp) {                          // BMP280(作業): 温度・気圧
    float t = bmp.readTemperature(), p = bmp.readPressure() / 100.0f;
    if (isnan(a)  && !isnan(t)) a  = t;
    if (plausP(p)) workP = p;
  }
  if (hasSht || hasBme680) haveRef = readRefAvg(refT, refH, refP); // 基準(温湿+BME680気圧)
  i2cUnlock();
  if (isnan(a) && hasDie) a = temperatureRead();               // 温度フォールバック: C3 ダイ
  if (isnan(a))  a  = 26.0f + 2.5f * sinf(demoPhase() + 1.0f);         // 最終フォールバック

  // 気圧表示の優先順位:
  //   1) 基準 BME680 があれば基準(無補正・高精度)を表示。
  //   2) 基準が無ければ 作業 BMP280 + 学習オフセット(g_calib.press) = 独り立ち。
  //   3) どちらも無ければ合成値。
  //  ※オフセットは BMP280 が表示ソースの時のみ適用 (基準には足さない)。
  float pr;
  if      (plausP(refP))  pr = refP;
  else if (plausP(workP)) pr = workP + g_calib.press;
  else                    pr = 1013.0f + 6.0f * sinf(demoPhase() * 0.66f);

  a  += g_calib.air;                     // 温度オフセット (AHT に適用)
  bool huValid = !isnan(hu);
  if (huValid) hu += g_calib.humid;

  // 校正デバッグ: 生の差分 (基準 - 作業)。オフセット未書込(0)でも測定の生存を確認できる。
  bool diffValid  = haveRef && !isnan(ahtT) && !isnan(ahtH);
  bool diffPValid = plausP(refP) && plausP(workP);

  state_lock();
  g_live.air = a; g_live.press = pr;
  g_live.humidityValid = huValid;
  if (huValid) g_live.humidity = hu;     // 湿度センサ無しなら前回値を保持 (Valid=false)
  g_live.calibDiffValid = diffValid;
  if (diffValid) { g_live.calibDiffAir = refT - ahtT; g_live.calibDiffHumid = refH - ahtH; }
  g_live.calibDiffPressValid = diffPValid;
  if (diffPValid) g_live.calibDiffPress = refP - workP;   // 基準-作業 (BMP280に足すと基準へ)
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
// 基準センサの温度・湿度(rt/rh) + BME680 気圧(rp) を返す。
//  ★温湿の基準は SHT31 を優先: BME680 はガスヒータの自己発熱で温度が +0.5〜1℃ 高めに
//    出るため温度基準に不適。SHT31(±0.2℃) を温湿基準、BME680 は気圧基準専用とする。
//    SHT31 が無い時のみ BME680 の温湿をフォールバックで使う。i2cLock は呼出側保持。
static bool readRefAvg(float& rt, float& rh, float& rp) {
  float t = NAN, h = NAN; rp = NAN;
  if (hasSht) {
    float a = sht31.readTemperature(), b = sht31.readHumidity();
    if (!isnan(a) && !isnan(b)) { t = a; h = b; }       // 温湿基準 = SHT31 単独 (高精度)
  }
  if (hasBme680) {
    if (bme680.performReading()) {
      float p = bme680.pressure / 100.0f;
      if (p > 800.0f && p < 1085.0f) rp = p;             // 気圧基準 = BME680
      if (isnan(t) && !isnan(bme680.temperature) && !isnan(bme680.humidity)) {
        t = bme680.temperature; h = bme680.humidity;     // SHT31 無し時のみ温湿フォールバック
      }
    }
  }
  if (isnan(t) || isnan(h)) return false;
  rt = t; rh = h; return true;
}
// 1回の測定: 基準 - 作業 の差分。
//  温湿 (okTH): 基準(SHT31/BME680) - 作業(AHT)。 気圧 (okP): 基準(BME680) - 作業(BMP280)。
//  それぞれ独立に有効/無効を返す (気圧センサ構成が違っても片方だけ校正できる)。
static void calibReadOnce(float& dTemp, float& dHumid, float& dPress, bool& okTH, bool& okP) {
  okTH = false; okP = false;
  // ★気流ゲート: ファン気流が十分ある時のみ校正。静止空気は設置位置の熱勾配で
  //   作業/基準が乖離し offset を汚染するため (実測 ±1℃)。気流下は純粋誤差(≈0)へ収束。
  float duty; state_lock(); duty = g_live.fanDuty; state_unlock();
  if (duty < calibauto::MIN_DUTY_FOR_CALIB) return;
  i2cLock();
  float rt, rh, rp; bool haveRef = readRefAvg(rt, rh, rp);
  sensors_event_t he, te; aht.getEvent(&he, &te);
  float workP = NAN;
  if (hasBmp) { float p = bmp.readPressure() / 100.0f; if (p > 800.0f && p < 1085.0f) workP = p; }
  i2cUnlock();
  if (haveRef && !isnan(te.temperature) && !isnan(he.relative_humidity)) {
    dTemp  = rt - te.temperature;         // 基準 - 作業 (作業値に足すと基準へ近づく)
    dHumid = rh - he.relative_humidity;
    okTH = true;
  }
  if (!isnan(rp) && !isnan(workP)) { dPress = rp - workP; okP = true; }
}
// 校正タスク: 12分ごとに [1秒×12回, 最小最大除外の平均] を1サンプル取得。
// 5サンプル (=1時間) ごとに最小最大除外平均を g_calib へ反映し calib.json 保存。
static void calibTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  const int SPW = calibauto::SAMPLES_PER_WRITE;
  float sT[16], sH[16], sP[16]; int nS = 0, nSP = 0;   // 時間集約バッファ (SPW<=16 前提)
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(calibauto::SAMPLE_PERIOD_MS));   // 12分待つ
    float dT[calibauto::INNER_N], dH[calibauto::INNER_N], dP[calibauto::INNER_N];
    int m = 0, mp = 0;
    for (int i = 0; i < calibauto::INNER_N; i++) {
      float t, h, p; bool okTH, okP; calibReadOnce(t, h, p, okTH, okP);
      if (okTH) { dT[m] = t; dH[m] = h; m++; }
      if (okP)  { dP[mp] = p; mp++; }
      vTaskDelay(pdMS_TO_TICKS(calibauto::INNER_STEP_MS));    // 1秒間隔
    }
    if (m  >= 3) { sT[nS]  = trimmedMean1(dT, m);  sH[nS]  = trimmedMean1(dH, m);  nS++; }
    if (mp >= 3) { sP[nSP] = trimmedMean1(dP, mp); nSP++; }
    if (nS >= SPW) {
      float oT = trimmedMean1(sT, nS), oH = trimmedMean1(sH, nS);
      state_lock(); g_calib.air = oT; g_calib.humid = oH; state_unlock();
      if (nSP >= 3) {                                          // 気圧オフセットも同時更新
        float oP = trimmedMean1(sP, nSP);
        state_lock(); g_calib.press = oP; state_unlock();
      }
      store::calibSave();                                      // 1時間に1回書き込み
      nS = 0; nSP = 0;
    }
  }
}

}  // namespace sensors
