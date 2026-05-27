/* ===================================================================
 * kvm_hid.c — HID 守护进程
 *
 * 职责：监听 Unix Domain Socket (/tmp/kvm_hid.sock)
 *       接收 6 字节二进制 HID 帧 → 写入 UART → CH9329
 *
 * 编译：gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L \
 *           -lpthread -o kvm_hid kvm_hid.c
 *
 * 用法：kvm_hid [-p <port>] [-d <device>]
 *   -p   Unix Socket 路径（默认 /tmp/kvm_hid.sock）
 *   -d   UART 设备路径（默认 /dev/ttyS1）
 *   -b   UART 波特率（默认 115200）
 *   -f   前台运行（默认后台化）
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
#include <sys/stat.h>
#include <termios.h>
#include <time.h>

/* ── 默认配置 ───────────────────────────────────── */
#define SOCK_PATH_DEFAULT "/tmp/kvm_hid.sock"
#define UART_DEV_DEFAULT  "/dev/ttyS1"
#define UART_BAUD_DEFAULT B115200

/* CH9329 帧格式（参考芯片手册）
 *
 * 鼠标帧 (7 字节，最后 1 字节校验和):
 *   [0]   0x02 (鼠标命令头)
 *   [1]   按键状态 (bit0=左键, bit1=右键, bit2=中键)
 *   [2-3] X 坐标 0-4095 (Little-Endian)
 *   [4-5] Y 坐标 0-4095 (Little-Endian)
 *   [6]   校验和 = (byte0+...+byte5) & 0xFF
 *
 * 键盘帧 (9 字节，最后 1 字节校验和):
 *   [0]   0x01 (键盘命令头)
 *   [1]   修饰键 (bit0=LCtrl, bit1=LShift, bit2=LAlt, bit3=LGui)
 *   [2]   保留 (0x00)
 *   [3-7] 按键码 (最多同时 5 键，HID Usage ID)
 *   [8]   校验和 = (byte0+...+byte7) & 0xFF
 *
 * 注意：实际 CH9329 协议因版本可能有差异，此为基础框架。
 * 详细协议请参考 CH9329 数据手册（南京沁恒）。
 */

/* ── 全局状态 ───────────────────────────────────── */
static char g_sock_path[108] = SOCK_PATH_DEFAULT;
static char g_uart_dev[256]  = UART_DEV_DEFAULT;
static int  g_uart_baud      = UART_BAUD_DEFAULT;
static int  g_uart_baud_val  = 115200;     /* 用户指定的数值（用于日志） */
static int  g_uart_fd        = -1;
static int  g_srv_fd         = -1;
static volatile bool g_running = true;

/* ── 信号处理 ───────────────────────────────────── */
static void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

/* ── UART 初始化 ─────────────────────────────────── */
static int uart_open(const char *dev, speed_t baud) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "[kvm_hid] ERROR: open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "[kvm_hid] ERROR: tcgetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    /* raw 模式：8N1，无流控，最低延迟 */
    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);

    tty.c_iflag  = 0;           /* 关闭所有输入处理 */
    tty.c_oflag  = 0;           /* 关闭所有输出处理 */
    tty.c_lflag  = 0;           /* 关闭行处理/回声 */

    tty.c_cc[VMIN]  = 0;        /* 非阻塞读 */
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[kvm_hid] ERROR: tcsetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);

    fprintf(stderr, "[kvm_hid] UART %s opened (baud=%d)\n", dev, g_uart_baud_val);
    return fd;
}

/* ── Unix Domain Socket 监听 ─────────────────────── */
static int unix_listen(const char *path) {
    /* 删除遗留 socket 文件 */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[kvm_hid] ERROR: socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[kvm_hid] ERROR: bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    /* 权限：所有用户可读写 */
    chmod(path, 0666);

    if (listen(fd, 8) < 0) {
        fprintf(stderr, "[kvm_hid] ERROR: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    fprintf(stderr, "[kvm_hid] Listening on %s\n", path);
    return fd;
}

/* ── CH9329 校验 ─────────────────────────────────── */
static uint8_t ch9329_checksum(const uint8_t *data, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += data[i];
    return sum;
}

/* ── 写入 UART（自动追加 CH9329 校验和，短写重试）────── */
static int uart_write_frame(const uint8_t *data, int len) {
    if (g_uart_fd < 0) return -1;

    /* 追加校验和：CH9329 要求 (所有字节和 & 0xFF) 作为最后一字节 */
    uint8_t frame_with_cs[64];
    if (len + 1 > (int)sizeof(frame_with_cs)) {
        fprintf(stderr, "[kvm_hid] ERROR: frame too large (%d)\n", len);
        return -1;
    }
    memcpy(frame_with_cs, data, len);
    frame_with_cs[len] = ch9329_checksum(data, len);

    /* 短写重试：UART 内核缓冲区满时 write 可能返回部分字节 */
    int total = len + 1, written = 0;
    while (written < total) {
        int r = (int)write(g_uart_fd, frame_with_cs + written, total - written);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[kvm_hid] UART write error: %s\n", strerror(errno));
            return -1;
        }
        written += r;
    }
    return written;
}

/* 根据首字节确定预期帧长度 */
static int frame_len_by_type(uint8_t type) {
    switch (type) {
        case 0x01: return 8;   /* 键盘帧 */
        case 0x02: return 6;   /* 鼠标帧 */
        default:   return 0;   /* 未知类型 */
    }
}

/* ── 帧感知读取：确保读完完整一帧 ──────────────── */
static int read_frame(int fd, uint8_t *buf, int max_len) {
    /* 先读首字节确定类型（<=0 = EOF 或错误） */
    int got = (int)read(fd, buf, 1);
    if (got <= 0) return -1;

    int flen = frame_len_by_type(buf[0]);
    if (flen == 0) {
        fprintf(stderr, "[kvm_hid] WARN: unknown frame type 0x%02X\n", buf[0]);
        return -1;
    }
    if (flen > max_len) return -1;

    /* 再读剩余字节（复用 got 变量，消除遮蔽） */
    while (got < flen) {
        int n = (int)read(fd, buf + got, flen - got);
        if (n <= 0) return -1;
        got += n;
    }
    return got;
}

/* ── 客户端线程 ─────────────────────────────────── */
static void *handle_client_thread(void *arg) {
    int cfd = *(int *)arg;
    free(arg);
    uint8_t buf[64];

    while (g_running) {
        int n = read_frame(cfd, buf, (int)sizeof(buf));
        if (n < 0) break;

        /* 转发到 UART */
        int written = uart_write_frame(buf, n);
        if (written > 0) {
            fprintf(stderr, "[kvm_hid] TX %d bytes (type=0x%02X)\n",
                    written, buf[0]);
        }
    }

    fprintf(stderr, "[kvm_hid] Client disconnected\n");
    close(cfd);
    return NULL;
}

/* ── 后台化 ─────────────────────────────────────── */
static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[kvm_hid] fork: %s\n", strerror(errno));
        exit(1);
    }
    if (pid > 0) {
        /* 父进程退出 */
        fprintf(stderr, "[kvm_hid] Daemonized (pid=%d)\n", pid);
        _exit(0);
    }

    /* 子进程：新会话 */
    setsid();

    /* 关闭 std fd */
    close(0);
    close(1);
    close(2);
    open("/dev/null", O_RDONLY);  /* stdin */
    open("/dev/null", O_WRONLY);  /* stdout */
    open("/dev/null", O_WRONLY);  /* stderr */
}

/* ── 主循环 ─────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int foreground = 0;

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            strncpy(g_sock_path, argv[++i], sizeof(g_sock_path) - 1);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            strncpy(g_uart_dev, argv[++i], sizeof(g_uart_dev) - 1);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            int baud = atoi(argv[++i]);
            g_uart_baud_val = baud;
            switch (baud) {
                case 9600:    g_uart_baud = B9600;    break;
                case 19200:   g_uart_baud = B19200;   break;
                case 38400:   g_uart_baud = B38400;   break;
                case 57600:   g_uart_baud = B57600;   break;
                case 115200:  g_uart_baud = B115200;  break;
                case 230400:  g_uart_baud = B230400;  break;
                default:
                    fprintf(stderr, "[kvm_hid] Unsupported baud: %d\n", baud);
                    return 1;
            }
        } else if (strcmp(argv[i], "-f") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: kvm_hid [-p <sock_path>] [-d <uart_dev>] [-b <baud>] [-f]\n");
            printf("  -p   Unix Socket path (default: %s)\n", SOCK_PATH_DEFAULT);
            printf("  -d   UART device (default: %s)\n", UART_DEV_DEFAULT);
            printf("  -b   Baud rate (default: 115200)\n");
            printf("  -f   Foreground mode (no daemonize)\n");
            return 0;
        }
    }

    /* 后台化（除非 -f） */
    if (!foreground) {
        daemonize();
    }

    /* 信号处理 */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /* 打开 UART */
    g_uart_fd = uart_open(g_uart_dev, g_uart_baud);
    if (g_uart_fd < 0) return 1;

    /* 监听 Unix Socket */
    g_srv_fd = unix_listen(g_sock_path);
    if (g_srv_fd < 0) {
        close(g_uart_fd);
        return 1;
    }

    fprintf(stderr, "[kvm_hid] Ready. sock=%s uart=%s\n",
            g_sock_path, g_uart_dev);

    /* accept 循环 */
    while (g_running) {
        struct sockaddr_un peer;
        socklen_t peer_len = sizeof(peer);
        int cfd = accept(g_srv_fd, (struct sockaddr*)&peer, &peer_len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[kvm_hid] accept: %s\n", strerror(errno));
            break;
        }
        fprintf(stderr, "[kvm_hid] Client connected\n");
        int *cfd_ptr = (int *)malloc(sizeof(int));
        if (cfd_ptr) {
            *cfd_ptr = cfd;
            pthread_t t;
            pthread_create(&t, NULL, handle_client_thread, cfd_ptr);
            pthread_detach(t);
        } else {
            close(cfd);
        }
    }

    /* 清理 */
    fprintf(stderr, "[kvm_hid] Shutting down...\n");
    close(g_srv_fd);
    unlink(g_sock_path);
    close(g_uart_fd);
    return 0;
}
