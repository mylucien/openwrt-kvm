# 部署指南 — IP-KVM 在 OpenWRT 上的安装

## 前提

- OpenWRT 路由器/开发板
- MS2130 采集卡（USB Video Class，硬件 MJPEG）
- CH9329 串口转 USB HID 模块
- 已安装 opkg 和基础开发工具

## 第一步：安装依赖

```bash
opkg update
opkg install kmod-usb-video-uvc kmod-video-core mjpg-streamer
opkg install setserial  # 可选，用于串口低延迟调优
```

## 第二步：验证硬件

```bash
# MS2130 采集卡
ls /dev/video*
v4l2-ctl --device=/dev/video0 --list-formats-ext

# 期望输出中包含 MJPG (Motion-JPEG, compressed)
# 若只有 YUYV → 检查 UVC 驱动版本

# CH9329 UART
ls /dev/ttyS*
# 确认 CH9329 对应的串口设备
```

## 第三步：编译

在 OpenWRT 上直接编译：

```bash
# 安装编译器
opkg install gcc

# 编译
cd /root/ip-kvm
gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L \
    -lpthread -o /usr/sbin/kvm_hid src/kvm_hid.c
gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L \
    -lpthread -o /usr/sbin/kvm_manager src/kvm_manager.c
```

或在开发机上交叉编译（见 `../build.sh`）。

## 第四步：安装

```bash
# 前端文件
cp -r www/kvm /www/kvm

# 内核参数
cp deploy/sysctl.conf /etc/sysctl.d/99-kvm.conf
sysctl -p /etc/sysctl.d/99-kvm.conf

# UVC 驱动参数
cp deploy/uvcvideo.conf /etc/modprobe.d/uvcvideo.conf

# 开机自启
cp deploy/init.d/kvm /etc/init.d/kvm
chmod +x /etc/init.d/kvm
/etc/init.d/kvm enable
```

## 第五步：启动

```bash
/etc/init.d/kvm start
/etc/init.d/kvm status
```

## 验证

```bash
# 检查进程
pidof kvm_hid kvm_manager mjpg_streamer

# 检查端口
netstat -an | grep -E '8765|8080'

# 浏览器打开
# http://路由器IP:8080/kvm/
```

## CH9329 接线确认

| CH9329 引脚 | OpenWRT UART |
|-------------|-------------|
| TXD         | RXD         |
| RXD         | TXD         |
| VCC         | 3.3V 或 5V  |
| GND         | GND         |

> ⚠️ CH9329 在某些 OpenWRT 板上需要 5V 供电才能稳定工作。
> 如果板载不提供 5V，可以从 USB 口取电。

## 性能验证

```bash
# 查看 CPU 占用
top

# 查看网络延迟（目标机上）
ping 路由器IP

# 观察 kvm_manager 日志
logread | grep kvm
```
