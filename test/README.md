# 测试指南

## 前置条件

```bash
# Ubuntu/Debian
sudo apt install socat netcat-openbsd python3 xxd

# macOS
brew install socat netcat

# OpenWRT
opkg install socat python3
```

## 运行测试

```bash
# 先编译
cd /path/to/ip-kvm
make

# 运行全部测试
./test/integration.sh

# 运行特定模块
./test/integration.sh basic    # 编译检查 + 参数解析
./test/integration.sh hid      # HID 帧转发测试
./test/integration.sh stream   # 推流生命周期测试

# 带详细输出
bash -x ./test/integration.sh basic
```

## 测试架构

```
┌──────────────────┐     WS BIN      ┌────────────────┐     Unix Socket
│  Python WS Client │ ──────────────► │  kvm_manager   │ ──────────────► ┌──────────┐
│  (test script)    │   HID 二进制帧  │  :8765         │                 │ kvm_hid  │
└──────────────────┘                 └────────────────┘                 └─────┬────┘
                                                                              │ UART
                                                                              ▼
                                                                       ┌──────────────┐
                                                                       │  socat 虚拟   │
                                                                       │  串口对       │
                                                                       └──────┬───────┘
                                                                              │
                                                                       ┌──────▼───────┐
                                                                       │ 监听端验证    │
                                                                       │ (xxd 检查帧)  │
                                                                       └──────────────┘
```

## 测试内容

| # | 测试 | 描述 | 验证点 |
|---|------|------|--------|
| 1 | 编译检查 | 两个二进制编译成功 | 可执行文件存在 |
| 2 | 参数解析 | kvm_hid 命令参数正常 | --help, -p, -d 参数 |
| 3 | HID 转发 | kvm_hid 正确转发帧到 UART | 串口收到 ≥6 字节 |
| 4 | WS 握手 | kvm_manager 完成 WebSocket 握手 | 返回 101 + Accept |
| 5 | 全链路 | WS → manager → hid → UART | 端到端帧传输 |
| 6 | 推流生命周期 | 服务正常监听 | 端口可连接 |

## 常见问题

**socat 不工作：** 确认 `VMIN=0 VTIME=0` 设置使串口非阻塞。

**nc 连接超时：** 增加 `-w` 参数的超时值。

**Python ws_client 报错：** 确认已安装 python3，且系统有标准库。
