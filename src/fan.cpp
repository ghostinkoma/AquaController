// =====================================================================
//  fan.cpp  -  ファン PWM 実装
//  タコ実測 rpm があれば PID で目標 rpm (rpmFromDuty(指令duty)) へ追従。
//  tach 未接続 (dummy) の場合は従来通り duty 直書き + rpm 推定のみ。
// =====================================================================
#include "fan.h"
#include "fan_tach.h"
#include "control.h"
#include <Arduino.h>
#include <math.h>

namespace fan {

static bool s_dummy = false;      // PWM 自体が dummy
static bool s_tachDummy = false;  // タコ (rpm 実測) が dummy

// PID 状態 (fan.cpp 内で完結)
static float s_integral = 0.0f;
static float s_effectiveDuty = 0.0f;   // 実際に PWM へ書いている duty (PID 補正後)
static float s_lastCommanded = -1000.0f;  // 直近の指令 duty (急変検出用。初回は必ず reset させる番人値)
static uint32_t s_lastPidMs = 0;
static uint32_t s_graceUntilMs = 0;    // この時刻までは fault 判定/PID補正を保留 (助走)

// fault 判定用の連続逸脱タイマー
static uint32_t s_errSinceMs = 0;
static bool s_errActive = false;

// タコ実測 rpm の最新値を保持 (120ms 制御ループのうち実測は 1s に1回のため、
// 保持しないと大半のサイクルで推定値へ戻り rpm 表示がちらつく)。
static int  s_measRpm   = 0;      // 生の実測 (PID フィードバック用。遅れ無し)
static bool s_measValid = false;
static float s_dispRpm  = 0.0f;   // 表示・ログ用の EMA 平滑値 (ソフト LPF)
static bool  s_dispInit = false;

static void resetPid(uint32_t now, float commandedDuty) {
  s_integral = 0.0f;
  s_effectiveDuty = commandedDuty;   // 初回サンプルまでは指令 duty をそのまま適用 (0 固着を防ぐ)
  s_graceUntilMs = now + ctrl::FAN_RPM_STARTUP_MS;
  s_errSinceMs = 0;
  s_errActive = false;
}

// 指令 duty (温度カーブ由来) を受け取り、PID 補正込みで PWM 出力・g_live 更新。
static void writeDuty(float commandedDuty) {
  uint32_t now = millis();

  if (fabsf(commandedDuty - s_lastCommanded) > 5.0f) {
    // 指令が大きく変わった (温度カーブ切替/オーバーライド) → PID をリセットし助走猶予
    resetPid(now, commandedDuty);
  }
  s_lastCommanded = commandedDuty;

  int targetRpm = rpmFromDuty(commandedDuty);

  if (!s_tachDummy && (now - s_lastPidMs) >= timing::FAN_TACH_SAMPLE_MS) {
    s_lastPidMs = now;
    bool valid;
    int rpm = fan_tach::sampleRpm(&valid);
    if (valid) {
      s_measRpm = rpm;             // 最新実測を保持 (PID は遅れ無しのこの生値を使う)
      s_measValid = true;
      // 表示・ログ用の一次 IIR ローパス (EMA)。制御には使わない = ループに遅れを入れない。
      if (!s_dispInit) { s_dispRpm = (float)rpm; s_dispInit = true; }
      else             { s_dispRpm += ctrl::FAN_TACH_LPF_ALPHA * ((float)rpm - s_dispRpm); }

      if (commandedDuty < 1.0f || targetRpm <= 0) {
        // 停止指令中は PID/fault 評価をスキップ
        s_integral = 0.0f;
        s_errActive = false;
      } else {
        float error = (float)(targetRpm - s_measRpm);
        s_integral += error * (timing::FAN_TACH_SAMPLE_MS / 1000.0f);
        // 積分ワインドアップ対策: I 項を ±I_LIMIT[%duty] にクランプ
        //  (タコ喪失や到達不能目標でも duty が 100% へ暴走しないように)
        float iTerm = ctrl::FAN_PID_KI * s_integral;
        if      (iTerm >  ctrl::FAN_PID_I_LIMIT) { iTerm =  ctrl::FAN_PID_I_LIMIT; s_integral =  ctrl::FAN_PID_I_LIMIT / ctrl::FAN_PID_KI; }
        else if (iTerm < -ctrl::FAN_PID_I_LIMIT) { iTerm = -ctrl::FAN_PID_I_LIMIT; s_integral = -ctrl::FAN_PID_I_LIMIT / ctrl::FAN_PID_KI; }
        float out = ctrl::FAN_PID_KP * error + iTerm;
        // ON 時は制御可能域 [FAN_MIN_DUTY_ON, 100] にクランプ (12%未満は制御不能)。
        s_effectiveDuty = curve::clampf(commandedDuty + out, ctrl::FAN_MIN_DUTY_ON, 100.0f);

        // 異常判定は「回転不足」方向のみ (measured が target を 15% 超下回る)。
        //  実機は最低速 ≈3000rpm 未満へ落とせず、低い目標では過回転になるが、これは
        //  ファン故障ではなく指令が下限割れなだけなので警告しない。詰まり/停止で
        //  回らない = 冷却失敗 の危険側 (回転不足) のみを検知する。助走猶予中は評価しない。
        bool underSpeed = (error > 0) && (error / (float)targetRpm * 100.0f > ctrl::FAN_RPM_ERROR_PCT);
        if ((int32_t)(now - s_graceUntilMs) >= 0 && underSpeed) {
          if (!s_errActive) { s_errActive = true; s_errSinceMs = now; }
        } else {
          s_errActive = false;
        }
      }
    }
  }

  bool fault = s_errActive && (now - s_errSinceMs) >= ctrl::FAN_RPM_FAULT_MS;

  float outDuty = (s_tachDummy || commandedDuty < 1.0f) ? commandedDuty : s_effectiveDuty;
  // 制御不能域(12%未満)の回避: 実質 OFF 指令(<1%)は完全停止、それ以外の ON 指令は 12% 下限。
  if (commandedDuty < 1.0f)                 outDuty = 0.0f;
  else if (outDuty < ctrl::FAN_MIN_DUTY_ON) outDuty = ctrl::FAN_MIN_DUTY_ON;
  if (!s_dummy) ledcWrite(pin::FAN_PWM, dutyToByte(outDuty));

  // 報告 rpm: タコ実測の EMA 平滑値 (表示・ログ用)。未取得(起動直後/dummy)は推定値。
  int reportRpm = s_measValid ? (int)(s_dispRpm + 0.5f) : rpmFromDuty(outDuty);
  state_lock();
  g_live.fanDuty    = outDuty;
  g_live.fanRpm     = reportRpm;
  g_live.airflow    = airflowFromRpm(reportRpm);
  g_live.fanRpmFault = fault;
  state_unlock();
}

void begin() {
  s_dummy = pin::isDummy(pin::FAN_PWM);
  s_tachDummy = pin::isDummy(pin::FAN_TACH);
  if (s_dummy) return;
  ledcAttach(pin::FAN_PWM, pwm::FAN_FREQ_HZ, pwm::FAN_RES_BITS);
  ledcWrite(pin::FAN_PWM, 0);
}

void applyForTemp(float water) {
  curve::Point P[curve::MAX_POINTS]; int n;
  state_lock();
  n = g_set.fan.n;
  for (int i = 0; i < n; i++) P[i] = g_set.fan.p[i];
  state_unlock();
  float duty = dutyFromTemp(P, n, water);
  writeDuty(duty);
}

void setDuty(float dutyPct) {
  writeDuty(curve::clampf(dutyPct, 0.0f, 100.0f));
}

}  // namespace fan
