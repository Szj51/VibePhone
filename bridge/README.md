# Vibe Keyboard Bridge · 第一步

本地桥接程序：在控制台页面获得焦点时，按 `1` 打开 Codex 桌面应用，按 `2` 打开新的终端并运行 Claude Code。Windows 使用 CMD，Mac 使用系统 Terminal。无需 ESP32-S3，也无需安装第三方 npm 包。

## Windows 使用

1. 确认 Node.js 20 或更新版本已安装（`node --version`），Codex 桌面应用已安装，而且在新 CMD 中运行 `claude` 能成功。
2. 双击 `start-windows.cmd`。
3. 浏览器会打开 `http://127.0.0.1:17653/`。保留启动脚本的服务窗口。
4. 点击网页空白处，再按 `1` 或 `2`。也可点击应用卡片。
5. 应用打开后会获取焦点。再次测试前，需要重新点击桥接页面。
6. 在服务窗口按 Ctrl+C 停止。关闭网页本身不会停止服务。

Windows Terminal 可用时，程序用 `-w new` 强制新建一个窗口并在里面启动 CMD；否则使用独立 CMD 窗口。工作目录为当前用户主目录，不自动发送任何编程需求。

## macOS 使用

1. 安装 Node.js 20 或更新版本，确认 Codex 桌面应用存在，新的 Terminal 窗口中可以运行 `claude`。
2. 将整个项目文件夹复制到 Mac。首次在 Terminal 中运行 `chmod +x /你的路径/start-macos.command`（路径有空格时请加引号）。
3. 双击 `start-macos.command`。如果 Finder 不允许直接运行，可在 Terminal 中使用 `zsh /你的路径/start-macos.command`。
4. 按 `2` 时如提示自动化权限，请允许启动桥接程序的终端控制 Terminal。
5. 已有 Codex 窗口时，新建窗口需要 System Events 自动化／辅助功能权限。仅在系统实际提示时，给启动服务的终端授权。

macOS 实现已经提供，但没有在本机 Windows 环境中做 Mac 真机验证。

## Codex “新窗口”的边界

- 不调用 `codex` CLI，不新建 Codex 任务，不发送聊天消息。
- Windows 首次启动：按 `windowsStartAppNames` 查找开始菜单入口，用它的 AUMID 启动；失败时回退到 `codex://` 协议。可在配置中明确指定桌面应用 exe。
- **商店（MSIX）版必须用 AUMID 启动**：`WindowsApps` 目录受 ACL 保护，直接 `Start-Process` 那个 exe 路径不可靠。
- Mac 首次启动：用 Launch Services 打开 Codex.app。
- **已有窗口时（Windows）：通过辅助功能（UI Automation）直接调用「文件 → 新建窗口」菜单项。** 不再发送按键。

### 为什么不用快捷键新建窗口

实测当前商店版（`OpenAI.Codex` 26.915.4065.0）的「文件」菜单为：

| 菜单项 | 快捷键 |
| --- | --- |
| 新建窗口 | **无** |
| 新聊天 | Ctrl+N |
| 新建临时聊天 | **Ctrl+Shift+N** |
| 打开文件夹 | Ctrl+O |

「新建窗口」**没有分配任何快捷键**，所以靠 `SendKeys` 永远发不出来；而 `Ctrl+Shift+N` 实际是「新建临时聊天」，它在同一个窗口内开新聊天，不产生新 HWND——这正是早期版本误判失败的原因。现在改为 UI Automation 调用菜单项，并**仍然校验新 HWND**：没检测到就报告失败，不会把切回旧窗口记为成功。

### 两个必须遵守的实现约束

**① 调用必须在菜单仍展开时进行。** 先 `Collapse()` 会让弹出菜单元素失效，`GetCurrentPattern(InvokePattern)` 会抛「不支持的模式」。

**② 必须先激活窗口，再等菜单栏出现。** Chromium 的**原生菜单栏是懒创建的**：窗口从未被激活过时，辅助功能树里**一个 MenuItem 都没有**，看起来就像「没有菜单栏」。实测数据：

| 窗口状态 | UIA 中 MenuItem 数量 |
| --- | --- |
| 未获得焦点 | **0** |
| 刚切到前台约 1 秒 | **0** |
| 保持焦点约 3 秒后 | **4** |
| 之后失去焦点 | 4（一旦创建就保留） |

因此 `Invoke-CodexNewWindow` 会先 `ShowWindowAsync` + `AppActivate` + `SetForegroundWindow` 激活窗口，再**轮询最多 5 秒**等菜单栏出现。

> 这正是「刚启动 Codex 后第二次按 `1` 报『没有可用的菜单栏』」的原因：那时窗口刚被创建、从未获得过焦点（焦点还在网页上），菜单栏尚未生成。**副作用是按下 `1` 会把 Codex 切到前台**——这是预期行为，也和「按完要重新点网页」的一致预期相符。

- Mac 端仍使用 Command+Shift+N，**未做真机验证**。

## 配置

`config.json` 与 `bridge.mjs` 放在同一目录。编辑后先停止服务，再重新启动。

```json
{
  "port": 17653,
  "keys": { "codex": "1", "claude": "2" },
  "cooldownMs": 600,
  "codex": {
    "windowsExecutable": "",
    "windowsStartAppNames": ["Codex", "ChatGPT"],
    "windowsProcessNames": ["ChatGPT", "Codex"],
    "macApp": "Codex",
    "newWindowShortcut": "shift+n"
  },
  "device": {
    "enabled": true,
    "port": "",
    "buttons": ["codex", "claude"]
  }
}
```

| 项目 | 说明 |
| --- | --- |
| `port` | 本地网页服务端口，仅监听 127.0.0.1 |
| `keys` | **网页触发**用的按键。普通数字键或功能键 |
| `cooldownMs` | 防止误触发的最短间隔；启动过程中也会阻止重复请求 |
| `windowsExecutable` | 留空自动发现；必要时填写 Codex **桌面应用** exe，JSON 中可使用 `/` 分隔路径。不要填写 CLI exe |
| `windowsStartAppNames` | 开始菜单里 Codex 桌面应用的名称候选。当前商店版显示为 **ChatGPT**，不是 Codex，所以两个名字都列上 |
| `windowsProcessNames` | 窗口检测用的进程名候选。商店版进程是 **ChatGPT.exe**，旧版是 `Codex.exe`。改名不会让检测静默失效 |
| `macApp` | 通常为 `Codex`，也可指定 Codex.app 的完整路径 |
| `newWindowShortcut` | 保留字段。Windows 已改为通过辅助功能菜单调用，不再依赖快捷键 |
| `device.enabled` | 是否启用 **ESP32 物理按键**触发。整段 `device` 不写就等于关闭 |
| `device.port` | ESP32 的串口号。**留空表示自动发现**（推荐），填了则强制用它 |
| `device.buttons` | 按**按键编号**顺序列出要打开的应用。`["codex","claude"]` = 编号1→Codex、编号2→Claude Code |

服务通过启动脚本所属的用户账户打开程序。不会保存账号密码、API Key，也不修改 Codex / Claude 配置。

## 两个独立的触发源

按键和网页是**互相独立**的两条路，共用同一套启动逻辑（去重、冷却、日志）：

```
① 网页按键   →  POST /api/launch/{action}  →  ┐
                                              ├→  triggerLaunch()  →  打开应用
② ESP32 按键 →  串口 JSON  →  子进程 stdout  →  ┘
```

| | 网页触发 | ESP32 物理按键 |
| --- | --- | --- |
| 生效条件 | 页面获得焦点 | 任何情况下都生效 |
| 配置项 | `keys` | `device.buttons` |
| 需要什么 | 只要桥接程序在跑 | 还要板子插在 `USB/OTG` 口 |

### ESP32 按键这条链路怎么走的

`bridge.mjs` 启动时（`device.enabled` 为 true）会拉一个子进程：

```
powershell -ExecutionPolicy Bypass -File platform/serial-windows.ps1
```

这个脚本负责：

1. **自动找串口** —— 按 `VID_303A`（Espressif 原生 USB）列出候选
2. **逐个验证** —— 连上后等最多 10 秒，收到数据才确认是设备口
3. **断线重连** —— 设备拔出/重启后自动重新寻找
4. 把设备发来的每一行 JSON **原样打到 stdout**

`bridge.mjs` 只读它的 stdout，不碰串口本身。收到 `{"t":"key","btn":N,"ev":"down"}` 就查 `device.buttons[N-1]` 得到要打开的应用，然后调用与网页完全相同的 `triggerLaunch()`。

> **为什么是 stdout 管道而不是 HTTP**：这是父子进程之间的私有通道，省掉了 token 传递和端口占用。脚本用 stderr 报状态，stdout 只放协议数据，两者不会混。

> **为什么要"逐个验证端口"**：设备刚上电的一两秒里，ESP32-S3 还挂在片内 ROM 的 USB-Serial-JTAG 上，
> 那时也会冒出一个 `VID_303A` 的串口——连上去能打开、却永远收不到数据。所以判断依据是
> "设备有没有在说话"（固件每 3 秒发一条心跳），而不是猜接口编号。

## 常见情况

- **按键无反应**：先点击网页；浏览器地址栏、其他应用、输入法组合输入或按住 Ctrl/Alt/Shift 时不会触发。
- **CMD 提示找不到 claude**：必须在新的 CMD 中能直接运行 `claude`。VS Code 的 Claude 插件已安装不等于命令行已安装。
- **提示端口占用**：打开已有控制台，或停止原服务；也可以修改配置端口。程序不会关闭未知进程。
- **Codex 未出现新窗口**：查看页面的完整错误。可能是 Codex 版本改版后菜单项名称变了（改 `platform/launch-windows.ps1` 里匹配的「新建窗口」字样）、Codex 没有可见窗口，或 Mac 权限不足。
- **按 1 没反应但也不报错**：确认开始菜单里有 Codex 或 ChatGPT 条目；两份实际名称可写进 `codex.windowsStartAppNames`。
- **终端已打开但 Claude 报错**：以终端内输出为准。桥接程序只确认打开终端并提交命令，不伪装成已经登录成功。

## 检查与测试

```sh
node bridge.mjs --check
node --test test/*.test.mjs
node bridge.mjs --test-mode
```

`--test-mode` 具有明显的黄色横幅，只验证按键、HTTP 请求和记录，不打开任何应用。正常使用时无需添加该参数。

`bridge.log` 只保存目标应用、时间和启动结果。此程序使用本地令牌、Origin/Host 校验和启动目标白名单，网页不能提交任意系统命令。

### 维护须知：`.ps1` 必须带 UTF-8 BOM

`platform/launch-windows.ps1` **必须保存为带 BOM 的 UTF-8**。Windows PowerShell 5.1 执行无 BOM 的脚本时会按系统 ANSI（简体中文下是 GBK）解码，文件里的中文注释随即变成乱码、引号配对错乱，于是 `Add-Type` 的 `@'...'@` here-string 打不开、C# 被当成 PowerShell 解析——该脚本会报 **34 处语法错误**，启动器完全不工作。

用编辑器另存时注意别把 BOM 弄丢；`test/bridge.test.mjs` 里有一条断言守着这一点。

## 分享给他人

整个文件夹可以直接打包发送。**源码里没有写死任何绝对路径**：内部路径一律从 `import.meta.url` 推导自身位置，启动器的用户主目录取自 `os.homedir()`。

已实测：把整个目录复制到带**空格和中文**的路径（`...\Temp\port test 移植测试\`）后，单元测试、服务启动、真实启动全部正常。对方解压到哪都行。

### 发送前

**删掉 `bridge.log`**（它记录了本机的启动历史），其余文件都可原样发送。

### 接收方需要具备

| 条件 | 缺少的后果 |
| --- | --- |
| **Windows 10/11** | macOS 版代码已附带但未真机验证；Linux 不支持 |
| **Node.js ≥ 20** | 双击 `start-windows.cmd` 会提示并直接退出 |
| **CMD 里能运行 `claude`** | 按 `2` 会开出终端，但里面 `claude` 报错 |
| **Codex 桌面应用** | 按 `1` 失败；按 `2` 不受影响 |
| 默认浏览器 | 需手动打开 `http://127.0.0.1:17653/` |

> 注意：**装了 VS Code 的 Claude 插件 ≠ 命令行能用 `claude`**，两者相互独立。

### ⚠️ PowerShell 执行策略（最可能出问题的一项）

本项目需要执行两个 `.ps1` 脚本（`launch-windows.ps1` 和 `serial-windows.ps1`）。Windows 默认**禁止执行脚本文件**，所以每次调用都显式带了 `-ExecutionPolicy Bypass`。

| 对方的机器 | 结果 |
| --- | --- |
| 普通个人 Windows（默认 `Restricted`） | ✅ **直接可用**，`Bypass` 覆盖得掉 |
| **企业/学校等有组策略强制限制的机器** | ❌ **会失效**，`Bypass` 会被组策略覆盖，报「在此系统上禁止运行脚本」 |

**遇到这种机器的解决办法**——对方在 PowerShell 里跑一次：

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

这是微软对开发机的推荐配置：**本地自己写的脚本放行，从网上下载的未签名脚本仍然被拦**，比每次都 `Bypass` 更安全。

### 接收方首次运行注意

- **Windows 可能提示「已保护你的电脑」**：文件来自网络会被打上「阻止」标记。右键 `start-windows.cmd` → 属性 → 勾选「解除锁定」。
- **不要用编辑器另存任何 `.ps1`**：会把 UTF-8 BOM 弄丢，脚本彻底不工作（见下方维护须知）。`platform/` 下的两个脚本都必须是**带 BOM 的 UTF-8**。
- **Codex 桌面应用的名字随版本而变**：开始菜单里可能是 `Codex` 或 `ChatGPT`，进程名可能是 `Codex.exe` 或 `ChatGPT.exe`。对不上时，把实际名字加进 `config.json` 的 `windowsStartAppNames` / `windowsProcessNames`。
- **「新建窗口」菜单项若改版改名**：需同步修改 `platform/launch-windows.ps1` 中匹配的「新建窗口」字样。
- **不用 ESP32 的话**：把 `config.json` 里 `device.enabled` 改成 `false`，就完全回到只靠网页按键的行为，不会有任何串口相关提示。

## 项目结构

```text
使用教程.md                 面向使用的完整图文教程（先看这个）
config.json                 用户配置
bridge.mjs                  本地 HTTP 服务与启动调度（含串口触发源的接入）
launcher.mjs                Windows/macOS 启动适配
public/                     前台按键控制台
platform/
  launch-windows.ps1        Windows 应用启动（UIA 调菜单、开终端）
  launch-macos.applescript  macOS 应用启动
  serial-windows.ps1        ESP32 串口监听（端口发现、验证、重连）
start-windows.cmd           Windows 启动入口
start-macos.command         Mac 启动入口
test/                       核心行为与请求边界测试
```

**第一次使用请先读 [使用教程.md](使用教程.md)**，README 偏实现说明。
