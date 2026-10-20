/*
 * nas-kvmd — Nokia XG-040G-MD 光猫 KVM 守护进程
 *   - 内置最小 HTTP / WebSocket 服务（单线程 poll 事件循环）
 *   - CH9329 键鼠（经 CH340G 串口）
 *   - 开机宏（连发开机菜单键 / BIOS 键）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <termios.h>
#include <glob.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "ch9329.h"
#include "hid_map.h"
#include "sha1b64.h"

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */
enum { T_STR, T_INT, T_BOOL };
struct opt { const char *name; int type; void *p; int sz, min, max, hot; };

static struct {
	char serial[64], bind[64], token[80];
	char boot_key[8], bios_key[8], www[96], ctl[96];
	char serial_ids[96], sysfs[96], devdir[64];
	int baud, port, video_port;
	int macro_dur_s, macro_int_ms, hold_ms;
	int verbose;
} C = {
	.serial = "", .bind = "127.0.0.1", .token = "",
	.boot_key = "F11", .bios_key = "Del",
	.www = "/www/kvm", .ctl = "/usr/bin/nas-kvm-ctl",
	.serial_ids = "1a86:7523,1a86:5523", .sysfs = "/sys", .devdir = "/dev",
	.baud = 9600, .port = 18080, .video_port = 18081,
	.macro_dur_s = 10, .macro_int_ms = 100, .hold_ms = 30,
};

static const struct opt OPTS[] = {
	{"serial",           T_STR,  C.serial,    sizeof C.serial,    0, 0, 0},
	{"serial_ids",       T_STR,  C.serial_ids, sizeof C.serial_ids, 0, 0, 0},
	{"baud",             T_INT,  &C.baud,     0, 1200, 115200, 0},
	{"listen_port",      T_INT,  &C.port,     0, 1, 65535, 0},
	{"token",            T_STR,  C.token,     sizeof C.token,     0, 0, 0},
	{"video_port",       T_INT,  &C.video_port, 0, 1, 65535, 0},
	{"boot_menu_key",    T_STR,  C.boot_key,  sizeof C.boot_key,  0, 0, 1},
	{"bios_key",         T_STR,  C.bios_key,  sizeof C.bios_key,  0, 0, 1},
	{"macro_duration_s", T_INT,  &C.macro_dur_s, 0, 1, 60, 1},
	{"macro_interval_ms",T_INT,  &C.macro_int_ms, 0, 20, 1000, 1},
	{"key_hold_ms",      T_INT,  &C.hold_ms,  0, 10, 200, 1},
};
#define NOPTS (sizeof OPTS / sizeof OPTS[0])

static void dbg(const char *fmt, ...)
{
	va_list ap;
	if (!C.verbose) return;
	va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
	fputc('\n', stderr);
}

/* serial_ids：逗号分隔的 vid:pid（各 4 位十六进制），至少一项 */
static int ids_valid(const char *s)
{
	int tok = 0;
	while (*s) {
		int i;
		for (i = 0; i < 9; i++) {
			if (i == 4) { if (s[i] != ':') return 0; }
			else if (!isxdigit((unsigned char)s[i])) return 0;
		}
		s += 9; tok++;
		if (*s == ',') s++;
		else if (*s) return 0;
	}
	return tok > 0;
}

static int baud_valid(long b)
{
	static const int ok[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
	size_t i;
	for (i = 0; i < sizeof ok / sizeof ok[0]; i++) if (ok[i] == b) return 1;
	return 0;
}

/* apply=0 只校验、不写入；apply=1 校验通过后写入。返回 0 / -1 */
static int cfg_set(const struct opt *o, const char *v, int apply)
{
	char *e; long n;
	switch (o->type) {
	case T_STR:
		if (strlen(v) >= (size_t)o->sz) return -1;
		if (!strcmp(o->name, "boot_menu_key") || !strcmp(o->name, "bios_key"))
			if (!hid_from_macro_name(v)) return -1;
		if (!strcmp(o->name, "serial_ids") && !ids_valid(v)) return -1;
		if (apply) strcpy((char *)o->p, v);
		return 0;
	case T_BOOL:
		if (!strcmp(v, "1") || !strcmp(v, "true")) { if (apply) *(int *)o->p = 1; return 0; }
		if (!strcmp(v, "0") || !strcmp(v, "false")) { if (apply) *(int *)o->p = 0; return 0; }
		return -1;
	default:
		n = strtol(v, &e, 10);
		if (*v == 0 || *e || n < o->min || n > o->max) return -1;
		if (!strcmp(o->name, "baud") && !baud_valid(n)) return -1;
		if (apply) *(int *)o->p = (int)n;
		return 0;
	}
}

/* 解析 `uci show` 输出（nas-kvm.main.key='v'）或 key=value 文件 */
static void cfg_parse_line(char *l)
{
	char *k, *v, *q;
	size_t i;
	l[strcspn(l, "\r\n")] = 0;
	if (!(v = strchr(l, '='))) return;
	*v++ = 0; k = l;
	if (!strncmp(k, "nas-kvm.main.", 13)) k += 13;
	if (*v == '\'' ) { v++; if ((q = strrchr(v, '\''))) *q = 0; }
	for (i = 0; i < NOPTS; i++)
		if (!strcmp(OPTS[i].name, k)) {
			if (cfg_set(&OPTS[i], v, 1) < 0)
				fprintf(stderr, "nas-kvmd: invalid value for %s, keeping default\n", k);
			return;
		}
}

static void cfg_load(const char *file)
{
	char line[256];
	FILE *f = file ? fopen(file, "r") : popen("uci -q show nas-kvm.main 2>/dev/null", "r");
	if (!f) return;
	while (fgets(line, sizeof line, f)) cfg_parse_line(line);
	if (file) fclose(f); else pclose(f);
}

/* ------------------------------------------------------------------ */
/* 基础工具                                                            */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ct_eq(const char *a, const char *b)  /* 常量时间比较 */
{
	size_t la = strlen(a), lb = strlen(b), i;
	unsigned d = la ^ lb;
	for (i = 0; i < la && i < lb; i++) d |= (unsigned char)a[i] ^ (unsigned char)b[i];
	return d == 0;
}

/* ------------------------------------------------------------------ */
/* 串口 / CH9329                                                       */
/* ------------------------------------------------------------------ */
static int ser_fd = -1;
static uint64_t ser_retry, last_probe, last_reply;
static uint8_t txbuf[4096];
static size_t txlen;
static struct ch_parser parser;
static int usb_enum;
static int ch_ver;

static speed_t baud_const(int b)  /* 配置层已限定为下列标准值 */
{
	switch (b) {
	case 1200: return B1200;   case 2400: return B2400;   case 4800: return B4800;
	case 9600: return B9600;   case 19200: return B19200; case 38400: return B38400;
	case 57600: return B57600; case 115200: return B115200;
	default: return B9600;
	}
}

static int ser_flush(void)
{
	while (ser_fd >= 0 && txlen) {
		ssize_t n = write(ser_fd, txbuf, txlen);
		if (n < 0) { if (errno == EAGAIN || errno == EINTR) return 0; return -1; }
		memmove(txbuf, txbuf + n, txlen - n);
		txlen -= n;
	}
	return 0;
}

static void ser_close(void)
{
	if (ser_fd >= 0) close(ser_fd);
	ser_fd = -1; txlen = 0; parser.n = 0; usb_enum = 0;
	ser_retry = now_ms() + 2000;
}

static void ser_send(const uint8_t *b, size_t n)
{
	if (ser_fd < 0) return;
	if (txlen + n > sizeof txbuf) { dbg("tx overflow, dropped"); return; }
	memcpy(txbuf + txlen, b, n); txlen += n;
	if (C.verbose) {
		size_t i; fprintf(stderr, "TX");
		for (i = 0; i < n; i++) fprintf(stderr, " %02X", b[i]);
		fputc('\n', stderr);
	}
	if (ser_flush() < 0) ser_close();
}

/* ---- 自动探测：只接受 USB VID:PID 在 serial_ids 里的串口，绝不碰其它设备 ---- */
static int read_line(const char *path, char *out, size_t sz)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	if (!fgets(out, sz, f)) { fclose(f); return -1; }
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
	return out[0] ? 0 : -1;
}

/* 从 tty 的 sysfs 节点向上找 USB 设备并取 idVendor:idProduct（小写）。
 * ttyUSBn 的 device 是 usb-serial 端口，要再往上两级才是 USB 设备；
 * ttyACMn 的 device 是 USB 接口，往上一级就是。层数因驱动而异，所以最多向上找 4 级，
 * 哪一级先出现 idVendor 就用哪一级。 */
static int tty_usb_id(const char *name, char *id, size_t sz)
{
	static const char *up[] = { "", "/..", "/../..", "/../../.." };
	char p[256], v[16], pr[16];
	size_t d, i;
	for (d = 0; d < sizeof up / sizeof up[0]; d++) {
		snprintf(p, sizeof p, "%s/class/tty/%s/device%s/idVendor", C.sysfs, name, up[d]);
		if (read_line(p, v, sizeof v) < 0) continue;
		snprintf(p, sizeof p, "%s/class/tty/%s/device%s/idProduct", C.sysfs, name, up[d]);
		if (read_line(p, pr, sizeof pr) < 0) return -1;
		snprintf(id, sz, "%s:%s", v, pr);
		for (i = 0; id[i]; i++) id[i] = (char)tolower((unsigned char)id[i]);
		return 0;
	}
	return -1;
}

static int ids_has(const char *list, const char *id)
{
	size_t n = strlen(id);
	const char *p = list;
	while (*p) {
		size_t l = strcspn(p, ", ");
		if (l == n && !strncasecmp(p, id, n)) return 1;
		p += l;
		while (*p == ',' || *p == ' ') p++;
	}
	return 0;
}

static void serial_autodetect(char *path, size_t psz)
{
	static const char *pat[] = { "ttyUSB*", "ttyACM*" };
	static int warned;
	int skipped = 0;
	size_t k, i;
	path[0] = 0;
	for (k = 0; k < 2 && !path[0]; k++) {
		char gp[128]; glob_t g;
		snprintf(gp, sizeof gp, "%s/%s", C.devdir, pat[k]);
		if (glob(gp, 0, NULL, &g) == 0) {
			for (i = 0; i < g.gl_pathc && !path[0]; i++) {
				const char *name = strrchr(g.gl_pathv[i], '/');
				char id[40] = "unknown";
				name = name ? name + 1 : g.gl_pathv[i];
				if (tty_usb_id(name, id, sizeof id) == 0 && ids_has(C.serial_ids, id))
					snprintf(path, psz, "%s", g.gl_pathv[i]);
				else { skipped++; dbg("serial autodetect: skipping %s (usb id %s)", g.gl_pathv[i], id); }
			}
		}
		globfree(&g);
	}
	if (path[0]) warned = 0;
	else if (skipped && !warned) {
		warned = 1;
		fprintf(stderr, "nas-kvmd: %d serial device(s) present but none matches serial_ids=%s; "
		        "not touching them (set option serial to use another device)\n", skipped, C.serial_ids);
	}
}

static void ser_open(void)
{
	char path[128] = "";
	struct termios t;

	if (C.serial[0]) snprintf(path, sizeof path, "%s", C.serial);
	else serial_autodetect(path, sizeof path);
	ser_retry = now_ms() + 2000;
	if (!path[0]) return;
	ser_fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (ser_fd < 0) return;
	if (tcgetattr(ser_fd, &t) < 0) { ser_close(); return; }
	cfmakeraw(&t);
	cfsetispeed(&t, baud_const(C.baud));
	cfsetospeed(&t, baud_const(C.baud));
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
	t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
	if (tcsetattr(ser_fd, TCSANOW, &t) < 0) { ser_close(); return; }
	tcflush(ser_fd, TCIOFLUSH);
	fprintf(stderr, "nas-kvmd: serial %s opened @%d\n", path, C.baud);
	last_probe = 0;  /* 重开后立刻探测链路 */
}

static void ser_read(void)
{
	uint8_t buf[128], cmd, data[64], len;
	ssize_t n, i;
	if (ser_fd < 0) return;
	n = read(ser_fd, buf, sizeof buf);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { ser_close(); return; }
	for (i = 0; i < n; i++)
		if (ch_feed(&parser, buf[i], &cmd, data, &len)) {
			last_reply = now_ms();
			if (C.verbose) dbg("RX cmd=%02X len=%d", cmd, len);
			if (cmd == (CH_CMD_INFO | 0x80) && len >= 2) {
				ch_ver = data[0];
				usb_enum = data[1] != 0;
			}
		}
}

static void probe_chip(void)
{
	uint8_t f[16];
	ser_send(f, ch_info(f));
	last_probe = now_ms();
}

/* ------------------------------------------------------------------ */
/* 键盘 / 鼠标                                                         */
/* ------------------------------------------------------------------ */
static uint8_t kb_mod, kb_keys[6];
static struct { int btn, x, y, wheel, pending, sent_btn; uint64_t last_tx; } MS;

static void send_kbd(uint8_t mod, const uint8_t keys[6])
{
	uint8_t f[16];
	kb_mod = mod; memcpy(kb_keys, keys, 6);
	ser_send(f, ch_kbd(f, mod, keys));
}

static void release_all(void)
{
	static const uint8_t z[6];
	uint8_t f[16];
	send_kbd(0, z);
	if (MS.btn || MS.sent_btn) {
		MS.btn = 0;
		ser_send(f, ch_mouse_abs(f, 0, MS.x, MS.y, 0));
		MS.sent_btn = 0; MS.pending = 0;
	}
}

static int mouse_interval_ms(void)  /* 保证串口占用不超过 ~45% */
{
	int frame_ms = 13 * 10 * 1000 / (C.baud ? C.baud : 9600) + 1;
	int iv = frame_ms * 22 / 10;
	return iv < 16 ? 16 : iv;
}

static void mouse_flush(uint64_t now)
{
	uint8_t f[16];
	ser_send(f, ch_mouse_abs(f, MS.btn, MS.x, MS.y, MS.wheel));
	MS.wheel = 0; MS.sent_btn = MS.btn; MS.pending = 0; MS.last_tx = now;
}

/* ------------------------------------------------------------------ */
/* 开机宏                                                              */
/* ------------------------------------------------------------------ */
enum { M_IDLE, M_RUN };
static struct { int st, usage, single, pressed, count; uint64_t end, next, rel_at; } M;

static int macro_start(int bios, int test)
{
	int u = hid_from_macro_name(bios ? C.bios_key : C.boot_key);
	uint64_t now = now_ms();
	if (!u) return -2;
	if (M.st != M_IDLE) return -1;
	memset(&M, 0, sizeof M);
	M.usage = u; M.single = test;
	M.st = M_RUN; M.next = now; M.end = now + C.macro_dur_s * 1000ULL;
	return 0;
}

static void macro_finish(void)
{
	M.st = M_IDLE; M.pressed = 0;
	release_all();       /* 结束或取消时补发全部释放帧 */
}

static void macro_cancel(void) { if (M.st != M_IDLE) macro_finish(); }

static void macro_tick(uint64_t now)
{
	uint8_t keys[6] = {0};
	if (M.st != M_RUN) return;
	if (M.pressed) {
		if (now >= M.rel_at) {
			send_kbd(0, keys); M.pressed = 0;
			if (M.single || now >= M.end) macro_finish();
		}
	} else if (M.single && M.count) {
		macro_finish();
	} else if (now >= M.end) {
		macro_finish();
	} else if (now >= M.next) {
		keys[0] = M.usage; send_kbd(0, keys);
		M.pressed = 1; M.count++;
		M.rel_at = now + C.hold_ms; M.next = now + C.macro_int_ms;
	}
}

/* ------------------------------------------------------------------ */
/* 探测子进程（HDMI / ping），异步                                    */
/* ------------------------------------------------------------------ */
static int probe_fd = -1, hdmi_state = -1, ping_state = -1;
static uint64_t probe_next;
static char probe_buf[128]; static size_t probe_len;

static void probe_start(uint64_t now)
{
	int p[2];
	pid_t pid;
	probe_next = now + 5000;
	if (access(C.ctl, X_OK) != 0 || pipe(p) < 0) return;
	pid = fork();
	if (pid < 0) { close(p[0]); close(p[1]); return; }
	if (pid == 0) {
		close(p[0]); dup2(p[1], 1); close(p[1]);
		execl(C.ctl, C.ctl, "probe", (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	probe_fd = p[0]; probe_len = 0;
}

static void probe_read(void)
{
	ssize_t n = read(probe_fd, probe_buf + probe_len, sizeof probe_buf - 1 - probe_len);
	if (n > 0) { probe_len += n; return; }
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
	probe_buf[probe_len] = 0;
	{ int h = -1, g = -1; if (sscanf(probe_buf, "hdmi=%d ping=%d", &h, &g) >= 1) { hdmi_state = h; ping_state = g; } }
	close(probe_fd); probe_fd = -1;
}

/* ------------------------------------------------------------------ */
/* 网络：客户端、HTTP、WebSocket                                       */
/* ------------------------------------------------------------------ */
#define MAXC 16
#define INSZ 8192
struct client {
	int fd, ws, closing, waiting;
	uint8_t in[INSZ]; size_t inlen;
	uint8_t *out; size_t outlen, outoff;
	uint64_t last_rx;
};
static struct client *CL[MAXC];
static struct client *st_wait;         /* 正在等待串口测试应答的客户端 */
static uint64_t st_deadline, st_t0;
static int lfd[2] = { -1, -1 };

static void client_close(struct client *c)
{
	int i, was_ws = c->ws;
	if (st_wait == c) st_wait = NULL;
	for (i = 0; i < MAXC; i++) if (CL[i] == c) CL[i] = NULL;
	close(c->fd); free(c->out); free(c);
	if (was_ws) { dbg("ws closed → release all"); release_all(); }
}

static void queue(struct client *c, const void *d, size_t n)
{
	uint8_t *p;
	if (c->outlen - c->outoff + n > (2u << 20)) { c->closing = 1; return; }
	p = realloc(c->out, c->outlen + n);
	if (!p) { c->closing = 1; return; }
	c->out = p; memcpy(c->out + c->outlen, d, n); c->outlen += n;
}

static int client_flush(struct client *c)  /* 返回 -1 表示应关闭 */
{
	while (c->outoff < c->outlen) {
		ssize_t n = send(c->fd, c->out + c->outoff, c->outlen - c->outoff, MSG_NOSIGNAL);
		if (n < 0) { if (errno == EAGAIN || errno == EINTR) return 0; return -1; }
		c->outoff += n;
	}
	free(c->out); c->out = NULL; c->outlen = c->outoff = 0;
	return (c->closing && !c->waiting) ? -1 : 0;
}

static void http_reply(struct client *c, int code, const char *ctype, const void *body, size_t n, int close_after)
{
	char h[300];
	const char *msg = code == 200 ? "OK" : code == 101 ? "Switching Protocols" : code == 400 ? "Bad Request" :
		code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : code == 409 ? "Conflict" :
		code == 413 ? "Payload Too Large" : code == 429 ? "Too Many Requests" : "Internal Server Error";
	int hl = snprintf(h, sizeof h,
		"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
		"X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n",
		code, msg, ctype, n);
	queue(c, h, hl); if (n) queue(c, body, n);
	if (close_after) c->closing = 1;
}

static void json_reply(struct client *c, int code, const char *fmt, ...)
{
	char b[1200]; va_list ap;
	va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
	http_reply(c, code, "application/json", b, strlen(b), 1);
}

/* --- 极简 JSON 取值（仅服务于自家前端发来的扁平对象） --- */
static const char *json_find(const char *j, const char *key)
{
	char pat[48]; const char *p;
	snprintf(pat, sizeof pat, "\"%s\"", key);
	if (!(p = strstr(j, pat))) return NULL;
	p += strlen(pat);
	while (*p == ' ' || *p == ':') p++;
	return p;
}
static int json_str(const char *j, const char *key, char *out, size_t sz)
{
	const char *p = json_find(j, key), *e;
	if (!p || *p != '"') return -1;
	p++; e = strchr(p, '"');
	if (!e || (size_t)(e - p) >= sz) return -1;
	memcpy(out, p, e - p); out[e - p] = 0;
	return 0;
}
static int json_int(const char *j, const char *key, int *out)
{
	const char *p = json_find(j, key); char *e;
	if (!p) return -1;
	if (!strncmp(p, "true", 4)) { *out = 1; return 0; }
	if (!strncmp(p, "false", 5)) { *out = 0; return 0; }
	*out = (int)strtol(p, &e, 10);
	return e == p ? -1 : 0;
}

/* --- 状态 JSON --- */
static int link_ok(void) { return ser_fd >= 0 && last_reply && now_ms() - last_reply < 5000; }

static int status_json(char *b, size_t n, int ws)
{
	const char *mc = M.st == M_IDLE ? "idle" : "running";
	int link = link_ok();
	return snprintf(b, n,
		"{%s\"serial\":%s,\"usb\":%s,\"hdmi\":%d,\"ping\":%d,\"macro\":\"%s\","
		"\"video_port\":%d,\"cfg\":{\"boot_menu_key\":\"%s\",\"bios_key\":\"%s\",\"macro_duration_s\":%d,"
		"\"macro_interval_ms\":%d,\"key_hold_ms\":%d}}",
		ws ? "\"t\":\"status\"," : "",
		link ? "true" : "false", (link && usb_enum) ? "true" : "false", hdmi_state, ping_state, mc,
		C.video_port, C.boot_key, C.bios_key, C.macro_dur_s, C.macro_int_ms, C.hold_ms);
}

/* --- WebSocket --- */
static void ws_send(struct client *c, const char *txt, size_t n)
{
	uint8_t h[4]; size_t hl = 2;
	h[0] = 0x81;
	if (n < 126) h[1] = (uint8_t)n;
	else { h[1] = 126; h[2] = n >> 8; h[3] = n & 0xFF; hl = 4; }
	queue(c, h, hl); queue(c, txt, n);
}

static void ws_broadcast_status(void)
{
	char b[1200]; int n, i;
	n = status_json(b, sizeof b, 1);
	for (i = 0; i < MAXC; i++) if (CL[i] && CL[i]->ws) ws_send(CL[i], b, n);
}

/* {"t":"key","down":["ControlLeft","KeyA"]} —— 每条消息都是完整按键状态 */
static void ws_key(const char *m)
{
	uint8_t keys[6] = {0}, mod = 0; int nk = 0;
	const char *p = json_find(m, "down"), *e;
	char code[32];
	if (!p || *p != '[') return;
	p++;
	while (*p && *p != ']') {
		if (*p == '"') {
			size_t l; int u;
			p++; e = strchr(p, '"');
			if (!e) break;
			l = e - p;
			if (l < sizeof code) {
				memcpy(code, p, l); code[l] = 0;
				u = hid_from_code(code);
				if (u >= 0xE0) mod |= 1 << (u - 0xE0);
				else if (u && nk < 6) keys[nk++] = u;
			}
			p = e + 1;
		} else p++;
	}
	if (mod != kb_mod || memcmp(keys, kb_keys, 6)) send_kbd(mod, keys);
}

static void ws_mouse(const char *m)
{
	int btn = 0, x = 0, y = 0, w = 0; uint64_t now = now_ms();
	json_int(m, "btn", &btn); json_int(m, "x", &x); json_int(m, "y", &y); json_int(m, "wheel", &w);
	MS.x = x < 0 ? 0 : x > 4095 ? 4095 : x;
	MS.y = y < 0 ? 0 : y > 4095 ? 4095 : y;
	MS.btn = btn & 7;
	if (w) { w = w > 127 ? 127 : w < -127 ? -127 : w; MS.wheel = w; }
	MS.pending = 1;
	if (MS.btn != MS.sent_btn || MS.wheel || now - MS.last_tx >= (uint64_t)mouse_interval_ms())
		mouse_flush(now);
}

static void ws_message(struct client *c, const char *m)
{
	char t[16];
	if (json_str(m, "t", t, sizeof t) < 0) return;
	if (!strcmp(t, "key")) ws_key(m);
	else if (!strcmp(t, "mouse")) ws_mouse(m);
	else if (!strcmp(t, "release")) release_all();
	else if (!strcmp(t, "ping")) ws_send(c, "{\"t\":\"pong\"}", 12);
}

/* 返回 -1 表示应关闭连接 */
static int ws_process(struct client *c)
{
	for (;;) {
		uint8_t *p = c->in; size_t need, hl = 2, plen; int op, masked, i;
		if (c->inlen < 2) return 0;
		op = p[0] & 0x0F; masked = p[1] & 0x80; plen = p[1] & 0x7F;
		if (plen == 126) { if (c->inlen < 4) return 0; plen = p[2] << 8 | p[3]; hl = 4; }
		else if (plen == 127) return -1;
		if (!masked) return -1;
		need = hl + 4 + plen;
		if (need > INSZ) return -1;
		if (c->inlen < need) return 0;
		for (i = 0; i < (int)plen; i++) p[hl + 4 + i] ^= p[hl + (i & 3)];
		p[need] = 0;  /* need <= INSZ-1 保证见下方 INSZ 检查 */
		if (op == 1) ws_message(c, (char *)p + hl + 4);
		else if (op == 8) { uint8_t cl[2] = {0x88, 0}; queue(c, cl, 2); c->closing = 1; return 0; }
		else if (op == 9) { uint8_t pg[2] = {0x8A, 0}; queue(c, pg, 2); }
		memmove(c->in, c->in + need, c->inlen - need);
		c->inlen -= need;
	}
}

/* --- HTTP --- */
static const char *mime(const char *path)
{
	const char *e = strrchr(path, '.');
	if (!e) return "application/octet-stream";
	if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
	if (!strcmp(e, ".js")) return "application/javascript";
	if (!strcmp(e, ".css")) return "text/css";
	if (!strcmp(e, ".json")) return "application/json";
	if (!strcmp(e, ".svg")) return "image/svg+xml";
	if (!strcmp(e, ".png")) return "image/png";
	if (!strcmp(e, ".ico")) return "image/x-icon";
	return "application/octet-stream";
}

static void serve_static(struct client *c, const char *target)
{
	char path[360], rel[256]; struct stat st; FILE *f; char *buf; size_t i;
	snprintf(rel, sizeof rel, "%s", target);
	rel[strcspn(rel, "?#")] = 0;
	if (!strcmp(rel, "/")) strcpy(rel, "/index.html");
	for (i = 0; rel[i]; i++)
		if (!(isalnum((unsigned char)rel[i]) || strchr("/._-", rel[i])) ) { json_reply(c, 400, "{\"ok\":false}"); return; }
	if (strstr(rel, "..")) { json_reply(c, 400, "{\"ok\":false}"); return; }
	snprintf(path, sizeof path, "%s%s", C.www, rel);
	if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > (1 << 20) || !(f = fopen(path, "rb"))) {
		json_reply(c, 404, "{\"ok\":false,\"error\":\"not found\"}"); return;
	}
	buf = malloc(st.st_size + 1);
	if (!buf) { fclose(f); json_reply(c, 500, "{\"ok\":false}"); return; }
	i = fread(buf, 1, st.st_size, f); fclose(f);
	http_reply(c, 200, mime(path), buf, i, 1);
	free(buf);
}

static int hget(const char *h, const char *name, char *out, size_t sz)
{
	size_t nl = strlen(name); const char *l = strchr(h, '\n');
	while (l && *++l) {
		if (!strncasecmp(l, name, nl) && l[nl] == ':') {
			const char *v = l + nl + 1; size_t n;
			while (*v == ' ') v++;
			n = strcspn(v, "\r\n");
			if (n >= sz) return -1;
			memcpy(out, v, n); out[n] = 0; return 0;
		}
		l = strchr(l, '\n');
	}
	return -1;
}

static int authed(const char *hdr, const char *target)
{
	char v[128]; const char *q;
	if (hget(hdr, "Authorization", v, sizeof v) == 0 && !strncmp(v, "Bearer ", 7))
		return ct_eq(v + 7, C.token);
	if ((q = strstr(target, "token="))) {
		char t[96]; size_t n;
		q += 6; n = strcspn(q, "&# ");
		if (n >= sizeof t) return 0;
		memcpy(t, q, n); t[n] = 0;
		return ct_eq(t, C.token);
	}
	return 0;
}

static void api_config(struct client *c, const char *body)
{
	char vals[NOPTS][48];
	const struct opt *sel[NOPTS];
	char *av[2 + 2 * NOPTS + 1] = { (char *)C.ctl, "uciset" };
	int n = 0, i; size_t k;

	/* 第一遍：只校验。任何一项非法（含存在但格式错误的值）就整批拒绝，内存里不改任何东西 */
	for (k = 0; k < NOPTS; k++) {
		const struct opt *o = &OPTS[k];
		int iv;
		if (!o->hot || !json_find(body, o->name)) continue;
		if (o->type == T_STR) { if (json_str(body, o->name, vals[n], sizeof vals[n]) < 0) goto bad; }
		else { if (json_int(body, o->name, &iv) < 0) goto bad; snprintf(vals[n], sizeof vals[n], "%d", iv); }
		if (cfg_set(o, vals[n], 0) < 0) goto bad;
		sel[n++] = o;
	}
	/* 第二遍：统一应用并持久化 */
	for (i = 0; i < n; i++) cfg_set(sel[i], vals[i], 1);
	if (n && access(C.ctl, X_OK) == 0) {
		pid_t pid = fork();
		if (pid == 0) {
			for (i = 0; i < n; i++) { av[2 + 2 * i] = (char *)sel[i]->name; av[3 + 2 * i] = vals[i]; }
			av[2 + 2 * n] = NULL;
			execv(C.ctl, av); _exit(127);
		}
	}
	json_reply(c, 200, "{\"ok\":true,\"saved\":%d}", n);
	return;
bad:
	json_reply(c, 400, "{\"ok\":false,\"error\":\"invalid value\"}");
}

static void api_serial_test(struct client *c)
{
	if (ser_fd < 0) { json_reply(c, 200, "{\"ok\":false,\"error\":\"serial not open\"}"); return; }
	if (st_wait) { json_reply(c, 409, "{\"ok\":false,\"error\":\"busy\"}"); return; }
	st_wait = c; c->waiting = 1;           /* 异步：应答或超时后由 serial_test_tick 回复 */
	st_t0 = last_reply; st_deadline = now_ms() + 400;
	probe_chip();
}

static void serial_test_tick(uint64_t now)
{
	struct client *c = st_wait;
	if (!c) return;
	if (last_reply != st_t0) {
		st_wait = NULL; c->waiting = 0;
		json_reply(c, 200, "{\"ok\":true,\"version\":\"0x%02X\",\"usb\":%s}", ch_ver, usb_enum ? "true" : "false");
	} else if (now >= st_deadline || ser_fd < 0) {
		st_wait = NULL; c->waiting = 0;
		json_reply(c, 200, "{\"ok\":false,\"error\":\"no reply\"}");
	}
}

static void http_dispatch(struct client *c)
{
	char h[4096], method[8], target[256], key[64], clen[16];
	uint8_t *he; size_t hl, cl = 0; const char *body; int r;

	he = memmem(c->in, c->inlen, "\r\n\r\n", 4);
	if (!he) { if (c->inlen >= INSZ - 1) { json_reply(c, 413, "{\"ok\":false}"); } return; }
	hl = he - c->in + 4;
	if (hl >= sizeof h) { json_reply(c, 413, "{\"ok\":false}"); return; }
	memcpy(h, c->in, hl - 2); h[hl - 2] = 0;
	if (sscanf(h, "%7s %255s", method, target) != 2) { json_reply(c, 400, "{\"ok\":false}"); return; }
	if (hget(h, "Content-Length", clen, sizeof clen) == 0) cl = (size_t)strtoul(clen, NULL, 10);
	if (hl + cl >= INSZ) { json_reply(c, 413, "{\"ok\":false}"); return; }
	if (c->inlen < hl + cl) return;           /* 等待 body */
	c->in[hl + cl] = 0; body = (char *)c->in + hl;
	c->last_rx = now_ms();

	if (!strncmp(target, "/api/", 5) || !strncmp(target, "/ws", 3)) {
		if (!authed(h, target)) { json_reply(c, 401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return; }
	}

	if (!strncmp(target, "/ws", 3) && !strcasecmp(method, "GET")) {
		uint8_t sh[20]; char in[160], acc[40], resp[200]; int n, wsn = 0, i;
		for (i = 0; i < MAXC; i++) if (CL[i] && CL[i]->ws) wsn++;
		if (wsn >= 4 || hget(h, "Sec-WebSocket-Key", key, sizeof key) < 0) { json_reply(c, 400, "{\"ok\":false}"); return; }
		snprintf(in, sizeof in, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
		sha1((uint8_t *)in, strlen(in), sh); b64enc(sh, 20, acc);
		n = snprintf(resp, sizeof resp,
			"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
		queue(c, resp, n);
		c->ws = 1;
		memmove(c->in, c->in + hl, c->inlen - hl); c->inlen -= hl;
		ws_broadcast_status();
		return;
	}

	if (!strcmp(method, "GET") && !strncmp(target, "/api/status", 11)) {
		char b[1200]; int n = status_json(b, sizeof b, 0);
		http_reply(c, 200, "application/json", b, n, 1); return;
	}
	if (!strcmp(method, "GET") && strncmp(target, "/api/", 5)) { serve_static(c, target); return; }
	if (strcmp(method, "POST")) { json_reply(c, 404, "{\"ok\":false}"); return; }

	if (!strcmp(target, "/api/macro/run") || !strcmp(target, "/api/macro/test")) {
		char kind[8] = "";
		json_str(body, "kind", kind, sizeof kind);
		if (strcmp(kind, "menu") && strcmp(kind, "bios")) { json_reply(c, 400, "{\"ok\":false,\"error\":\"kind\"}"); return; }
		r = macro_start(!strcmp(kind, "bios"), !strcmp(target, "/api/macro/test"));
		if (r == 0) json_reply(c, 200, "{\"ok\":true}");
		else json_reply(c, r == -2 ? 400 : 409, "{\"ok\":false,\"error\":\"%s\"}", r == -2 ? "bad key" : "busy");
	} else if (!strcmp(target, "/api/macro/cancel")) {
		macro_cancel(); json_reply(c, 200, "{\"ok\":true}");
	} else if (!strcmp(target, "/api/release")) {
		release_all(); json_reply(c, 200, "{\"ok\":true}");
	} else if (!strcmp(target, "/api/config")) {
		api_config(c, body);
	} else if (!strcmp(target, "/api/serial/test")) {
		api_serial_test(c);
	} else {
		json_reply(c, 404, "{\"ok\":false}");
	}
}

static void client_read(struct client *c)
{
	ssize_t n = recv(c->fd, c->in + c->inlen, INSZ - 1 - c->inlen, 0);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { c->closing = 2; return; }
	if (n < 0) return;
	c->inlen += n; c->last_rx = now_ms();
	if (c->ws) { if (ws_process(c) < 0) c->closing = 2; }
	else if (!c->closing && !c->waiting) http_dispatch(c);
}

static void accept_on(int fd)
{
	int s, i;
	while ((s = accept(fd, NULL, NULL)) >= 0) {
		struct client *c; int one = 1;
		for (i = 0; i < MAXC && CL[i]; i++) ;
		if (i == MAXC || !(c = calloc(1, sizeof *c))) { close(s); continue; }
		fcntl(s, F_SETFL, O_NONBLOCK);
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		c->fd = s; c->last_rx = now_ms(); CL[i] = c;
	}
}

static int listen_on(const char *ip, int port)
{
	struct sockaddr_in a; int fd, one = 1;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &a.sin_addr) != 1 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
		fprintf(stderr, "nas-kvmd: cannot listen on %s:%d: %s\n", ip, port, strerror(errno));
		close(fd); return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

/* ------------------------------------------------------------------ */
/* 主循环                                                              */
/* ------------------------------------------------------------------ */
static int token_ok(const char *t)
{
	size_t i, n = strlen(t);
	if (n < 8) return 0;
	for (i = 0; i < n; i++) if (!(isalnum((unsigned char)t[i]) || t[i] == '-' || t[i] == '_')) return 0;
	return 1;
}

int main(int argc, char **argv)
{
	const char *conf = NULL; int i;
	struct sigaction sa;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf = argv[++i];
	cfg_load(conf);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--conf")) i++;
		else if (!strcmp(argv[i], "--bind") && i + 1 < argc) snprintf(C.bind, sizeof C.bind, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--serial") && i + 1 < argc) snprintf(C.serial, sizeof C.serial, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--sysfs") && i + 1 < argc) snprintf(C.sysfs, sizeof C.sysfs, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--devdir") && i + 1 < argc) snprintf(C.devdir, sizeof C.devdir, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--www") && i + 1 < argc) snprintf(C.www, sizeof C.www, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--ctl") && i + 1 < argc) snprintf(C.ctl, sizeof C.ctl, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) C.port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v")) C.verbose = 1;
		else { fprintf(stderr, "usage: nas-kvmd [--conf file] [--bind ip] [--serial dev] [--sysfs dir] [--devdir dir] [--www dir] [--ctl path] [--port n] [-v]\n"); return 2; }
	}
	if (!token_ok(C.token)) {
		fprintf(stderr, "nas-kvmd: token missing or too weak (need >=8 chars of [A-Za-z0-9_-]); refusing to start\n");
		return 1;
	}
	if (!strcmp(C.bind, "0.0.0.0"))
		fprintf(stderr, "nas-kvmd: WARNING: binding 0.0.0.0, make sure WAN firewall rejects this port\n");

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_sig; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	lfd[0] = listen_on(C.bind, C.port);
	if (strcmp(C.bind, "127.0.0.1") && strcmp(C.bind, "0.0.0.0")) lfd[1] = listen_on("127.0.0.1", C.port);
	if (lfd[0] < 0 && lfd[1] < 0) return 1;

	fprintf(stderr, "nas-kvmd: listening on %s:%d\n", C.bind, C.port);

	uint64_t last_status = 0;
	unsigned last_sig = 0;
	while (!g_stop) {
		struct pollfd pf[2 + MAXC + 2]; int map[2 + MAXC + 2], np = 0, k, busy;
		uint64_t now;

		for (k = 0; k < 2; k++) if (lfd[k] >= 0) { pf[np] = (struct pollfd){ lfd[k], POLLIN, 0 }; map[np++] = -1 - k; }
		for (k = 0; k < MAXC; k++) if (CL[k]) {
			short ev = POLLIN; if (CL[k]->outoff < CL[k]->outlen) ev |= POLLOUT;
			pf[np] = (struct pollfd){ CL[k]->fd, ev, 0 }; map[np++] = k;
		}
		if (ser_fd >= 0) { pf[np] = (struct pollfd){ ser_fd, POLLIN | (txlen ? POLLOUT : 0), 0 }; map[np++] = 100; }
		if (probe_fd >= 0) { pf[np] = (struct pollfd){ probe_fd, POLLIN, 0 }; map[np++] = 101; }

		busy = M.st != M_IDLE || st_wait;
		poll(pf, np, busy ? 5 : 100);

		for (k = 0; k < np; k++) {
			int m = map[k];
			if (!pf[k].revents) continue;
			if (m < 0) accept_on(pf[k].fd);
			else if (m == 100) { if (pf[k].revents & POLLIN) ser_read(); if (ser_fd >= 0 && (pf[k].revents & (POLLERR | POLLHUP))) ser_close(); else ser_flush(); }
			else if (m == 101) probe_read();
			else if (CL[m]) {
				struct client *c = CL[m];
				if (pf[k].revents & POLLIN) client_read(c);
				if (c->closing == 2 || (client_flush(c) < 0)) client_close(c);
			}
		}

		now = now_ms();
		if (ser_fd < 0 && now >= ser_retry) ser_open();
		if (ser_fd >= 0 && now - last_probe >= 2000) probe_chip();
		serial_test_tick(now);
		macro_tick(now);
		if (MS.pending && now - MS.last_tx >= (uint64_t)mouse_interval_ms()) mouse_flush(now);
		if (probe_fd < 0 && now >= probe_next) probe_start(now);
		for (k = 0; k < MAXC; k++) if (CL[k]) {
			struct client *c = CL[k];
			if (c->ws && now - c->last_rx > 6000) { dbg("ws heartbeat timeout"); client_close(c); continue; }
			if (!c->ws && now - c->last_rx > 10000) { client_close(c); continue; }
			if (c->outoff < c->outlen && client_flush(c) < 0) client_close(c);
		}
		{   /* 宏/链路状态一变化就立即推送，前端按钮不用等下一个 1 秒周期 */
			unsigned sig = M.st | ((link_ok() ? 1u : 0) << 6) | ((usb_enum ? 1u : 0) << 7) |
				((unsigned)(hdmi_state + 1) << 8) | ((unsigned)(ping_state + 1) << 10);
			if (sig != last_sig || now - last_status >= 1000) { last_sig = sig; last_status = now; ws_broadcast_status(); }
		}
	}

	release_all();
	if (ser_fd >= 0) ser_flush();
	fprintf(stderr, "nas-kvmd: stopped\n");
	return 0;
}
