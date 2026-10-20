#!/usr/bin/env python3
"""前端冒烟测试：真实 kvmd + pty 串口 + jsdom 跑 www/kvm/index.html。"""
import json, os, pty, subprocess, sys, tempfile, threading, time, tty
BIN, WWW, NODE_PATH = sys.argv[1], sys.argv[2], sys.argv[3]
PORT, TOKEN = 18090, "uitesttoken123456"
m, s = pty.openpty(); tty.setraw(s); name = os.ttyname(s)
frames = []; os.set_blocking(m, False); buf = bytearray(); stop = False
def reader():
    while not stop:
        try: d = os.read(m, 256)
        except BlockingIOError: time.sleep(0.005); continue
        buf.extend(d)
        while True:
            i = buf.find(b"\x57\xAB")
            if i < 0 or len(buf) < i + 5: break
            n = 6 + buf[i + 4]
            if len(buf) < i + n: break
            f = bytes(buf[i:i + n]); del buf[:i + n]; frames.append(f)
            if f[3] == 1:
                r = bytes([0x57, 0xAB, 0, 0x81, 8, 0x30, 1, 0, 0, 0, 0, 0, 0]); os.write(m, r + bytes([sum(r) & 255]))
threading.Thread(target=reader, daemon=True).start()
tmp = tempfile.mkdtemp()
conf = f"{tmp}/conf"; open(conf, "w").write(f"token='{TOKEN}'\n")
p = subprocess.Popen([BIN, "--conf", conf, "--port", str(PORT), "--serial", name, "--www", WWW, "--ctl", "/x"], stderr=subprocess.DEVNULL)
time.sleep(0.6)
env = dict(os.environ, PORT=str(PORT), TOKEN=TOKEN, JSDOM_PATH=NODE_PATH)
r = subprocess.run(["node", os.path.join(os.path.dirname(__file__), "ui_smoke.js")], env=env, capture_output=True, text=True, timeout=60)
p.terminate(); p.wait(); stop = True
if r.returncode: print(r.stdout, r.stderr); sys.exit(1)
out = json.loads(r.stdout.strip().splitlines()[-1])
ok = failed = 0
def check(n, c, extra=""):
    global ok, failed
    if c: ok += 1; print("  ok  ", n)
    else: failed += 1; print("  FAIL", n, extra)
check("WebSocket connected", out["wsChip"] == "已连接", out["wsChip"])
check("serial chip from status push", "正常" in out["serialChip"], out["serialChip"])
check("USB chip from status push", "已枚举" in out["usbChip"], out["usbChip"])
check("login hidden with valid token", out["loginVisible"] is False)
check("token taken from URL hash then cleared", out["hashCleared"])
check("video src points at ustreamer port", out["imgSrc"] and ":18081/stream" in out["imgSrc"], str(out["imgSrc"]))
check("macro form filled from server config", out["cfgFilled"] == ["F11", "Del", "10", "100"], str(out["cfgFilled"]))
kb = [f for f in frames if f[3] == 2]; ms = [f for f in frames if f[3] == 4]
check("keyboard: KeyA press frame", any(f[7] == 0x04 and f[5] == 0 for f in kb))
check("keyboard: Ctrl+C modifier frame", any(f[5] == 0x01 and f[7] == 0x06 for f in kb))
check("keyboard: Ctrl+Alt+Del frame", any(f[5] == 0x05 and f[7] == 0x4C for f in kb))
check("mouse corner → (4095,4095)", any(f[7:11] == bytes([0xFF, 0x0F, 0xFF, 0x0F]) for f in ms) and out["mpos"].endswith("4095, 4095"), out["mpos"])
check("mouse left button down/up", any(f[6] == 1 for f in ms) and any(f[6] == 0 for f in ms[-3:]))
check("no power controls left in the page", out["noPower"], str(out["noPower"]))
check("macro buttons enabled when idle", out["macroBtns"] == [False, False], str(out["macroBtns"]))
check("saved macro key F12 used by test button", any(f[7] == 0x45 for f in kb), str([f[7] for f in kb][-6:]))
check("blur/release sends all-up frame", kb and kb[-1][5:13] == bytes(8))
check("bad token → login screen", out["badTokenLogin"] is True)
check("no JS errors in page", not out["errors"], str(out["errors"]))
print(f"\n{ok} passed, {failed} failed"); sys.exit(1 if failed else 0)
