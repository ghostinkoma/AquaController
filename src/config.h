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
constexpr int LED_COUNT = 8;                   // ★ストリップのピクセル数に合わせて変更

// --- ファン (4 線 PWM) --- 未接続のためダミー (rpm は duty 推定。tach 無し) ---
constexpr int FAN_PWM = DUMMY;                 // 接続したら GPIO7 等へ

// --- 水温 (DS18B20 / OneWire) --- 未接続のためダミー波形 ---
constexpr int DS18B20 = DUMMY;                 // 接続したら GPIO1 等へ

// --- I2C 共有バス (OLED / BME280・BMP280) --- DUMMY 可 (両方 DUMMY で I2C 無効) ---
constexpr int I2C_SDA = 5;                     // GPIO5 (SuperMini OLED)。未使用なら DUMMY
constexpr int I2C_SCL = 6;                     // GPIO6。未使用なら DUMMY

// --- ヒーター (リレー/SSR) --- 接続済前提。未使用なら DUMMY に ---
constexpr int HEATER  = 10;                    // デジタル出力 ON/OFF

// --- ファクトリーリセットボタン --- 長押しで wifi 設定のみ消去 (プリセットは保持) ---
constexpr int RESET_BTN = 9;                   // GPIO9 (SuperMini の BOOT ボタン)。未使用なら DUMMY
}  // namespace pin

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

// ---------- 気温・気圧ソース ----------
//  外部センサ未接続でも C3 内蔵ダイ温度で「気温」を実測可能。
//  SOURCE:
//    DUMMY  … サイン波 (HW 不要)
//    DIE    … ESP32-C3 ダイ温度 (temperatureRead)。気圧はダミー。
//    BME280 … I2C 温湿圧 (湿度は未使用)。要 I2C 実ピン。
//    BMP280 … I2C 温圧。要 I2C 実ピン。
//  I2C を DUMMY にした場合 BME/BMP は使えないので自動的に DIE/DUMMY へ降格。
namespace air {
enum Source { DUMMY = 0, DIE = 1, BME280 = 2, BMP280 = 3 };
constexpr Source SOURCE = DIE;        // 既定: 外部センサ未接続 → C3 ダイ温度
constexpr int    ADDR   = 0x76;       // BME/BMP の I2C アドレス (0x76 / 0x77)
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
//  air::SOURCE=DIE 運用時は AIR_OFFSET_C を負値にして室温相当へ補正することを推奨。
//  気圧は個体差のオフセット校正のほか、海面更正 (現地気圧→海面気圧) の下駄にも使える
//  (概ね標高 8m 上昇ごとに約 -1 hPa。例: 標高 80m なら +10 hPa 程度)。
namespace calib {
constexpr float WATER_OFFSET_C   = 0.0f;   // DS18B20 水温 補正 (°C)
constexpr float AIR_OFFSET_C     = 0.0f;   // 気温 (DIE/BME280/BMP280) 補正 (°C)
constexpr float PRESS_OFFSET_HPA = 0.0f;   // 気圧 (BME280/BMP280) 補正 (hPa)
}  // namespace calib
