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
#endif
