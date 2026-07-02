// =====================================================================
//  config.h  -  ハードウェア/制御の全定数 (ここだけ編集すれば基板に適合)
//  ※ Arduino.h を include しない純粋ヘッダ。ホストテストからも参照可能。
//
//  【重要・前提の明示】以下はモック v3 のデータモデルに完全一致させた既定値です。
//  あなたの既存基板のピン配置に合わせて必ず見直してください。
//
//  ESP32-C3 ピン注意:
//    - strapping: GPIO2, GPIO8, GPIO9 (起動時レベル制約)。I2C はアイドル High の
//      ためプルアップ付き GPIO8/9 を SDA/SCL に充てるのは許容。
//    - USB-Serial-JTAG: GPIO18/19、UART0: GPIO20/21 → 書込/ログ用に温存。
//    - LEDC は全 GPIO で利用可。RISC-V 単コア・FPU 非搭載。
// =====================================================================
#pragma once
#include <stdint.h>

// ---------- ピン配置 (要・基板適合確認) ----------
//  ★ ダミー運用: ピンに DUMMY(0xFF) を設定すると、その系統は HW アクセスせず
//    シミュレート動作 (未接続のセンサ/アクチュエータをそのまま起動できる)。
namespace pin {
constexpr int DUMMY = 0xFF;                    // この値のピン = ダミー (HW 触らない)
inline constexpr bool isDummy(int p) { return p == DUMMY; }

// --- ライト: WS2812 / SK6812 RGBW (アドレサブル, データ線 1 本) ---
constexpr int LED_DATA  = 2;                   // GPIO2 (現在接続)。
// 注意: GPIO2 は strapping。WS2812 DIN はアイドル Low のため、起動時に外部が
//   Low へ引くと書込モードへ入る恐れ。誤動作する場合はレベル/プルアップを確認。
constexpr int LED_COUNT = 13;                   // ★ストリップのピクセル数に合わせて変更

// --- ファン (4 線 PWM) --- 未接続のためダミー (rpm は duty 推定。tach 無し) ---
constexpr int FAN_PWM = DUMMY;                 // 接続したら GPIO7 等へ

// --- 水温 (DS18B20 / OneWire) --- 未接続のためダミー波形 ---
constexpr int DS18B20 = DUMMY;                 // 接続したら GPIO1 等へ

// --- I2C バス0 (Wire): 内蔵 OLED 専用 --- SuperMini は OLED を GPIO5/6 にハードワイヤ ---
constexpr int OLED_SDA = 5;                    // GPIO5 (SuperMini 内蔵 OLED)。未使用なら DUMMY
constexpr int OLED_SCL = 6;                    // GPIO6。
// --- I2C バス1 (Wire1): 外部センサ (AHT25/BMP280/BME680/SHT31 等) --- DUMMY で無効 ---
//  ★ OLED と別バスにする理由: 内蔵 OLED は 5/6 固定のため、センサは 8/9 に分離する。
constexpr int I2C_SDA = 8;                     // GPIO8 (センサ SDA)。未使用なら DUMMY
constexpr int I2C_SCL = 9;                     // GPIO9 (センサ SCL)。

// --- ヒーター (リレー/SSR) --- 接続済前提。未使用なら DUMMY に ---
constexpr int HEATER  = 10;                    // デジタル出力 ON/OFF

// --- ファクトリーリセットボタン --- 長押しで wifi 設定のみ消去 (プリセットは保持) ---
constexpr int RESET_BTN = 5;                   // GPIO9 (SuperMini の BOOT ボタン)。未使用なら DUMMY
}  // namespace pin

// ---------- LED ストリップ種別 (自作モジュール等に合わせて調整) ----------
//  Adafruit_NeoPixel の「色順 + チャンネル数 + 通信速度」フラグ。leds.cpp で使用。
//  色が入れ替わる/化ける場合はここだけ変更する:
//    4ch RGBW (白LEDあり): NEO_GRBW(SK6812一般) / NEO_RGBW / NEO_GRBW ほか
//    3ch RGB  (白LEDなし): NEO_GRB(WS2812一般)  / NEO_RGB  ほか  ※W カーブは無視される
//  速度は通常 NEO_KHZ800。古い WS2811 等は NEO_KHZ400。
//  ※ config.h は純ヘッダのため NEO_* マクロは leds.cpp で展開される (#define で遅延評価)。
//  ★ OST45050C1A-W: 実測で NEO_RGBW だと 設定R→実測G / G→R と入替。R↔G を戻す
//    NEO_GRBW が正 (B/W は一致)。※データシート表記と実配線で R/G が入替のため実測優先。
#define AQ_LED_TYPE (NEO_GRBW + NEO_KHZ800)

// ---------- OLED (SSD1306 72x40 ビルトイン / SuperMini) ----------
//  物理 128x64 の中央 72x40 を使用。実測オフセット (27,23) で描画。
//  BadCodec c3SuperMiniOLED の実績値に一致。
namespace oled {
constexpr bool    ENABLE = true;               // false で無効化
constexpr uint8_t ADDR   = 0x3C;
constexpr int     PHYS_W = 128, PHYS_H = 64;
constexpr int     DISP_W = 72,  DISP_H = 40;
constexpr int     X_OFF  = 27,  Y_OFF  = 23;   // 実測オフセット
}  // namespace oled

// ---------- 気温・気圧・湿度 センサ ----------
//  I2C バスに最大2個のセンサを併用できる (例: 気圧=BMP280 + 温湿=AHT20)。
//  各センサが提供する項目:
//    BME280      … 温度 / 気圧 / 湿度     (I2C 0x76 or 0x77)
//    BMP280      … 温度 / 気圧            (I2C 0x76 or 0x77)
//    AHT20/AHT25 … 温度 / 湿度            (I2C 0x38 固定, Adafruit_AHTX0)
//    DIE         … ESP32-C3 内蔵ダイ温度のみ (外部センサ不要)
//    NONE        … 未接続
//  各測定値(温度/気圧/湿度)は「提供できる最初のセンサ」から取得する。無い場合:
//    温度 → DIE (C3ダイ) → ダミー波形 / 気圧 → 合成値 / 湿度 → 無効 (UI 非表示)
//  I2C を DUMMY にすると I2C センサは初期化されない (DIE/ダミーへ降格)。
namespace air {
enum Type { NONE = 0, DIE = 1, BME280 = 2, BMP280 = 3, AHT20 = 4, AHT25 = 5 };
constexpr Type    SENSOR1   = AHT25;   // ★現在接続: AHT25 (I2C 0x38, 温度/湿度)
constexpr Type    SENSOR2   = BMP280;  // ★現在接続: BMP280 (I2C 0x76, 温度/気圧)
constexpr uint8_t BARO_ADDR = 0x76;    // BME280/BMP280 の I2C アドレス (0x76 / 0x77)
}  // namespace air

// ---------- PWM / LED ストリップ ----------
namespace pwm {
constexpr int LED_MAX      = 255;    // 0..255 (channelByte のスケール; モデルと一致)
constexpr int LED_BRIGHT   = 255;    // ストリップ全体の上限輝度 (0..255)
constexpr int FAN_CH       = 4;      // (Arduino 3.x は pin 単位 attach のため参考値)
constexpr int FAN_FREQ_HZ  = 25000;  // 4 線ファン規格
constexpr int FAN_RES_BITS = 8;      // 0..255
constexpr int FAN_MAX      = 255;
constexpr bool HEATER_ACTIVE_HIGH = true;  // リレーモジュール極性。Active-Low なら false。
}  // namespace pwm

// ---------- 制御パラメータ (モック v3 と一致) ----------
namespace ctrl {
// ライト: x=0..1440 分, y=0..100 %
constexpr float LIGHT_X_MIN = 0.0f, LIGHT_X_MAX = 1440.0f;
// ファン: x=水温22..35°C, y=風量0..100%
constexpr float FAN_T_MIN = 22.0f,  FAN_T_MAX = 35.0f;
constexpr int   FAN_RPM_MAX   = 2200;     // duty100% 相当
constexpr float FAN_AIRFLOW_K = 0.013f;   // rpm -> m^3/h
constexpr int   FAN_RPM_MIN_ON= 350;      // 起動可能最低 rpm
// ヒーター: 目標温度キープ。35°C ハードキャップ (生体保護)。
constexpr float HEATER_MAX_TARGET = 35.0f;
constexpr float HEATER_MIN_TARGET = 18.0f;
constexpr float HEATER_DEF_TARGET = 25.0f;
constexpr float HEATER_DEF_HYST   = 0.3f;
// 安全域 (既存仕様 23–29°C)。逸脱でフェイルセーフ。
constexpr float SAFE_LO = 23.0f, SAFE_HI = 29.0f;
}  // namespace ctrl

// ---------- 生体保護 異常検知 (アラート) ----------
//  実際に温度が意図通り動いているかを監視し、機器故障を早期に検知する。
namespace safety {
constexpr uint32_t SENSOR_TIMEOUT_MS = 10000;         // 水温が規定時間 無効 → センサ異常
// ヒーター/ファンの「効いていない」判定: 連続稼働 EVAL_MS の間に MIN_DELTA も
// 温度が動かず、かつ目標方向に達していない場合に異常とみなす。
constexpr uint32_t HEAT_EVAL_MS  = 15UL * 60 * 1000;  // ヒーター効果判定窓 (15分)
constexpr uint32_t COOL_EVAL_MS  = 15UL * 60 * 1000;  // ファン効果判定窓 (15分)
constexpr float    HEAT_MIN_RISE_C = 0.3f;            // 窓内に最低これだけ上がるべき
constexpr float    COOL_MIN_DROP_C = 0.3f;            // 窓内に最低これだけ下がるべき
// タスクウォッチドッグ: 水温/制御タスクがこの時間 feed しなければ panic→リセット。
// (水温2秒/制御120ms 周期に対し十分な余裕。再起動でアクチュエータは OFF 安全状態へ)
constexpr uint32_t WDT_TIMEOUT_MS = 8000;
}  // namespace safety

// ---------- タスク周期 ----------
namespace timing {
constexpr uint32_t WATER_PERIOD_MS  = 2000;  // 水温 最優先 (生死直結)
constexpr uint32_t CONTROL_PERIOD_MS= 120;   // ライト適用を高速化 (LED スクラブのレイテンシ低減)
constexpr uint32_t FAN_PERIOD_MS    = 500;   // ファン/ヒーター評価
constexpr uint32_t AIR_PERIOD_MS    = 5000;  // 気温・気圧
constexpr uint32_t HIST_PERIOD_MS   = 1000;  // 履歴 tick
constexpr uint32_t RESET_HOLD_MS    = 5000;  // リセットボタン長押し時間
constexpr uint32_t AP_OFF_DELAY_MS  = 6000;  // STA 確立後、応答到達を待って AP を切るまでの猶予
}  // namespace timing

// ---------- 履歴 (RAM リングバッファ; ESP32-C3 SRAM ~400KB 内) ----------
//  full-depth が要るなら LittleFS への追記ログへ拡張する。
namespace hist {
constexpr uint32_t F_STEP_S = 12;   constexpr int F_CAP = 600;   // ~2h
constexpr uint32_t M_STEP_S = 60;   constexpr int M_CAP = 1440;  // 24h
constexpr uint32_t H_STEP_S = 720;  constexpr int H_CAP = 1440;  // ~12日
// 1 サンプル 15B 程度 × (600+1440+1440)=~52KB
}  // namespace hist

// ---------- ネットワーク ----------
#define AQ_AP_SSID   "AquaController"
// 既定 AP パスワード。※WPA2 は8文字以上必須。8文字未満は無条件で「開放AP」になる
// (net.cpp softAPup 参照) ため、ここは必ず8文字以上にすること。
#define AQ_AP_PW_DEFAULT "aqua1234"
#define AQ_AP_IP     "192.168.4.1"
#define AQ_HOSTNAME  "aquacontroller"

static_assert(sizeof(AQ_AP_PW_DEFAULT) - 1 == 0 || sizeof(AQ_AP_PW_DEFAULT) - 1 >= 8,
              "AQ_AP_PW_DEFAULT: WPA2 は8文字以上必須 (空文字なら意図的な開放APとして許容)");

// ---------- センサ 補正値 (個体差 / 実測ズレの校正) ----------
//  実測値との差分をここに入れて補正する (例: 基準温度計と比較して +0.3°C 低く出るなら +0.3)。
//  DIE (ESP32-C3 内蔵ダイ温度) は自己発熱の影響で室温より数°C高く出るのが通例のため、
//  気温ソースが DIE のときは AIR_OFFSET_C を負値にして室温相当へ補正することを推奨。
//  気圧は個体差のオフセット校正のほか、海面更正 (現地気圧→海面気圧) の下駄にも使える
//  (概ね標高 8m 上昇ごとに約 -1 hPa。例: 標高 80m なら +10 hPa 程度)。
//  ※ これらは「既定値」。実行時は /calib.json が優先され (store::calibLoad)、
//    基準センサ (SHT31 + BME680) 接続時は自動校正で上書き・保存される。
namespace calib {
constexpr float WATER_OFFSET_C   = 0.0f;   // DS18B20 水温 補正 (°C)
constexpr float AIR_OFFSET_C     = 0.0f;   // 気温 (DIE/BME280/BMP280/AHT) 補正 (°C)
constexpr float PRESS_OFFSET_HPA = 0.0f;   // 気圧 (BME280/BMP280) 補正 (hPa)
constexpr float HUMID_OFFSET_PCT = 0.0f;   // 湿度 (BME280/AHT20/AHT25) 補正 (%RH)
}  // namespace calib

// ---------- 自動校正 (基準センサとの差分学習) ----------
//  近接設置した高精度基準センサ (AE-SHT31 + BME680) と作業センサ(AHT等)の差分を学習。
//  ★ SHT31 と BME680 の両方が接続されているときのみ校正する (どちらか欠けたら無効)。
//  測定: 1秒×INNER_N 回読み、最小最大を捨てて中央 INNER_KEEP 個を平均 = 1サンプル。
//        それを SAMPLE_PERIOD ごとに取得し、WRITE ごとに平均して calib.json へ保存。
namespace calibauto {
constexpr bool     ENABLE       = true;
constexpr uint8_t  SHT31_ADDR   = 0x45;              // AE-SHT31 (実測 0x45。0x44 の個体もあり)
constexpr uint8_t  BME680_ADDR  = 0x77;              // BME680 秋月 (SDO pull-up→0x77)。BMP280(0x76)と別
constexpr int      INNER_N      = 12;                // 1秒間隔の読み取り回数
constexpr int      INNER_DROP   = 1;                 // 上下それぞれ捨てる個数 (→中央10個平均)
constexpr uint32_t INNER_STEP_MS   = 1000;           // 読み取り間隔 (1秒)
constexpr uint32_t SAMPLE_PERIOD_MS= 12UL * 60 * 1000; // サンプル取得間隔 (12分)
constexpr int      SAMPLES_PER_WRITE = 5;            // 1時間に5サンプル → 平均して保存
}  // namespace calibauto

// ---------- タイムゾーン ----------
//  epoch は常に UTC。表示・NTP・LED スケジュールの「時刻(0..1440分)」はこの
//  オフセットを足したローカル時刻で扱う。日本は JST = UTC+9。
//  ※ フロント表示 (aqua-live.js) も +9h 前提。変更時は両方合わせること。
namespace tz {
constexpr long OFFSET_SEC = 9 * 3600;      // JST = UTC+9 (DST 無し)
}  // namespace tz
