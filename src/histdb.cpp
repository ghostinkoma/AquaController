// =====================================================================
//  histdb.cpp  -  FlashDB TSDB(時系列DB) 実装 (ファイルモード on LittleFS)
//  15バイト固定小数点レコード(HistRec)を epoch タイムスタンプで追記・範囲照会する。
// =====================================================================
#include "histdb.h"
// FlashDB ライブラリが導入・設定済みのときのみ本実装を有効化 (build_flags に
// -D HISTDB_USE_FLASHDB を追加)。未導入時はビルドを壊さないようスキップする。
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
  uint32_t max_size = 256 * 1024;      // ~14日@60s (15B/レコード + FlashDB管理領域)
  bool     file_mode = true;
  fdb_tsdb_control(&s_tsdb, FDB_TSDB_CTRL_SET_SEC_SIZE, &sec_size);
  fdb_tsdb_control(&s_tsdb, FDB_TSDB_CTRL_SET_FILE_MODE, &file_mode);
  fdb_tsdb_control(&s_tsdb, FDB_TSDB_CTRL_SET_MAX_SIZE, &max_size);

  // path は VFS 実パス。arduino-esp32 の LittleFS 既定マウントは "/littlefs"。
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

int query(uint32_t from, uint32_t to, int maxPoints, IterCb cb, void* arg) {
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
//  2ファイル ping-pong: active が満杯なら相手を破棄して切替 → 追記継続。
//  追記は毎回 open/write/close で確実にフラッシュ (電源断で末尾1件のみ失う)。
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace histdb {

static const char* PATH[2] = { "/histA.tsdb", "/histB.tsdb" };
struct __attribute__((packed)) DiskRec { HistRec r; uint16_t crc; };
static_assert(sizeof(DiskRec) == 17, "DiskRec は 17 バイト");

static int      s_active = 0;
static uint32_t s_activeCount = 0;
static bool     s_ready = false;
static SemaphoreHandle_t s_mtx = nullptr;

static uint16_t crc16(const uint8_t* d, size_t n) {   // CRC16-CCITT (0x1021)
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1);
  }
  return c;
}
// ファイルを走査し、有効レコード数と最終有効 epoch を返す。
static uint32_t scanFile(int idx, uint32_t* validCount) {
  uint32_t cnt = 0, last = 0;
  if (!LittleFS.exists(PATH[idx])) { if (validCount) *validCount = 0; return 0; }  // 無ければ静かに 0
  File f = LittleFS.open(PATH[idx], "r");
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
  uint32_t ca = 0, cb = 0;
  uint32_t ea = scanFile(0, &ca), eb = scanFile(1, &cb);
  if (eb > ea) { s_active = 1; s_activeCount = cb; }   // 新しい方を active に
  else         { s_active = 0; s_activeCount = ca; }
  s_ready = true;
  Serial.printf("[histdb] TSDB(自作) ready A(n=%u,last=%u) B(n=%u,last=%u) active=%d\n",
                (unsigned)ca, (unsigned)ea, (unsigned)cb, (unsigned)eb, s_active);
  return true;
}

bool append(const HistRec& r) {
  if (!s_ready) return false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  bool fresh = false;
  if (s_activeCount >= histdb_cfg::CAP) {           // 満杯 → 相手を破棄して切替
    s_active ^= 1;
    LittleFS.remove(PATH[s_active]);
    s_activeCount = 0; fresh = true;
  } else if (s_activeCount == 0) {
    fresh = !LittleFS.exists(PATH[s_active]);
  }
  DiskRec d; d.r = r; d.crc = crc16((const uint8_t*)&r, sizeof(r));
  File f = LittleFS.open(PATH[s_active], (fresh || s_activeCount == 0) ? "w" : "a");
  bool ok = false;
  if (f) { ok = (f.write((const uint8_t*)&d, sizeof(d)) == sizeof(d)); f.close(); }
  if (ok) s_activeCount++;
  xSemaphoreGive(s_mtx);
  return ok;
}

// 1ファイルを読み、[from,to] の有効レコードを stride 間引きで cb へ。cursor は通し番号。
static void iterFile(int idx, uint32_t from, uint32_t to, int stride, int& cursor,
                     IterCb cb, void* arg, int& out) {
  if (!LittleFS.exists(PATH[idx])) return;
  File f = LittleFS.open(PATH[idx], "r");
  if (!f) return;
  DiskRec d;
  while (f.read((uint8_t*)&d, sizeof(d)) == (int)sizeof(d)) {
    if (crc16((const uint8_t*)&d.r, sizeof(d.r)) != d.crc) continue;   // 破損除外
    if (d.r.epoch < from || d.r.epoch > to) continue;
    if ((cursor++ % stride) == 0) { cb(d.r.epoch, d.r, arg); out++; }
  }
  f.close();
}

int query(uint32_t from, uint32_t to, int maxPoints, IterCb cb, void* arg) {
  if (!s_ready) return 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  int oldIdx = s_active ^ 1;   // 非 active = 古い側
  // 1パス目: 範囲内件数を数え stride 決定 (no-op cb で out に件数を集計)
  IterCb noop = [](uint32_t, const HistRec&, void*){};
  int n = 0, cur0 = 0;
  iterFile(oldIdx,   from, to, 1, cur0, noop, nullptr, n);
  iterFile(s_active, from, to, 1, cur0, noop, nullptr, n);
  int stride = 1;
  if (maxPoints > 0 && n > maxPoints) stride = (n + maxPoints - 1) / maxPoints;
  // 2パス目: 出力 (古い→新しい順)
  int out = 0, cursor = 0;
  iterFile(oldIdx, from, to, stride, cursor, cb, arg, out);
  iterFile(s_active, from, to, stride, cursor, cb, arg, out);
  xSemaphoreGive(s_mtx);
  return out;
}

uint32_t count() {
  if (!s_ready) return 0;
  uint32_t a = 0, b = 0; scanFile(0, &a); scanFile(1, &b);
  return a + b;
}

}  // namespace histdb
#endif
