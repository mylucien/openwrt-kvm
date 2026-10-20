#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CH340G + CH9329 键鼠模拟线 —— Windows 硬件验证脚本（对应设计文档 M2 阶段）

假设接法：
  CH340G 的 USB 口  -> 本 Windows（脚本通过它发 CH9329 串口帧）
  CH9329 的 USB 口  -> 本 Windows 的另一个 USB 口（Windows 同时充当「NAS」，接收键鼠）
  CH340G 的 TX -> CH9329 的 RX，RX -> TX，GND 共地

测试项：
  1. 串口枚举：只认 VID:PID 为 1a86:7523 / 1a86:5523 的口（与 kvmd 的 serial_ids 一致）
  2. 波特率探测（发「获取芯片信息」帧，找出有应答的波特率）
  3. 链路延迟 / 丢包、芯片版本、USB 枚举位
  4. 键盘：a、Shift+A、F2/F11/F12/Esc/Del/Tab（开机宏要用的键）、按键保持时间扫描、全部释放、CapsLock LED 回读
  5. 绝对鼠标：5 点定位精度、左/中/右键、滚轮
  6. 按 kvmd 的自适应间隔连发鼠标帧，统计应答率

用法：
  pip install pyserial
  python kvm_hw_test.py                  # 自动找 CH340G，自动探测波特率
  python kvm_hw_test.py --port COM5 --baud 9600
  python kvm_hw_test.py --no-local       # CH9329 的 USB 端不在本机：只测串口链路
  python kvm_hw_test.py --selftest       # 不接硬件，只自检帧编码

运行结束会生成 kvm_test_report.txt，把它发给我即可。
注意：测试期间会移动真实鼠标指针、向测试窗口注入按键，请不要同时操作键鼠。
"""
import argparse
import ctypes
import locale
import os
import subprocess
import sys
import time
from collections import Counter

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

# ----------------------------------------------------------------- 报告输出
LOG = []
RESULTS = []


def out(s=""):
    print(s, flush=True)
    LOG.append(s)


def rec(status, name, detail=""):
    RESULTS.append((status, name, detail))
    out("  [%s] %s%s" % (status, name, (" — " + detail) if detail else ""))


def head(title):
    out("")
    out("=" * 64)
    out(title)
    out("=" * 64)


# ----------------------------------------------------------------- CH9329 帧（与 ch9329.c 一致）
def frame(cmd, data=b"", addr=0):
    b = bytes([0x57, 0xAB, addr, cmd, len(data)]) + bytes(data)
    return b + bytes([sum(b) & 0xFF])


def f_info():
    return frame(0x01)


def f_kbd(mod, keys):
    keys = (list(keys) + [0] * 6)[:6]
    return frame(0x02, bytes([mod, 0] + keys))


def f_mouse_abs(btn, x, y, wheel=0):
    return frame(0x04, bytes([0x02, btn, x & 0xFF, x >> 8, y & 0xFF, y >> 8, wheel & 0xFF]))


class Parser:
    """与 ch_feed 相同的逐字节解析。"""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, b):
        p = self.buf
        if len(p) == 0 and b != 0x57:
            return None
        if len(p) == 1 and b != 0xAB:
            p.clear()
            if b == 0x57:
                p.append(b)
            return None
        p.append(b)
        if len(p) < 5:
            return None
        if p[4] > 64:
            p.clear()
            return None
        total = 6 + p[4]
        if len(p) < total:
            return None
        ok = (sum(p[:total - 1]) & 0xFF) == p[total - 1]
        cmd, data = p[3], bytes(p[5:5 + p[4]])
        p.clear()
        return (cmd, data) if ok else None


# 命令应答状态码（按 CH9329 数据手册整理，以手册为准）
STATUS = {0x00: "成功", 0xE1: "串口接收超时", 0xE2: "帧头错误", 0xE3: "命令码错误",
          0xE4: "校验和错误", 0xE5: "参数错误", 0xE6: "操作失败"}

# HID usage（设计文档 5.5）与 Tk keysym
KEYS = {
    "a": (0x04, "a"), "F2": (0x3B, "F2"), "F11": (0x44, "F11"), "F12": (0x45, "F12"),
    "Esc": (0x29, "Escape"), "Del": (0x4C, "Delete"), "Tab": (0x2B, "Tab"),
}
BAUDS = [9600, 115200, 57600, 38400, 19200, 4800, 2400, 1200]
KNOWN_IDS = {(0x1A86, 0x7523), (0x1A86, 0x5523)}


def selftest():
    ok = True
    cases = [
        ("按下 a", f_kbd(0, [0x04]), "57AB00020800000400000000" + "0010"),
        ("全部释放", f_kbd(0, []), "57AB000208000000000000000" + "00C"),
        ("获取芯片信息", f_info(), "57AB00010003"),
    ]
    for name, got, exp in cases:
        exp = exp.replace(" ", "")
        if got.hex().upper() != exp:
            out("  [FAIL] 自检 %s: %s != %s" % (name, got.hex().upper(), exp))
            ok = False
    p = Parser()
    reply = frame(0x81, bytes([0x30, 0x01, 0x00, 0, 0, 0, 0, 0]))
    r = None
    for b in b"\x00\x57" + reply:
        r = p.feed(b) or r
    if not r or r[0] != 0x81 or r[1][0] != 0x30 or r[1][1] != 1:
        out("  [FAIL] 自检 应答解析")
        ok = False
    return ok


# ----------------------------------------------------------------- 串口设备封装
class Dev:
    def __init__(self, ser):
        self.ser = ser
        self.parser = Parser()
        self.codes = Counter()

    def write(self, data):
        self.ser.write(data)
        self.ser.flush()

    def drain(self):
        got = []
        n = self.ser.in_waiting
        if n:
            for b in self.ser.read(n):
                r = self.parser.feed(b)
                if r:
                    got.append(r)
                    if r[0] != 0x81 and r[1]:
                        self.codes[(r[0], r[1][0])] += 1
        return got

    def xfer(self, data, timeout=0.3):
        self.ser.reset_input_buffer()
        self.parser = Parser()
        t0 = time.perf_counter()
        self.write(data)
        got = []
        while time.perf_counter() - t0 < timeout:
            got += self.drain()
            if got:
                break
            time.sleep(0.001)
        return got, (time.perf_counter() - t0) * 1000.0


def spin(sec):
    end = time.perf_counter() + sec
    while time.perf_counter() < end:
        pass


# ----------------------------------------------------------------- 步骤 1：串口与 Windows 设备枚举
def pnp_scan():
    if os.name != "nt":
        return None
    ps = ("Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_1A86' } | "
          "ForEach-Object { '{0}|{1}|{2}|{3}' -f $_.Class,$_.Status,$_.FriendlyName,$_.InstanceId }")
    try:
        p = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, timeout=25)
    except Exception:
        return None
    txt = p.stdout.decode(locale.getpreferredencoding(False), errors="replace")
    rows = []
    for line in txt.splitlines():
        parts = line.strip().split("|", 3)
        if len(parts) == 4:
            rows.append(parts)
    return rows



def pnp_ports():
    """列出 Windows 的「端口」类设备：名称、厂商、驱动服务、实例 ID（不向任何串口发数据）。"""
    if os.name != "nt":
        return None
    ps = ("Get-CimInstance Win32_PnPEntity -Filter \"PNPClass='Ports'\" | ForEach-Object "
          "{ '{0}|{1}|{2}|{3}' -f $_.Name,$_.Manufacturer,$_.Service,$_.PNPDeviceID }")
    try:
        p = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, timeout=25)
    except Exception:
        return None
    txt = p.stdout.decode(locale.getpreferredencoding(False), errors="replace")
    return [l.strip().split("|", 3) for l in txt.splitlines() if l.count("|") == 3]


def port_desc(p):
    vid = "%04x" % p.vid if p.vid is not None else "----"
    pid = "%04x" % p.pid if p.pid is not None else "----"
    return "%s  %s:%s  厂商=%s  产品=%s  序列号=%s\n             hwid=%s" % (
        p.device, vid, pid, p.manufacturer, p.product, p.serial_number, p.hwid)


def identify(args):
    """拔插差分：找出「拔掉后消失、插回后出现」的串口，不向任何串口发数据。"""
    from serial.tools import list_ports

    def snap():
        return {p.device: p for p in list_ports.comports()}

    out("")
    out("  --- 拔插识别 CH340（只比较串口列表，不发送任何数据）---")
    try:
        a = snap()
        input("  1) 请把 CH340 适配器的 USB 从电脑拔掉，然后按回车…")
        time.sleep(1.0)
        b = snap()
        input("  2) 请把它插回【同一个】USB 口，等 3 秒后按回车…")
        time.sleep(1.5)
        c = snap()
    except (EOFError, KeyboardInterrupt):
        rec("SKIP", "拔插识别", "没有交互输入")
        return None
    changed = {}
    for k in a:
        if k not in b:
            changed[k] = a[k]
    for k in c:
        if k not in b:
            changed[k] = c[k]
    if not changed:
        rec("FAIL", "拔插前后串口列表没有变化",
            "Windows 没把这个适配器当成串口：换 USB 口/线；设备管理器里看它是否在「通用串行总线设备」或「其他设备」下；"
            "你用来连路由器 TTL 的适配器，和现在插在 CH9329 上的是不是同一个？")
        return None
    for dev, p in changed.items():
        out("    变化的串口：" + port_desc(p))
    if len(changed) > 1:
        rec("WARN", "有多个串口发生变化 %s" % list(changed), "请用 --port 指定")
        return None
    dev, p = list(changed.items())[0]
    svc = ""
    for name, mfg, service, iid in (pnp_ports() or []):
        if "(%s)" % dev in name:
            svc = service
            out("    Windows 驱动服务：%s  厂商：%s  实例：%s" % (service, mfg, iid))
    vp = "%04x:%04x" % (p.vid or 0, p.pid or 0)
    rec("PASS", "识别出适配器串口 %s（USB ID %s）" % (dev, vp),
        "不在默认 serial_ids 里，后续按手动指定处理")
    if (svc or "").lower() == "usbser" or (p.vid, p.pid) == (0x0843, 0x5740):
        out("    ⚠ 这个口走的是 Windows 自带 CDC 驱动（usbser），芯片不是 CH340：")
        out("      光猫上对应驱动是 kmod-usb-acm（设备名 /dev/ttyACM0），不是 ch341；")
        out("      设计文档里的 ch341 依赖和 serial_ids 默认值都要相应调整。")
    else:
        out("    光猫配置建议：UCI 里把 serial_ids 加上 %s，或直接设置 serial（手动指定不检查 USB ID）。" % vp)
    return dev


def step_ports(args):
    head("1. 设备枚举")
    from serial.tools import list_ports
    ports = list(list_ports.comports())
    cands = []
    out("  串口列表：")
    if not ports:
        out("    （没有任何串口）")
    for p in ports:
        vid = "%04x" % p.vid if p.vid is not None else "----"
        pid = "%04x" % p.pid if p.pid is not None else "----"
        m = p.vid is not None and (p.vid, p.pid) in KNOWN_IDS
        out("    %-8s %s:%s  %s  matched=%s" % (p.device, vid, pid, p.description, "yes" if m else "no"))
        out("             厂商=%s 产品=%s hwid=%s" % (p.manufacturer, p.product, p.hwid))
        if m:
            cands.append(p.device)

    prow = pnp_ports()
    if prow is not None:
        out("  Windows「端口」类设备（名称 | 厂商 | 驱动服务）：")
        for name, mfg, service, iid in prow:
            out("    %s | %s | %s  [%s]" % (name, mfg, service, iid))
    rows = pnp_scan()
    hid_found = False
    if rows is not None:
        out("  Windows 里 VID_1A86 的设备：")
        if not rows:
            out("    （无）")
        for cls, st, name, iid in rows:
            out("    %-10s %-8s %s  [%s]" % (cls, st, name, iid))
            if cls in ("Keyboard", "Mouse", "HIDClass") or "PID_E129" in iid.upper():
                hid_found = True
        if hid_found:
            rec("PASS", "CH9329 的 HID 端已被 Windows 识别")
        else:
            rec("WARN", "没有在 VID_1A86 设备里看到 HID 键盘/鼠标",
                "商家可能改过 VID/PID；以后面键鼠实测为准")

    port = args.port
    if not port and args.identify:
        port = identify(args)
        if port:
            return port
    if port:
        rec("PASS", "使用手动指定的串口 %s" % port, "不检查 USB ID（与 kvmd 手动指定 serial 的行为一致）")
    elif len(cands) == 1:
        port = cands[0]
        rec("PASS", "找到 CH340G：%s" % port, "USB ID 在 serial_ids 中，设计文档 §9 可回填")
    elif len(cands) > 1:
        rec("FAIL", "匹配到多个串口 %s" % cands, "请用 --port 指定其中一个")
    else:
        out("    没有 USB ID 在 serial_ids 里的串口，ID 可能与默认值不同")
        if not args.yes and sys.stdin.isatty():
            port = identify(args)
        else:
            rec("FAIL", "没有找到 CH340 串口",
                "运行 python kvm_hw_test.py --identify 拔插识别，或用 --port COMx / --ids vid:pid 指定")
    return port


# ----------------------------------------------------------------- 步骤 2/3：波特率、链路、芯片信息
def open_port(port, baud):
    import serial
    return serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=0,
                         rtscts=False, dsrdtr=False, xonxoff=False)


def step_link(args, port):
    head("2. 波特率探测与链路")
    found = None
    ser = None
    for baud in ([args.baud] if args.baud else BAUDS):
        try:
            ser = open_port(port, baud)
        except Exception as e:
            rec("FAIL", "打开 %s 失败" % port, "%s（被别的程序占用？）" % e)
            return None, None, None
        time.sleep(0.1)
        dev = Dev(ser)
        got, ms = dev.xfer(f_info(), 0.4)
        info = [g for g in got if g[0] == 0x81]
        out("    %6d bps: %s" % (baud, ("有应答 %.1f ms" % ms) if info else "无应答"))
        if info:
            found = (baud, info[0][1], dev)
            break
        ser.close()
        ser = None
    if not found:
        rec("FAIL", "所有波特率都没有应答",
            "按顺序检查：1) CH340G.TX→CH9329.RX、CH340G.RX→CH9329.TX 是否接反/接错；2) 两板是否共地；"
            "3) CH9329 是否上电（USB 端已插）；4) 电平：CH340G 模块跳线应为 3.3V 或 5V，与 CH9329 一致；"
            "5) CH9329 若被商家改成了非协议模式（透传模式），则不响应 0x57 0xAB 帧")
        return None, None, None
    baud, data, dev = found
    ver = data[0] if data else 0
    usb = (data[1] != 0) if len(data) > 1 else False
    led = data[2] if len(data) > 2 else 0
    rec("PASS", "CH9329 应答：波特率 %d" % baud,
        "（9600 为出厂默认；设计文档 §9「成品线波特率」填 %d）" % baud)
    out("    应答数据 %s  版本=0x%02X  USB枚举=%s  LED=0x%02X" % (data.hex(" "), ver, usb, led))
    if baud == 9600:
        out("    提示：9600 下鼠标上限约 33Hz；CH9329 可用配置工具改到 115200（设计文档 5.3）")

    # 延迟 / 丢包
    lat, miss = [], 0
    for _ in range(20):
        got, ms = dev.xfer(f_info(), 0.3)
        if any(g[0] == 0x81 for g in got):
            lat.append(ms)
        else:
            miss += 1
        time.sleep(0.05)
    if lat:
        detail = "20 次探测，丢 %d；延迟 min/avg/max = %.1f/%.1f/%.1f ms" % (
            miss, min(lat), sum(lat) / len(lat), max(lat))
        rec("PASS" if miss == 0 else "WARN", "链路稳定性", detail)
    else:
        rec("FAIL", "链路稳定性", "后续探测全部无应答")
        return None, None, None

    if usb:
        rec("PASS", "USB 枚举位 = 1", "CH9329 的 USB 端已被一台主机枚举（kvmd 据此判断 NAS 是否在线）")
    else:
        rec("FAIL", "USB 枚举位 = 0", "CH9329 的 USB 口没被任何主机识别：确认它插在 Windows 的另一个 USB 口、"
            "不是插在 CH340G 同一侧；若 HID 端本来就接别的机器，加 --no-local")
    return dev, baud, usb


# ----------------------------------------------------------------- 本机接收端（tkinter）
class Capture:
    def __init__(self):
        import tkinter as tk
        self.tk = tk
        self.root = tk.Tk()
        self.root.title("KVM 硬件测试 — 请勿操作键鼠")
        self.root.geometry("420x220+140+140")
        self.root.attributes("-topmost", True)
        self.canvas = tk.Canvas(self.root, bg="#1e2a3a", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.text = self.canvas.create_text(210, 110, fill="white", width=380, font=("Microsoft YaHei", 11),
                                            text="点击此窗口任意位置开始")
        self.ev = []
        r = self.root
        r.bind_all("<KeyPress>", lambda e: self.ev.append(("kd", e.keysym)))
        r.bind_all("<KeyRelease>", lambda e: self.ev.append(("ku", e.keysym)))
        # Tk 对 Tab 有内置焦点切换绑定，会盖过通用 <KeyPress>，所以显式绑定并拦截
        for seq in ("<Key-Tab>", "<Shift-Key-Tab>"):
            r.bind_all(seq, lambda e: (self.ev.append(("kd", e.keysym)), "break")[1])
        for n in (1, 2, 3):
            r.bind_all("<ButtonPress-%d>" % n, lambda e, n=n: self.ev.append(("bd", n)))
            r.bind_all("<ButtonRelease-%d>" % n, lambda e, n=n: self.ev.append(("bu", n)))
        r.bind_all("<MouseWheel>", lambda e: self.ev.append(("wh", e.delta)))
        self.root.update()
        self.root.focus_force()

    def say(self, s):
        self.canvas.itemconfig(self.text, text=s)
        self.root.update()

    def pump(self, sec):
        end = time.perf_counter() + sec
        while time.perf_counter() < end:
            self.root.update()
            time.sleep(0.002)

    def clear(self):
        self.ev.clear()

    def has(self, kind, val):
        return any(k == kind and v == val for k, v in self.ev)

    def center(self):
        self.root.update()
        return (self.root.winfo_rootx() + self.root.winfo_width() // 2,
                self.root.winfo_rooty() + self.root.winfo_height() // 2)

    def wait_click(self, timeout=60):
        self.clear()
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            if any(k == "bd" for k, _ in self.ev):
                return True
        return False


def dpi_aware():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


# ----------------------------------------------------------------- 步骤 4：键盘
VK = {"a": 0x41, "F2": 0x71, "F11": 0x7A, "F12": 0x7B, "Esc": 0x1B, "Del": 0x2E, "Tab": 0x09}


def tap(dev, cap, usage, hold_ms, mod=0, settle=0.25, vk=None):
    """按下→保持→释放。vk 不为空时，保持期间用 GetAsyncKeyState 轮询，返回是否在系统层看到按下。"""
    cap.clear()
    dev.write(f_kbd(mod, [usage]))
    polled = False
    end = time.perf_counter() + hold_ms / 1000.0
    while time.perf_counter() < end:
        if vk and ctypes.windll.user32.GetAsyncKeyState(vk) & 0x8000:
            polled = True
    dev.write(f_kbd(0, []))
    cap.pump(settle)
    dev.drain()
    return polled


def step_keys(dev, cap, args):
    head("3. 键盘（CH9329 → Windows）")
    cap.say("键盘测试中…\n请不要点击别处")
    cap.root.focus_force()
    cap.pump(0.3)

    # 基本键
    tap(dev, cap, 0x04, 40)
    if not (cap.has("kd", "a") and cap.has("ku", "a")):
        rec("WARN", "未收到 a 键", "测试窗口可能没有焦点；请点一下测试窗口，3 秒后重试")
        cap.pump(3)
        cap.root.focus_force()
        tap(dev, cap, 0x04, 40)
    if cap.has("kd", "a") and cap.has("ku", "a"):
        rec("PASS", "按下/释放 a（0x04）", "Windows 收到 KeyPress+KeyRelease")
    else:
        rec("FAIL", "按下/释放 a", "收到事件：%s；CH9329 应答了命令但键盘没到 Windows：检查是否被识别为键盘设备" % cap.ev)
        return

    # Shift+A
    cap.clear()
    dev.write(f_kbd(0x02, [0x04]))
    cap.pump(0.15)
    dev.write(f_kbd(0, []))
    cap.pump(0.2)
    dev.drain()
    if cap.has("kd", "Shift_L") and cap.has("kd", "A"):
        rec("PASS", "修饰键 Shift+A", "修饰字节 0x02 生效")
    else:
        rec("FAIL", "修饰键 Shift+A", "收到：%s" % cap.ev)

    # 开机宏会用到的键：Tk 事件 + 系统层 GetAsyncKeyState 双重判定
    bad, notes = [], []
    for name, (usage, sym) in KEYS.items():
        if name == "a":
            continue
        polled = tap(dev, cap, usage, 80, vk=VK[name])
        tk_ok = cap.has("kd", sym) and cap.has("ku", sym)
        if tk_ok:
            continue
        if polled and cap.has("ku", sym):
            notes.append("%s 的按下事件 Tk 没捕获，但系统层已看到按下（Tk 的事件处理问题，非硬件）" % name)
        else:
            bad.append("%s(0x%02X) Tk事件=%s 系统层按下=%s" % (name, usage, cap.ev, polled))
    if not bad:
        rec("PASS", "宏用键 F2 F11 F12 Esc Del Tab", "usage 码与设计文档 5.5 一致" + ("；" + "；".join(notes) if notes else ""))
    else:
        rec("FAIL", "宏用键映射", "；".join(bad))

    # 保持时间扫描
    res = []
    for hold in (5, 10, 20, 30, 50):
        okc = 0
        for _ in range(3):
            tap(dev, cap, 0x44, hold, settle=0.12)
            if cap.has("kd", "F11") and cap.has("ku", "F11"):
                okc += 1
        res.append((hold, okc))
    out("    保持时间扫描（F11，每档 3 次）：" + "  ".join("%dms:%d/3" % r for r in res))
    ok30 = [r for r in res if r[0] == 30][0][1] == 3
    rec("PASS" if ok30 else "WARN", "key_hold_ms=30 的可靠性",
        "Windows 的 USB 轮询比 BIOS 快，BIOS 阶段若漏键再调大" if ok30 else "30ms 在 Windows 下都不稳，建议先用 50ms 以上")

    # 全部释放（防卡键）
    cap.clear()
    dev.write(f_kbd(0, [0x04]))
    cap.pump(0.15)
    dev.write(f_kbd(0, []))
    cap.pump(0.25)
    dev.drain()
    if cap.has("ku", "a"):
        rec("PASS", "全部释放帧可解除按键", "kvmd 断线补发释放帧的机制成立")
    else:
        rec("FAIL", "全部释放帧", "按下 a 后发全 0 帧，Windows 没有收到 KeyRelease")

    # CapsLock LED 回读
    def led():
        got, _ = dev.xfer(f_info(), 0.3)
        i = [g for g in got if g[0] == 0x81]
        return (i[0][1][2] if i and len(i[0][1]) > 2 else None)

    before = led()
    tap(dev, cap, 0x39, 40, settle=0.3)
    mid = led()
    tap(dev, cap, 0x39, 40, settle=0.3)
    after = led()
    if before is None or mid is None:
        rec("WARN", "CapsLock LED 回读", "info 应答里没有 LED 字节")
    elif (before ^ mid) & 0x02 and after == before:
        rec("PASS", "CapsLock LED 回读 %02X→%02X→%02X" % (before, mid, after),
            "CH9329 → 主机 的反向通道正常（可在页面显示锁定键状态）")
    else:
        rec("WARN", "CapsLock LED 回读 %s→%s→%s" % (before, mid, after),
            "LED 位未按预期翻转；不影响键鼠控制")


# ----------------------------------------------------------------- 步骤 5：鼠标
class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def cur():
    p = POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(p))
    return p.x, p.y


def step_mouse(dev, cap, baud):
    head("4. 绝对鼠标（0..4095）")
    u = ctypes.windll.user32
    W, H = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    orig = cur()
    out("    主屏分辨率 %dx%d（DPI 感知后的物理像素），当前指针 %s" % (W, H, orig))
    if u.GetSystemMetrics(80) > 1:
        out("    注意：检测到多显示器，绝对坐标映射到哪块屏取决于主机，误差可能偏大")

    def wait(s):
        (cap.pump if cap else time.sleep)(s)

    pts = [(2048, 2048), (1024, 1024), (3072, 1024), (1024, 3072), (3072, 3072), (0, 0), (4095, 4095)]
    moved, bad, rows = 0, [], []
    last = None
    for x, y in pts:
        dev.write(f_mouse_abs(0, x, y))
        wait(0.2)
        dev.drain()
        px, py = cur()
        ex, ey = x * (W - 1) / 4095.0, y * (H - 1) / 4095.0
        tol_x, tol_y = max(6, 0.012 * W), max(6, 0.012 * H)
        okp = abs(px - ex) <= tol_x and abs(py - ey) <= tol_y
        if (px, py) != last:
            moved += 1
        last = (px, py)
        rows.append("(%d,%d)->期望(%.0f,%.0f) 实测(%d,%d) %s" % (x, y, ex, ey, px, py, "OK" if okp else "偏差"))
        if not okp:
            bad.append((x, y))
    for r in rows:
        out("    " + r)
    if moved <= 1:
        rec("FAIL", "绝对鼠标定位", "指针基本没动：CH9329 可能没配置为支持绝对鼠标的工作模式，"
            "或 Windows 把它当成了无鼠标设备；向卖家确认工作模式/用配置工具核对")
    elif not bad:
        rec("PASS", "绝对鼠标定位（7 点）", "坐标换算与前端一致，误差在 1.2%% 屏幕以内")
    else:
        rec("WARN", "绝对鼠标定位", "有 %d 个点偏差超限，把上面 7 行贴给我分析（可能是多屏或缩放）" % len(bad))

    # 边角专项：定位 (0,0) 不动的原因
    probe = [(0, 0), (1, 1), (8, 8), (0, 2048), (2048, 0), (4095, 0), (0, 4095), (32, 32), (0, 0)]
    out("    边角专项（每点先回中心，再移动）：")
    pres = {}
    for x, y in probe:
        dev.write(f_mouse_abs(0, 2048, 2048))
        wait(0.2)
        dev.write(f_mouse_abs(0, x, y))
        wait(0.25)
        dev.drain()
        px, py = cur()
        ex, ey = x * (W - 1) / 4095.0, y * (H - 1) / 4095.0
        okp = abs(px - ex) <= max(6, 0.012 * W) and abs(py - ey) <= max(6, 0.012 * H)
        pres.setdefault((x, y), []).append(okp)
        out("      (%4d,%4d)->期望(%.0f,%.0f) 实测(%d,%d) %s" % (x, y, ex, ey, px, py, "OK" if okp else "未到位"))
    if all(all(v) for v in pres.values()):
        rec("PASS", "边角坐标", "含 (0,0) 均到位：前面 (0,0) 失败可能与上一个点的状态有关，不稳定复现")
    elif not any(pres[(0, 0)]) and pres[(1, 1)][0]:
        rec("WARN", "绝对坐标 (0,0) 被忽略，(1,1) 正常",
            "疑似全零坐标报告不生效；前端把绝对坐标下限夹到 1 即可规避")
    else:
        rec("WARN", "边角坐标有未到位点", "见上面逐点结果，贴给我分析")

    # 按键与滚轮：需要接收窗口
    if cap:
        cx, cy = cap.center()
        ax, ay = int(round(cx * 4095.0 / (W - 1))), int(round(cy * 4095.0 / (H - 1)))
        dev.write(f_mouse_abs(0, ax, ay))
        cap.pump(0.25)
        cap.clear()
        res = []
        for name, hidbit, tkbtn in (("左键", 1, 1), ("中键", 4, 2), ("右键", 2, 3)):
            cap.clear()
            dev.write(f_mouse_abs(hidbit, ax, ay))
            cap.pump(0.12)
            dev.write(f_mouse_abs(0, ax, ay))
            cap.pump(0.2)
            dev.drain()
            res.append((name, cap.has("bd", tkbtn), cap.has("bu", tkbtn)))
        for name, d, u_ in res:
            out("    %s: 按下=%s 释放=%s" % (name, d, u_))
        if all(d and u_ for _, d, u_ in res):
            rec("PASS", "鼠标按键 左/中/右", "位掩码 1/4/2 与前端约定一致")
        else:
            rec("FAIL", "鼠标按键", "有按键没有收到；见上面三行")

        cap.clear()
        dev.write(f_mouse_abs(0, ax, ay, 1))
        cap.pump(0.25)
        dev.write(f_mouse_abs(0, ax, ay, 0))
        cap.pump(0.1)
        dev.drain()
        whs = [v for k, v in cap.ev if k == "wh"]
        if not whs:
            rec("FAIL", "滚轮", "wheel=+1 没有产生滚动事件")
        elif whs[0] > 0:
            rec("PASS", "滚轮", "wheel=+1 → 向上滚（delta=%d），符号与前端约定一致" % whs[0])
        else:
            rec("WARN", "滚轮方向相反", "wheel=+1 → delta=%d；前端滚轮符号需取反" % whs[0])
    else:
        rec("SKIP", "鼠标按键与滚轮", "没有 tkinter 接收窗口")

    # 按 kvmd 自适应间隔连发
    frame_len = 13
    interval = max(0.016, 2.2 * frame_len * 10.0 / baud)
    out("    kvmd 自适应间隔：%.1f ms（约 %.0f Hz）" % (interval * 1000, 1 / interval))
    import math
    dev.ser.reset_input_buffer()
    dev.parser = Parser()
    n, replies = 90, 0
    ex = cur()
    t0 = time.perf_counter()
    for i in range(n):
        a = 2 * math.pi * i / 30.0
        dev.write(f_mouse_abs(0, int(2048 + 600 * math.cos(a)), int(2048 + 600 * math.sin(a))))
        end = t0 + (i + 1) * interval
        while time.perf_counter() < end:
            replies += len([g for g in dev.drain() if g[0] == 0x84])
            if cap:
                cap.root.update()
    el = time.perf_counter() - t0
    time.sleep(0.3)
    replies += len([g for g in dev.drain() if g[0] == 0x84])
    rate = n / el
    if replies >= n:
        rec("PASS", "连发 %d 帧鼠标（实际 %.0f Hz）" % (n, rate), "应答 %d/%d，无丢帧" % (replies, n))
    elif replies >= n * 0.95:
        rec("WARN", "连发鼠标应答 %d/%d" % (replies, n), "少量丢失；可降低鼠标频率或提高波特率")
    else:
        rec("FAIL", "连发鼠标应答 %d/%d" % (replies, n), "串口吞吐不足或线路质量差，检查接线长度与波特率")

    dev.write(f_mouse_abs(0, 2048, 2048))
    u.SetCursorPos(*orig)


# ----------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser(description="CH340G + CH9329 键鼠线 Windows 验证脚本")
    ap.add_argument("--port", help="手动指定串口，如 COM5（不检查 USB ID）")
    ap.add_argument("--baud", type=int, help="只测这个波特率，默认依次探测")
    ap.add_argument("--identify", action="store_true", help="拔插差分识别适配器串口（不发送数据）")
    ap.add_argument("--ids", help="追加可接受的 USB ID，如 0843:5740,1a86:7522")
    ap.add_argument("--no-local", action="store_true", help="CH9329 的 USB 端不在本机：只测串口链路")
    ap.add_argument("--no-keys", action="store_true", help="跳过键盘测试")
    ap.add_argument("--no-mouse", action="store_true", help="跳过鼠标测试（不会移动指针）")
    ap.add_argument("--yes", action="store_true", help="不等待点击确认，直接开始")
    ap.add_argument("--selftest", action="store_true", help="只做帧编码自检，不接硬件")
    ap.add_argument("--report", default="kvm_test_report.txt")
    args = ap.parse_args()

    for t in (args.ids or "").split(","):
        if ":" in t:
            try:
                v, d = t.strip().split(":")
                KNOWN_IDS.add((int(v, 16), int(d, 16)))
            except ValueError:
                pass
    out("CH340G + CH9329 硬件验证  %s  Python %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), sys.version.split()[0]))
    head("0. 帧编码自检（与设计文档 5.3 示例逐字节对照）")
    if selftest():
        rec("PASS", "帧编码/解析自检")
    else:
        rec("FAIL", "帧编码自检", "脚本自身与设计文档不一致，停止")
        return finish(args, 1)
    if args.selftest:
        return finish(args, 0)

    try:
        import serial  # noqa: F401
    except ImportError:
        out("缺少 pyserial，请先执行：pip install pyserial")
        return 2

    port = step_ports(args)
    if not port:
        return finish(args, 1)
    dev, baud, usb = step_link(args, port)
    if not dev:
        return finish(args, 1)

    cap = None
    try:
        local = (not args.no_local) and os.name == "nt"
        if not args.no_local and os.name != "nt":
            rec("SKIP", "本机键鼠验证", "仅支持 Windows")
        if local and not usb:
            rec("SKIP", "本机键鼠验证", "USB 枚举位为 0，没有可接收的主机")
            local = False
        if local:
            dpi_aware()
            try:
                cap = Capture()
            except Exception as e:
                rec("WARN", "无法创建 tkinter 窗口", "%s；键盘与按键测试将跳过" % e)
            if cap and not args.yes:
                out("")
                out("  >>> 请点击弹出的「KVM 硬件测试」窗口任意位置以开始（60 秒内）<<<")
                if not cap.wait_click(60):
                    rec("WARN", "没有点击窗口", "按无焦点继续，键盘可能收不到")
                else:
                    cap.pump(0.3)
            if cap and not args.no_keys:
                step_keys(dev, cap, args)
            elif not cap:
                rec("SKIP", "键盘测试", "没有接收窗口")
            if not args.no_mouse:
                step_mouse(dev, cap, baud)
    except KeyboardInterrupt:
        out("\n用户中断")
    finally:
        try:
            dev.write(f_kbd(0, []))
            dev.write(f_mouse_abs(0, 2048, 2048))
            dev.ser.close()
        except Exception:
            pass
        if cap:
            try:
                cap.root.destroy()
            except Exception:
                pass

    if dev.codes:
        out("")
        out("  命令应答状态码统计（0x00 为成功）：")
        for (cmd, code), c in sorted(dev.codes.items()):
            out("    cmd=0x%02X status=0x%02X(%s) × %d" % (cmd, code, STATUS.get(code, "未知"), c))
    out("")
    out("  可回填设计文档 §9：串口 %s；波特率 %s；USB 枚举 %s" % (port, baud, usb))
    return finish(args, 0)


def finish(args, code):
    head("汇总")
    cnt = Counter(s for s, _, _ in RESULTS)
    out("  PASS=%d  WARN=%d  FAIL=%d  SKIP=%d" % (cnt["PASS"], cnt["WARN"], cnt["FAIL"], cnt["SKIP"]))
    for s, n, d in RESULTS:
        if s in ("FAIL", "WARN"):
            out("  [%s] %s%s" % (s, n, (" — " + d) if d else ""))
    try:
        with open(args.report, "w", encoding="utf-8-sig") as f:
            f.write("\n".join(LOG) + "\n")
        out("\n报告已保存：%s" % os.path.abspath(args.report))
    except Exception as e:
        out("报告保存失败：%s" % e)
    return 1 if cnt["FAIL"] else code


if __name__ == "__main__":
    sys.exit(main())
