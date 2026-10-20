#!/usr/bin/env python3
"""nas-kvm-ctl 的 shell 逻辑：disable-upstream 作用域、port-check、serial-scan。
用法: python3 tests/test_ctl.py [path/to/nas-kvm-ctl]
uci 用一个文件态的最小模拟代替，/sys 用普通目录代替。需要 /proc/net/tcp（Linux）。
"""
import ctypes, os, socket, subprocess, sys, tempfile, textwrap, threading, time

CTL = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "files/usr/bin/nas-kvm-ctl"))
ok = failed = 0
def check(n, c, extra=""):
    global ok, failed
    if c: ok += 1; print("  ok  ", n)
    else: failed += 1; print("  FAIL", n, extra)

tmp = tempfile.mkdtemp()
os.makedirs(f"{tmp}/bin")
STATE, LOG = f"{tmp}/state", f"{tmp}/log"
open(f"{tmp}/bin/uci", "w").write(textwrap.dedent(f"""\
    #!/bin/sh
    S={STATE}
    [ "$1" = "-q" ] && shift
    case "$1" in
    show) grep "^$2\\." $S | sed "s/=\\(.*\\)/='\\1'/";;
    get) grep "^$2=" $S | head -1 | cut -d= -f2-;;
    set) k="${{2%%=*}}"; v="${{2#*=}}"; grep -v "^$k=" $S > $S.n; echo "$k=$v" >> $S.n; mv $S.n $S;;
    commit) echo "commit $2" >> {LOG};;
    esac
"""))
open(f"{tmp}/upstream_init", "w").write(f"#!/bin/sh\necho \"upstream $1\" >> {LOG}\n")
for f in ("bin/uci", "upstream_init"): os.chmod(f"{tmp}/{f}", 0o755)
ENV = dict(os.environ, PATH=f"{tmp}/bin:" + os.environ["PATH"], UPSTREAM_INIT=f"{tmp}/upstream_init", SYSFS=f"{tmp}/sys")

def ctl(*args, timeout=30):
    return subprocess.run(["sh", CTL, *args], env=ENV, capture_output=True, text=True, timeout=timeout)
def state(lines): open(STATE, "w").write("\n".join(lines) + "\n"); open(LOG, "w").write("")
def enabled(): return {l.split(".")[1]: l.split("=")[1] for l in open(STATE).read().split() if ".enabled=" in l}

print("disable-upstream：只关冲突的 section")
state(["ustreamer.video0.enabled=1", "ustreamer.video0.device=/dev/video0", "ustreamer.video0.port=8080",   # 设备冲突
       "ustreamer.cam2.enabled=1", "ustreamer.cam2.device=/dev/video2", "ustreamer.cam2.port=8090",          # 无关
       "ustreamer.cam3.enabled=1", "ustreamer.cam3.device=/dev/video3", "ustreamer.cam3.port=18081",         # 占视频流端口
       "ustreamer.cam4.enabled=1", "ustreamer.cam4.device=/dev/video4", "ustreamer.cam4.port=18080",         # 占管理页端口
       "ustreamer.cam5.enabled=1", "ustreamer.cam5.port=9000",                                               # device 缺省 → /dev/video0
       "ustreamer.cam6.enabled=0", "ustreamer.cam6.device=/dev/video0",                                      # 本来就关着
       "ustreamer.cam7.enabled=1", "ustreamer.cam7.device=/dev/video7"])                                     # port 缺省 → 8080，与我们的默认端口不冲突
r = ctl("disable-upstream")
e = enabled()
check("设备相同的 section 被关闭", e["video0"] == "0")
check("占视频流端口的 section 被关闭", e["cam3"] == "0")
check("占管理页端口的 section 被关闭", e["cam4"] == "0")
check("device 缺省（等同 /dev/video0）的 section 被关闭", e["cam5"] == "0")
check("无关的摄像头实例保持启用", e["cam2"] == "1", str(e))
check("device 与 port 都不冲突的实例（port 缺省 8080）保持启用", e["cam7"] == "1", str(e))
check("输出里点名被关闭的 section", "ustreamer.video0" in r.stdout and "cam2" not in r.stdout, r.stdout)
log = open(LOG).read()
check("有冲突时 commit 并 restart 上游（不是 stop 全部）", "commit ustreamer" in log and "upstream restart" in log and "upstream stop" not in log, log)
state(open(STATE).read().split())
r = ctl("disable-upstream")
check("第二次运行：没有输出", r.stdout.strip() == "")
check("第二次运行：不 commit、不动上游", open(LOG).read() == "", open(LOG).read())

state(["ustreamer.cam2.enabled=1", "ustreamer.cam2.device=/dev/video2", "ustreamer.cam2.port=8090"])
r = ctl("disable-upstream")
check("只有无关实例时：完全不碰", enabled()["cam2"] == "1" and open(LOG).read() == "")
state([]); open(STATE, "w").write("")
check("没有上游配置时：安静退出", ctl("disable-upstream").returncode == 0)

state(["nas-kvm.main.video_device=/dev/video2", "nas-kvm.main.listen_port=19000", "nas-kvm.main.video_port=19001",
       "ustreamer.video0.enabled=1", "ustreamer.video0.device=/dev/video0", "ustreamer.video0.port=8080",
       "ustreamer.cam2.enabled=1", "ustreamer.cam2.device=/dev/video2", "ustreamer.cam2.port=8090"])
ctl("disable-upstream")
e = enabled()
check("自定义 video_device：按配置的设备判断冲突", e["cam2"] == "0" and e["video0"] == "1", str(e))

print("port-check：端口被占用的处理")
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
state([])
p = free_port()
r = ctl("port-check", str(p), "1", "127.0.0.1")
check("空闲端口：返回 0", r.returncode == 0, r.stdout + r.stderr)

def set_comm(name):
    try: ctypes.CDLL(None).prctl(15, name.encode(), 0, 0, 0)   # PR_SET_NAME
    except Exception: pass
old_comm = open("/proc/self/comm").read().strip()
set_comm("fakeapp")
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("0.0.0.0", 0)); srv.listen(1)
p = srv.getsockname()[1]
t0 = time.time(); r = ctl("port-check", str(p), "1", "192.168.77.1", "127.0.0.1")
check("别的程序在通配地址上监听：返回 1", r.returncode == 1, r.stdout)
check("输出占用者进程名", r.stdout.strip() == "fakeapp", repr(r.stdout))
check("等待时间受 SECS 限制（约 1 秒）", 0.8 < time.time() - t0 < 4, f"{time.time()-t0:.1f}s")
srv.close()

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("127.0.0.1", 0)); srv.listen(1)
p = srv.getsockname()[1]
r = ctl("port-check", str(p), "0", "192.168.77.1", "127.0.0.1")
check("别的程序在 127.0.0.1 上监听：kvmd 要绑回环，所以冲突", r.returncode == 1, r.stdout)
r = ctl("port-check", str(p), "0", "192.168.77.1")
check("同一监听对只绑 LAN IP 的 ustreamer 不冲突", r.returncode == 0, r.stdout)
srv.close()

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("127.0.0.2", 0)); srv.listen(1)
p = srv.getsockname()[1]
r = ctl("port-check", str(p), "0", "127.0.0.1")
check("别的地址上的同端口监听不算冲突", r.returncode == 0, r.stdout)
srv.close()

try:
    srv = socket.socket(socket.AF_INET6); srv.bind(("::", 0)); srv.listen(1); p = srv.getsockname()[1]
    r = ctl("port-check", str(p), "0", "127.0.0.1")
    check("IPv6 通配监听（上游 ustreamer 的 [::]）算冲突", r.returncode == 1, r.stdout)
    srv.close()
except OSError:
    print("  skip IPv6 test (no IPv6)")

# 沙箱可能没有 IPv6：用伪造的 /proc/net/tcp{,6} 验证 IPv6 / 4-in-6 地址的解析
fake = f"{tmp}/procnet"; os.makedirs(fake, exist_ok=True)
HDR = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
def line(addr, port, inode=99999, st="0A"):
    return f"   0: {addr}:{port:04X} 00000000000000000000000000000000:0000 {st} 00000000:00000000 00:00000000 00000000     0        0 {inode} 1 0000000000000000 100 0 0 10 0\n"
def fake_check(v4=(), v6=(), port=18080, *ips):
    open(f"{fake}/tcp", "w").write(HDR + "".join(v4)); open(f"{fake}/tcp6", "w").write(HDR + "".join(v6))
    r = subprocess.run(["sh", CTL, "port-check", str(port), "0", *ips], env=dict(ENV, PROCNET=fake), capture_output=True, text=True)
    return r.returncode
Z32 = "0" * 32
check("tcp6：[::] 通配监听 → 冲突", fake_check((), [line(Z32, 18080)], 18080, "192.168.1.1") == 1)
check("tcp6：4-in-6 的 127.0.0.1 → 对 kvmd 冲突", fake_check((), [line("0000000000000000FFFF00000100007F", 18080)], 18080, "192.168.1.1", "127.0.0.1") == 1)
check("tcp6：4-in-6 的 LAN IP (192.168.1.1) → 冲突", fake_check((), [line("0000000000000000FFFF0000" + "0101A8C0", 18080)], 18080, "192.168.1.1") == 1)
check("tcp6：[::1] 不影响 IPv4 绑定 → 不冲突", fake_check((), [line("00000000000000000000000001000000", 18080)], 18080, "192.168.1.1", "127.0.0.1") == 0)
check("tcp6：端口不同 → 不冲突", fake_check((), [line(Z32, 18999)], 18080, "192.168.1.1") == 0)
check("tcp：非 LISTEN 状态（01=ESTABLISHED）不算占用", fake_check([line("00000000", 18080, st="01")], (), 18080, "192.168.1.1") == 0)
check("tcp：LAN IP 的小端十六进制 (192.168.1.1 → 0101A8C0) 匹配", fake_check([line("0101A8C0", 18080)], (), 18080, "192.168.1.1") == 1)
check("tcp：别的网段 IP 不匹配", fake_check([line("0102A8C0", 18080)], (), 18080, "192.168.1.1") == 0)

set_comm("nas-kvmd")
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("0.0.0.0", 0)); srv.listen(1)
p = srv.getsockname()[1]
t0 = time.time(); r = ctl("port-check", str(p), "3", "127.0.0.1")
check("占用者就是 nas-kvmd：立即返回 0（重复 start / 旧进程退出中）", r.returncode == 0 and time.time() - t0 < 2, r.stdout)
srv.close()

def delayed_close(sock, d): time.sleep(d); sock.close()
set_comm("fakeapp")
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("0.0.0.0", 0)); srv.listen(1)
p = srv.getsockname()[1]
threading.Thread(target=delayed_close, args=(srv, 1.2)).start()
r = ctl("port-check", str(p), "4", "127.0.0.1")
check("等待期间对方释放端口：返回 0", r.returncode == 0, r.stdout)
set_comm(old_comm)

print("serial-scan：与自动探测同一套规则")
sysr = f"{tmp}/sys"
def usb_dev(name, vid, pid):
    d = f"{sysr}/devices/usb1/{name}"; os.makedirs(d, exist_ok=True)
    open(f"{d}/idVendor", "w").write(vid + "\n"); open(f"{d}/idProduct", "w").write(pid + "\n"); return d
def add_tty(tty, usbname, vid, pid, kind):
    dev = usb_dev(usbname, vid, pid); iface = f"{dev}/{usbname}:1.0"; os.makedirs(iface, exist_ok=True)
    target = iface
    if kind == "usbserial": target = f"{iface}/{tty}"; os.makedirs(target, exist_ok=True)   # ttyUSB：device 是 usb-serial 端口
    cd = f"{sysr}/class/tty/{tty}"; os.makedirs(cd, exist_ok=True)
    os.symlink(os.path.relpath(target, cd), f"{cd}/device")
add_tty("ttyUSB0", "1-1", "1A86", "7523", "usbserial")      # 大写也要认
add_tty("ttyUSB1", "1-2", "12d1", "1001", "usbserial")      # 4G 模块
add_tty("ttyACM0", "1-3", "1a86", "5523", "acm")
add_tty("ttyACM1", "1-4", "2c7c", "0125", "acm")
state([])
out = ctl("serial-scan").stdout
check("CH340G (1A86:7523，ttyUSB 层级) → matched", "/dev/ttyUSB0 1a86:7523 matched=yes" in out, out)
check("4G 模块 (12d1:1001) → 不匹配", "/dev/ttyUSB1 12d1:1001 matched=no" in out, out)
check("CH341 串口模式 (1a86:5523，ttyACM 层级) → matched", "/dev/ttyACM0 1a86:5523 matched=yes" in out, out)
check("其它 ACM 设备 → 不匹配", "/dev/ttyACM1 2c7c:0125 matched=no" in out, out)
state(["nas-kvm.main.serial_ids=2c7c:0125"])
out = ctl("serial-scan").stdout
check("serial_ids 可配置", "ttyACM1 2c7c:0125 matched=yes" in out and "ttyUSB0 1a86:7523 matched=no" in out, out)

print(f"\n{ok} passed, {failed} failed")
sys.exit(1 if failed else 0)
