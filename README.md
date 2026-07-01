# AquaController v3 — ESP32-C3 ファームウェア

モック v3（タップ式 RGBW ライト曲線・ファン曲線・ヒーター目標キープ）を実機化した
PlatformIO/Arduino プロジェクト。既存 AquaController の規約（`leds::setSchedule()` 系の
名前空間、`/api/settings`、DS18B20 の 2秒最優先監視、AP/STA/スタンドアローン）に合わせています。

> **検証状況（重要・正直な明示）**
> - **制御コア（Catmull-Rom 曲線 / ファン duty・rpm / ヒーターのヒステリシス＋35°C キャップ）は
>   ホストで g++ コンパイルし、モック(JS) と数値突合済み。** 量子化出力（LED バイト・rpm・風量・
>   ラッチ判定・キャップ）は **全件 bit 一致**、生カーブ差は float32 表現精度の 3.6e-5 のみ。
> - **HAL 層（LEDC / AsyncWebServer / LittleFS / FreeRTOS / センサ）はこのサンドボックスでは
>   コンパイルしていません**（Espressif ツールチェーンが無いため）。API どおりに記述してあります。
>   実機では `pio run` でビルドしてください。下記の前提（特にピン配置）は必ず確認を。

---

## ハードウェア前提（要・基板適合確認）

モック v3 のデータモデルに完全一致させた既定です。`src/config.h` だけ直せば差し替え可能。

| 機能 | 既定 | 備考 |
|------|------|------|
| 水温 | **DS18B20** (OneWire) → 既定はダミー | `pin::DS18B20=DUMMY` で未接続のままダミー波形 |
| 気温・気圧 | **可変ソース** (`air::SOURCE`) | 下記の通り。既定 DIE=C3ダイ温度（外部センサ不要で実測） |
| ライト | **WS2812 / SK6812 RGBW** (アドレサブル, 1 データ線) | GPIO2。全画素を時刻ごとの全体色で塗る |
| ファン | **4 線 PWM** → 既定はダミー | `pin::FAN_PWM=DUMMY`。rpm は duty から推定（tach 無し） |
| ヒーター | **リレー/SSR** (GPIO10) | ラッチ式ヒステリシス。極性 `HEATER_ACTIVE_HIGH` |
| OLED | **SSD1306 72x40** (SuperMini ビルトイン, I2C 0x3C) | GPIO5/6。物理128x64 の (27,23) 窓に描画 |
| 時刻 | **NTP** (ntp.nict.jp / JST) | STA 接続時に同期。AP/standalone は同期不可→起動経過を表示 |

### ダミー運用 (0xFF)

`pin::DUMMY (=0xFF)` を任意のピンに設定すると、その系統は**ハードウェアに触れず**シミュレート
動作します（`config.h` の `isDummy()`）。センサ/ファン未接続でも全制御ループ・UI・API を
そのまま起動・確認できます。既定は「**LED(GPIO2)・OLED(5/6) 実機、気温=C3ダイ温度(実測)、水温/ファンはダミー**」＝
現在のベンチ構成です。ダミー水温は 24.5–27.5°C を 10 分周期で往復し、ファン閾値とヒーター目標を
跨ぐので、点灯色・ファン duty・ヒーター ON/OFF の連動が観察できます。接続したら各ピンを実 GPIO に
戻すだけです。

### ピン配置（ESP32-C3 / `config.h`）

既定（現ベンチ）: LED ストリップのみ実機。`DUMMY(0xFF)` は未接続=ダミー。

| 信号 | GPIO | | 信号 | GPIO |
|------|------|---|------|------|
| WS2812 RGBW DATA | **2** | | HEATER (リレー) | 10 |
| FAN PWM | DUMMY | | DS18B20 水温 | DUMMY |
| I2C SDA/SCL (OLED/BME) | **5 / 6** | | LED_COUNT | 8 (★要設定) |

> I2C は OLED で実機化(GPIO5/6)。気温は `air::SOURCE` で選択（既定 DIE）。BME/BMP を使うなら同じバス上で
> 接続したら `0x76`/`0x77` を設定）。FAN/DS18B20 実接続時の推奨は GPIO7 / GPIO1。
> GPIO2 は strapping のため WS2812 DIN のアイドルレベルに留意。

### OLED 表示（72x40）

3 行を表示します。物理 128x64 SSD1306 の中央 72x40 窓（オフセット **27,23**＝BadCodec の実測値）に
描画するため、SuperMini ビルトインモジュールでそのまま映ります。

1. **モード + SSID/IP** … AP/standalone は SSID と IP を 3 秒ごとに交互、STA は IP（`AP`/`SA`/`STA` タグ付き）
2. **時刻** … NTP 同期時は JST 実時刻 `HH:MM:SS`、未同期（AP/standalone でネット無し）は起動からの経過
3. **水温** … `25.6°C`（ダミー時もリアルタイムに変化）

`oled::ENABLE=false` で無効化、`X_OFF/Y_OFF` でパネル個体差を微調整できます。

> ESP32-C3 は RISC-V 単コア・**FPU 非搭載**。float はソフトエミュレーションですが、曲線サンプルは
> 制御周期（≤1Hz の冷経路）でしか走らないため実用上のコストは無視できます（ホットループは無し）。

---

## ビルド & 書き込み

arduino-esp32 **3.x** 前提（新 LEDC API / ArduinoJson 7）。公式 espressif32 はコア 2.x 停滞のため
`pioarduino` プラットフォームを使用。

```bash
# ファーム
pio run -t upload
# UI/設定 (data/ → LittleFS)
pio run -t uploadfs
# シリアル
pio device monitor
```

> **コア 2.x を使う場合**: `leds.cpp`/`fan.cpp` の `ledcAttach`→`ledcSetup`+`ledcAttachPin`、
> ArduinoJson を v6 系へ、`ESPAsyncWebServer` を esphome 版へ差し替え。

---

## アクセス方法 / 「not found」の対処

AP `AquaController` に接続 → ブラウザで **`http://192.168.4.1/`** を開く。

以前 not found が出たのは **LittleFS(UI) 未書込み**が原因でした。現在は **完全版ダッシュボードを
ファームに gzip 埋め込み**してあり、**`uploadfs` 不要**で `192.168.4.1` に全画面
（モニタ/履歴/ライト/制御/Wi-Fi/スタンドアローン）が出ます。UI を差し替えたいときだけ
`data/` を編集して `pio run -t uploadfs`（あれば LittleFS 側を優先）。

> 完全版は `data/` を単一ファイル化→gzip→`src/web_ui_gz.h` に埋め込み済み。UI 改修時は
> 同梱の生成手順で再埋め込みします（履歴グラフの Chart.js のみ CDN。AP 単独=オフラインでは
> グラフは描画されません＝他機能は動作。ローカル同梱が必要なら申し付けください）。

### 初回起動と Wi-Fi 設定（提案どおり実装）

- Wi-Fi 資格情報は LittleFS の **`/wifi.json`** に保存。**このファイルが無い＝初回起動**とみなし、
  **AP セットアップ（`AquaController` / 192.168.4.1）**で起動します。STA 接続に成功すると保存し、
  次回からその AP へ自動接続。
- **ファクトリーリセット**: `pin::RESET_BTN`（既定 **GPIO9 = SuperMini の BOOT ボタン**）を
  **5秒以上長押し**すると **`/wifi.json` のみ削除**して再起動＝初回起動状態へ。
  **LED/ファン/温度のプリセット（`/settings.json`）は消えません**。不要なら `RESET_BTN=DUMMY`。

### デバッグ機能（完全版ダッシュボードに統合）

- **ライト画面**: グラフ下に**時間スライダ + ▶再生 / ❚❚一時停止 / 自動**。▶ は **1秒＝1時間**で
  時刻送りし、その時刻の色で**実 LED**を点灯。LED 適用は制御周期を **120ms** に短縮し低レイテンシ化。
- **ファン画面**: **▶運転 / ❚❚停止**トグル。運転中はグラフにカーソルを当てた位置の風量(%)で**実ファン**を運転。
- **NTP 時刻**は上部モードバーに常時表示（同期時は JST、未同期は起動経過）。
- 内部は `POST /api/test` 経由。override は **TTL 4秒で自動解除**、**高温フェイルセーフが優先**。

## フレームワーク選定（Arduino フレームワーク vs ESP-IDF）

結論: **2秒間隔の達成自体は Arduino フレームワークでも十分達成可能**（専用高優先タスク＋
ハードタイマ基準を使う限り）。ただし「未監視で生体を守る装置の堅牢性」を重視するなら
**ESP-IDF の方が望ましい**、というあなたの直感は正しい方向です。理由を分けて整理します。

- **2秒は RTOS 的に非常に緩い締切**（=2,000,000µs）。Arduino-ESP32 は内部が ESP-IDF + FreeRTOS
  なので、`xTaskCreate` で優先度 3 の周期タスク（本実装の `waterTask`）を立てれば、Arduino の
  `loopTask`(優先度1) や AsyncTCP/Web より先に走り、締切を外す要因は ms 単位（WiFi バースト、
  フラッシュ書込みストール、NeoPixel の割込み窓）だけ。いずれも 2秒には遠く及ばない。
- **ESP-IDF が効く所**: ① 水温サンプリングを **gptimer/esp_timer の ISR** あるいは最優先タスクで
  WiFi からも独立に駆動できる、② AsyncWebServer/Arduino `String` 由来の **ヒープ断片化**を避けられ
  （長期連続運転で効く）、③ ウォッチドッグ/ブラウンアウト/パーティション/OTA を明示制御できる。
  締切が µs〜低 ms なら IDF 一択だが、本件(2s)では**主に長期堅牢性**の差。
- **実務的推奨**: まず Arduino で制御ロジックを検証 → 本番常用で長期無人運転するなら、
  (a) 水温 tick をハードタイマ化、(b) `AsyncWebServer`→`esp_http_server`、(c) ホットパスから
  LittleFS 書込みを排除、を施す（または ESP-IDF へ移行）。**制御コア(`curve.*`/`control.h`)は
  Arduino 非依存の純粋 C++ なので IDF へそのまま移植可能**。差し替えが要るのは HAL のみ。

> Arduino IDE でも本コードはビルド可（arduino-esp32 3.x ボードパッケージ）。`platformio.ini` は
> PlatformIO 用で Arduino IDE アプリとは別。質問の本質（フレームワーク選定）は上記のとおり。

## API 契約（ダッシュボード ↔ ファーム）

| メソッド | パス | 内容 |
|----------|------|------|
| GET  | `/api/state` | ライブ値 `{water,waterValid,air,press,led{r,g,b,w},fan{duty,rpm,airflow},heater{on,target},alarm,alarmDir,mode,time}` |
| GET  | `/api/settings` | 設定一式 `{light{r,g,b,w:[{x,y}]},fan{points:[{x,y}]},heater{target,hyst},safe{lo,hi}}` |
| POST | `/api/settings` | 部分更新＋保存。`heater.target` は **35°C ハードキャップ** |
| GET  | `/api/history?tier=f\|m\|h` | 履歴（ストリーム）`{tier,step,base,water[],air[],press[],rpm[],airflow[],fanOn[]}` |
| GET  | `/api/wifi` | `{mode,ssid,ip,rssi}` |
| POST | `/api/wifi` | `{ssid,pass}` で STA 接続（成功で AP オフ） |
| POST | `/api/mode` | `{mode:"ap"\|"standalone"}` |
| POST | `/api/test` | デバッグ override `{led:{min:N\|off}, fan:{duty:N\|off}}` (TTL4s) |

`data/` の UI は **同じ 1 つのファイルで「モック」と「実機」両対応**。`aqua-live.js` が API 到達を
検出すると実機値へ切替、失敗時はブラウザ内シミュレーションにフォールバックします。

---

## 既存ファームへの組み込み

モジュールは名前空間で分離。新規に必要なのは制御コアと API ハンドラだけなので、
既存の sensors/wifi/history/server があるならそこへ次を移植すれば足ります。

- `curve.*` / `control.h` … 純粋ロジック（依存なし。そのまま流用可）
- `leds.*` … `leds::apply(minuteOfDay)`（既存 `leds::setSchedule()` 直後に置換）
- `fan.*` / `heater.*` … `applyForTemp()` / `update()` を制御ループへ
- `store.*` の `toJson/fromJson` … 既存 `/api/settings` ハンドラへ統合（`fromJson` が 35°C キャップ済み）

## ファイル構成

```
AquaController/
├─ platformio.ini / partitions.csv
├─ src/
│  ├─ config.h          ピン・定数（ここを編集）
│  ├─ curve.{h,cpp}     Catmull-Rom（純粋・検証済み）
│  ├─ control.h         ライト/ファン/ヒーターの純粋ロジック
│  ├─ state.{h,cpp}     共有状態＋既定カーブ（モック一致）
│  ├─ leds/fan/heater   LEDC・リレー HAL
│  ├─ sensors.{h,cpp}   DS18B20＋気温(C3ダイ/BME280/BMP280/ダミー)
│  ├─ store.{h,cpp}     LittleFS JSON 永続化
│  ├─ history.{h,cpp}   多階層リング＋JSON ストリーム
│  ├─ net.{h,cpp}       AP/STA/standalone＋NTP
│  ├─ display.{h,cpp}   SSD1306 72x40 OLED (SSID/IP・時刻・水温)
│  ├─ web.{h,cpp}       AsyncWebServer＋REST
│  └─ main.cpp          FreeRTOS タスク（水温最優先2秒／制御1秒）
├─ data/                LittleFS（UI＋API 配線済みダッシュボード）
├─ test/test_core.cpp   ホスト相互検証
└─ scripts/ref.js       JS リファレンス（モック相当）
```

## 検証の再現

```bash
g++ -std=c++17 -O2 test/test_core.cpp src/curve.cpp -o /tmp/tc
/tmp/tc > /tmp/c.csv && node scripts/ref.js > /tmp/j.csv && diff <(...)  # 量子化列は完全一致
```
