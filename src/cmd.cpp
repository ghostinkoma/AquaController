// =====================================================================
//  cmd.cpp  -  USB シリアル ファイルコマンド (ls / get / put)
// =====================================================================
#include "cmd.h"
#include <Arduino.h>
#include <LittleFS.h>

namespace cmd {

static String norm(const String& name) { return name.startsWith("/") ? name : "/" + name; }

static void doLs() {
  Serial.println("<<<LS");
  File root = LittleFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      Serial.printf("%u\t%s\n", (unsigned)f.size(), f.name());
      f = root.openNextFile();
    }
  }
  Serial.printf(">>>LS total=%u used=%u\n",
                (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
}

static void doGet(const String& nameIn) {
  String path = norm(nameIn);
  if (!LittleFS.exists(path)) { Serial.printf("<<<ERR no file %s\n", path.c_str()); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.printf("<<<ERR open %s\n", path.c_str()); return; }
  size_t sz = f.size();
  Serial.printf("<<<GET %s %u\n", path.c_str(), (unsigned)sz);
  uint8_t buf[128];
  while (f.available()) { size_t n = f.read(buf, sizeof(buf)); if (n) Serial.write(buf, n); }
  f.close();
  Serial.println();                 // 本体とトレーラを確実に改行分離
  Serial.println(">>>GET");
}

static void doPut(const String& nameIn, size_t sz) {
  String path = norm(nameIn);
  File f = LittleFS.open(path, "w");
  if (!f) { Serial.printf("<<<ERR open %s\n", path.c_str()); return; }
  Serial.printf("<<<PUT-READY %s %u\n", path.c_str(), (unsigned)sz);   // 送信開始の合図
  size_t got = 0; uint8_t buf[128];
  uint32_t t0 = millis();
  while (got < sz && millis() - t0 < 15000) {
    size_t want = sz - got; if (want > sizeof(buf)) want = sizeof(buf);
    size_t n = Serial.readBytes(buf, want);
    if (n) { f.write(buf, n); got += n; t0 = millis(); }
  }
  f.close();
  Serial.printf("<<<PUT %s %u/%u %s\n", path.c_str(),
                (unsigned)got, (unsigned)sz, got == sz ? "OK" : "TIMEOUT");
}

static void task(void*) {
  Serial.setTimeout(2000);
  for (;;) {
    if (Serial.available()) {
      String l = Serial.readStringUntil('\n'); l.trim();
      if (l == "ls") {
        doLs();
      } else if (l.startsWith("get ")) {
        doGet(l.substring(4));
      } else if (l.startsWith("put ")) {
        String rest = l.substring(4); rest.trim();
        int sp = rest.lastIndexOf(' ');
        if (sp > 0) {
          String nm = rest.substring(0, sp);
          size_t sz = (size_t)rest.substring(sp + 1).toInt();
          doPut(nm, sz);
        } else {
          Serial.println("<<<ERR usage: put <name> <size>");
        }
      } else if (l.length()) {
        Serial.printf("<<<ERR unknown: %s\n", l.c_str());
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void begin() { xTaskCreate(task, "cmd", 4096, nullptr, 1, nullptr); }

}  // namespace cmd
