# VibePhone

用一块 **ESP32-S3** 做的物理按键小键盘：按一下按钮，电脑上就打开一个新的 **Codex** 或 **Claude Code** 窗口。

按键同时走两条通道——作为标准 USB 键盘发出 `F13` / `F14`，以及通过 USB-CDC 串口把事件发给电脑端的桥接程序。桥接程序监听 `127.0.0.1:17653`，收到事件后负责把对应的 App / 终端拉起来。

**当前状态**：v1.0 已固化，端到端验证 31 次触发 0 失败。详见 [`版本记录.md`](版本记录.md)。

---

## 仓库结构

| 目录 | 内容 |
| --- | --- |
| [`firmware/`](firmware/) | ESP32-S3 固件（PlatformIO + Arduino core 4.x） |
| [`bridge/`](bridge/) | 电脑端桥接程序（Node，**零第三方依赖**，监听 `127.0.0.1:17653`） |

两边各有自己的 README：固件看 [`firmware/README.md`](firmware/README.md)，桥接程序和网页控制台看 [`bridge/README.md`](bridge/README.md) 与 [`bridge/使用教程.md`](bridge/使用教程.md)。

## 硬件

| 项 | 值 |
| --- | --- |
| 板子 | 优信 YD-ESP32-23 V1.3（N16R8，ESP32-S3） |
| PlatformIO 板级 | `esp32-s3-devkitc-1` |
| 按键 | `GPIO4` → `F13`（Codex）、`GPIO5` → `F14`（Claude） |
| 屏幕 | 1.54" ST7789，**已接线但未调通**，本版 `TFT_ENABLE=0` 关闭 |

---

## ⚠️ 三条改动/排查前必读的规则

这三条都是踩过坑才总结出来的。

### 1. `COM`（FTDI）线日常必须拔掉

那块板把 FTDI 的 `DTR` 接在了芯片的 `GPIO0`（BOOT 脚）上。**任何程序打开 `COM` 口都可能把芯片复位进下载模式**，而且会一直卡住。

| 场景 | `COM` 线 |
| --- | --- |
| 日常使用 | ❌ 拔掉 |
| 烧录固件 | ✅ 插上 → 烧完 → 拔掉 |

**故障现象**：设备管理器里只剩串口、**没有键盘**，按键全哑。
**恢复**：拔掉 `COM` 线 → 按 `RST` → 重启桥接程序。

这块板不能用 `COM` 口做实时日志。

### 2. 烧录前先关掉桥接程序

否则可能在写完 bootloader 后报「芯片停止响应」。

### 3. `bridge/platform/` 下的 `.ps1` 必须保持带 UTF-8 BOM

丢了会报几十处语法错误——PowerShell 5.1 会按 GBK 解码中文注释。

---

## 快速开始

**固件**（需要自己装 PlatformIO，本仓库不含构建产物）：

```bash
cd firmware
pio run -t upload --upload-port COM7
```

**桥接程序**（不需要 `npm install`，没有第三方依赖）：

```bash
cd bridge
node bridge.mjs
```

Windows 上也可以直接跑 `bridge\start-windows.cmd`。

---

## 已知限制

- **屏幕未启用**：`TFT_ENABLE = 0`。驱动代码在 `firmware/src/display.cpp`，接线后只有背光、无画面，需要时再单独排查。
- **macOS 未真机验证**：代码已提供（`start-macos.command`、`platform/launch-macos.applescript`），但只在 Windows 上实际跑过。

## 系统足迹

```
✅ 不写注册表          ✅ 不装服务
✅ 不加开机自启        ✅ 不开防火墙端口（只监听 127.0.0.1）
✅ 项目外无文件        ✅ 关掉就全停，串口释放
```
