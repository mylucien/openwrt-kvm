# IP-KVM 系统

基于 **OpenWRT + MS2130 + CH9329** 的 IP-KVM 方案。  
通过浏览器远程监控目标电脑 HDMI 画面 + 键鼠控制，**被控端零软件安装**。

## 硬件连接拓扑

```
目标电脑
  ├── HDMI 输出 ──────► MS2130 (HDMI→USB) ──► OpenWRT USB3 (/dev/video0)
  └── USB-A 口 ◄────── CH9329 (UART→USB HID) ──► OpenWRT UART (/dev/ttyS1)
```

### 硬件清单

| 组件 | 型号 | 作用 |
|------|------|------|
| 采集卡 | MS2130 芯片 (HDMI→USB) | 硬件 MJPEG 编码，USB Video Class |
| HID 模块 | CH9329 (UART→USB HID) | 模拟标准 USB 键盘鼠标 |
| 路由器 | 任意 OpenWRT 设备 (需 USB3 + UART) | 主控，运行两个 C 二进制 |
| 网线 | 千兆以太网 | 局域网视频+控制链路 |

### CH9329 接线

| CH9329 引脚 | OpenWRT UART |
|-------------|--------------|
| TXD | RXD (GPIO 对应引脚) |
| RXD | TXD (GPIO 对应引脚) |
| VCC | 3.3V |
| GND | GND |

> ⚠️ CH9329 5V 供电性能更稳定——如果 OpenWRT 不提供 5V 输出，从 USB 取电。

## 软件架构

```
┌─ 视频链路 ─────────────────────────────────┐
│  HDMI → MS2130 → /dev/video0              │
│  → mjpg_streamer → HTTP :8080 → 浏览器     │
└────────────────────────────────────────────┘

┌─ 键鼠链路 ─────────────────────────────────┐
│  浏览器 → WebSocket :8765 → kvm_manager    │
│  → Unix Socket → kvm_hid → UART           │
│  → CH9329 → USB HID → 目标电脑             │
└────────────────────────────────────────────┘
```

### 两个二进制

| 进程 | 文件 | 职责 |
|------|------|------|
| **kvm_hid** | `src/kvm_hid.c` | 监听 Unix Socket，接收 HID 帧，写入 UART→CH9329 |
| **kvm_manager** | `src/kvm_manager.c` | WebSocket 服务器 + 按需启停 mjpg_streamer + 看门狗 |

## 快速开始

```bash
# 编译
make

# 运行（开发机测试）
# 需要预先安装 mjpg_streamer

# 部署到 OpenWRT
make install
/etc/init.d/kvm enable
/etc/init.d/kvm start
```

## 目录结构

```
ip-kvm/
├── Makefile             # 构建系统
├── README.md            # 本文件
├── src/
│   ├── kvm_hid.c        # HID 守护进程
│   └── kvm_manager.c    # WebSocket 服务器 + 推流管理
├── www/kvm/
│   ├── index.html       # 前端网页
│   ├── kvm.js           # 前端逻辑（视频+键鼠）
│   └── style.css        # 样式
├── deploy/
│   ├── init.d/kvm       # OpenWRT 开机自启
│   ├── sysctl.conf      # 网络内核参数
│   ├── uvcvideo.conf    # UVC 驱动参数
│   └── README.md        # 部署说明
├── test/
│   ├── integration.sh   # 集成测试（模拟硬件）
│   └── README.md        # 测试说明
└── build/               # 编译产物
```

## HID 二进制协议

```
WebSocket → kvm_manager → kvm_hid (Unix Socket) → UART → CH9329

帧格式（固定长度 6 字节）:
  Byte 0:  类型 (0x01=键盘, 0x02=鼠标)
  Byte 1:  按键/按钮 (键盘: 修饰键, 鼠标: 按键位掩码)
  Byte 2-3: X 坐标 (0-4095, Little-Endian) — 鼠标时有效
  Byte 4-5: Y 坐标 (0-4095, Little-Endian) — 鼠标时有效
  校验:    所有字节之和 & 0xFF (在帧尾部或附加字节)

注意: CH9329 具体协议需参考芯片手册，此处为设计框架。
实际帧格式根据 CH9329 数据手册调整。
```

## 系统调优

```bash
# USB autosuspend 关闭
echo -1 > /sys/module/usbcore/parameters/autosuspend

# UVC 驱动
echo 'options uvcvideo nodrop=1 quirks=0x80' > /etc/modprobe.d/uvcvideo.conf

# 网络缓冲区
sysctl -w net.core.wmem_max=4194304
sysctl -w net.core.rmem_max=4194304

# HID 进程实时优先级
chrt -f 50 /usr/sbin/kvm_hid &
