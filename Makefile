# Makefile — IP-KVM 系统
# 目标: kvm_hid (HID守护进程) + kvm_manager (WebSocket服务器+推流管理)
#
# 使用:
#   make              — 编译两个二进制
#   make kvm_hid      — 仅编译 HID 守护进程
#   make kvm_manager  — 仅编译 WebSocket 服务器
#   make clean        — 清除产物
#   make install      — 安装到 OpenWRT 目标路径
#   CROSS=      — 交叉编译前缀，如 CROSS=mipsel-linux-musl-

CROSS   ?=
CC      ?= $(CROSS)gcc
LD      ?= $(CROSS)ld
STRIP   ?= $(CROSS)strip

CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -lpthread

SRC_DIR := src
BUILD_DIR := build

# 源文件
SRCS := $(SRC_DIR)/kvm_hid.c $(SRC_DIR)/kvm_manager.c
OBJS := $(BUILD_DIR)/kvm_hid.o $(BUILD_DIR)/kvm_manager.o
BINS := $(BUILD_DIR)/kvm_hid $(BUILD_DIR)/kvm_manager

.PHONY: all clean install

all: $(BINS)

# kvm_hid — HID 守护进程
$(BUILD_DIR)/kvm_hid: $(BUILD_DIR)/kvm_hid.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@ 2>/dev/null || true

$(BUILD_DIR)/kvm_hid.o: $(SRC_DIR)/kvm_hid.c
	$(CC) $(CFLAGS) -c -o $@ $<

# kvm_manager — WebSocket 服务器
$(BUILD_DIR)/kvm_manager: $(BUILD_DIR)/kvm_manager.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@ 2>/dev/null || true

$(BUILD_DIR)/kvm_manager.o: $(SRC_DIR)/kvm_manager.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)/*.o $(BINS)

install: $(BINS)
	cp $(BUILD_DIR)/kvm_hid /usr/sbin/kvm_hid
	cp $(BUILD_DIR)/kvm_manager /usr/sbin/kvm_manager
	cp -r www/kvm /www/kvm
	cp deploy/init.d/kvm /etc/init.d/kvm
	cp deploy/sysctl.conf /etc/sysctl.d/99-kvm.conf
	cp deploy/uvcvideo.conf /etc/modprobe.d/uvcvideo.conf
	chmod +x /etc/init.d/kvm
	@echo "Installation complete. Run: /etc/init.d/kvm start"
