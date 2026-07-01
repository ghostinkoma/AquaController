// =====================================================================
//  net.h / net.cpp  -  Wi-Fi 管理 (wifi.ini 基準) + NTP
//  wifi.ini 無し(初回) → AP セットアップ。有り → mode に従い AP or STA で起動。
//  STA 接続時は入力 SSID の AP を作らない(クリーンな AquaController AP のみ)。
//  接続確立後は猶予をおいて AP を切断し STA 単独 (両ポート同時開放を回避)。
// =====================================================================
#pragma once
#include "state.h"
#include <Arduino.h>

namespace net {

enum ConnState { CS_IDLE = 0, CS_CONNECTING = 1, CS_OK = 2, CS_FAIL = 3 };

void begin();                                   // wifi.ini に従い AP/STA、無ければ AP セットアップ
void loop();                                    // 接続要求/AP切断/再接続/再起動処理 (ノンブロッキング)

// --- セットアップ/ダッシュボードからの操作 (すべて loop で実行) ---
void requestConnect(const String& ssid, const String& pass); // STA 接続要求 (mode=sta 保存)
void requestStandaloneAP();                     // AP スタンドアローン確定 (mode=ap 保存)
// 保存+再起動。ap_pw は "" (開放) か 8文字以上必須 (WPA2 制約) — 違反時は false を返し保存しない。
bool applyConfig(const String& mode, const String& apSsid, const String& apPw,
                 const String& staSsid, const String& staPw, const String& mdns);
void eraseCreds();                              // wifi.ini 削除 (初回状態へ。プリセットは保持)

// 低レベル (begin から使用。ブロッキング: 起動シーケンス専用)
bool connectSTA(const String& ssid, const String& pass, bool keepAp = true);
void startAP();

// --- 状態取得 ---
NetMode  mode();
bool     configured();                          // wifi.ini があるか (=セットアップ済み)
ConnState connState();
String   ip();
String   ssid();                                // 現在の接続/AP SSID
String   apSsid();
String   apPw();
String   staSsid();
String   staPw();
String   mdnsName();
String   modeStr();                             // "ap" / "sta"
int      rssi();
uint32_t epoch();
bool     timeValid();

}  // namespace net
