#!/bin/bash
# integration.sh — IP-KVM 集成测试
#
# 在无真实硬件环境下模拟验证协议链路。
# 需要: socat, python3, nc (netcat), timeout
#
# 测试拓扑：
#   python WS客户端 → kvm_manager (:8765) → Unix Socket → kvm_hid
#   → socat 虚拟串口对 → 监听端验证 HID 帧
#
# 用法：
#   ./test/integration.sh          # 运行全部测试
#   ./test/integration.sh basic    # 仅基础测试
#   ./test/integration.sh hid      # 仅 HID 转发测试
#   ./test/integration.sh stream   # 仅推流生命周期测试

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
TMPDIR="/tmp/kvm-test-$$"

PASS=0
FAIL=0

cleanup() {
    set +e  # 清理过程不因错误退出
    echo ""
    echo "=== Cleaning up ==="
    kill "${HID_PID:-}" 2>/dev/null || true
    kill "${MGR_PID:-}" 2>/dev/null || true
    kill "${SOCAT_PTY_PID:-}" 2>/dev/null || true
    kill "${SOCAT_MON_PID:-}" 2>/dev/null || true
    rm -rf "${TMPDIR}"
    echo "Cleanup done."
}
trap cleanup EXIT

setup() {
    mkdir -p "${TMPDIR}"
    echo "=== Setup: Test environment ==="

    # 检查依赖
    for cmd in socat python3 nc timeout; do
        if ! command -v "$cmd" &>/dev/null; then
            echo "ERROR: $cmd not found. Install it first."
            exit 1
        fi
    done

    # 检查编译产物
    if [ ! -x "${BUILD_DIR}/kvm_hid" ] || [ ! -x "${BUILD_DIR}/kvm_manager" ]; then
        echo "Building binaries..."
        make -C "${PROJECT_DIR}" 2>&1
    fi

    echo "Setup complete."
    echo ""
}

# ── 测试 1: 基本编译检查 ──────────────────────────
test_compile() {
    echo "--- Test 1: Compilation check ---"

    if [ -x "${BUILD_DIR}/kvm_hid" ] && [ -x "${BUILD_DIR}/kvm_manager" ]; then
        echo "  PASS: Both binaries exist"
        file "${BUILD_DIR}/kvm_hid"
        file "${BUILD_DIR}/kvm_manager"
        return 0
    else
        echo "  FAIL: Missing binaries"
        return 1
    fi
}

# ── 测试 2: kvm_hid 命令行参数 ────────────────────
test_hid_args() {
    echo "--- Test 2: kvm_hid argument parsing ---"

    # 帮助信息
    "${BUILD_DIR}/kvm_hid" --help 2>&1 | grep -q "Usage:" || {
        echo "  FAIL: --help not showing usage"
        return 1
    }

    # 自定义参数（前台模式，短暂运行）
    if timeout 2 "${BUILD_DIR}/kvm_hid" -p "${TMPDIR}/test.sock" -d /dev/null -b 115200 -f \
        2>&1 | grep -q "Ready"; then
        rm -f "${TMPDIR}/test.sock"
        echo "  PASS: kvm_hid starts with custom args"
        return 0
    else
        rm -f "${TMPDIR}/test.sock" 2>/dev/null
        echo "  FAIL: kvm_hid did not start with custom args"
        return 1
    fi
}

# ── 测试 3: HID 帧转发链路 ────────────────────────
test_hid_forward() {
    echo "--- Test 3: HID frame forwarding chain ---"

    local SOCK="${TMPDIR}/kvm_hid.sock"
    local PTY_OUT="${TMPDIR}/ttyV0"
    local PTY_IN="${TMPDIR}/ttyV1"

    # 1. 创建虚拟串口对
    socat -d -d pty,link="${PTY_OUT}",raw,echo=0 \
             pty,link="${PTY_IN}",raw,echo=0 &
    SOCAT_PTY_PID=$!
    sleep 1

    if [ ! -e "${PTY_IN}" ]; then
        echo "  FAIL: socat virtual serial not created"
        return 1
    fi

    # 2. 在 PTY_OUT 侧监听（用 cat 读取）
    cat "${PTY_OUT}" > "${TMPDIR}/serial_output.bin" &
    CAT_PID=$!

    # 3. 启动 kvm_hid（前台模式，指向虚拟串口）
    timeout 10 "${BUILD_DIR}/kvm_hid" -p "${SOCK}" -d "${PTY_IN}" -b 115200 -f &
    HID_PID=$!
    sleep 1

    if [ ! -S "${SOCK}" ]; then
        echo "  FAIL: kvm_hid socket not created"
        return 1
    fi

    # 4. 通过 Unix Socket 发送 HID 鼠标帧
    # 鼠标帧: 0x02 0x00 X_lo X_hi Y_lo Y_hi
    printf '\x02\x00\x80\x08\x00\x10' | nc -w 1 -U "${SOCK}"
    sleep 1

    # 5. 检查串口输出
    kill "${CAT_PID}" 2>/dev/null || true
    local OUT_SIZE=0
    [ -f "${TMPDIR}/serial_output.bin" ] && OUT_SIZE=$(wc -c < "${TMPDIR}/serial_output.bin")

    if [ "$OUT_SIZE" -ge 6 ]; then
        echo "  PASS: HID frame forwarded through serial (${OUT_SIZE} bytes)"
        xxd "${TMPDIR}/serial_output.bin" | head -1
        ret=0
    else
        echo "  FAIL: Serial output only ${OUT_SIZE} bytes (expected ≥6)"
        ret=1
    fi

    # 清理
    kill "${HID_PID}" 2>/dev/null || true
    kill "${SOCAT_PTY_PID}" 2>/dev/null || true
    wait 2>/dev/null || true
    return $ret
}

# ── 测试 4: WebSocket 握手 ────────────────────────
test_ws_handshake() {
    echo "--- Test 4: WebSocket handshake ---"

    local SOCK="${TMPDIR}/hid.sock"
    rm -f "${SOCK}"

    # 启动 kvm_manager
    timeout 10 "${BUILD_DIR}/kvm_manager" -p 18765 -s "${SOCK}" -f &
    MGR_PID=$!
    sleep 1

    # 发送 HTTP Upgrade 请求（模拟 WebSocket 客户端）
    local KEY="dGhlIHNhbXBsZSBub25jZQ=="  # RFC 6455 示例 Key
    printf "GET / HTTP/1.1\r\nHost: localhost:18765\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" \
        "${KEY}" | nc -w 2 127.0.0.1 18765 > "${TMPDIR}/ws_response.txt" 2>&1 || true

    if grep -q "101 Switching Protocols" "${TMPDIR}/ws_response.txt" 2>/dev/null; then
        echo "  PASS: WebSocket handshake successful"
        grep "Sec-WebSocket-Accept" "${TMPDIR}/ws_response.txt"
    else
        echo "  FAIL: WebSocket handshake failed"
        cat "${TMPDIR}/ws_response.txt"
        return 1
    fi
}

# ── 测试 5: 完整链路测试（Python WS 客户端） ──────
test_full_chain() {
    echo "--- Test 5: Full chain (WS → kvm_manager → kvm_hid → serial) ---"

    local SOCK="${TMPDIR}/full_hid.sock"
    local PTY_OUT="${TMPDIR}/full_ttyV0"
    local PTY_IN="${TMPDIR}/full_ttyV1"
    rm -f "${SOCK}" "${PTY_OUT}" "${PTY_IN}"

    # 虚拟串口
    socat -d -d pty,link="${PTY_OUT}",raw,echo=0 \
             pty,link="${PTY_IN}",raw,echo=0 &
    local SOCAT2_PID=$!
    sleep 1

    # 串口监听
    cat "${PTY_OUT}" > "${TMPDIR}/full_serial.bin" &
    local CAT2_PID=$!

    # kvm_hid
    timeout 15 "${BUILD_DIR}/kvm_hid" -p "${SOCK}" -d "${PTY_IN}" -b 115200 -f &
    local HID2_PID=$!
    sleep 1

    # kvm_manager
    timeout 15 "${BUILD_DIR}/kvm_manager" -p 18766 -s "${SOCK}" -f &
    local MGR2_PID=$!
    sleep 1

    # Python WS 客户端：连接 + 发送 HID 帧
    python3 -c "
import struct, socket, base64, hashlib

def ws_connect(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    key = base64.b64encode(b'kvm-test-key').decode()
    req = (
        f'GET / HTTP/1.1\r\n'
        f'Host: {host}:{port}\r\n'
        f'Upgrade: websocket\r\n'
        f'Connection: Upgrade\r\n'
        f'Sec-WebSocket-Key: {key}\r\n'
        f'Sec-WebSocket-Version: 13\r\n\r\n'
    ).encode()
    sock.send(req)
    resp = sock.recv(4096)
    if b'101' not in resp:
        print('Handshake failed:', resp[:200])
        return None
    return sock

def ws_send_bin(sock, data):
    # RFC 6455: 客户端→服务端帧必须 masked
    import os
    mask = os.urandom(4)
    masked_data = bytes([b ^ mask[i % 4] for i, b in enumerate(data)])

    frame = bytearray()
    frame.append(0x82)  # FIN + BIN opcode
    if len(data) < 126:
        frame.append(0x80 | len(data))  # 0x80 = MASK bit
    else:
        frame.append(0x80 | 126)
        frame.extend(struct.pack('>H', len(data)))
    frame.extend(mask)
    frame.extend(masked_data)
    sock.send(frame)

# 测试用 HID 鼠标帧
hid_frame = bytes([0x02, 0x01, 0x00, 0x08, 0x00, 0x08])

sock = ws_connect('127.0.0.1', 18766)
if sock:
    print('Connected, sending HID frame...')
    ws_send_bin(sock, hid_frame)
    import time
    time.sleep(1)
    sock.close()
    print('Test complete')
else:
    exit(1)
    " 2>&1 | tee "${TMPDIR}/ws_client.log"

    sleep 1
    kill "${CAT2_PID}" 2>/dev/null || true

    local SERIAL_SIZE=0
    [ -f "${TMPDIR}/full_serial.bin" ] && SERIAL_SIZE=$(wc -c < "${TMPDIR}/full_serial.bin")

    if grep -q "Test complete" "${TMPDIR}/ws_client.log" 2>/dev/null; then
        echo "  Serial output: ${SERIAL_SIZE} bytes"
        if [ "${SERIAL_SIZE}" -ge 7 ]; then
            echo "  PASS: Full chain test passed"
            [ -f "${TMPDIR}/full_serial.bin" ] && xxd "${TMPDIR}/full_serial.bin" | head -2 || true
        else
            echo "  FAIL: Serial output too short (${SERIAL_SIZE}, expected ≥7)"
            return 1
        fi
    else
        echo "  FAIL: Full chain test failed (WS client error)"
        cat "${TMPDIR}/ws_client.log"
        return 1
    fi

    # 清理
    kill "${HID2_PID}" "${MGR2_PID}" "${SOCAT2_PID}" 2>/dev/null || true
    wait 2>/dev/null || true
    return 0
}

# ── 测试 6: 按需推流 ──────────────────────────────
test_stream_lifecycle() {
    echo "--- Test 6: Stream on-demand lifecycle (simulated) ---"

    local SOCK="${TMPDIR}/stream_hid.sock"
    rm -f "${SOCK}"

    # kvm_hid (mock)
    timeout 15 "${BUILD_DIR}/kvm_hid" -p "${SOCK}" -d /dev/null -b 115200 -f &
    local HID3_PID=$!
    sleep 1

    # kvm_manager (mjpg_streamer 可能不存在，看 fork 行为)
    timeout 15 "${BUILD_DIR}/kvm_manager" -p 18767 -s "${SOCK}" -f &
    local MGR3_PID=$!
    sleep 1

    # 检查服务监听
    if nc -z 127.0.0.1 18767 2>/dev/null; then
        echo "  PASS: kvm_manager listening on :18767"
    else
        echo "  FAIL: kvm_manager not listening"
        return 1
    fi

    # 清理
    kill "${HID3_PID}" "${MGR3_PID}" 2>/dev/null || true
    wait 2>/dev/null || true
    return 0
}

# ── 运行 ──────────────────────────────────────────
run_test() {
    local name="$1"
    local func="$2"

    echo ""
    if $func; then
        echo "  ✓ $name"
        PASS=$((PASS + 1))
    else
        echo "  ✗ $name"
        FAIL=$((FAIL + 1))
    fi
}

# ── Main ──────────────────────────────────────────
echo "============================================"
echo " IP-KVM Integration Test Suite"
echo " Project: ${PROJECT_DIR}"
echo " Binary:  ${BUILD_DIR}"
echo " Date:    $(date)"
echo "============================================"

setup

case "${1:-all}" in
    basic)
        run_test "Compilation check" test_compile
        run_test "kvm_hid arguments" test_hid_args
        run_test "WebSocket handshake" test_ws_handshake
        ;;
    hid)
        run_test "HID frame forwarding" test_hid_forward
        run_test "Full chain test" test_full_chain
        ;;
    stream)
        run_test "Stream lifecycle" test_stream_lifecycle
        ;;
    all)
        run_test "Compilation check" test_compile
        run_test "kvm_hid arguments" test_hid_args
        run_test "HID frame forwarding" test_hid_forward
        run_test "WebSocket handshake" test_ws_handshake
        run_test "Full chain test" test_full_chain
        run_test "Stream lifecycle" test_stream_lifecycle
        ;;
esac

# ── 结果 ──────────────────────────────────────────
echo ""
echo "============================================"
echo " Results: ${PASS} passed, ${FAIL} failed"
echo "============================================"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
