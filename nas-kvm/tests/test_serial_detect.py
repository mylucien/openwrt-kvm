#!/usr/bin/env python3
"""nas-kvmd 串口自动探测：只打开 USB VID:PID 在 serial_ids 里的串口，别的设备一个字节都不发。
用法: python3 tests/test_serial_detect.py /path/to/nas-kvmd
用假 sysfs（目录 + 相对符号链接，层级与真机一致）和假 /dev（指向 pty 的符号链接）。
"""
import os, pty, select, subprocess, sys, tempfile, time, tty

BIN = sys.argv[1]
ok = failed = 0
def check(n, c, extra=""):
    global ok, failed
    if c: ok += 1; print("  ok  ", n)
    else: failed += 1; print("  FAIL", n, extra)

TOKEN = "detecttoken123456"
PORT = 18120

def build_sys(root, entries):
    """entries: [(tty名, usb总线名, vid, pid, 种类)]；种类 usbserial = ttyUSB 层级（device 是 usb-serial 端口，
    USB 设备在上两级），acm = ttyACM 层级（device 是接口，USB 设备在上一级）。"""
    for tty_name, bus, vid, pid, kind in entries:
        dev = f"{root}/sys/devices/usb1/{bus}"; os.makedirs(dev, exist_ok=True)
        open(f"{dev}/idVendor", "w").write(vid + "\n"); open(f"{dev}/idProduct", "w").write(pid + "\n")
        iface = f"{dev}/{bus}:1.0"; os.makedirs(iface, exist_ok=True)
        target = iface
        if kind == "usbserial": target = f"{iface}/{tty_name}"; os.makedirs(target, exist_ok=True)
        cd = f"{root}/sys/class/tty/{tty_name}"; os.makedirs(cd, exist_ok=True)
        os.symlink(os.path.relpath(target, cd), f"{cd}/device")

def scenario(name, entries, conf_extra="", serial_arg=None, expect_open=None, wait=3.6):
    """entries 里每个 tty 都对应一个 pty；返回 {tty名: 收到的字节}。"""
    global PORT
    PORT += 1
    root = tempfile.mkdtemp(); os.makedirs(f"{root}/dev")
    build_sys(root, entries)
    masters = {}
    for tty_name, *_ in entries:
        m, s = pty.openpty(); tty.setraw(s); masters[tty_name] = (m, s)
        os.symlink(os.ttyname(s), f"{root}/dev/{tty_name}")
    conf = f"{root}/conf"
    open(conf, "w").write(f"token='{TOKEN}'\n{conf_extra}")
    cmd = [BIN, "--conf", conf, "--port", str(PORT), "--sysfs", f"{root}/sys", "--devdir", f"{root}/dev",
           "--ctl", "/x"]
    if serial_arg: cmd += ["--serial", serial_arg.replace("@ROOT@", root)]
    p = subprocess.Popen(cmd, stderr=subprocess.PIPE, text=True)
    got = {k: b"" for k in masters}
    t0 = time.time()
    while time.time() - t0 < wait:
        r, _, _ = select.select([m for m, _ in masters.values()], [], [], 0.2)
        for k, (m, _) in masters.items():
            if m in r:
                try: got[k] += os.read(m, 256)
                except OSError: pass
    p.terminate()
    try: err = p.communicate(timeout=5)[1]
    except Exception: p.kill(); err = ""
    return got, err

CH340 = ("ttyUSB0", "1-1", "1a86", "7523", "usbserial")
MODEM = ("ttyUSB1", "1-2", "12d1", "1001", "usbserial")
print("自动探测只选 serial_ids 里的设备")
got, err = scenario("ch340", [CH340])
check("CH340G (ttyUSB 层级) 被打开并收到 CH9329 探测帧", got["ttyUSB0"].startswith(b"\x57\xAB"), repr(got["ttyUSB0"][:8]))

got, err = scenario("modem-only", [MODEM])
check("只有 4G 模块时：一个字节都不发", got["ttyUSB1"] == b"", repr(got["ttyUSB1"][:8]))
check("…并在日志里说明没有匹配项、没有碰它", "none matches serial_ids" in err and "not touching" in err, err)

got, err = scenario("both", [MODEM, CH340])
check("4G 模块排在前面也不会被选中：模块无输出", got["ttyUSB1"] == b"", repr(got["ttyUSB1"][:8]))
check("…CH340G 被选中", got["ttyUSB0"].startswith(b"\x57\xAB"))

got, err = scenario("acm", [("ttyACM0", "1-3", "1a86", "5523", "acm")])
check("ttyACM 层级（device 是接口）的 CH341 (1a86:5523) 被识别", got["ttyACM0"].startswith(b"\x57\xAB"), repr(got["ttyACM0"][:8]))

got, err = scenario("acm-other", [("ttyACM1", "1-4", "2c7c", "0125", "acm")])
check("别的 ACM 设备 (2c7c:0125)：不碰", got["ttyACM1"] == b"")

got, err = scenario("upper", [("ttyUSB0", "1-1", "1A86", "7523", "usbserial")])
check("sysfs 里是大写十六进制也能匹配", got["ttyUSB0"].startswith(b"\x57\xAB"))

got, err = scenario("custom-ids", [("ttyACM1", "1-4", "2c7c", "0125", "acm"), CH340], conf_extra="serial_ids='2c7c:0125'\n")
check("serial_ids 可配置：自定义 ID 的设备被选中", got["ttyACM1"].startswith(b"\x57\xAB"))
check("…默认的 CH340G 因不在列表里而不被碰", got["ttyUSB0"] == b"")

got, err = scenario("bad-ids", [CH340], conf_extra="serial_ids='whatever'\n")
check("serial_ids 格式非法：日志告警并回退默认值", "invalid value for serial_ids" in err and got["ttyUSB0"].startswith(b"\x57\xAB"), err)

got, err = scenario("empty-ids", [CH340], conf_extra="serial_ids=''\n")
check("serial_ids 为空：同样回退默认值", got["ttyUSB0"].startswith(b"\x57\xAB"))

print("手动指定 serial 时不做探测")
got, err = scenario("explicit", [MODEM], serial_arg="@ROOT@/dev/ttyUSB1")
check("用户明确指定的设备：不检查 VID:PID，照常打开", got["ttyUSB1"].startswith(b"\x57\xAB"))

print("sysfs 读不到时")
root = tempfile.mkdtemp(); os.makedirs(f"{root}/dev"); os.makedirs(f"{root}/sys/class/tty")
m, s = pty.openpty(); tty.setraw(s); os.symlink(os.ttyname(s), f"{root}/dev/ttyUSB0")
open(f"{root}/conf", "w").write(f"token='{TOKEN}'\n")
PORT += 1
p = subprocess.Popen([BIN, "--conf", f"{root}/conf", "--port", str(PORT), "--sysfs", f"{root}/sys", "--devdir", f"{root}/dev",
                      "--ctl", "/x"], stderr=subprocess.PIPE, text=True)
data = b""; t0 = time.time()
while time.time() - t0 < 3.6:
    if select.select([m], [], [], 0.2)[0]: data += os.read(m, 256)
p.terminate(); err = p.communicate(timeout=5)[1]
check("查不到 USB ID 的串口（无法确认是不是 CH340）：宁可不碰", data == b"", repr(data[:8]))
check("…并提示改用 serial 手动指定", "set option serial" in err, err)

print(f"\n{ok} passed, {failed} failed")
sys.exit(1 if failed else 0)
