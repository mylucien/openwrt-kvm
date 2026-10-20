#!/usr/bin/env python3
"""init 脚本 start_service 的分支逻辑：用桩函数代替 rc.common / procd，真实调用 nas-kvm-ctl。
用法: python3 tests/test_init.py [files 目录，默认 ../files]
"""
import ctypes, os, socket, subprocess, sys, tempfile, textwrap

FILES = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "files"))
ok = failed = 0
def check(n, c, extra=""):
    global ok, failed
    if c: ok += 1; print("  ok  ", n)
    else: failed += 1; print("  FAIL", n, extra)

tmp = tempfile.mkdtemp(); os.makedirs(f"{tmp}/bin")
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
os.chmod(f"{tmp}/bin/uci", 0o755)
open(f"{tmp}/upstream_init", "w").write(f"#!/bin/sh\necho \"upstream $1\" >> {LOG}\n"); os.chmod(f"{tmp}/upstream_init", 0o755)
open(f"{tmp}/fake_ustreamer", "w").write("#!/bin/sh\n"); os.chmod(f"{tmp}/fake_ustreamer", 0o755)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def run(conf, upstream=(), foreign_ports=()):
    """返回 (标准输出, 日志)。foreign_ports: 在这些端口上起别的程序的监听。"""
    lines = [f"nas-kvm.main.{k}={v}" for k, v in conf.items()]
    for sec, kv in upstream:
        lines += [f"ustreamer.{sec}.{k}={v}" for k, v in kv.items()]
    open(STATE, "w").write("\n".join(lines) + "\n"); open(LOG, "w").write("")
    harness = f"""
        config_load() {{ :; }}
        config_get() {{ eval "$1=\\"\\$(uci -q get nas-kvm.$2.$3)\\""; eval "[ -n \\"\\${{$1}}\\" ]" || eval "$1=\\"$4\\""; }}
        config_get_bool() {{ config_get "$@"; }}
        procd_open_instance() {{ echo "INSTANCE-OPEN $1"; cur=$1; }}
        procd_set_param() {{ echo "PARAM $cur $*"; }}
        procd_close_instance() {{ :; }}
        logger() {{ shift 2; echo "LOG: $*"; }}
        . {FILES}/etc/init.d/nas-kvm
        CTL={FILES}/usr/bin/nas-kvm-ctl; USTREAMER={tmp}/fake_ustreamer
        start_service
    """
    socks = []
    for p in foreign_ports:
        s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("0.0.0.0", p)); s.listen(1); socks.append(s)
    try: ctypes.CDLL(None).prctl(15, b"fakeapp", 0, 0, 0)
    except Exception: pass
    env = dict(os.environ, PATH=f"{tmp}/bin:" + os.environ["PATH"], UPSTREAM_INIT=f"{tmp}/upstream_init")
    r = subprocess.run(["sh", "-c", harness], env=env, capture_output=True, text=True, timeout=60)
    for s in socks: s.close()
    return r.stdout + r.stderr, open(LOG).read()

TOKEN = "a" * 32
pk, pv = free_port(), free_port()
base = {"enabled": 1, "token": TOKEN, "bind": "127.0.0.1", "listen_port": pk, "video_port": pv, "video_device": "/dev/video0"}

print("端口空闲：两个 instance 都启动")
out, log = run(base)
check("kvmd 启动", "INSTANCE-OPEN kvmd" in out, out)
check("ustreamer 启动", "INSTANCE-OPEN ustreamer" in out, out)
check("没有告警", "NOT started" not in out, out)

print("管理页端口被别的程序占用")
out, log = run(base, foreign_ports=[pk])
check("kvmd 不启动", "INSTANCE-OPEN kvmd" not in out, out)
check("日志点名占用者与端口，并提示改哪个选项", f"port {pk} is in use by 'fakeapp'" in out and "listen_port" in out, out)
check("ustreamer 不受影响，照常启动", "INSTANCE-OPEN ustreamer" in out, out)

print("视频流端口被别的程序占用")
out, log = run(base, foreign_ports=[pv])
check("ustreamer 不启动", "INSTANCE-OPEN ustreamer" not in out, out)
check("日志点名占用者与端口，并提示改哪个选项", f"port {pv} is in use by 'fakeapp'" in out and "video_port" in out, out)
check("kvmd 照常启动", "INSTANCE-OPEN kvmd" in out, out)

print("上游 ustreamer 实例")
up = [("video0", {"enabled": 1, "device": "/dev/video0", "port": 8080}),
      ("cam2", {"enabled": 1, "device": "/dev/video2", "port": 8090})]
out, log = run(base, upstream=up)
check("冲突的上游 section 被关闭，并写入日志", "LOG: disabled conflicting upstream ustreamer section ustreamer.video0" in out, out)
check("无关的 cam2 不受影响、不出现在日志里", "cam2" not in out, out)
st = open(STATE).read()
check("cam2 仍为启用", "ustreamer.cam2.enabled=1" in st and "ustreamer.video0.enabled=0" in st, st)
check("本包的两个 instance 照常启动", "INSTANCE-OPEN kvmd" in out and "INSTANCE-OPEN ustreamer" in out, out)

print("enabled=0 / 绑定地址未就绪")
out, log = run(dict(base, enabled=0))
check("enabled=0：什么都不启动", "INSTANCE-OPEN" not in out and "LOG" not in out, out)

print(f"\n{ok} passed, {failed} failed")
sys.exit(1 if failed else 0)
