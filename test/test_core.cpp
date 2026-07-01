// host cross-check: C++ 制御コアの出力を CSV で吐く (JS モックと突合)
#include <cstdio>
#include "../src/control.h"
using curve::Point;

static Point R[] = {{0,0},{360,24},{540,47},{1020,47},{1140,47},{1260,0},{1440,0}};
static Point G[] = {{0,0},{360,16},{540,55},{1020,55},{1140,12},{1260,0},{1440,0}};
static Point B[] = {{0,0},{360,4 },{540,71},{1020,71},{1140,4 },{1260,0},{1440,0}};
static Point W[] = {{0,3},{300,3},{360,8},{540,24},{1020,24},{1140,4},{1260,3},{1440,3}};
static Point F[] = {{24,0},{26,0},{27,40},{28.5f,70},{30,100},{35,100}};
static const int nR=7,nG=7,nB=7,nW=8,nF=6;

int main(){
  // ライト: 全分の raw sampleCurve とバイト値
  for(int m=0;m<=1440;m++){
    printf("L,%d,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%u\n", m,
      curve::sample(R,nR,m), curve::sample(G,nG,m),
      curve::sample(B,nB,m), curve::sample(W,nW,m),
      leds::channelByte(R,nR,m), leds::channelByte(G,nG,m),
      leds::channelByte(B,nB,m), leds::channelByte(W,nW,m));
  }
  // ファン: 22..35 を 0.05 刻みで duty / rpm / airflow
  for(int k=0;k<=260;k++){
    float t = 22.0f + k*0.05f;
    float d = fan::dutyFromTemp(F,nF,t);
    int rpm = fan::rpmFromDuty(d);
    printf("F,%.2f,%.6f,%d,%.4f\n", t, d, rpm, fan::airflowFromRpm(rpm));
  }
  // ヒーター: target=25, hyst=0.3, water 22.0..28.0 を 0.05 刻みで掃引 (ラッチ追跡)
  {
    bool on=false;
    for(int k=0;k<=120;k++){
      float w = 22.0f + k*0.05f;          // 上昇
      on = heater::decide(w, 25.0f, 0.3f, on);
      printf("H,up,%.2f,%d\n", w, on?1:0);
    }
    for(int k=120;k>=0;k--){
      float w = 22.0f + k*0.05f;          // 下降
      on = heater::decide(w, 25.0f, 0.3f, on);
      printf("H,dn,%.2f,%d\n", w, on?1:0);
    }
    // ハードキャップ: target=40 を要求しても 35 に丸める
    printf("CAP,%.2f\n", heater::capTarget(40.0f));
    printf("CAP,%.2f\n", heater::capTarget(10.0f));
  }
  return 0;
}
