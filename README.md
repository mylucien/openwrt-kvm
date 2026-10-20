# nas-kvm 0.4.0

openwrt上的 NAS 远程 KVM

- `nas-kvm/`            守护进程 nas-kvmd、KVM 控制页、init 脚本、默认配置、测试
- `luci-app-nas-kvm/`   LuCI 配置页（服务 › NAS KVM）

## 放进源码树
    cp -r nas-kvm            <ponwrt>/package/utils/nas-kvm
    cp -r luci-app-nas-kvm   <ponwrt>/feeds/luci/applications/luci-app-nas-kvm    # 或放进自己的 feed
    make menuconfig   # Utilities → nas-kvm；LuCI → Applications → luci-app-nas-kvm；另需 ustreamer、kmod-video-uvc、kmod-usb-serial-ch341
    make package/nas-kvm/compile package/luci-app-nas-kvm/compile V=s


## 首次使用
Token 在首次启动时自动生成：`uci get nas-kvm.main.token`，或在 LuCI 里查看。
KVM 控制页：`http://<光猫 LAN IP>:18080/`，首次打开输入 Token（也可用 `#token=…` 一次性带入）。

## 与系统里其它应用共存
- 默认端口 18080（控制页）与 18081（视频流），避开 8080 等常用端口；启动前会检查端口，被别的程序占用就不启动并在系统日志（`logread -e nas-kvm`）里写明占用者。
- 上游 ustreamer 包自带的默认实例会抢 /dev/video0；本包只关闭与自己冲突的 section（设备或端口相同），别的摄像头实例不受影响。
- 串口自动探测只打开 USB ID 为 1a86:7523 / 1a86:5523 的设备（可在 LuCI 改 `serial_ids`），4G 模块等其它串口不会被碰。`nas-kvm-ctl serial-scan` 可查看当前串口及是否匹配。

## 自动编译（GitHub Actions）
`.github/workflows/build.yml`：主机测试 → 用 ImmortalWrt SDK 编译 aarch64（AN7581 光猫）与 x86_64 → 打 tag 发布 Release。详见设计文档第 11 节。
- 把本目录作为仓库根推到 GitHub 即可；在 Actions 页面可手动运行。产物在运行页面的 Artifacts（`packages-aarch64_cortex-a53`、`packages-x86_64`）。
- ponwrt 固件使用 **apk**，产物是 `.apk`：`apk add --allow-untrusted ./nas-kvm-*.apk ./luci-app-nas-kvm-*.apk`。依赖的 kmod 必须与设备内核匹配，最稳妥的做法仍是把 nas-kvm 编进 ponwrt 固件。
- 发布：两个 Makefile 的 `PKG_VERSION` 一致，然后 `git tag v0.4.0 && git push --tags`。

