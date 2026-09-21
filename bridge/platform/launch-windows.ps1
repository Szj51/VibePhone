param(
    [Parameter(Mandatory=$true)][ValidateSet('codex','claude')][string]$Action,
    [Parameter(Mandatory=$true)][string]$ConfigPath,
    [Parameter(Mandatory=$true)][string]$UserHome
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$cfg = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json

function Open-ClaudeWindow {
    # The user's interactive CMD may add tools to PATH through its AutoRun.
    # Keep AutoRun enabled, and use a fixed command rather than shell user input.
    $terminal = Get-Command wt.exe -ErrorAction SilentlyContinue
    if ($terminal) {
        $terminalArgs = @('-w', 'new', 'new-tab', '--startingDirectory', ('"' + $UserHome + '"'), 'cmd.exe', '/k', 'claude')
        Start-Process -FilePath $terminal.Source -ArgumentList $terminalArgs -WorkingDirectory $UserHome -WindowStyle Normal | Out-Null
    } else {
        Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList @('/k', 'claude') -WorkingDirectory $UserHome -WindowStyle Normal | Out-Null
    }
    Write-Output '已打开新的 CMD 窗口，并提交 claude 命令。请在终端确认 Claude Code 启动结果。'
}

function Get-CodexProcessNames {
    # The Microsoft Store build of the desktop app runs as ChatGPT.exe; other
    # builds run as Codex.exe. Matching on a configurable list keeps a future
    # rename from silently breaking window detection.
    $names = @($cfg.codex.windowsProcessNames | Where-Object { $_ })
    if ($names.Count -eq 0) { $names = @('ChatGPT', 'Codex') }
    return $names
}

function Get-CodexWindows {
    # A non-zero MainWindowHandle with a non-empty title excludes the background
    # CLI/app-server/compute-use processes, which have no window.
    $names = Get-CodexProcessNames
    return @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $names -contains $_.ProcessName -and $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle
    })
}

function Start-CodexDesktop {
    $configured = [string]$cfg.codex.windowsExecutable
    if ($configured) {
        if (-not (Test-Path -LiteralPath $configured -PathType Leaf)) {
            throw "config.json 中的 Codex 桌面应用路径不存在：$configured"
        }
        Start-Process -FilePath $configured -WorkingDirectory $UserHome -WindowStyle Normal | Out-Null
        return
    }
    $names = @($cfg.codex.windowsStartAppNames | Where-Object { $_ })
    if ($names.Count -eq 0) { $names = @('Codex', 'ChatGPT') }
    $getStartApps = Get-Command Get-StartApps -ErrorAction SilentlyContinue
    if ($getStartApps) {
        $startApps = Get-StartApps -ErrorAction SilentlyContinue
        foreach ($name in $names) {
            $app = $startApps | Where-Object { $_.Name -eq $name } | Select-Object -First 1
            if ($app) {
                # Store (MSIX) apps must be launched through their AppUserModelID:
                # their package directory under WindowsApps is ACL-protected, so
                # Start-Process on the exe path is not reliable.
                Start-Process -FilePath "$env:SystemRoot\explorer.exe" -ArgumentList @("shell:AppsFolder\$($app.AppID)") -WindowStyle Normal | Out-Null
                return
            }
        }
    }
    # The desktop app registers this scheme. Do not run the unrelated `codex` CLI.
    try {
        Start-Process -FilePath 'codex://' -WindowStyle Normal | Out-Null
    } catch {
        throw '找不到 Codex 桌面应用入口。请把开始菜单里的实际名称（例如 ChatGPT）加入 config.json 的 codex.windowsStartAppNames，或在 codex.windowsExecutable 中填写桌面应用的完整 exe 路径。不要填写 Codex CLI 路径。'
    }
}

function Invoke-CodexNewWindow {
    # The shipped app's 文件 menu does offer 新建窗口, but assigns it no keyboard
    # accelerator, so it cannot be reached with SendKeys. Ctrl+Shift+N is bound to
    # 新建临时聊天, which opens a chat inside the same window and produces no new
    # HWND. Drive the menu item through UI Automation instead.
    #
    # The invoke must happen while the menu is still expanded: collapsing first
    # invalidates the popup element and GetCurrentPattern then throws
    # "不支持的模式".
    param(
        [Parameter(Mandatory=$true)][IntPtr]$RootHandle,
        [Parameter(Mandatory=$true)][int]$ProcessId
    )
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $menuCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::MenuItem)

    # Chromium builds its native menu bar lazily: until the window has been
    # activated at least once, the accessibility tree contains no MenuItem at all,
    # so a freshly launched Codex looks like it "has no menu bar". Activate it and
    # poll until the menu really shows up, rather than assuming it is there.
    [VibeWindow]::ShowWindowAsync($RootHandle, 9) | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $shell.AppActivate($ProcessId) | Out-Null
    [VibeWindow]::SetForegroundWindow($RootHandle) | Out-Null

    $menuBar = $null
    for ($i = 0; $i -lt 25; $i++) {
        Start-Sleep -Milliseconds 200
        $element = [System.Windows.Automation.AutomationElement]::FromHandle($RootHandle)
        if (-not $element) { continue }
        $found = $element.FindAll([System.Windows.Automation.TreeScope]::Descendants, $menuCondition)
        if ($found.Count -gt 0) { $menuBar = $found; break }
    }
    if (-not $menuBar) { throw 'Codex 窗口已激活，但仍未出现菜单栏，无法新建窗口。' }
    $expand = $menuBar.Item(0).GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)

    # 展开菜单后必须【轮询】等菜单项出现，不能只等一个固定时间。
    #
    # 这里原来是一句 Start-Sleep 700ms 然后只找一次，实测不够稳：
    # Codex 弹出菜单的渲染时间会波动，慢的时候 700ms 还没画出来，就会误报
    # 「菜单里没有「新建窗口」项」——这正是实测中出现过的间歇性失败。
    #
    # 现在改成：每次展开后最多等 3 秒（15 × 200ms），没等到就收起重来，共 3 轮。
    $item = $null
    for ($attempt = 1; $attempt -le 3 -and -not $item; $attempt++) {
        try { $expand.Expand() } catch { }
        for ($i = 0; $i -lt 15 -and -not $item; $i++) {
            Start-Sleep -Milliseconds 200
            # 弹出菜单在它自己的顶层窗口里，所以要从桌面根节点往下找
            $popup = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Descendants, $menuCondition)
            for ($j = 0; $j -lt $popup.Count; $j++) {
                if ($popup.Item($j).Current.Name -eq '新建窗口') { $item = $popup.Item($j); break }
            }
        }
        if (-not $item) {
            try { $expand.Collapse(); Start-Sleep -Milliseconds 300 } catch { }
        }
    }

    if (-not $item) { throw '菜单里没有「新建窗口」项，当前 Codex 版本可能已改版。' }

    try {
        $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    } finally {
        try { $expand.Collapse() } catch {}
    }
}

function Open-CodexWindow {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VibeWindow {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hWnd);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr param);
    public static long[] VisibleHandles(int[] pids) {
        var handles = new System.Collections.Generic.List<long>();
        EnumWindows((h,p) => {
            uint id; GetWindowThreadProcessId(h, out id);
            if (Array.IndexOf(pids, (int)id) >= 0 && IsWindowVisible(h) && GetWindowTextLength(h) > 0) handles.Add(h.ToInt64());
            return true;
        }, IntPtr.Zero);
        return handles.ToArray();
    }
}
'@
    $before = @(Get-CodexWindows)
    $beforeHandles = @([VibeWindow]::VisibleHandles([int[]]@($before | ForEach-Object { $_.Id })))
    if ($before.Count -eq 0) {
        Start-CodexDesktop
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Milliseconds 400
            $opened = @(Get-CodexWindows)
            if ($opened.Count -gt 0) { Write-Output 'Codex 桌面窗口已打开。'; return }
        }
        throw '已提交 Codex 启动请求，但未检测到桌面窗口。请确认桌面应用已安装，必要时设置 codex.windowsExecutable。'
    }
    # A window already exists: open another one through the menu. No focus stealing
    # is needed because UI Automation invokes the item in the background, and a new
    # HWND is verified rather than trusting that the request was accepted.
    Invoke-CodexNewWindow -RootHandle $before[0].MainWindowHandle -ProcessId $before[0].Id

    # 等新窗口出现。原来只等 6 秒（30 × 200ms），实测不够：
    # Codex 正在启动或正忙的时候，开一个新窗口可能要十几秒。
    # 放宽到 20 秒——上层 bridge.mjs 给这个脚本的超时是 45 秒，还有余量。
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 200
        $pids = [int[]]@(Get-CodexWindows | ForEach-Object { $_.Id })
        $after = @([VibeWindow]::VisibleHandles($pids))
        if (@($after | Where-Object { $beforeHandles -notcontains $_ }).Count -gt 0) {
            Write-Output '已检测到新的 Codex 桌面窗口。'
            return
        }
    }
    throw '已通过菜单调用「新建窗口」，但 20 秒内未检测到新窗口。请手动确认 Codex 是否响应。'
}

try {
    if ($Action -eq 'claude') { Open-ClaudeWindow } else { Open-CodexWindow }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
