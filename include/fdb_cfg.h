// =====================================================================
//  fdb_cfg.h  -  FlashDB 設定 (TSDB / ファイルモード on LittleFS)
//  ※ ファイルモードなので専用パーティション/fal 移植は不要。
//     DB 実体は LittleFS 上のディレクトリにファイルとして保持する。
// =====================================================================
#ifndef _FDB_CFG_H_
#define _FDB_CFG_H_

/* 時系列DB を使用（KVDB は使わない） */
#define FDB_USING_TSDB

/* ファイルモード（POSIX open/read/write。ESP32 の VFS→LittleFS 経由で動作） */
#define FDB_USING_FILE_MODE
#define FDB_USING_FILE_POSIX_MODE

/* 書き込み粒度: ファイルモードは 1 byte */
#define FDB_WRITE_GRAN 1

/* ログ出力（必要時に有効化） */
/* #define FDB_DEBUG_ENABLE */
#define FDB_PRINT(...)   Serial.printf(__VA_ARGS__)

#endif /* _FDB_CFG_H_ */
