/* ===================================================================
 * kvm_manager.c — WebSocket 服务器 + 推流管理器
 *
 * 职责：
 *   1. TCP 监听 :8765，WebSocket 握手（纯 C，SHA-1 + Base64）
 *   2. 每客户端一线程，接收 HID 二进制帧 → 转发 kvm_hid
 *   3. 按需启动/停止 mjpg_streamer
 *   4. Watchdog 线程：无客户端 15s 后自动停流
 *
 * 编译：gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L \
 *           -lpthread -o kvm_manager kvm_manager.c
 *
 * 用法：kvm_manager [-p <ws_port>] [-s <hid_sock>]
 *   -p   WebSocket 端口（默认 8765）
 *   -s   kvm_hid Unix Socket 路径（默认 /tmp/kvm_hid.sock）
 *   -f   前台模式
 * =================================================================== */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/stat.h>

/* ── 配置 ───────────────────────────────────────── */
#define WS_PORT_DEFAULT     8765
#define HID_SOCK_DEFAULT    "/tmp/kvm_hid.sock"
#define STREAM_TIMEOUT      15          /* 秒：无客户端后停流 */
#define MJPG_PATH           "/usr/bin/mjpg_streamer"
#define MJPG_ARGS_I         "input_uvc.so -d /dev/video0 -r 1280x720 -f 30 -q 80 --format MJPEG"
#define MJPG_ARGS_O         "output_http.so -p 8080 -w /www/kvm"
#define WS_MAGIC            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define PING_INTERVAL       5           /* 秒：期望客户端 ping 间隔 */

/* ── WebSocket Opcodes ───────────────────────────── */
#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* ── 前向声明 ───────────────────────────────────── */
static int send_all(int fd, const void *data, int len);
static const char *stristr(const char *haystack, const char *needle);

/* ── 全局状态 ───────────────────────────────────── */
static int            g_ws_port     = WS_PORT_DEFAULT;
static char           g_hid_sock[108] = HID_SOCK_DEFAULT;
static pthread_mutex_t g_lock       = PTHREAD_MUTEX_INITIALIZER;
static int            g_clients     = 0;
static int            g_client_seq  = 0;   /* 单调递增客户端唯一 ID */
static time_t         g_last_hb     = 0;
static pid_t          g_streamer_pid = -1;
static int            g_streamer_spawning = 0; /* TOCTOU 防护：标记正在 fork */

/* ════════════════════════════════════════════════════
   SHA-1 (RFC 3174) — 无外部依赖
   ════════════════════════════════════════════════════ */
typedef struct {
    uint32_t s[5];
    uint8_t  buf[64];
    uint64_t len;
    int      blen;
} SHA1_CTX;

static void sha1_init(SHA1_CTX *c) {
    c->s[0] = 0x67452301; c->s[1] = 0xEFCDAB89;
    c->s[2] = 0x98BADCFE; c->s[3] = 0x10325476;
    c->s[4] = 0xC3D2E1F0;
    c->len = 0; c->blen = 0;
}

#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))

static void sha1_block(SHA1_CTX *c, const uint8_t *p) {
    uint32_t w[80], a, b, d, e, f, k, t;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8)  | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = c->s[0]; b = c->s[1];
    uint32_t cc = c->s[2]; d = c->s[3]; e = c->s[4];

    for (int i = 0; i < 80; i++) {
        if (i < 20)       { f = (b & cc) | (~b & d);  k = 0x5A827999; }
        else if (i < 40)  { f = b ^ cc ^ d;           k = 0x6ED9EBA1; }
        else if (i < 60)  { f = (b & cc) | (b & d) | (cc & d);
                                                       k = 0x8F1BBCDC; }
        else              { f = b ^ cc ^ d;           k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->s[0] += a; c->s[1] += b; c->s[2] += cc;
    c->s[3] += d; c->s[4] += e;
}

static void sha1_update(SHA1_CTX *c, const void *data, int n) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    while (n > 0) {
        int cp = 64 - c->blen;
        if (cp > n) cp = n;
        memcpy(c->buf + c->blen, p, cp);
        c->blen += cp; p += cp; n -= cp;
        if (c->blen == 64) { sha1_block(c, c->buf); c->blen = 0; }
    }
}

static void sha1_final(SHA1_CTX *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    while (c->blen != 56) { uint8_t z = 0; sha1_update(c, &z, 1); }
    for (int i = 7; i >= 0; i--) {
        uint8_t bv = (uint8_t)(bits >> (i * 8));
        sha1_update(c, &bv, 1);
    }
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->s[i] >> 24);
        out[i*4+1] = (uint8_t)(c->s[i] >> 16);
        out[i*4+2] = (uint8_t)(c->s[i] >> 8);
        out[i*4+3] = (uint8_t)(c->s[i]);
    }
}

/* ── 大小写不敏感子串查找（纯 ASCII，无外部依赖） ──── */
static const char *stristr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (*h == *n || (*h | 0x20) == (*n | 0x20))) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

/* ════════════════════════════════════════════════════
   Base64 编码
   ════════════════════════════════════════════════════ */
static const char B64T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, int n, char *out) {
    int i = 0, j = 0;
    while (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < n) v |= in[i+2];
        out[j++] = B64T[(v >> 18) & 63];
        out[j++] = B64T[(v >> 12) & 63];
        out[j++] = (i + 1 < n) ? B64T[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < n) ? B64T[v & 63] : '=';
        i += 3;
    }
    out[j] = '\0';
}

/* ════════════════════════════════════════════════════
   WebSocket 握手
   ════════════════════════════════════════════════════ */
static int ws_handshake(int fd) {
    /* 设置接收超时（5 秒），防止恶意连接不发数据一直阻塞 */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[2048];
    int n = 0;
    /* 循环读取直到 HTTP 头结束（\r\n\r\n），防止 TCP 分段截断 */
    while (n < (int)sizeof(buf) - 1) {
        int r = (int)recv(fd, buf + n, sizeof(buf) - 1 - n, 0);
        if (r <= 0) return -1;
        n += r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    /* 查找 Sec-WebSocket-Key（大小写不敏感，兼容非标准客户端实现） */
    const char *key_label = "sec-websocket-key:";
    const char *key_start = stristr(buf, key_label);
    if (!key_start) {
        fprintf(stderr, "[manager] No Sec-WebSocket-Key in request\n");
        return -1;
    }
    key_start += strlen(key_label);
    while (*key_start == ' ') key_start++;

    char key[64];
    int ki = 0;
    while (key_start[ki] && key_start[ki] != '\r' && key_start[ki] != '\n' && ki < 63) {
        key[ki] = key_start[ki];
        ki++;
    }
    key[ki] = '\0';

    /* SHA-1(key + magic)
     * combined 缓冲区: key 最长 24 字节 + magic 36 字节 + null = 61 字节, 128 安全 */
    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_MAGIC);

    SHA1_CTX sha;
    sha1_init(&sha);
    sha1_update(&sha, combined, (int)strlen(combined));
    uint8_t digest[20];
    sha1_final(&sha, digest);

    char accept_b64[32];
    base64_encode(digest, 20, accept_b64);

    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", accept_b64);

    if (send_all(fd, resp, rlen) < 0) {
        fprintf(stderr, "[manager] Handshake send failed\n");
        return -1;
    }

    /* 握手完成 → 清除 SO_RCVTIMEO，帧循环需要无限阻塞等待 */
    struct timeval tv_inf = { .tv_sec = 0, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_inf, sizeof(tv_inf));

    fprintf(stderr, "[manager] WS handshake OK (key=%s)\n", key);
    return 0;
}

/* ════════════════════════════════════════════════════
   WebSocket 帧收发 (RFC 6455)
   ════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  op;
    uint8_t *data;
    int      len;
} WsFrame;

static int recv_exact(int fd, uint8_t *buf, int n) {
    int got = 0;
    while (got < n) {
        int r = (int)recv(fd, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* 读取一个 WebSocket 帧。返回 0=ok, -1=错误/关闭。
 * 调用者负责 free(frame->data) */
static int ws_read_frame(int fd, WsFrame *frame) {
    uint8_t hdr[2];
    if (recv_exact(fd, hdr, 2) < 0) return -1;

    frame->op = hdr[0] & 0x0F;
    int masked = (hdr[1] >> 7) & 1;
    uint64_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        uint8_t ext[2];
        if (recv_exact(fd, ext, 2) < 0) return -1;
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        /* 不处理超大帧（> 64KB） */
        return -1;
    }

    frame->data = (uint8_t *)malloc((size_t)plen + 1);
    if (!frame->data) return -1;

    uint8_t mask[4] = {0};
    if (masked) {
        if (recv_exact(fd, mask, 4) < 0) {
            free(frame->data);
            return -1;
        }
    }
    frame->len = (int)plen;

    if (plen > 0 && recv_exact(fd, frame->data, (int)plen) < 0) {
        free(frame->data);
        return -1;
    }

    if (masked) {
        for (uint64_t i = 0; i < plen; i++)
            frame->data[i] ^= mask[i % 4];
    }
    frame->data[plen] = '\0';
    return 0;
}

static void ws_send_frame(int fd, uint8_t op, const void *data, int len) {
    uint8_t hdr[4];
    int hlen = 2;
    hdr[0] = 0x80 | op;  /* FIN + opcode */
    if (len < 126) {
        hdr[1] = (uint8_t)len;
    } else {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    }
    send_all(fd, hdr, hlen);
    if (len > 0) send_all(fd, data, len);
}

static void ws_send_text(int fd, const char *msg) {
    ws_send_frame(fd, WS_OP_TEXT, msg, (int)strlen(msg));
}

/* send_all — 确保所有字节发送完成 */
static int send_all(int fd, const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    int sent = 0;
    while (sent < len) {
        int r = (int)send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (r <= 0) return -1;
        sent += r;
    }
    return sent;
}

/* ════════════════════════════════════════════════════
   HID 转发（客户端线程内使用持久连接）
   返回 0=ok, -1=连接不可用
   ════════════════════════════════════════════════════ */
static int hid_connect(int *hid_fd) {
    if (*hid_fd >= 0) return 0;  /* 已有连接 */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_hid_sock, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        *hid_fd = fd;
        return 0;
    }
    close(fd);
    *hid_fd = -1;
    fprintf(stderr, "[manager] WARN: cannot connect to kvm_hid: %s\n",
            strerror(errno));
    return -1;
}

static void forward_hid_persist(int *hid_fd, const uint8_t *data, int len) {
    if (hid_connect(hid_fd) < 0) return;

    if (send_all(*hid_fd, data, len) < 0) {
        /* 连接断开，关闭并重试 */
        close(*hid_fd);
        *hid_fd = -1;
        if (hid_connect(hid_fd) == 0)
            send_all(*hid_fd, data, len);
    }
}

/* ════════════════════════════════════════════════════
   推流管理
   ════════════════════════════════════════════════════ */
static void streamer_start(void) {
    pthread_mutex_lock(&g_lock);

    /* TOCTOU 防护：正在 fork 或已在运行则不重复创建 */
    if (g_streamer_spawning) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    int already_running = 0;
    if (g_streamer_pid > 0) {
        int ret = waitpid(g_streamer_pid, NULL, WNOHANG);
        if (ret == 0) {
            already_running = 1;
        } else if (ret > 0) {
            /* 子进程已退出，同步清零（与 streamer_running 一致） */
            g_streamer_pid = -1;
        }
    }
    if (already_running) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[manager] Streamer already running (pid=%d)\n",
                g_streamer_pid);
        return;
    }

    /* 标记正在 fork，防止并发双 fork */
    g_streamer_spawning = 1;
    pthread_mutex_unlock(&g_lock);

    /* fork（锁外执行，避免子进程继承锁） */
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 3; i < 256; i++) close(i);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, 1);
            dup2(null_fd, 2);
            if (null_fd > 2) close(null_fd);
        }
        execlp(MJPG_PATH, "mjpg_streamer",
               "-i", MJPG_ARGS_I,
               "-o", MJPG_ARGS_O,
               NULL);
        _exit(1);
    }

    pthread_mutex_lock(&g_lock);
    g_streamer_spawning = 0;
    if (pid < 0) {
        fprintf(stderr, "[manager] ERROR: fork streamer: %s\n", strerror(errno));
    } else {
        g_streamer_pid = pid;
        fprintf(stderr, "[manager] Streamer started (pid=%d)\n", pid);
    }
    pthread_mutex_unlock(&g_lock);
}

static void streamer_stop(void) {
    pid_t pid_to_wait = -1;

    pthread_mutex_lock(&g_lock);
    if (g_streamer_pid > 0) {
        fprintf(stderr, "[manager] Stopping streamer (pid=%d)...\n", g_streamer_pid);
        kill(g_streamer_pid, SIGTERM);
        pid_to_wait = g_streamer_pid;
    }
    pthread_mutex_unlock(&g_lock);

    /* 锁外等待子进程退出，不阻塞其他线程 */
    if (pid_to_wait > 0) {
        for (int i = 0; i < 3; i++) {
            if (waitpid(pid_to_wait, NULL, WNOHANG) > 0) goto reaped;
            sleep(1);
        }
        /* 3 秒超时仍未退出 → SIGKILL 强制终止，然后阻塞回收 */
        kill(pid_to_wait, SIGKILL);
        waitpid(pid_to_wait, NULL, 0);
reaped:
        pthread_mutex_lock(&g_lock);
        if (g_streamer_pid == pid_to_wait)
            g_streamer_pid = -1;
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[manager] Streamer stopped\n");
    }
}

static int streamer_running(void) {
    pthread_mutex_lock(&g_lock);
    int running = 0;
    if (g_streamer_pid > 0) {
        int ret = waitpid(g_streamer_pid, NULL, WNOHANG);
        if (ret == 0) {
            running = 1;  /* 仍在运行 */
        } else if (ret > 0) {
            /* 子进程已退出，清理状态避免 streamer_stop 二次 waitpid 报 ECHILD */
            g_streamer_pid = -1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return running;
}

/* ════════════════════════════════════════════════════
   Watchdog 线程 — 无客户端超时停流
   ════════════════════════════════════════════════════ */
static void *watchdog_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "[manager] Watchdog started\n");
    while (1) {
        sleep(3);

        pthread_mutex_lock(&g_lock);
        int     clients = g_clients;
        time_t  last_hb = g_last_hb;
        pthread_mutex_unlock(&g_lock);

        if (clients > 0) continue;

        if (streamer_running() && (time(NULL) - last_hb > STREAM_TIMEOUT)) {
            fprintf(stderr, "[manager] Watchdog: no clients for %ds, stopping streamer\n",
                    STREAM_TIMEOUT);
            streamer_stop();
        }
    }
    return NULL;
}

/* ════════════════════════════════════════════════════
   键盘状态追踪
   维护每客户端当前按下的键集合，组装完整 CH9329 HID 报告
   ════════════════════════════════════════════════════ */
typedef struct {
    uint8_t modifiers;       /* 当前修饰键位掩码 */
    uint8_t keys[5];         /* 当前按下的按键码 (最多 5 个) */
    int     key_count;       /* 当前按下键数 */
} KbdState;

static void kbd_init(KbdState *k) {
    k->modifiers = 0;
    k->key_count = 0;
    memset(k->keys, 0, sizeof(k->keys));
}

static int kbd_find_key(KbdState *k, uint8_t keycode) {
    for (int i = 0; i < k->key_count; i++) {
        if (k->keys[i] == keycode) return i;
    }
    return -1;
}

static void kbd_press(KbdState *k, uint8_t keycode, uint8_t modifiers) {
    k->modifiers = modifiers;
    if (kbd_find_key(k, keycode) >= 0) return;
    if (k->key_count < 5)
        k->keys[k->key_count++] = keycode;
}

static void kbd_release(KbdState *k, uint8_t keycode, uint8_t modifiers) {
    k->modifiers = modifiers;
    int idx = kbd_find_key(k, keycode);
    if (idx < 0) return;
    k->keys[idx] = k->keys[--k->key_count];
}

/* 用当前键盘状态组装 CH9329 完整报告 (8 字节数据, kvm_hid 追加校验和) */
static void kbd_build_report(KbdState *k, uint8_t out[8]) {
    memset(out, 0, 8);
    out[0] = 0x01;               /* 键盘命令头 */
    out[1] = k->modifiers;       /* 修饰键位掩码 */
    /* out[2] = 0x00; 保留 */
    for (int i = 0; i < k->key_count && i < 5; i++)
        out[3 + i] = k->keys[i];
}

/* ════════════════════════════════════════════════════
   客户端线程
   ════════════════════════════════════════════════════ */
static void *client_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    fprintf(stderr, "[manager] New client (fd=%d)\n", fd);

    /* WebSocket 握手 */
    if (ws_handshake(fd) < 0) {
        close(fd);
        return NULL;
    }

    /* 初始化键盘状态（每客户端独立追踪） */
    KbdState kb;
    kbd_init(&kb);

    /* 初始化 HID 持久连接（-1 表示未连接，首次使用时建立） */
    int hid_fd = -1;

    /* BIN 帧收到后也要更新心跳——但放在 switch 外做会更简洁，下面单独处理 */

    /* 注册客户端 — 此时才启动推流 */
    pthread_mutex_lock(&g_lock);
    g_clients++;
    g_last_hb = time(NULL);
    int client_id = ++g_client_seq;   /* 单调递增，日志中唯一标识 */
    pthread_mutex_unlock(&g_lock);

    fprintf(stderr, "[manager] Client #%d registered, starting streamer...\n",
            client_id);

    streamer_start();
    /* 等待 mjpg_streamer HTTP 端口就绪（轮询 :8080，最多 3 秒） */
    {
        int mjpg_ok = 0;
        for (int i = 0; i < 30; i++) {
            int probe = socket(AF_INET, SOCK_STREAM, 0);
            if (probe >= 0) {
                /* 设置 connect 超时：100ms 内无响应即跳过 */
                struct timeval probe_tv = { .tv_sec = 0, .tv_usec = 100000 };
                setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, &probe_tv, sizeof(probe_tv));

                struct sockaddr_in mjpg_addr;
                memset(&mjpg_addr, 0, sizeof(mjpg_addr));
                mjpg_addr.sin_family = AF_INET;
                mjpg_addr.sin_port   = htons(8080);
                mjpg_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (connect(probe, (struct sockaddr*)&mjpg_addr,
                            sizeof(mjpg_addr)) == 0) {
                    mjpg_ok = 1;
                    close(probe);
                    break;
                }
                close(probe);
            }
            usleep(100000);  /* 100ms */
        }
        if (!mjpg_ok) {
            fprintf(stderr, "[manager] WARN: mjpg_streamer not ready after 3s\n");
        }
    }
    ws_send_text(fd, "{\"type\":\"stream_ready\",\"port\":8080}");

    /* 帧循环 */
    WsFrame frame;
    while (ws_read_frame(fd, &frame) == 0) {
        switch (frame.op) {
        case WS_OP_PING: {
            /* 回复 PONG */
            ws_send_frame(fd, WS_OP_PONG, frame.data, frame.len);
            break;
        }
        case WS_OP_TEXT: {
            /* 处理文本消息（心跳 ping/pong） */
            if (strcmp((const char *)frame.data, "ping") == 0) {
                ws_send_text(fd, "pong");
            }
            /* 任何客户端消息都刷新心跳（包括 ping、HID 帧期间的活跃标记） */
            pthread_mutex_lock(&g_lock);
            g_last_hb = time(NULL);
            pthread_mutex_unlock(&g_lock);
            break;
        }
        case WS_OP_BIN: {
            /* HID 指令 */
            if (frame.len < 2) break;

            if (frame.data[0] == 0x01 && frame.len >= 4) {
                /* 键盘帧：通过状态追踪构建完整 CH9329 报告 */
                uint8_t keycode   = frame.data[3];
                uint8_t modifiers = frame.data[1];
                uint8_t pressed   = frame.data[2];

                if (pressed)
                    kbd_press(&kb, keycode, modifiers);
                else
                    kbd_release(&kb, keycode, modifiers);

                /* 即使没有按键按下也发送空报告（释放所有键） */
                uint8_t report[8];
                kbd_build_report(&kb, report);
                forward_hid_persist(&hid_fd, report, 8);
            } else {
                /* 鼠标帧：直接转发 */
                forward_hid_persist(&hid_fd, frame.data, frame.len);
            }
            /* BIN 帧也刷新心跳（鼠标移动、按键事件都算活跃） */
            pthread_mutex_lock(&g_lock);
            g_last_hb = time(NULL);
            pthread_mutex_unlock(&g_lock);
            break;
        }
        case WS_OP_CLOSE:
            free(frame.data);
            goto done;
        case WS_OP_CONT:
        default:
            break;
        }
        free(frame.data);
    }

done:
    /* 客户端断开：发送空键盘报告释放所有按键（防止被控端残留按键状态） */
    {
        uint8_t release[8] = {0x01, 0, 0, 0,0,0,0,0};
        forward_hid_persist(&hid_fd, release, 8);
    }

    /* 关闭 HID 持久连接 */
    if (hid_fd >= 0) close(hid_fd);

    pthread_mutex_lock(&g_lock);
    g_clients--;
    g_last_hb = time(NULL);
    fprintf(stderr, "[manager] Client #%d disconnected (remaining=%d)\n",
            client_id, g_clients);
    pthread_mutex_unlock(&g_lock);

    close(fd);
    return NULL;
}

/* ════════════════════════════════════════════════════
   main
   ════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    int foreground = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_ws_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            strncpy(g_hid_sock, argv[++i], sizeof(g_hid_sock) - 1);
        } else if (strcmp(argv[i], "-f") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: kvm_manager [-p <ws_port>] [-s <hid_sock>] [-f]\n");
            printf("  -p   WebSocket port (default: %d)\n", WS_PORT_DEFAULT);
            printf("  -s   kvm_hid Unix socket path (default: %s)\n",
                   HID_SOCK_DEFAULT);
            printf("  -f   Foreground mode\n");
            return 0;
        }
    }

    /* 后台化（除非 -f） */
    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) {
            fprintf(stderr, "[manager] Daemonized (pid=%d)\n", pid);
            _exit(0);
        }
        setsid();
        close(0); close(1); close(2);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
    }

    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);

    /* 启动 Watchdog 线程 */
    pthread_t wt;
    pthread_create(&wt, NULL, watchdog_thread, NULL);
    pthread_detach(wt);

    /* TCP 监听 */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)g_ws_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        fprintf(stderr, "[manager] ERROR: cannot bind port %d\n", g_ws_port);
        return 1;
    }

    listen(srv, 16);
    fprintf(stderr, "[manager] Listening on ws://0.0.0.0:%d\n", g_ws_port);
    fprintf(stderr, "[manager] HID socket: %s\n", g_hid_sock);

    /* accept 循环 */
    while (1) {
        int *cfd = (int *)malloc(sizeof(int));
        if (!cfd) continue;
        *cfd = accept(srv, NULL, NULL);
        if (*cfd < 0) {
            free(cfd);
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, cfd) != 0) {
            fprintf(stderr, "[manager] ERROR: pthread_create failed\n");
            close(*cfd);
            free(cfd);
            continue;
        }
        pthread_detach(t);
    }

    /* 清理 */
    close(srv);
    streamer_stop();
    return 0;
}
