# -*- coding: utf-8 -*-
# USB シリアル経由の LittleFS ファイルコマンド クライアント (COM7)
#   python fscmd.py ls
#   python fscmd.py get <name>              (内容を標準出力へ)
#   python fscmd.py put <name> <localfile>  (localfile を <name> へ書き込み)
import sys, time, serial

PORT = "COM7"; BAUD = 115200

def main():
    if len(sys.argv) < 2:
        print("usage: fscmd.py ls | get <name> | put <name> <localfile>"); return
    # dsrdtr/rtscts=False: オープン時に DTR/RTS を叩いて C3 をリセットしないため
    s = serial.Serial(PORT, BAUD, timeout=1, dsrdtr=False, rtscts=False)
    time.sleep(0.3); s.reset_input_buffer()
    cmd = sys.argv[1]

    def send(line): s.write((line + "\n").encode())
    def rline(to=3):
        s.timeout = to
        return s.readline().decode("utf-8", "replace").rstrip("\r\n")

    if cmd == "ls":
        send("ls"); t = time.time(); on = False
        while time.time() - t < 6:
            ln = rline()
            if ln == "<<<LS": on = True; continue
            if ln.startswith(">>>LS"): print(ln); break
            if on and ln: print(ln)

    elif cmd == "get":
        name = sys.argv[2]; send("get " + name)
        t = time.time(); hdr = None
        while time.time() - t < 6:
            ln = rline()
            if ln.startswith("<<<GET "): hdr = ln; break
            if ln.startswith("<<<ERR"): print(ln); s.close(); return
        if not hdr: print("[fscmd] no header"); s.close(); return
        size = int(hdr.split()[2]); s.timeout = 6
        data = s.read(size)
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.write(b"\n[fscmd] got %d/%d bytes\n" % (len(data), size))

    elif cmd == "put":
        name = sys.argv[2]
        with open(sys.argv[3], "rb") as f: content = f.read()
        send("put %s %d" % (name, len(content)))
        t = time.time()
        while time.time() - t < 6:
            ln = rline()
            if ln.startswith("<<<PUT-READY"): break
            if ln.startswith("<<<ERR"): print(ln); s.close(); return
        s.write(content); s.flush()
        t = time.time()
        while time.time() - t < 20:
            ln = rline()
            if ln.startswith("<<<PUT "): print(ln); break
    s.close()

main()
