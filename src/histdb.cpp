// =====================================================================
//  histdb.cpp  -  履歴の永続 TSDB 実装
//  既定: 自作の追記型 TSDB (堅牢性重視・レコード指向) on LittleFS。
//  HISTDB_USE_FLASHDB 定義時のみ FlashDB 実装 (レジストリ未収録のため既定無効)。
// =====================================================================
#include "histdb.h"
#if defined(ARDUINO) && defined(HISTDB_USE_FLASHDB)
#include <Arduino.h>
#include <LittleFS.h>
#include <flashdb.h>

namespace histdb {

static struct fdb_tsdb s_tsdb;
static bool s_ready = false;
static uint32_t s_curEpoch = 0;      // append 時に get_time が返す値

static fdb_time_t get_time(void) { return (fdb_time_t)s_curEpoch; }

bool ready() { return s_ready; }

bool begin() {
  s_ready = false;
  if (!LittleFS.exists("/histdb")) LittleFS.mkdir("/histdb");   // DB ディレクトリ

  uint32_t sec_size = 4096;
  uint32_t max_size = 256 * 1024;
  bool     file_mode = true;
  fdb_tsdb_control(&s_tsdb, FDB_TSDB_CTRL_SET_SEC_SIZE, &sec_size);
  fdb_tsdb_control(&s_tsdb, FDB_TSDB_CTRL_SET_FILE_MODE, &file_mode);
  fdb_tsdb_control(&s_tsdb, FDB_TSDB_CTRL_SET_MAX_SIZE, &max_size);

  fdb_err_t err = fdb_tsdb_init(&s_tsdb, "hist", "/littlefs/histdb", get_time, sizeof(HistRec), NULL);
  s_ready = (err == FDB_NO_ERR);
  Serial.printf("[histdb] TSDB init %s (err=%d, rec=%uB)\n",
                s_ready ? "OK" : "FAIL", (int)err, (unsigned)sizeof(HistRec));
  return s_ready;
}

bool append(const HistRec& r) {
  if (!s_ready) return false;
  s_curEpoch = r.epoch;
  struct fdb_blob blob;
  fdb_err_t err = fdb_tsl_append(&s_tsdb, fdb_blob_make(&blob, &r, sizeof(r)));
  return err == FDB_NO_ERR;
}

uint32_t count() {
  if (!s_ready) return 0;
  return (uint32_t)fdb_tsl_query_count(&s_tsdb, 0, (fdb_time_t)0x7FFFFFFF, FDB_TSL_WRITE);
}

struct QCtx { IterCb cb; void* arg; int stride; int i; int out; };
static bool iter_cb(fdb_tsl_t tsl, void* arg) {
  QCtx* q = (QCtx*)arg;
  if ((q->i++ % q->stride) != 0) return false;   // 間引き
  struct fdb_blob blob; HistRec r;
  fdb_blob_read((fdb_db_t)&s_tsdb, fdb_tsl_to_blob(tsl, fdb_blob_make(&blob, &r, sizeof(r))));
  q->cb(r.epoch, r, q->arg); q->out++;
  return false;                                  // continue
}

int query(char /*tier*/, uint32_t from, uint32_t to, int maxPoints, IterCb cb, void* arg) {
  if (!s_ready) return 0;
  size_t n = fdb_tsl_query_count(&s_tsdb, (fdb_time_t)from, (fdb_time_t)to, FDB_TSL_WRITE);
  int stride = 1;
  if (maxPoints > 0 && (int)n > maxPoints) stride = ((int)n + maxPoints - 1) / maxPoints;
  QCtx q = { cb, arg, stride, 0, 0 };
  fdb_tsl_iter_by_time(&s_tsdb, (fdb_time_t)from, (fdb_time_t)to, iter_cb, &q);
  return q.out;
}

}  // namespace histdb

#elif defined(ARDUINO)
// =====================================================================
//  自作 TSDB (追記型 on LittleFS) — 堅牢性重視のレコード指向実装。
//  レコード: HistRec(15B) + CRC16(2B) = 17B。CRC 不一致は破損として除外。
//  解像度3層 (f=30s / m=10分 / h=2時間)。各層 ping-pong 2ファイル:
//  active が満杯なら相手を破棄して切替 → 追記継続 (保持 CAP..2×CAP 件)。
//  追記は毎回 open/write/close で確実にフラッシュ (電源断で末尾1件のみ失う)。
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace histdb {

// 各層の ping-pong ファイル (f は旧単層時代のファイル名を継承しデータを引き継ぐ)
static const char* PATHS[3][2] = {
  { "/histA.tsdb",  "/histB.tsdb"  },   // f: 30s
  { "/histmA.tsdb", "/histmB.tsdb" },   // m: 10分
  { "/histhA.tsdb", "/histhB.tsdb" },   // h: 2時間
};
struct __attribute__((packed)) DiskRec { HistRec r; uint16_t crc; };
static_assert(sizeof(DiskRec) == 17, "DiskRec は 17 バイト");

struct Series { int active; uint32_t count; uint32_t lastEpoch; };
static Series s_ser[3];
static bool s_ready = false;
static SemaphoreHandle_t s_mtx = nullptr;

static uint16_t crc16(const uint8_t* d, size_t n) {   // CRC16-CCITT (0x1021)
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1);
  }
  return c;
}
static inline int tierIdx(char t) { return t == 'm' ? 1 : t == 'h' ? 2 : 0; }

// ファイルを走査し、有効レコード数と最終有効 epoch を返す。
static uint32_t scanFile(const char* path, uint32_t* validCount) {
  uint32_t cnt = 0, last = 0;
  if (!LittleFS.exists(path)) { if (validCount) *validCount = 0; return 0; }
  File f = LittleFS.open(path, "r");
  if (f) {
    DiskRec d;
    while (f.read((uint8_t*)&d, sizeof(d)) == (int)sizeof(d)) {
      if (crc16((const uint8_t*)&d.r, sizeof(d.r)) == d.crc) { cnt++; last = d.r.epoch; }
    }
    f.close();
  }
  if (validCount) *validCount = cnt;
  return last;
}

bool ready() { return s_ready; }

bool begin() {
  s_mtx = xSemaphoreCreateMutex();
  for (int i = 0; i < 3; i++) {
    uint32_t ca = 0, cb = 0;
    uint32_t ea = scanFile(PATHS[i][0], &ca), eb = scanFile(PATHS[i][1], &cb);
    if (eb > ea) { s_ser[i].active = 1; s_ser[i].count = cb; }
    else         { s_ser[i].active = 0; s_ser[i].count = ca; }
    s_ser[i].lastEpoch = (ea > eb) ? ea : eb;
    Serial.printf("[histdb] %c: A(n=%u) B(n=%u) active=%d last=%u\n",
                  "fmh"[i], (unsigned)ca, (unsigned)cb, s_ser[i].active, (unsigned)s_ser[i].lastEpoch);
  }
  s_ready = true;
  return true;
}

// 1レコードを層 i の active ファイルへ書く (満杯なら ping-pong 切替)。
static bool writeRec(int i, const DiskRec& d) {
  Series& se = s_ser[i];
  bool fresh = false;
  if (se.count >= histdb_cfg::TIER_CAP[i]) {          // 満杯 → 相手を破棄して切替
    se.active ^= 1;
    LittleFS.remove(PATHS[i][se.active]);
    se.count = 0; fresh = true;
  } else if (se.count == 0) {
    fresh = !LittleFS.exists(PATHS[i][se.active]);
  }
  File f = LittleFS.open(PATHS[i][se.active], (fresh || se.count == 0) ? "w" : "a");
  bool ok = false;
  if (f) { ok = (f.write((const uint8_t*)&d, sizeof(d)) == sizeof(d)); f.close(); }
  if (ok) se.count++;
  return ok;
}

bool append(const HistRec& r) {
  if (!s_ready) return false;
  bool any = false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  DiskRec d; d.r = r; d.crc = crc16((const uint8_t*)&r, sizeof(r));
  for (int i = 0; i < 3; i++) {                       // 各層: 記録間隔が経過していれば追記
    if (s_ser[i].lastEpoch != 0 && r.epoch - s_ser[i].lastEpoch < histdb_cfg::TIER_PERIOD_S[i]) continue;
    if (writeRec(i, d)) { s_ser[i].lastEpoch = r.epoch; any = true; }
  }
  xSemaphoreGive(s_mtx);
  return any;
}

// 1ファイルを読み、[from,to] の有効レコードを stride 間引きで cb へ。cursor は通し番号。
static void iterFile(const char* path, uint32_t from, uint32_t to, int stride, int& cursor,
                     IterCb cb, void* arg, int& out) {
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;
  DiskRec d;
  while (f.read((uint8_t*)&d, sizeof(d)) == (int)sizeof(d)) {
    if (crc16((const uint8_t*)&d.r, sizeof(d.r)) != d.crc) continue;   // 破損除外
    if (d.r.epoch < from || d.r.epoch > to) continue;
    if ((cursor++ % stride) == 0) { if (cb) cb(d.r.epoch, d.r, arg); out++; }
  }
  f.close();
}

int query(char tier, uint32_t from, uint32_t to, int maxPoints, IterCb cb, void* arg) {
  if (!s_ready) return 0;
  int ti = tierIdx(tier);
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  const char* oldP = PATHS[ti][s_ser[ti].active ^ 1];   // 非 active = 古い側
  const char* actP = PATHS[ti][s_ser[ti].active];
  // 1パス目: 範囲内件数を数え stride 決定 (cb=null で件数のみ)
  int n = 0, cur0 = 0;
  iterFile(oldP, from, to, 1, cur0, nullptr, nullptr, n);
  iterFile(actP, from, to, 1, cur0, nullptr, nullptr, n);
  int stride = 1;
  if (maxPoints > 0 && n > maxPoints) stride = (n + maxPoints - 1) / maxPoints;
  // 2パス目: 出力 (古い→新しい順)
  int out = 0, cursor = 0;
  iterFile(oldP, from, to, stride, cursor, cb, arg, out);
  iterFile(actP, from, to, stride, cursor, cb, arg, out);
  xSemaphoreGive(s_mtx);
  return out;
}

uint32_t count() {
  if (!s_ready) return 0;
  uint32_t a = 0, b = 0; scanFile(PATHS[0][0], &a); scanFile(PATHS[0][1], &b);
  return a + b;
}

}  // namespace histdb
#endif
