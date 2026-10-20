# Nokia XG-040G-MD 光猫 KVM 设计文档

版本 v0.4.0 · 2026-10-18 · 基于 ponwrt（ImmortalWrt）· 已移除电源控制，只保留视频与键鼠

## 0. 变更记录

### v0.4（移除电源控制，只保留视频、键盘、鼠标）

以下历史版本里涉及电源、光耦、GPIO、LED oneshot 的条目已不再适用。

| # | 改动 | 涉及章节 | 原因 |
| --- | --- | --- | --- |
| 23 | **删除电源控制**：kvmd 里的电源状态机、LED sysfs 与 oneshot 脉冲、`/api/power/*`、`power_led`、`press_ms`、`force_ms`、`cycle_wait_s`、`min_interval_s`、`pre_press` 选项，网页的电源页，LuCI 的电源页与脉冲测试按钮，`nas-kvm-ctl power-*`，`kmod-ledtrig-oneshot` 依赖，rpcd 对 `/sys/class/leds` 的读权限，`test_oneshot.py` | 1、2、3、4、5.1、5.4 至 5.8、6、7、8、9、10 | 预留 LED 焊位的 GPIO 查不到，也不再需要通过光猫开关机；不再需要修改设备树 |
| 24 | **开机宏只发键**：去掉「开机前先短按电源」与 `waiting` 状态，按钮改为「连发启动菜单键 / BIOS 键」，由用户在持续时间内自己给 NAS 开机 | 5.5、5.7 | 没有电源脉冲可用 |
| 25 | **驱动依赖**：新增 `kmod-usb-acm`，与 `kmod-usb-serial-ch341` 并存 | 2、4.2、11 | 实测的串口适配器在 Windows 里是 CDC 串口（USB ID 0843:5740），不是 CH340，光猫上对应 `cdc-acm` 与 `/dev/ttyACM0` |
| 26 | **LuCI 新增「扫描串口设备」**：列出当前 USB 串口与 USB ID，对不在白名单里的一键加入 `serial_ids`（之后点保存并应用） | 5.6、5.8 | 适配器的 USB ID 会因型号而异，不让用户手敲 `vid:pid`；仍保留手动指定 `serial` 作为兜底 |
| 27 | **绝对鼠标坐标 (0,0) 改发 (1,1)** | 5.3 | 在 Windows 上实测，x、y 同时为 0 的绝对坐标报告被忽略，(1,1) 正常落在屏幕左上角 |
| 28 | **升级迁移**：`uci-defaults` 清除旧配置里的 6 个电源选项；kvmd 对配置文件里的未知键本来就忽略，旧配置也能正常启动 | 5.6 | 避免旧选项残留 |
| 29 | 新增 `tools/kvm_hw_test.py`：在 Windows 上验证 CH340G/CH9329 成品线（串口探测、键盘、绝对鼠标、按键保持时间、吞吐） | 7、10 | M2 的实测工具，已在真实硬件上跑通 |

### v0.3.2（兼容性评审后：不能影响系统里的其它应用）

| # | 改动 | 涉及章节 | 原因 |
| --- | --- | --- | --- |
| 17 | **`disable-upstream` 只关闭与本包冲突的上游 section**（设备或端口相同）；有冲突时对上游做一次 `restart`，没有冲突时完全不碰；无关实例不受影响 | 5.2、5.9、6、8 | v0.3.1 把所有上游 section 置 0 并 stop 上游，且每次启动都重做，会让系统里另一个摄像头的 ustreamer 实例无法使用，用户重新启用也会被改回去 |
| 18 | 默认端口改为 **18080 / 18081**；init 在启动每个 instance 前用 `port-check` 检查端口，被别的程序占用就跳过并在日志里写明占用者与要改的选项 | 5.6、5.9、8 | 8080 等常用端口容易与其它应用冲突；同一端口不论谁先监听，另一方都会绑定失败，可能让对方应用起不来 |
| 19 | 串口自动探测改为**只打开 USB VID:PID 在 `serial_ids` 里的设备**（默认 1a86:7523、1a86:5523），其它串口一个字节都不发；新增 UCI 选项 `serial_ids`、LuCI 字段和 `nas-kvm-ctl serial-scan` | 5.3、5.6、8 | 原实现取第一个 ttyUSB\* 或 ttyACM\*，系统里有 4G 模块、UPS 等串口设备时，会改它的 termios 并每 2 秒发探测帧 |
| 20 | 勘误：v0.3.1 文档把「全部置 0 并停掉上游」「用户重新启用也会被纠正」写成特性，本版撤销 | 5.2 | 与「不能影响系统其它应用」冲突 |
| 21 | 新增测试 `test_ctl.py`（35 项）、`test_serial_detect.py`（15 项）、`test_init.py`（14 项）；shell 脚本在 busybox 1.36 下复测；前端冒烟测试在 jsdom 上实跑 | 10 | 覆盖本版全部改动 |
| 22 | 新增 **GitHub Actions 自动编译流程**（主机测试、aarch64 与 x86_64 编译、打 tag 发布 Release）；文档里的「ipk」改称「软件包」 | 11、全文 | 需要在 CI 里编译；ponwrt 的 `configs/an7581.config` 里 `CONFIG_USE_APK=y` 且未选 opkg，设备上用的是 apk，装不了 .ipk |

### v0.3.1（代码评审后）

| # | 改动 | 涉及章节 | 原因 |
| --- | --- | --- | --- |
| 11 | **与上游 ustreamer 包共存**：上游包自带 `/etc/config/ustreamer`（默认 `enabled '1'`，监听 `[::]:8080`，640x480）、`/etc/init.d/ustreamer`（START=90）和 USB hotplug（插入即 `start`）。新增 `nas-kvm-ctl disable-upstream`，在安装时（uci-defaults）和每次 `nas-kvm` 启动时把上游所有 `enabled` 为真的 section 置 0（v0.3.2 起改为只处理冲突的 section，见第 17 条） 并停止其实例 | 4.2、5.2、5.9、6 | 否则上游实例先于 nas-kvm 占住 `/dev/video0` 与 8080：kvmd 绑定失败、我们的 ustreamer 重启循环，且上游视频流无认证对所有接口开放 |
| 12 | 电源 LED 脉冲优先使用内核 **oneshot 触发器**（`kmod-ledtrig-oneshot`），脉冲宽度由内核计时；不可用时退回写 brightness 并在日志告警 | 5.4、8 | kvmd 被 SIGKILL 或 OOM 杀掉时，原方案 LED 可能停在亮的状态，NAS 看到电源键一直被按住 |
| 13 | `/api/config` 改为**先整体校验、再统一应用**；存在但格式错误的值（如 `"press_ms":"abc"`）也返回 400，不再被静默忽略 | 5.7 | 原实现边校验边写内存，中途遇到非法值时前面的项已被改动 |
| 14 | 波特率校验限定为 1200、2400、4800、9600、19200、38400、57600、115200 | 5.3、5.6 | 原配置校验允许 1200 到 115200 任意整数，非标准值会静默落到 9600 |
| 15 | 串口链路测试改为**异步**：等待应答期间 HTTP、WebSocket、宏继续处理 | 5.3 | 原实现最多阻塞主循环 400ms |
| 16 | 勘误：v0.3 文档称 `/api/config`「整批拒绝，不部分写入」，但 v0.3 代码并非如此；v0.3.1 代码已与文档一致，并有测试覆盖 | 5.7 | 文档与实现不一致 |

### v0.3

| # | 改动 | 涉及章节 | 原因 |
| --- | --- | --- | --- |
| 1 | ustreamer 与 kvmd 统一绑定 br-lan 的 IP，不再使用 `0.0.0.0` | 3、5.2、5.9、6 | ustreamer 自身没有认证，原示例与「只绑 br-lan」矛盾 |
| 2 | kvmd 按波特率自适应降低鼠标发送频率；建议先问卖家能否用 115200 | 5.3 | 9600 下 60Hz 绝对鼠标会占满约 80% 串口带宽 |
| 3 | 启动菜单键默认值由 F2 改为 F11 | 5.5、5.6、9 | 多数主板 F2 是进 BIOS，启动菜单常见为 F11 或 F12 |
| 4 | 光耦接线补充实测项：PWR\_SW 空载电压、闭合压降、光耦耐压 | 2、7（M3） | 待机脚可能是 3.3V 或 5V，需确认光耦输出端耐压与饱和压降 |
| 5 | 安全章节补充：HTTP 明文 Token 的限制、防重放、连接数上限 | 6 | 明确 LAN 内可接受的边界，远程仅走 WireGuard |
| 6 | 配置项新增：`enabled`、`bind`、`video_device`、`resolution`、`desired_fps`、`video_port`、`cycle_wait_s`、`min_interval_s`、`pre_press`、`key_hold_ms`、`nas_ip` | 5.6 | 去掉硬编码，页面可配置 |
| 7 | 新增配置页 `luci-app-nas-kvm`（方案 A）；KVM 控制页只保留开机宏设置 | 5.8 | 与光猫登录态统一，其余设置在 LuCI |
| 8 | WebSocket 确定为自写最小实现，不依赖 libwebsockets；`nas-power` 脚本由 kvmd 内置电源状态机取代 | 5.1、5.4 | 少依赖、省闪存，并在 kvmd 内统一互斥与间隔 |
| 9 | 明确 kvmd 自带 HTTP 服务；uhttpd 只有 LuCI 需要 | 4.2 | 避免误以为 KVM 页依赖 uhttpd |
| 10 | 新增 HTTP / WebSocket 协议章节与源码测试附录 | 5.7、10 | 前后端对接与验收依据 |

## 1. 目标与范围

**目标**：在光猫（AN7581，OpenWrt）上用一个软件包实现对 NAS 主机的远程控制：

1. HDMI 视频回传（MS2130 采集卡）
2. 远程键盘与鼠标（CH9329 加 CH340G 成品线）
3. 开机宏：连发启动菜单键或 BIOS 设置键（由用户自己给 NAS 开机），按键在页面上选择

**前提**：固件基于 ponwrt 自行编译。

**交付物**：`nas-kvm`（守护进程、控制页、init 脚本、默认配置）与 `luci-app-nas-kvm`（LuCI 配置页）两个包。

## 2. 硬件与接口分配

| 接口 | 设备 | 说明 |
| --- | --- | --- |
| 光猫 USB3.0（usb0） | MS2130 | dts 中 usb0 带 USB2 与 USB3 PHY。不要用 usb-sfp 变体，它会把 usb0 降为仅 USB2 |
| 光猫 USB2.0（usb1） | CH9329 加 CH340G 成品线的串口端 | usb1 仅 USB2；驱动为 ch341（CH340G）或 cdc-acm（CDC 串口，实测适配器即此类），设备节点为 /dev/ttyUSB0 或 /dev/ttyACM0 |
| 光猫板载 uart1 | 不使用 | 保持为控制台，用于调试和救砖 |
| NAS USB 口 | 成品线的 HID 端 |  |
| NAS HDMI 输出 | MS2130 的 HDMI 输入 |  |
| NAS 网卡 | 光猫 LAN 口 | 用于网页访问 |

两个 USB 口共用一个 5V 开关（gpio24，dts 中的 usb-power 节点），设备默认带 kmod-regulator-userspace-consumer，可在用户态整体断电再上电。软复位会让 MS2130 和成品线同时重新枚举。

### 成品线选购要点

1. **必须是键鼠模拟线，不是普通公对公数据线。** 普通线两端直连，两边 5V 会短接，可能烧口。
2. **问清默认波特率和工作模式。** CH9329 出厂默认 9600，商家可能改过；要确认是标准协议传输模式，能用 0x57 0xAB 帧头的命令控制，并支持绝对鼠标。**同时问能否改到 115200**（见 5.3）。
3. **确认串口芯片类型。** CH340G 常见 VID:PID 为 1a86:7523，内核 ch341 驱动支持；也可能是 CDC 串口（如实测的 0843:5740，`cdc-acm` 驱动）。两种都可用，固件已同时依赖这两个驱动；非 CH340 的 ID 用 LuCI 的「扫描串口设备」加入白名单。

## 3. 总体架构

```
浏览器 --HTTP/WebSocket--> 光猫 br-lan IP（只绑内网地址，不绑 0.0.0.0）
  |- ustreamer :18081 <-- /dev/video0 <-- MS2130 (USB3) <-- NAS HDMI
  |- nas-kvmd  :18080  单线程 poll 事件循环，自带 HTTP / WebSocket
  |    |- 静态网页（/www/kvm）+ REST API + WebSocket（键盘、鼠标事件，状态推送）
  |    |- /dev/ttyUSB0 --> 成品线 (CH340G + CH9329) --USB--> NAS 键鼠
  |    |- 开机宏（连发启动菜单键 / BIOS 键）
  |    |- 异步子进程 nas-kvm-ctl probe：HDMI 信号（ustreamer /state）与 NAS Ping
  |- 板载 uart1：控制台，不使用

LuCI（uhttpd :80）：服务 › NAS KVM = luci-app-nas-kvm，读写 /etc/config/nas-kvm，控制服务
```

kvmd 额外监听 127.0.0.1，供 `nas-kvm-ctl`（LuCI 的链路测试按钮）调用，不对外暴露。

## 4. 固件改动（ponwrt）

### 4.1 设备树

**不需要修改设备树。** 电源控制已移除，不再占用预留 LED 焊位的 GPIO；USB 口的 5V 开关（gpio24，usb-power 节点）保持固件默认即可。

### 4.2 menuconfig

| 项 | 状态 |
| --- | --- |
| kmod-video-uvc | 需新增勾选（`nas-kvm` 已依赖） |
| ustreamer | 需新增勾选（`nas-kvm` 已依赖）。注意上游包会同时装上自己的 config、init 与 USB hotplug，`nas-kvm` 会把其中的实例关闭（见 5.2） |
| kmod-usb-serial-ch341 | 需新增勾选（`nas-kvm` 已依赖） |
| kmod-usb-acm | 需新增勾选（`nas-kvm` 已依赖）；串口适配器是 CDC 类型时需要，设备节点为 /dev/ttyACM0 |
| kmod-usb3、kmod-regulator-userspace-consumer | 设备默认已包含 |
| nas-kvm（本项目软件包） | 新增 |
| luci-app-nas-kvm（本项目软件包） | 新增；依赖 luci-base 与 uhttpd |
| uhttpd | 仅 LuCI 需要；**KVM 控制页由 kvmd 自带 HTTP 提供，不依赖 uhttpd** |
| kmod-usb-storage、kmod-fs-ext4、block-mount、dnsmasq TFTP | 不再需要，默认带着无妨 |

### 4.3 防火墙

不新增任何规则。LAN 到路由器本机默认放行，WAN 默认拒绝，保持即可；不做端口映射。

## 5. 软件包设计

### 5.1 目录结构

```
nas-kvm/
  Makefile
  src/
    kvmd.c            事件循环、HTTP/WebSocket、串口、开机宏、API
    ch9329.c/.h       CH9329 组帧、校验和、应答解析
    hid_map.c/.h      浏览器 KeyboardEvent.code 到 HID usage 的映射；宏键名映射
    sha1b64.c/.h      WebSocket 握手所需的 SHA-1 与 Base64
  files/
    etc/config/nas-kvm    默认配置（UCI）
    etc/init.d/nas-kvm    procd 启动脚本
    etc/uci-defaults/90-nas-kvm   安装/首次启动时关掉上游 ustreamer 的默认实例
    usr/bin/nas-kvm-ctl   辅助脚本（解析绑定地址、探测、写 UCI、本机 API 调用）
    www/kvm/index.html    KVM 控制页（视频、键鼠、开机宏）
  tests/                  见第 10 节
  tools/kvm_hw_test.py    Windows 上的成品线硬件验证脚本（M2，见第 10 节）

luci-app-nas-kvm/
  Makefile
  htdocs/luci-static/resources/view/nas-kvm/config.js
  root/usr/share/luci/menu.d/luci-app-nas-kvm.json
  root/usr/share/rpcd/acl.d/luci-app-nas-kvm.json
```

语言用 C，无第三方库依赖（只依赖 libc），避免 Go 或 Python 占用闪存。WebSocket 为自写最小实现（仅文本帧、客户端必须掩码、不支持分片）。x86-64 上 `-Os` 剥离后约 44KB。

### 5.2 视频

ustreamer 由 procd 托管，监听 br-lan 的 IP（**不是 0.0.0.0**），参数由 init 脚本从 UCI 生成：

```
ustreamer --device /dev/video0 --format MJPEG --resolution 1920x1080 \
          --host <br-lan IP> --port 18081 --desired-fps 30
```

具体参数以 `ustreamer --help` 为准。

**与上游 ustreamer 包共存。** 上游 `multimedia/ustreamer` 除二进制外还装了三样东西：`/etc/config/ustreamer`（默认 `enabled '1'`、设备 `/dev/video0`、监听 `::` 的 8080、640x480）、`/etc/init.d/ustreamer`（`START=90`，早于本包的 `START=95`）、`/etc/hotplug.d/usb/20-ustreamer`（USB `add` 时只要存在 `/dev/video*` 就无条件 `/etc/init.d/ustreamer start`，USB 软复位也会触发）。放着不管，MS2130 一出现上游实例就先占住 `/dev/video0` 与 8080，导致 kvmd 绑定失败、本包的 ustreamer 打不开设备而重启循环，并且上游视频流没有认证、对所有接口开放。

处理方式：上游 init 只启动 `enabled` 为真的 `ustreamer` section。`nas-kvm-ctl disable-upstream` **只关闭与本包冲突的 section**：`device` 与本包的 `video_device` 相同，或 `port` 等于本包的 `listen_port` 或 `video_port`（section 里没写时按上游 init 的缺省值补齐：device 为 `/dev/video0`，port 为 `8080`）。冲突的 section 置 0 并提交后，对上游 init 做一次 `restart`，之后 hotplug 再调 `start` 就是空操作。**无关的实例（例如另一个摄像头）保持原样，不会被关闭，也不会被停掉**；没有冲突时完全不碰上游（不 commit、不 restart），所以每次启动都执行也没有副作用。如果用户之后又启用了一个冲突的实例，下一次启动会再次关闭它并在日志里点名。执行时机有两处：安装与首次启动时的 `/etc/uci-defaults/90-nas-kvm`，以及每次 `nas-kvm` 启动前。不删除、不修改上游包的任何文件，所以 sysupgrade 与升级都不受影响。MS2130 硬件输出 MJPEG，光猫只转发，不转码。若 USB 带宽或供电不稳，降到 720p。前端用 `<img src="http://光猫IP:18081/stream">` 直接加载 MJPEG 流，失败时每 3 秒重试。

### 5.3 键鼠：CH9329 协议

帧格式：0x57 0xAB、地址（默认 0x00）、命令、数据长度、数据、校验和（前面所有字节相加取低 8 位）。以下命令凭资料整理，开发时必须对照 CH9329 数据手册核对：

| 命令 | 含义 |
| --- | --- |
| 0x01 | 获取芯片信息（应答命令字为 0x81，含芯片版本与 USB 是否已枚举） |
| 0x02 | 键盘：8 字节（修饰键、保留、6 个按键） |
| 0x04 | 绝对鼠标：0x02、按键、X 低高、Y 低高、滚轮 |
| 0x05 | 相对鼠标（本版本未使用） |

示例：按下 a（HID usage 0x04）为 57 AB 00 02 08 00 00 04 00 00 00 00 00 10；全部释放为 57 AB 00 02 08 00 00 00 00 00 00 00 00 0C。这两帧已在自动化测试中与 kvmd 的实际输出逐字节核对。

设计要点：

- 串口默认 9600，波特率在配置里改，以成品线实际为准；取值限定为 1200、2400、4800、9600、19200、38400、57600、115200，其它值在配置校验时被拒绝（配置文件里的非法值保留默认并在日志提示）。`serial` 留空时自动探测，规则是**只打开 USB VID:PID 在 `serial_ids`（默认 1a86:7523、1a86:5523）里的 /dev/ttyUSB\* 或 /dev/ttyACM\***：从 `/sys/class/tty/<名>/device` 向上最多找 4 级的 `idVendor`、`idProduct`（ttyUSB 的 `device` 是 usb-serial 端口，USB 设备在上两级；ttyACM 的 `device` 是接口，在上一级，所以逐级找）。4G 模块、UPS 等其它串口设备一律不碰，一个字节都不发；查不到 USB ID 的口同样不碰，日志提示改用 `serial` 手动指定。手动指定 `serial` 后不再检查 USB ID，LuCI 页面可设置。在光猫上执行 `nas-kvm-ctl serial-scan` 可列出各串口的 USB ID 以及是否会被选中。
- 键盘事件由前端发送**当前全部按下的 KeyboardEvent.code**，kvmd 用 hid\_map 转为 HID usage（修饰键合成修饰字节，最多 6 个普通键），状态没有变化时不重发。
- 鼠标用绝对模式，前端按「视频实际显示区域」把浏览器坐标换算到 0 至 4095，网页上点哪里 NAS 屏幕就点哪里。需要成品线配置到对应工作模式。
- **鼠标发送频率按波特率自适应**：帧长按 13 字节估算，发送间隔取 `max(16ms, 2.2 × 单帧传输时间)`，使串口占用不超过约 45%。9600 下约 30ms（约 33Hz），115200 下为 16ms（约 60Hz）。按键状态变化与滚轮立即发送，不被合并。
- WebSocket 断开，或 6 秒内没有收到任何消息（前端每 2 秒发一次 ping），立即发送全部释放帧，避免卡键。页面失焦、标签页隐藏时前端也主动发送释放。
- 串口读写出错或设备消失时，自动关闭并每 2 秒尝试重开，重开后先发一帧「获取芯片信息」确认链路。
- 串口链路测试（`/api/serial/test`）是异步的：发出探测帧后最多等待 400ms，等待期间其它 HTTP 请求、WebSocket 和宏照常处理；同一时刻只允许一个测试（并发返回 409）。
- 每 2 秒发一次「获取芯片信息」探测：5 秒内有应答视为链路正常，应答里的 USB 枚举位作为 NAS 是否已开机的判断来源之一，其余来源还有 ping NAS 与 HDMI 有无信号。

**绝对鼠标 (0,0) 的特例。** 在 Windows 上实测，x、y 同时为 0 的绝对坐标报告被忽略，其余边角坐标都正常；`ch_mouse_abs()` 在两者都为 0 时改发 (1,1)，在屏幕上落在同一个左上角像素。

### 5.4 电源控制（已移除）

v0.4 起不再提供电源控制：没有电源状态机、`/api/power/*`、`power_led` 等选项，也不再操作 `/sys/class/leds`。需要给 NAS 开机时，由用户手动操作；NAS 的 BIOS 建议打开 Restore on AC Power Loss = Power On，作为停电恢复的保底。

### 5.5 开机宏与启动菜单键

启动菜单键和 BIOS 设置键在页面上由用户自己选，不写进固件：

| 选项 | 默认 | 候选值 |
| --- | --- | --- |
| 启动菜单键 | **F11** | F1 到 F12、Esc、Del、Tab |
| BIOS 设置键 | Del | 同上 |
| 持续时间 | 10 秒 | 点击后持续发键的时间，请在这段时间内给 NAS 开机 |
| 发送间隔 | 100ms |  |
| 按键保持时间 | 30ms | 按下帧与释放帧之间的间隔 |

页面按钮：连发启动菜单键、连发 BIOS 键、测试按键（只发一次所选键，用来确认键位没选错）、取消。

实现要点：

- 前端存键名（如 F11），后端查表转成 HID 用法码：F1 到 F12 为 0x3A 到 0x45（F2 为 0x3B），Esc 为 0x29，Del 为 0x4C，Tab 为 0x2B。
- 连发时每次发按下帧、保持 `key_hold_ms` 后发释放帧，成对发出；高波特率下两帧几乎同时到达，保持时间过短时 BIOS 的 USB 轮询可能漏键，所以保持时间可配。宏结束或被取消时再补发一次全部释放帧。
- 开机初期成品线的 USB 可能尚未枚举，最前面几次按键会丢，所以用持续连发，而不是只发一次。
- 宏设置通过 POST /api/config 写入，同时调用 `nas-kvm-ctl uciset` 持久化到 /etc/config/nas-kvm，**不需要重启服务**。

### 5.6 配置文件 /etc/config/nas-kvm

| 选项 | 默认 | 说明 | 生效方式 |
| --- | --- | --- | --- |
| enabled | 1 | 总开关 | 重启服务 |
| listen\_port | 18080 | 管理页、API 与 WebSocket；默认值避开 8080 等常用端口，被占用时服务不会强行启动（见 5.9） | 重启服务 |
| bind | br-lan | 监听接口：`br-lan`（解析为 LAN IP）、`127.0.0.1` 或具体 IPv4 | 重启服务 |
| token | （首次启动随机生成 32 位） | 接口认证，16 到 64 位 `[A-Za-z0-9_-]` | 重启服务 |
| serial | 自动探测 | 留空时只打开 USB ID 在 `serial\_ids` 里的串口；手动指定后不检查 USB ID。预计为 /dev/ttyUSB0 或 /dev/ttyACM0 | 重启服务 |
| serial\_ids | 1a86:7523,1a86:5523 | 自动探测接受的 USB vid:pid（CH340G 与 CH341 串口模式），逗号分隔；非 CH340 的适配器在 LuCI 里点「扫描串口设备」一键加入 | 重启服务 |
| baud | 9600 | 以成品线实际为准；仅允许 8 个标准值（见 5.3） | 重启服务 |
| video\_device | /dev/video0 | MS2130 的 UVC 节点 | 重启服务 |
| resolution | 1920x1080 | 不稳时用 1280x720 | 重启服务 |
| desired\_fps | 30 |  | 重启服务 |
| video\_port | 18081 | 不能与 listen\_port 相同 | 重启服务 |
| boot\_menu\_key / bios\_key | F11 / Del | 页面可改 | 热更新 |
| macro\_duration\_s / macro\_interval\_ms | 10 / 100 | 开机宏参数 | 热更新 |
| key\_hold\_ms | 30 | 宏每次按键的保持时间 | 热更新 |
| nas\_ip | （空） | 用于 Ping 状态检测，留空显示「未配置」 | 即时（探测脚本每次读取） |

kvmd 启动时执行 `uci -q show nas-kvm.main` 读取配置，非法值保留默认并在日志中提示。

**已移除的选项**：`power_led`、`press_ms`、`force_ms`、`cycle_wait_s`、`min_interval_s`、`pre_press`。旧配置文件里残留这些键时被忽略，升级时 `uci-defaults` 会把它们清掉。

### 5.7 HTTP / WebSocket 协议

所有 `/api/*` 与 `/ws` 都要求 token：`Authorization: Bearer <token>` 请求头，或 WebSocket 的 `?token=` 查询参数（浏览器无法为 WebSocket 加请求头）。静态页面不需要 token，页面首次打开时输入并保存在浏览器 localStorage，也可用 `#token=…` 一次性带入。

**REST**

| 方法与路径 | 请求体 | 成功响应 | 失败 |
| --- | --- | --- | --- |
| GET /api/status | — | 状态 JSON（见下） | 401 |
| POST /api/macro/run | `{"kind":"menu"\|"bios"}` | `{"ok":true}` | 400 键名无效 · 409 正在运行 |
| POST /api/macro/test | 同上 | `{"ok":true}` | 同上 |
| POST /api/macro/cancel | `{}` | `{"ok":true}` | — |
| POST /api/release | `{}` | `{"ok":true}` | — |
| POST /api/config | 5.6 中「热更新」字段的任意子集 | `{"ok":true,"saved":N}` | 400：任一值非法或格式错误即**整批拒绝**，内存与 UCI 都不改变；先全部校验、再统一应用 |
| POST /api/serial/test | `{}` | `{"ok":true,"version":"0x30","usb":true}`（异步，最多等 400ms） | `{"ok":false,"error":"no reply"}` · 409 已有测试在进行 |

**状态 JSON**（`GET /api/status`，以及 WebSocket 里带 `"t":"status"` 的推送）

```json
{"serial":true,"usb":true,"hdmi":1,"ping":-1,"macro":"idle","video_port":18081,
 "cfg":{"boot_menu_key":"F11","bios_key":"Del","macro_duration_s":10,"macro_interval_ms":100,
        "key_hold_ms":30}}
```

`hdmi` 与 `ping`：1 有、0 无、-1 未知或未配置。`macro`：idle / running。状态每秒推送一次，**宏、链路、USB 枚举、HDMI、Ping 任何一项变化时立即推送**。

**WebSocket 客户端消息**（文本帧 JSON）

| 消息 | 含义 |
| --- | --- |
| `{"t":"key","down":["ControlLeft","KeyA"]}` | 当前全部按下的键（完整状态，幂等） |
| `{"t":"mouse","btn":0,"x":0,"y":0,"wheel":0}` | 绝对坐标 0 到 4095；btn 为位掩码（1 左、2 右、4 中）；wheel 为 -127 到 127 |
| `{"t":"release"}` | 立即释放全部按键与鼠标键 |
| `{"t":"ping"}` | 心跳，回 `{"t":"pong"}` |

### 5.8 LuCI 配置页（luci-app-nas-kvm）

菜单位置：服务 › NAS KVM。内容：

- 服务状态（nas-kvmd、ustreamer 的运行状态与 PID，每 5 秒刷新）、启动 / 停止 / 重启、打开 KVM 控制页
- 四个标签：基本设置、键鼠（串口）、视频、开机宏，覆盖 5.6 的全部选项，并带输入校验（端口范围、时长范围、Token 格式、视频端口不得等于管理页端口）
- 串口设备下拉自动列出 `/dev/ttyUSB*` 与 `/dev/ttyACM*`；「扫描串口设备」按钮列出每个串口的 USB ID，对不在白名单里的可一键加入
- Token 重新生成按钮、串口链路测试按钮
- 「保存并应用」触发 procd reload，服务重启

rpcd ACL：读 uci 与 `service list`、列 `/dev`；写 uci，执行 `/etc/init.d/nas-kvm` 与 `/usr/bin/nas-kvm-ctl`。

### 5.9 进程托管与启动

- init 脚本为 procd 托管，`kvmd` 与 `ustreamer` 各一个 instance，崩溃自动重启（3600 秒内最多 5 次）。
- 每次启动先执行 `nas-kvm-ctl disable-upstream`：只关闭与本包冲突的上游 ustreamer 实例（设备或端口相同），无关实例不动（见 5.2）。
- 首次启动或 Token 少于 16 位时，init 自动生成随机 Token 写入 UCI。
- 监听地址由 `nas-kvm-ctl bindip` 解析（`br-lan` 取 lan 接口 IP）。地址尚未就绪时先不启动，等 lan 接口 up 的 procd 触发器再拉起。
- 启动每个 instance 前用 `nas-kvm-ctl port-check` 检查端口（读 `/proc/net/tcp{,6}`，不依赖 netstat 或 ss）：端口被别的程序监听（通配地址，或本包要绑的地址）时最多等 3 秒；仍被占用就**不启动该 instance**，在日志里写明占用者进程名和要修改的 UCI 选项，不会进入重启循环，也不会把对方挤掉。kvmd 与 ustreamer 各自独立检查，互不影响。占用者就是 `nas-kvmd` 或 `ustreamer` 时（重复执行 start、旧进程正在退出）视为没问题，交给 procd 处理；局限是占用者若是另一个 ustreamer 实例也会被当成自己人，此时本包的 ustreamer 会绑定失败并由 procd 重试，错误可在日志里看到。
- `uci commit`（例如 kvmd 热更新宏参数）不会触发服务重启；只有 LuCI「保存并应用」或 `reload_config` 才会。
- 日志写入 logd（tmpfs），不写闪存。

## 6. 安全

- 所有服务只绑定 br-lan 的 IP（kvmd 额外绑 127.0.0.1），**ustreamer 使用同一个地址，不再 0.0.0.0**；上游 ustreamer 包自带的默认实例（监听 `[::]:8080`、无认证）占用本包的设备时被 `disable-upstream` 关闭，其它无关实例不动；WAN（PON 接口）防火墙保持默认拒绝，不新增规则。
- 远程访问通过 WireGuard 进入内网，不做端口映射。
- 开机宏、配置、状态等 API 与 WebSocket 必须带 token；Token 比较为常量时间；Token 少于 8 位或含非法字符时 kvmd 拒绝启动。
- 没有文件上传和 CGI；静态文件路径只允许 `[A-Za-z0-9/._-]` 且禁止 `..`，单文件上限 1MB；HTTP 请求最大 8KB。攻击面只有 kvmd 的 HTTP 与 WebSocket 接口。
- **HTTP 明文限制**：Token 在 LAN 内以明文传输，同一局域网内的抓包者可以取得。因此远程只走 WireGuard；如果以后要让不可信网段访问，需要在前面加 TLS 反向代理，而不是直接暴露 18080。
- **防重放**：已取得 Token 的人可以重放请求；宏接口用互斥（409）限制重复启动。
- WebSocket 不使用 Cookie，Token 在请求里显式携带，所以不存在跨站请求伪造；kvmd 不记录 URL，Token 不会出现在自身日志里。
- 连接数上限：同时最多 16 个连接，其中 WebSocket 最多 4 个；非 WebSocket 连接空闲 10 秒断开。仅 LAN 内可达，不针对恶意占满连接做更多防护。

## 7. 实施里程碑与验收

| 阶段 | 内容 | 验收标准 |
| --- | --- | --- |
| M0 | 备份原厂固件与当前可用固件；编译含 UVC、ustreamer、ch341、cdc-acm 的固件 | /dev/video0 与串口节点（/dev/ttyUSB0 或 /dev/ttyACM0）都出现；lsusb 看到适配器的 USB ID（CH340G 为 1a86:7523）；MS2130 在 USB3 口；`nas-kvm-ctl serial-scan` 里适配器显示 `matched=yes`（不是则在 LuCI 里扫描并加入白名单）（核对 sysfs 层级假设） |
| M1 | 视频 | 1080p MJPEG 稳定出图，记录帧率与光猫 CPU 占用；`netstat -ltn` 显示 ustreamer 只在 br-lan IP:18081 上监听，**不存在占用 /dev/video0 的上游实例**，其它无关的上游实例（如有）仍在运行；USB 软复位后（触发上游 hotplug）依然如此 |
| M2 | 接成品线，手动发帧 | 用 stty 设好波特率后发按下与释放帧，NAS 上出现字符 a；`nas-kvm-ctl serial-test` 能读到应答，USB 枚举位与实际一致；绝对鼠标在 NAS 上点击位置正确 |
| M3 | （已取消）电源控制 | 功能已移除，不再验收 |
| M4 | 整合：安装软件包与 luci-app，认证、开机宏 | 重启光猫后所有服务自动恢复；LuCI 页面能改配置并生效；宏能进入启动菜单（在目标主板上选定 F11/F12/F2/Del） |
| M5 | 稳定性 | 连续运行 24 小时；断开浏览器后无卡键；USB 软复位后串口能自动恢复；高波特率下宏不漏键 |

## 8. 风险与待确认

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 成品线协议或波特率与预期不符 | 键鼠无响应 | 下单前问清；到手后用 M2 的两帧实测，不行就向卖家要协议文档 |
| CH9329 应答帧格式与 USB 枚举位含义以资料为准，未对照手册 | 链路与 NAS 状态判断不准 | M2 对照数据手册与实测核对 `ch9329.c` 中 0x81 应答的解析 |
| 误买普通公对公数据线 | 两端 5V 短接，可能烧口 | 采购时确认是键鼠模拟线 |
| ttyUSB0 重新枚举 | 键鼠短暂失效 | kvmd 自动重开串口并重新确认链路（已有自动化测试） |
| USB 供电不足或 MS2130 回落到 USB2 | 视频断流 | 使用 USB3 口；必要时带电源 hub；用 usb-power 软复位 |
| 开机初期丢键，或 BIOS 阶段键盘兼容性 | 宏进不了启动菜单 | 持续连发；`key_hold_ms` 调大；用测试按键先确认 |
| 高波特率下按下帧与释放帧间隔过短 | BIOS 轮询漏键 | 保持时间可配，默认 30ms |
| 9600 波特率鼠标发送频率受限（约 33Hz） | 鼠标略有拖尾 | 先问成品线能否提高波特率 |
| LAN 地址启动时尚未就绪 | 服务未启动 | init 用 lan 接口 up 触发器重拉（真机待验证） |
| 闪存空间 | 软件包体积 | 用 C 实现、无第三方库；日志写 tmpfs |
| 上游 ustreamer 包的 init、config、hotplug 与本包冲突 | kvmd 绑定失败、视频设备被抢、无认证视频流暴露 | `disable-upstream` 只关冲突的 section（5.2）；M1 用 `netstat -ltn` 验收；若上游以后改了 section 类型或 hotplug 行为需重新核对 |
| 默认端口与光猫上其它应用冲突（8080 常被 qBittorrent、mjpg-streamer 等使用） | 对方先占：本包起不来；本包先占：对方重启后绑定失败（同一端口不论谁先监听，另一方都会 EADDRINUSE） | 默认改为 18080 与 18081；启动前 `port-check` 发现占用就跳过并写明占用者（5.9）；仍冲突时改 `listen_port`、`video_port` |
| 光猫上另有 USB 串口设备（4G 模块、UPS 等） | 若自动探测不挑设备，会被改 termios 并每 2 秒收到探测帧 | 只打开 `serial_ids` 里的设备，其它设备一个字节都不发（有测试覆盖） |
| 串口自动探测依赖 sysfs 层级 | 真机层级与假设不符时查不到 USB ID，自动探测不会选中任何串口，键鼠无响应 | 宁可不碰也不误碰；M0 用 `nas-kvm-ctl serial-scan` 核对，不符时手动设置 `serial` |

## 9. 待填参数

| 参数 | 当前值 |
| --- | --- |
| 成品线默认波特率 | 实测 9600 可用（CH9329 版本字节 0x34）；可再确认能否改 115200 |
| 串口适配器 VID:PID | 实测 0843:5740（CDC 串口，Windows 驱动 usbser）；CH9329 的 USB 端为 1a86:e129 |
| `nas-kvm-ctl serial-scan` 的输出 | 待测（光猫上应看到 `/dev/ttyACM0 0843:5740`，并在 LuCI 里加入白名单） |
| 18080、18081 是否空闲 | 待测（`netstat -ltn`） |
| 启动菜单键 | 默认 F11，页面可改 |
| BIOS 设置键 | 默认 Del，页面可改 |
| NAS IP（Ping 检测，可选） | 待填 |

## 10. 源码与测试

源码位置见 5.1。在 Linux 主机上可以直接构建并跑全部测试，不需要光猫：

```sh
cd nas-kvm

# 构建守护进程
gcc -O2 -Wall -Wextra -o nas-kvmd src/kvmd.c src/ch9329.c src/hid_map.c src/sha1b64.c

# 守护进程端到端（pty 充当串口）
python3 tests/test_kvmd.py ./nas-kvmd

# 串口自动探测（假 sysfs 与假 /dev）
python3 tests/test_serial_detect.py ./nas-kvmd

# nas-kvm-ctl：disable-upstream 作用域、port-check、serial-scan（假 uci，需要 Linux 的 /proc/net/tcp）
python3 tests/test_ctl.py

# init 脚本 start_service 的分支（桩 rc.common / procd，真实调用 ctl）
python3 tests/test_init.py

# 前端冒烟（jsdom 在真实 kvmd 上运行 index.html；需先 npm install jsdom）
python3 tests/test_ui.py ./nas-kvmd files/www/kvm /path/to/node_modules/jsdom

# LuCI 视图在最小 mock 下执行
node tests/luci_view_mock.js ../luci-app-nas-kvm/htdocs/luci-static/resources/view/nas-kvm/config.js
```

**已在 PC 上验证**

- kvmd 守护进程 57 项端到端测试全部通过，并在 AddressSanitizer 与 UBSan 构建下通过：鉴权、静态文件与路径穿越、WebSocket 握手（RFC 6455 示例密钥）、键盘帧与设计文档示例逐字节一致、绝对鼠标、鼠标合并、断开与心跳超时释放、电源接口已移除（返回 404）、旧配置里残留的电源选项被忽略、宏连发与取消、配置热更新与校验、串口无应答与恢复，以及 v0.3.1 新增的：配置整批拒绝且不留任何改动、格式错误值返回 400、配置文件里的非标准波特率被拒绝、串口测试等待期间 HTTP 与 WebSocket 仍即时响应、并发测试返回 409。
- `test_ctl.py` 35 项通过：`disable-upstream` 只关设备或端口冲突的 section（含 device 缺省、自定义 `video_device`），无关的摄像头实例保持启用，有冲突时 restart 而不是 stop，第二次运行不 commit、不碰上游，没有冲突或没有上游配置时静默；`port-check` 在通配地址、127.0.0.1、IPv6 通配与 4-in-6 地址、非 LISTEN 状态、别的地址上的同端口监听等情形下判断正确，能输出占用者进程名，占用者是 `nas-kvmd` 时立即放行，等待期间对方释放端口时返回 0；`serial-scan` 在 ttyUSB 与 ttyACM 两种 sysfs 层级、大写十六进制、自定义 `serial_ids` 下输出正确。
- `test_serial_detect.py` 15 项通过：CH340G 被选中并收到探测帧；只有 4G 模块时一个字节都不发并在日志里说明；4G 模块排在前面也不会被选中；ttyACM 层级、大写十六进制、自定义与非法或空的 `serial_ids` 都正确处理；手动指定 `serial` 时不检查 USB ID；查不到 USB ID 的串口宁可不碰。
- `test_init.py` 14 项通过：端口空闲时两个 instance 都启动；管理页端口被占时只跳过 kvmd，视频流端口被占时只跳过 ustreamer，日志点名占用者与要改的选项；冲突的上游 section 被关闭并写入日志，无关的 cam2 保持启用；`enabled=0` 时什么都不启动。
- 所有 shell 脚本用 `busybox ash -n` 做了语法检查，`test_ctl.py` 与 `test_init.py` 在 busybox 1.36 的 ash、awk、ls、tr 下复测通过。
- 随机畸形 HTTP 与 WebSocket 数据 fuzz 后进程不崩溃、不报 sanitizer 错误，仍能正常服务。
- 串口设备被拔掉时进程不崩溃，状态变为串口无应答。
- 前端 18 项冒烟测试通过（本版在 jsdom 上实际运行过）：连接与状态、视频地址、键盘与鼠标帧、页面不再有任何电源控件、宏设置保存、失焦释放、错误 Token 显示登录框。
- `nas-kvm-ctl` 在假的 uci、wget、ping 下验证了 bindip、probe、uciset、API 调用；kvmd 经真实 ctl 脚本完成配置落盘与 HDMI/Ping 探测回填。
- LuCI 视图在 mock 下执行无错误，20 个选项全部注册、电源选项与标签页已不存在，串口下拉过滤正确，「扫描串口设备」弹窗与一键加入白名单行为正确。
- 使用 OpenWrt 风格的编译选项（`-Werror=format-security`、`_FORTIFY_SOURCE=2`、`-fstack-protector-strong`）无警告。
- **成品线在真实硬件上验证（Windows，`tools/kvm_hw_test.py`）**：CH9329 的 HID 端被识别为键盘与鼠标；串口 9600 波特率有应答，20 次探测无丢包、延迟约 21.5ms，USB 枚举位为 1；键盘 a、Shift+A、F2/F11/F12/Esc/Del/Tab 正确，保持 5ms 即可识别，全部释放帧有效，CapsLock 的 LED 回读正确；绝对鼠标 5 个点误差为 0 像素，左/中/右键与滚轮方向正确；按自适应间隔连发 90 帧鼠标应答 90 个，无丢帧。已知特例：(0,0) 坐标被忽略，已在 `ch9329.c` 里规避。

**尚未验证（需要真机或固件构建环境）**

- 在 ponwrt 的 AArch64 工具链上交叉编译，以及 `nas-kvm` 与 `luci-app-nas-kvm` 的软件包打包（ponwrt 为 apk，见第 11 节）。
- procd init 脚本、lan 接口触发器、`network_get_ipaddr` 在真机上的行为（init 的分支逻辑只在桩环境验证，真实 rc.common 与 procd 未跑过）。
- OpenWrt 自带 busybox 的构建选项与 Ubuntu 的 busybox 1.36 可能不同，`port-check` 用到的 `ls -l /proc/*/fd`、`awk`、`tr` 需在真机上跑一遍。
- 串口自动探测的 sysfs 层级（向上最多 4 级找 `idVendor`）只在假 sysfs 上验证，真机请用 `nas-kvm-ctl serial-scan` 核对。
- LuCI 页面在真实 LuCI 中的渲染、ACL 与「保存并应用」流程。
- CH9329 的真实应答帧、USB 枚举位的含义、绝对鼠标工作模式（M2）。
- ustreamer 的实际参数、`/state` 输出格式（HDMI 探测依赖其中的 `online` 字段）、MS2130 的 MJPEG 帧率与 CPU 占用（M1）。
- 上游 ustreamer 包的行为依据其源码（Makefile、init、hotplug 三个文件）核对，未在真机上跑过 `disable-upstream` 与 USB 软复位后的 hotplug 流程；`uci show` 对匿名 section 的输出格式以真机为准。
- GitHub Actions 工作流没有在 GitHub 上实际跑过：只做了 actionlint 静态检查，并在本地用 YAML 里的原始命令跑通了 test 任务和「收集」「打包」两个 shell 步骤。SDK 容器 `airoha-an7581`、`x86-64` 两个 tag 的命名依据 immortalwrt/docker 的构建脚本核对，未在 Docker Hub 上确认；在 SDK 里能否顺利编译 `kmod-*` 依赖、快照 SDK 实际产出的包格式，都要看第一次运行的结果。

## 11. 自动编译（GitHub Actions）

工作流文件为 `.github/workflows/build.yml`，由三个任务组成：

| 任务 | 作用 |
| --- | --- |
| test | 在 Ubuntu 上构建 nas-kvmd（ASan、UBSan、OpenWrt 风格加固选项），运行第 10 节的全部测试，shell 脚本另用 busybox 复测；打 tag 时先核对 tag 与两个 Makefile 的 `PKG_VERSION` 一致 |
| build | 用 `immortalwrt/gh-action-sdk`（固定到提交，不跟分支）在官方 SDK 容器里编译 `nas-kvm` 与 `luci-app-nas-kvm`。矩阵：`aarch64_cortex-a53`（SDK 容器 `airoha-an7581`，即 AN7581 光猫）与 `x86_64`（SDK 容器 `x86-64`） |
| release | 仅在打 `v*` tag 时运行：把各架构的包打成 tar.gz，附 `SHA256SUMS`，创建 GitHub Release |

**触发条件**：推送到 main 或 master、Pull Request、`v*` tag、手动运行（可填 `sdk_suffix`，留空即最新快照 SDK，与跟随 ImmortalWrt master 的 ponwrt 最接近；例如填 `-openwrt-24.10` 使用该分支的 SDK）。只改 `.md` 文件不会触发。

**包格式**：ponwrt 的 `configs/an7581.config` 里 `CONFIG_USE_APK=y`，并且没有选 opkg，所以设备上用的是 **apk**，产物是 `.apk`；只有指向仍使用 ipk 的旧分支 SDK 时才会得到 `.ipk`，收集步骤两种都认。产物只包含 `nas-kvm` 与 `luci-app-nas-kvm`，依赖包（ustreamer、kmod）不收集。

**安装**：未签名的包在设备上这样装（路径按实际）：

```sh
apk add --allow-untrusted ./nas-kvm-*.apk ./luci-app-nas-kvm-*.apk
```

软件包依赖 `kmod-video-uvc`、`kmod-usb-serial-ch341`、`kmod-usb-acm` 和 `ustreamer`。kmod 与设备内核严格对应，所以**最稳妥的做法仍然是把 nas-kvm 编进 ponwrt 固件**；单独装 apk 时，设备的软件源里必须有与其内核匹配的这些包。

**签名（可选）**：在仓库的 Secrets 里设置 `APK_PRIVATE_KEY`（apk）或 `IPK_KEY_BUILD`（ipk），只在打 tag 的构建里才会传给构建容器；不设置则产出未签名的包。签名后设备需要信任对应的公钥，方法见 OpenWrt 的 apk 文档。

**加架构**：在 build 任务的矩阵里加一项，`sdk` 填 ImmortalWrt 的 `<target>`（斜杠换成横线）或软件包架构，`label` 填软件包架构，文件里有注释示例。

**发布流程**：同时修改 `nas-kvm/Makefile` 与 `luci-app-nas-kvm/Makefile` 的 `PKG_VERSION`，提交后执行 `git tag v<版本> && git push --tags`。构建失败时，对应架构的日志会作为 `logs-<架构>` 工件上传。
