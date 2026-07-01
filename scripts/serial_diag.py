# -*- coding: utf-8 -*-
# COM7 を開いて C3 を RTS リセットし、シリアル出力を1行ずつ即時表示する診断ツール。
# 使い方: python scripts/serial_diag.py [seconds]
import sys, time, serial

PORT = "COM7"
BAUD = 115200
DUR   = int(sys.argv[1]) if len(sys.argv) > 1 else 40
RESET = (sys.argv[2] != "0") if len(sys.argv) > 2 else True  # 第2引数 0 でリセット無し

s = serial.Serial(PORT, BAUD, timeout=0.2)
if RESET:
    # ESP32-C3 USB-Serial-JTAG: RTS -> EN でハードリセット
    s.setDTR(False)
    s.setRTS(True); time.sleep(0.1)
    s.setRTS(False); time.sleep(0.1)
    print("[diag] reset pulse sent; capturing %ds ..." % DUR, flush=True)
else:
    print("[diag] no-reset capture %ds (scan を実行してください) ..." % DUR, flush=True)

t0 = time.time()
buf = b""
while time.time() - t0 < DUR:
    data = s.read(256)
    if data:
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print(line.decode("utf-8", "replace").rstrip("\r"), flush=True)
if buf:
    print(buf.decode("utf-8", "replace"), flush=True)
s.close()
print("[diag] done", flush=True)
