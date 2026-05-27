#!/bin/sh
# build.sh — IP-KVM 交叉编译脚本
#
# 用法：
#   ./build.sh                      # 本机编译
#   ./build.sh mipsel               # MIPS (Little-Endian) — MT7621 等
#   ./build.sh mips                 # MIPS (Big-Endian)
#   ./build.sh arm                  # ARM 32-bit — 树莓派等
#   ./build.sh aarch64              # ARM 64-bit — 新款路由/开发板
#   CROSS_PREFIX=/path/to/sdk ./build.sh mipsel  # 指定 SDK 路径
#
# 环境变量：
#   CROSS_PREFIX    — 交叉编译工具链前缀目录
#   JOBS            — 并行编译任务数（默认 4）
#
# 示例（使用 OpenWRT SDK）：
#   CROSS_PREFIX=~/openwrt-sdk/staging_dir/toolchain-mipsel_24kc_gcc-12.3.0_musl \
#   ./build.sh mipsel

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
JOBS="${JOBS:-4}"

# ── 目标架构配置 ─────────────────────────────────
case "${1:-native}" in
    native)
        CROSS=""
        ARCH_DIR="${BUILD_DIR}/native"
        ;;
    mipsel)
        # MIPS 小端 — MT7620/MT7621 (OpenWRT 常见)
        ARCH="mipsel"
        CROSS="${ARCH}-linux-musl-"
        ARCH_DIR="${BUILD_DIR}/${ARCH}"
        ;;
    mips)
        # MIPS 大端
        ARCH="mips"
        CROSS="${ARCH}-linux-musl-"
        ARCH_DIR="${BUILD_DIR}/${ARCH}"
        ;;
    arm)
        # ARM 32-bit — Cortex-A7/A53
        ARCH="arm"
        CROSS="${ARCH}-linux-musleabi-"
        ARCH_DIR="${BUILD_DIR}/${ARCH}"
        ;;
    aarch64)
        # ARM 64-bit — Cortex-A53/A72
        ARCH="aarch64"
        CROSS="${ARCH}-linux-musl-"
        ARCH_DIR="${BUILD_DIR}/${ARCH}"
        ;;
    *)
        echo "Usage: $0 [native|mipsel|mips|arm|aarch64]"
        echo "Default: native"
        exit 1
        ;;
esac

# ── 工具链路径 ───────────────────────────────────
if [ -n "$CROSS_PREFIX" ]; then
    export PATH="${CROSS_PREFIX}/bin:${PATH}"
fi

CC="${CROSS}gcc"
STRIP="${CROSS}strip"

# ── 输出目录 ─────────────────────────────────────
mkdir -p "${ARCH_DIR}"

echo "============================================"
echo " IP-KVM Build"
echo " Target:      ${1:-native}"
echo " CC:          ${CC}"
echo " Output:      ${ARCH_DIR}"
echo " Jobs:        ${JOBS}"
echo "============================================"

# ── 编译 flags ──────────────────────────────────
CFLAGS="-O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L"
LDFLAGS="-lpthread"

# ── 编译 kvm_hid ─────────────────────────────────
echo ""
echo ">>> Building kvm_hid..."
${CC} ${CFLAGS} -o "${ARCH_DIR}/kvm_hid" \
    "${PROJECT_DIR}/src/kvm_hid.c" ${LDFLAGS}

if command -v "${STRIP}" >/dev/null 2>&1; then
    "${STRIP}" "${ARCH_DIR}/kvm_hid" 2>/dev/null || true
fi

echo "  -> ${ARCH_DIR}/kvm_hid"
${CC} --version | head -1

# ── 编译 kvm_manager ─────────────────────────────
echo ""
echo ">>> Building kvm_manager..."
${CC} ${CFLAGS} -o "${ARCH_DIR}/kvm_manager" \
    "${PROJECT_DIR}/src/kvm_manager.c" ${LDFLAGS}

if command -v "${STRIP}" >/dev/null 2>&1; then
    "${STRIP}" "${ARCH_DIR}/kvm_manager" 2>/dev/null || true
fi

echo "  -> ${ARCH_DIR}/kvm_manager"

# ── 文件信息 ─────────────────────────────────────
echo ""
echo ">>> Build artifacts:"
ls -lh "${ARCH_DIR}/kvm_hid" "${ARCH_DIR}/kvm_manager"

echo ""
echo ">>> Build complete!"
echo "    Copy to target:"
echo "    scp ${ARCH_DIR}/kvm_hid root@192.168.1.1:/usr/sbin/"
echo "    scp ${ARCH_DIR}/kvm_manager root@192.168.1.1:/usr/sbin/"
