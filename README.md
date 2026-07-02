# AquaController — ESP32-C3 アクアリウム制御ファームウェア

ESP32-C3（RISC-V 単コア, USB-Serial-JTAG）で動く、アクアリウム用の LED / ファン /
ヒーター制御・環境センシング・Web UI 一体型ファームウェアです。PlatformIO + Arduino
(pioarduino platform, arduino-esp32 3.x) で構築しています。

- **ライト**: WS2812 / SK6812 系 RGBW ストリップを 24 時間スケジュール（Catmull-Rom 補間）で制御
- **ファン**: 水温カーブ（%）で PWM 制御（4 線ファン）
- **ヒーター**: 目標温度キープ + ヒステリシス + 35°C ハードキャップ
- **センサ**: 水温(DS18B20) / 気温・湿度(AHT20/25, BME280) / 気圧(BMP280, BME280)
- **自動校正**: 高精度基準(SHT31 / BME680)と作業センサの差分を学習し `calib.json` に保存
- **生体保護**: 安全域監視 + センサ無応答 / ヒーター無効 / ファン無効 の異常検知
- **ネットワーク**: Wi-Fi STA / AP（初回セットアップ）、mDNS、NTP 時刻同期
- **UI**: 内蔵 Web UI（モニタ / 履歴 / ライト / 制御 / システム）、0.42" OLED
- **永続化**: 設定・プリセット・校正値を LittleFS に CRC + `.old` フェイルセーフ付きで保存

---

## ハードウェア構成

| 系統 | デバイス | 接続 | 備考 |
|---|---|---|---|
| ライト | RGBW アドレサブル LED ×13 | GPIO2 (data) | `config.h AQ_LED_TYPE` で色順指定 (NEO_GRBW 等) |
| ファン | 4 線 PWM ファン | GPIO7 等 | 25kHz。未接続時は DUMMY |
| 水温 | DS18B20 (OneWire) | GPIO1 等 | 2 秒周期・最優先タスク |
| ヒーター | リレー/SSR | GPIO10 | Active-High（極性は config で反転可） |
| OLED | SSD1306 72x40 (内蔵) | **SW I2C** GPIO5/6 | C3 は HW I2C が 1 つのみのため SW I2C |
| センサ I2C | AHT25 / BMP280 / BME680 / SHT31 | **HW I2C (Wire)** GPIO8/9 | OLED と別バス |

**I2C アドレス**: AHT20/25=0x38, SHT31=0x44/0x45, BME280/BMP280=0x76/0x77, BME680=0x76/0x77

> **重要（ESP32-C3 の I2C は 1 系統）**: 内蔵 OLED は GPIO5/6 に固定配線のため
> **OLED=ソフトウェア I2C(5/6)**、**センサ=ハードウェア I2C(Wire, 8/9)** に分離しています。

---

## センサ構成（config.h）

```cpp
namespace air {
  // 表示用センサ（最大2個併用）。各測定値は提供できるセンサから収集
  constexpr Type SENSOR1 = AHT25;   // 温度/湿度 (0x38)
  constexpr Type SENSOR2 = BMP280;  // 温度/気圧 (0x76)
  constexpr uint8_t BARO_ADDR = 0x76;
}
namespace calibauto {
  // 自動校正の基準（高精度）。SHT31 と/または BME680 が有れば校正
  constexpr uint8_t SHT31_ADDR  = 0x45;
  constexpr uint8_t BME680_ADDR = 0x77;
}
```

- 中華 BMP280 クローン（chipID 0x58 以外）も可変 chipID で受理しますが、補正係数が
  不整合な個体は気圧がガベージになるため、地表実在範囲外(>1085 / <800 hPa)を棄却します。
  → **正規 BMP280 (0x58) か BME680 を推奨**。

---

## 自動校正（calib.json）

近接設置した高精度基準センサ（SHT31 / BME680）と作業センサ（AHT）の差分を学習し、
補正オフセットを `/calib.json` に永続化します。

- **測定**: 1 秒間隔で 12 回読み、最小・最大を捨てた中央 10 個の平均 = 1 サンプル
- **取得**: 12 分ごとに 1 サンプル
- **書込**: 5 サンプル（= 1 時間）ごとに最小最大除外平均を `calib.json` へ保存
- 基準センサが 1 つも無ければ校正しない（config.h の既定値を使用）
- I2C は mutex で `readAir` と排他

補正値の優先順位: **config.h 既定 < /calib.json < 自動校正**

---

## 生体保護（安全機能）

- **安全域監視**: 水温が `SAFE_LO..SAFE_HI` を逸脱 → フェイルセーフ（高温=ファン全開+ヒーターOFF / 低温=ヒーター強制ON）
- **センサ無応答**: 水温が規定時間 valid にならない → `sensorFault`
- **ヒーター無効**: ON 継続でも水温が上がらない → `heatFault`
- **ファン無効**: ON 継続でも水温が下がらない → `coolFault`
- **移行時停止**: STA 移行/設定保存の再起動猶予中はヒーター/ファンを強制 OFF
- **同期切断で自動復帰**: ブラウザからの手動オーバーライドは TTL(4s) で自動解除

---

## ネットワーク

- **初回**: Wi-Fi 未設定なら AP（SSID `AquaController` / PW `aqua1234`, 192.168.4.1）でセットアップ
- **STA 移行**: 資格情報を保存し**再起動して STA 単独で接続**（AP と STA を同時に上げない = セキュリティ）
- **メッシュ対応**: 同一 SSID 複数 AP 環境で最強 BSSID + ch にロックして接続、起動時 3 回リトライ
- **mDNS**: `http://aquacontroller.local`
- **NTP**: STA 接続時に同期（JST = UTC+9, `config.h tz::OFFSET_SEC`）。AP 運用時はシステムタブで手動時刻設定

---

## Web API

| メソッド | パス | 用途 |
|---|---|---|
| GET | `/api/state` | ライブ値（水温/気温/湿度/気圧/LED/ファン/異常フラグ） |
| GET/POST | `/api/settings` | カーブ・目標・安全域 |
| GET | `/api/history?tier=f\|m\|h` | 履歴（多階層リングバッファ） |
| GET/POST | `/api/wifi` `/api/wifi/scan` `/api/wifi/config` | Wi-Fi 状態・スキャン・設定 |
| GET/POST | `/api/presets` | LED/ファン プリセット（type=led/fan） |
| POST | `/api/time` | 手動時刻設定（UTC epoch） |
| POST | `/api/test` | LED/ファン デバッグ駆動（TTL 自動解除） |
| GET/POST | `/api/fs/ls` `/api/fs/get` `/api/fs/put` | デバッグ用ファイル操作 |

USB シリアルでも `ls` / `get <name>` / `put <name> <size>` が使えます（`scripts/fscmd.py`）。

---

## ビルド / 書き込み

```bash
# UI バンドル再生成（data/ を変更したら）
python scripts/build_webui.py
# ビルド + 書き込み（COM ポートは platformio.ini）
pio run -t upload
# シリアル診断（scripts/serial_diag.py <秒> <reset:0/1>）
python scripts/serial_diag.py 16 1
```

**重要な build_flags**（`platformio.ini`）: `ARDUINO_USB_CDC_ON_BOOT=1` /
`ARDUINO_USB_MODE=1` — C3 の `Serial` を USB-Serial-JTAG に載せる（既定は UART0 で
USB に出ない）。

---

## ソース構成

| ファイル | 役割 |
|---|---|
| `src/config.h` | 全ハード/制御定数（ここだけ編集して基板適合） |
| `src/main.cpp` | タスク: 水温(優先度3,2s) / 制御(2,120ms) / 校正(1) |
| `src/net.cpp` | Wi-Fi STA/AP、mDNS、NTP、時刻 |
| `src/sensors.cpp` | 水温・気温・湿度・気圧 + 自動校正 |
| `src/leds.cpp` `src/fan.cpp` `src/heater.cpp` | アクチュエータ HAL |
| `src/control.h` | 純粋ロジック（カーブ補間・判定。ホストテスト可） |
| `src/store.cpp` | LittleFS 永続化（settings/presets/calib, CRC+.old） |
| `src/web.cpp` | ESPAsyncWebServer ハンドラ + 埋め込み UI 配信 |
| `src/history.cpp` | 多階層リングバッファ履歴 |
| `src/display.cpp` | OLED（SW I2C） |
| `src/cmd.cpp` | USB シリアル ファイルコマンド |
| `data/` | Web UI（`build_webui.py` で `web_ui_gz.h` に埋め込み） |

---

## 既知の制限 / 注意

- **手動時刻は再起動で消える**（C3 にバッテリ RTC 無し）。恒久保持は外付け RTC が必要。
- **弱電波/メッシュ**での STA は環境依存。ハンドシェイク失敗時は AP フォールバック。
- **中華 BMP280 クローン**（chipID 0x40 等）は補正不整合で気圧が使えない場合あり。
- LittleFS は `spiffs` ラベルのパーティション（`partitions.csv`）を使用。
