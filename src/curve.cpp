// =====================================================================
//  curve.cpp  -  Catmull-Rom サンプラ実装 (純粋 / ホスト・デバイス共通)
//  内部 double 演算 → モック(JS, double) と数値一致。呼出は低頻度のため
//  ESP32-C3 のソフトfloatコストは無視できる。
// =====================================================================
#include "curve.h"

namespace curve {

double sample(const Point* pts, int n, float x) {
  if (n <= 0) return 0.0;
  if (x <= pts[0].x)      return pts[0].y;
  if (x >= pts[n - 1].x)  return pts[n - 1].y;

  int i = 0;
  while (i < n - 1 && !(x >= pts[i].x && x <= pts[i + 1].x)) i++;

  const Point& p1 = pts[i];
  const Point& p2 = pts[i + 1];
  const Point& p0 = (i - 1 >= 0) ? pts[i - 1] : p1;   // 端点は複製 (||p1)
  const Point& p3 = (i + 2 <  n) ? pts[i + 2] : p2;   // 端点は複製 (||p2)

  double span = (double)p2.x - (double)p1.x;
  if (span == 0.0) span = 1e-6;
  const double t  = ((double)x - (double)p1.x) / span;
  const double t2 = t * t;
  const double t3 = t2 * t;

  const double y = 0.5 * ((2.0 * p1.y)
                  + (-(double)p0.y + p2.y) * t
                  + (2.0 * p0.y - 5.0 * p1.y + 4.0 * p2.y - p3.y) * t2
                  + (-(double)p0.y + 3.0 * p1.y - 3.0 * p2.y + p3.y) * t3);
  return y;
}

}  // namespace curve
