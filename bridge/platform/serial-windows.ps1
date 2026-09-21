param(
    [string]$Port = '',
    [int]$Baud = 115200,
    [int]$ParentPid = 0
)

# ============================================================
#  VibePhone 串口监听
#
#  读 ESP32-S3 的 USB CDC 串口，把设备发来的每一行协议原样打到 stdout，
#  由 bridge.mjs 读取并转成"打开 Codex / Claude Code"。
#
#  ⚠️ 输出约定（很重要）：
#     stdout —— 只放【设备协议行】，一行一条，不掺任何其他东西。
#     stderr —— 所有状态、错误、重连提示。
#     混了就解析不了，所以下面一律用 [Console]::Error 打状态。
#
#  ⚠️ 文件必须保存为【带 UTF-8 BOM】——PowerShell 5.1 对无 BOM 的脚本
#     会按系统 ANSI 解码，中文注释会变成乱码并破坏语法。
#
#  ── 为什么要"逐个验证端口"而不是"取第一个" ──────────────────
#
#  设备刚上电的一两秒内，ESP32-S3 还挂在片内 ROM 的 USB-Serial-JTAG 上，
#  那时也会枚举出一个 VID_303A 的串口。如果监听脚本这时启动，取第一个
#  就会连到这个"假"口上——能打开、但永远收不到任何数据。
#
#  所以改成：把候选口逐个试，每个连上后等最多 $HandshakeTimeoutSec 秒，
#  期间收到任何一行数据才算"对"。固件每 3 秒发一条心跳，因此正常情况
#  几秒内就能确认；连错口则最多 10 秒就换下一个。
#
#  判断依据是"设备真的在说话"，而不是猜接口编号——后者在实测中不稳定。
# ============================================================

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

# 连上后多久收不到任何数据，就判定这个口是错的
$HandshakeTimeoutSec = 10
# 已经确认是设备口之后，多久没有数据算异常（固件每 3 秒一次心跳）
$IdleTimeoutSec = 20

function Write-Status([string]$Message) {
    [Console]::Error.WriteLine("[serial] $Message")
}

# ── 父进程看门狗 ──────────────────────────────────────────────
#
# 为什么需要它：这个脚本是 bridge.mjs 的子进程，用 CREATE_NO_WINDOW 启动，
# 有自己的隐藏控制台，【不会随着 bridge.mjs 所在的那个控制台窗口一起被杀】。
#
# 实测中它确实会在父进程死后自己退出（往断掉的 stdout 管道写数据会抛异常），
# 但那是碰巧，不是设计。万一哪天没退出，就会留下一个孤儿进程占着串口，
# 之后烧录固件时会打不开那个口。
#
# 所以这里每次循环都主动确认父进程还活着，不在就干净退出。
# 不传 -ParentPid 时（手动单独运行调试）跳过检查。
$script:ParentGone = $false

function Test-ParentAlive {
    if ($ParentPid -le 0) { return $true }
    if (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) { return $true }
    $script:ParentGone = $true
    return $false
}

# 列出所有 VID_303A（Espressif 原生 USB）的串口候选。
# COM 口号会随插拔、重启、换 USB 口而变化，所以绝对不能写死。
function Get-VibePorts {
    try {
        return @(
            Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_303A' } |
            ForEach-Object {
                if ($_.FriendlyName -match '\((COM\d+)\)') { $Matches[1] }
            }
        ) | Where-Object { $_ }
    } catch {
        return @()
    }
}

# 尝试一个端口。返回 $true 表示"曾经收到过数据"（即这就是设备口）。
# 函数内部会一直读到出问题为止。
function Serve-Port {
    param([string]$Target)

    $sp = $null
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Target, $Baud
        $sp.Encoding    = New-Object System.Text.UTF8Encoding $false
        $sp.NewLine     = "`n"
        $sp.ReadTimeout = 800
        $sp.DtrEnable   = $true
        $sp.Open()
    }
    catch {
        Write-Status "$Target 打不开：$($_.Exception.Message)"
        if ($sp) { try { $sp.Close() } catch { } }
        return $false
    }

    $sawData      = $false
    $idleDeadline = (Get-Date).AddSeconds($HandshakeTimeoutSec)

    try {
        while ($true) {
            # 父进程没了就立刻收工，别变成孤儿占着串口
            if (-not (Test-ParentAlive)) {
                Write-Status '父进程已退出，监听结束'
                break
            }

            $line = ''
            try {
                $line = $sp.ReadLine().Trim()
            }
            catch [System.TimeoutException] {
                # 正常情况：设备不是一直在说话
            }

            # ⚠️ 必须【按协议格式】判断，不能"有数据就算"。
            #
            # 踩过的坑：ESP32-S3 在下载模式下，片内 ROM 会往串口吐调试文字
            # （"waiting for download"、"ESP-ROM:..." 之类）。旧写法看到任意
            # 非空行就认定"这是设备口"，于是连到了一个只会说 ROM 话的假口上，
            # 表现为「显示已连上设备，但按按键毫无反应」。
            #
            # 我们自己的协议每行都是 {"t":"..."} 形式的 JSON，用这个特征过滤。
            if ($line -match '^\s*\{\s*"t"\s*:') {
                if (-not $sawData) {
                    Write-Status "$Target 收到协议数据，确认是设备口"
                    $sawData = $true
                }
                $idleDeadline = (Get-Date).AddSeconds($IdleTimeoutSec)
                [Console]::Out.WriteLine($line)
                [Console]::Out.Flush()
            }
            elseif ($line) {
                # 不是我们的协议 —— 大概率是 ROM 的调试输出，丢掉但记一笔
                Write-Status "$Target 收到非协议内容，忽略：$($line.Substring(0, [Math]::Min(40, $line.Length)))"
            }

            if ((Get-Date) -gt $idleDeadline) {
                if ($sawData) { Write-Status "$Target 静默超时，重新寻找设备" }
                else         { Write-Status "$Target $HandshakeTimeoutSec 秒无数据，判定不是设备口" }
                break
            }
        }
    }
    catch {
        # 设备被拔掉时，读取会抛非超时异常，走到这里
        Write-Status "$Target 读取中断：$($_.Exception.Message)"
    }
    finally {
        if ($sp -and $sp.IsOpen) { try { $sp.Close() } catch { } }
    }

    return $sawData
}

Write-Status "启动，等待设备…（父进程 PID $ParentPid）"

while (-not $script:ParentGone) {
    $targets = if ($Port) { @($Port) } else { @(Get-VibePorts) }

    if ($targets.Count -eq 0) {
        Write-Status '未找到 VibePhone 候选串口（VID_303A），2 秒后重试'
        Start-Sleep -Seconds 2
        continue
    }

    $found = $false
    foreach ($target in $targets) {
        if (Serve-Port -Target $target) { $found = $true; break }
    }

    if ($found) {
        Write-Status '与设备断开，2 秒后重新寻找'
    } else {
        Write-Status '所有候选口都没有数据，2 秒后重试'
    }
    Start-Sleep -Seconds 2
}

Write-Status '监听已退出，串口已释放'
