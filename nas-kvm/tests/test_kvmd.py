#!/usr/bin/env python3
"""nas-kvmd 端到端测试：用 pty 充当 CH340G/CH9329 串口。
用法: python3 tests/test_kvmd.py /path/to/nas-kvmd
"""
import base64, hashlib, json, os, pty, socket, struct, subprocess, sys, tempfile, threading, time, tty
import urllib.request, urllib.error

BIN = sys.argv[1] if len(sys.argv) > 1 else "./nas-kvmd"
PORT = 18080
TOKEN = "testtoken1234567890"
passed = failed = 0

def check(name, cond, extra=""):
    global passed, failed
    if cond: passed += 1; print(f"  ok   {name}")
    else: failed += 1; print(f"  FAIL {name} {extra}")

def chk(frame):  # CH9329 校验和
    return sum(frame) & 0xFF

def frame(cmd, data=b"", addr=0):
    f = bytes([0x57, 0xAB, addr, cmd, len(data)]) + data
    return f + bytes([chk(f)])

# ---------- 假串口 ----------
master, slave = pty.openpty()
tty.setraw(slave)
slave_name = os.ttyname(slave)
rx = bytearray(); frames = []; lock = threading.Lock()
respond = {"on": True, "usb": 1}
stop = False

def reader():
    global rx
    os.set_blocking(master, False)
    while not stop:
        try: d = os.read(master, 256)
        except BlockingIOError: time.sleep(0.005); continue
        except OSError: break
        with lock:
            rx += d
            while True:
                i = rx.find(b"\x57\xAB")
                if i < 0 or len(rx) < i + 5: break
                n = 6 + rx[i + 4]
                if len(rx) < i + n: break
                f = bytes(rx[i:i + n]); del rx[:i + n]
                ok = chk(f[:-1]) == f[-1]
                frames.append((time.time(), f, ok))
                if f[3] == 0x01 and respond["on"]:
                    os.write(master, frame(0x81, bytes([0x30, respond["usb"], 0, 0, 0, 0, 0, 0])))
threading.Thread(target=reader, daemon=True).start()

def mark():
    with lock: return len(frames)

def wait_frames(since, pred, timeout=2.0):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            got = [f for (_, f, ok) in frames[since:] if pred(f)]
        if got: return got
        time.sleep(0.01)
    return []

def kbd(f): return f[3] == 0x02
def mouse(f): return f[3] == 0x04

# ---------- 启动 ----------
tmp = tempfile.mkdtemp()
www = os.path.join(tmp, "www"); os.makedirs(www)
open(os.path.join(www, "index.html"), "w").write("<h1>kvm</h1>")
conf = os.path.join(tmp, "conf")
open(conf, "w").write(f"""nas-kvm.main=nas-kvm
nas-kvm.main.token='{TOKEN}'
nas-kvm.main.boot_menu_key='F2'
nas-kvm.main.bios_key='Del'
nas-kvm.main.power_led='green:nas-power'
nas-kvm.main.force_ms='3000'
nas-kvm.main.pre_press='1'
nas-kvm.main.macro_duration_s='1'
nas-kvm.main.macro_interval_ms='100'
nas-kvm.main.baud='14400'
""")
proc = subprocess.Popen([BIN, "--conf", conf, "--bind", "127.0.0.1", "--port", str(PORT),
                         "--serial", slave_name,
                         "--www", www, "--ctl", "/nonexistent", "-v"],
                        stderr=subprocess.PIPE, text=True)
errlog = []
threading.Thread(target=lambda: [errlog.append(l) for l in proc.stderr], daemon=True).start()
time.sleep(0.5)

def http(method, path, body=None, token=TOKEN):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", method=method,
                                 data=json.dumps(body).encode() if body is not None else (b"{}" if method == "POST" else None))
    if token: req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=5) as r: return r.status, r.read().decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()

class WS:
    def __init__(self, key="dGhlIHNhbXBsZSBub25jZQ==", token=TOKEN):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
        self.s.sendall((f"GET /ws?token={token} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""; self.hdr = self._read_headers()
    def _read_headers(self):
        while b"\r\n\r\n" not in self.buf:
            d = self.s.recv(4096)
            if not d: break
            self.buf += d
        h, _, self.buf = self.buf.partition(b"\r\n\r\n"); return h.decode(errors="replace")
    def send(self, obj, mask=True):
        p = json.dumps(obj).encode(); hdr = bytes([0x81])
        mk = bytes([1, 2, 3, 4])
        hdr += bytes([0x80 | len(p)]) if len(p) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(p))
        if not mask: hdr = bytes([0x81, len(p)]); self.s.sendall(hdr + p); return
        self.s.sendall(hdr + mk + bytes(b ^ mk[i & 3] for i, b in enumerate(p)))
    def recv_text(self, timeout=3):
        self.s.settimeout(timeout)
        while True:
            while len(self.buf) < 2:
                d = self.s.recv(4096)
                if not d: return None
                self.buf += d
            op = self.buf[0] & 0xF; n = self.buf[1] & 0x7F; hl = 2
            if n == 126: hl = 4; n = struct.unpack(">H", self.buf[2:4])[0]
            while len(self.buf) < hl + n:
                self.buf += self.s.recv(4096)
            pl = self.buf[hl:hl + n]; self.buf = self.buf[hl + n:]
            if op == 1: return json.loads(pl)
            if op == 8: return None
    def close(self): self.s.close()

try:
    print("HTTP / auth / static")
    st, _ = http("GET", "/api/status", token=None); check("status without token → 401", st == 401)
    st, _ = http("GET", "/api/status", token="wrong"); check("status wrong token → 401", st == 401)
    time.sleep(2.2)  # 等一次探测周期
    st, b = http("GET", "/api/status"); j = json.loads(b)
    check("status with token → 200", st == 200)
    check("serial link detected via info reply", j["serial"] is True, b)
    check("usb enumerated flag true", j["usb"] is True, b)
    st, b = http("GET", "/", token=None); check("static index served", st == 200 and "<h1>kvm</h1>" in b)
    st, _ = http("GET", "/../etc/passwd", token=None); check("path traversal rejected", st in (400, 404))
    st, _ = http("GET", "/%2e%2e/etc/passwd", token=None); check("encoded traversal rejected", st in (400, 404))

    print("WebSocket handshake + keyboard")
    ws = WS(); check("101 upgrade", ws.hdr.startswith("HTTP/1.1 101"), ws.hdr)
    check("RFC6455 accept key (SHA-1/Base64)", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in ws.hdr, ws.hdr)
    m = mark()
    ws.send({"t": "key", "down": ["KeyA"]})
    got = wait_frames(m, kbd)
    check("press 'a' frame matches design doc", got and got[0].hex(" ").upper() == "57 AB 00 02 08 00 00 04 00 00 00 00 00 10", got and got[0].hex(" "))
    m = mark(); ws.send({"t": "key", "down": []})
    got = wait_frames(m, kbd)
    check("release-all frame matches design doc", got and got[0].hex(" ").upper() == "57 AB 00 02 08 00 00 00 00 00 00 00 00 0C", got and got[0].hex(" "))
    m = mark(); ws.send({"t": "key", "down": ["ControlLeft", "ShiftRight", "KeyA", "F2"]})
    got = wait_frames(m, kbd)
    check("modifiers + multiple keys", got and got[0][5] == 0x21 and got[0][7:9] == bytes([0x04, 0x3B]), got and got[0].hex(" "))
    m = mark(); ws.send({"t": "key", "down": ["ControlLeft", "ShiftRight", "KeyA", "F2"]})
    check("duplicate state not resent", not wait_frames(m, kbd, 0.3))

    print("Mouse")
    m = mark(); ws.send({"t": "mouse", "btn": 0, "x": 4095, "y": 2048, "wheel": 0})
    got = wait_frames(m, mouse)
    check("abs mouse frame (x=4095 y=2048)", got and got[0][5:12] == bytes([2, 0, 0xFF, 0x0F, 0x00, 0x08, 0]) and got[0][-1] == chk(got[0][:-1]), got and got[0].hex(" "))
    m = mark()
    for i in range(30): ws.send({"t": "mouse", "btn": 0, "x": 100 + i, "y": 100, "wheel": 0}); time.sleep(0.002)
    time.sleep(0.4); n = len(wait_frames(m, mouse, 0.1))
    check("mouse motion coalesced (<30 frames for 30 events)", 0 < n < 15, f"n={n}")
    m = mark(); ws.send({"t": "mouse", "btn": 1, "x": 10, "y": 10, "wheel": 0})
    got = wait_frames(m, lambda f: mouse(f) and f[6] == 1); check("button press sent immediately", bool(got))
    m = mark(); ws.send({"t": "mouse", "btn": 0, "x": 10, "y": 10, "wheel": 0})
    got = wait_frames(m, lambda f: mouse(f) and f[6] == 0); check("button release sent immediately", bool(got))

    print("WS close / heartbeat → release")
    ws.send({"t": "key", "down": ["KeyB"]}); time.sleep(0.1)
    m = mark(); ws.close()
    got = wait_frames(m, lambda f: kbd(f) and f[5:13] == bytes(8)); check("socket close releases all keys", bool(got))
    ws2 = WS(); ws2.send({"t": "key", "down": ["KeyC"]}); time.sleep(0.1)
    m = mark()
    got = wait_frames(m, lambda f: kbd(f) and f[5:13] == bytes(8), 8.0)
    check("heartbeat timeout (no ping 6s) releases keys", bool(got))
    ws2.close()
    ws3 = WS(); ws3.send({"t": "ping"}); r = ws3.recv_text()
    while r and r.get("t") == "status": r = ws3.recv_text()
    check("ping → pong", r == {"t": "pong"}, r)
    ws3.close()
    wsb = WS(); wsb.send({"t": "key", "down": ["KeyA"]}, mask=False)
    time.sleep(0.2); wsb.s.settimeout(1)
    try: closed = wsb.s.recv(100) == b"" or True
    except Exception: closed = False
    check("unmasked client frame rejected (no crash)", proc.poll() is None)
    wsb.close()
    w4 = WS(token="bad"); check("ws with bad token → 401", "401" in w4.hdr.split("\r\n")[0], w4.hdr); w4.close()

    print("Power control removed")
    st, _ = http("POST", "/api/power/press"); check("power API gone → 404", st == 404)
    st, b = http("GET", "/api/status"); j = json.loads(b)
    check("status has no power/pre_press fields", "power" not in j and "pre_press" not in j["cfg"] and "press_ms" not in j["cfg"], b)
    check("legacy power options in an old config are ignored (daemon runs)", st == 200 and proc.poll() is None)
    check("no LED sysfs access attempted", not any("LED" in l or "oneshot" in l for l in errlog), "".join(errlog[:5]))

    print("Macro")
    st, _ = http("POST", "/api/macro/test", {"kind": "menu"}); check("macro test accepted", st == 200)
    got = wait_frames(mark() - 3 if mark() > 3 else 0, lambda f: kbd(f) and f[7] == 0x3B, 1.0)
    check("test key F2 → HID 0x3B pressed", bool(got))
    time.sleep(0.3)
    m = mark(); st, _ = http("POST", "/api/macro/test", {"kind": "bios"})
    got = wait_frames(m, lambda f: kbd(f) and f[7] == 0x4C); check("test key Del → HID 0x4C", bool(got))
    time.sleep(0.3)
    time.sleep(1.1)
    m = mark(); t0 = time.time(); st, _ = http("POST", "/api/macro/run", {"kind": "bios"}); check("macro run accepted", st == 200)
    time.sleep(2.4)
    with lock: seq = [f for (_, f, ok) in frames[m:] if kbd(f)]
    presses = [f for f in seq if f[7] == 0x4C]
    check("repeated Del presses over duration (~1s/100ms)", 5 <= len(presses) <= 12, f"{len(presses)}")
    check("macro ends with release-all", seq and seq[-1][5:13] == bytes(8))
    check("every press followed by release", all(seq[i + 1][5:13] == bytes(8) for i, f in enumerate(seq) if f[7] == 0x4C and i + 1 < len(seq)))
    with lock: allok = all(ok for (_, _, ok) in frames)
    check("all frames have valid checksum", allok)
    st, _ = http("POST", "/api/macro/cancel"); check("cancel when idle ok", st == 200)
    time.sleep(1.1)
    st, _ = http("POST", "/api/config", {"macro_duration_s": 5, "boot_menu_key": "F12"}); check("config update accepted", st == 200)
    m = mark(); st, _ = http("POST", "/api/macro/run", {"kind": "menu"}); check("macro run (menu key) accepted", st == 200)
    got = wait_frames(m, lambda f: kbd(f) and f[7] == 0x45); check("new boot_menu_key F12 → 0x45 in use", bool(got))
    st, _ = http("POST", "/api/macro/run", {"kind": "bios"}); check("second macro while running → 409", st == 409)
    m = mark(); st, _ = http("POST", "/api/macro/cancel"); time.sleep(0.2)
    got = wait_frames(m, lambda f: kbd(f) and f[5:13] == bytes(8)); check("cancel sends release-all", bool(got))
    st, _ = http("POST", "/api/config", {"boot_menu_key": "X1"}); check("invalid key name → 400", st == 400)
    st, _ = http("POST", "/api/config", {"key_hold_ms": 99999}); check("out-of-range value → 400", st == 400)
    st, b = http("GET", "/api/status"); check("status reflects new config", json.loads(b)["cfg"]["boot_menu_key"] == "F12")

    print("Config atomicity / baud whitelist")
    check("baud 14400 in config rejected, falls back to 9600", any("invalid value for baud" in l for l in errlog), "".join(errlog[-5:]))
    st, b0 = http("GET", "/api/status"); before = json.loads(b0)["cfg"]
    st, _ = http("POST", "/api/config", {"bios_key": "F5", "macro_duration_s": 7, "key_hold_ms": 99999})
    check("batch with one bad value → 400", st == 400)
    st, b1 = http("GET", "/api/status"); after = json.loads(b1)["cfg"]
    check("rejected batch changed nothing in memory", after == before, f"{before} -> {after}")
    st, _ = http("POST", "/api/config", {"bios_key": "F5", "key_hold_ms": "abc"})
    check("present-but-malformed value → 400 (not silently ignored)", st == 400)
    st, b2 = http("GET", "/api/status"); check("…and still nothing changed", json.loads(b2)["cfg"] == before)
    st, _ = http("POST", "/api/config", {"bios_key": "F5", "macro_duration_s": 7})
    st, b3 = http("GET", "/api/status"); c3 = json.loads(b3)["cfg"]
    check("valid batch applied together", c3["bios_key"] == "F5" and c3["macro_duration_s"] == 7, str(c3))

    print("Serial robustness")
    st, b = http("POST", "/api/serial/test"); j = json.loads(b); check("serial test API ok", j.get("ok") is True and j.get("version") == "0x30", b)
    respond["on"] = False; time.sleep(5.5)
    st, b = http("GET", "/api/status"); check("link reported down when chip silent >5s", json.loads(b)["serial"] is False, b)
    res = {}
    def slow(): t = time.time(); res["r"] = http("POST", "/api/serial/test"); res["dt"] = time.time() - t
    th = threading.Thread(target=slow); th.start(); time.sleep(0.1)
    t0 = time.time(); st, _ = http("GET", "/api/status"); dt = time.time() - t0
    check("status answered while serial test is pending (non-blocking)", st == 200 and dt < 0.2, f"{dt:.3f}s")
    ws5 = WS(); check("websocket accepted while serial test is pending", ws5.hdr.startswith("HTTP/1.1 101")); ws5.close()
    st2, _ = http("POST", "/api/serial/test"); check("second concurrent serial test → 409", st2 == 409)
    th.join()
    check("serial test reports no reply after ~400ms", json.loads(res["r"][1]).get("ok") is False and 0.35 < res["dt"] < 0.9, str(res))
    respond["on"] = True; time.sleep(2.3)
    st, b = http("GET", "/api/status"); check("link recovers when chip answers again", json.loads(b)["serial"] is True, b)

finally:
    proc.terminate()
    try: proc.wait(3)
    except Exception: proc.kill()
    stop = True
print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
