use framework "Foundation"
use scripting additions

on run argv
    set actionName to item 1 of argv
    set configPath to item 2 of argv
    set userHome to item 3 of argv
    if actionName is "claude" then
        -- With no target window, do script creates a NEW Terminal window.
        tell application "Terminal"
            do script "cd " & quoted form of userHome & "; claude"
            activate
        end tell
        return "已打开新的 Terminal 窗口，并提交 claude 命令。请在终端确认 Claude Code 启动结果。"
    end if
    if actionName is not "codex" then error "未知启动目标。"
    set configData to current application's NSData's dataWithContentsOfFile:configPath
    set configObject to current application's NSJSONSerialization's JSONObjectWithData:configData options:0 |error|:(missing value)
    if configObject is missing value then error "无法读取 config.json。"
    set appName to ((configObject's objectForKey:"codex")'s objectForKey:"macApp") as text
    -- Resolve the app by Launch Services, not the Codex CLI in PATH.
    set appPath to POSIX path of (path to application appName)
    set appURL to current application's NSURL's fileURLWithPath:appPath
    set appBundle to current application's NSBundle's bundleWithURL:appURL
    set bundleID to (appBundle's bundleIdentifier()) as text
    tell application "System Events"
        set wasRunning to exists (first application process whose bundle identifier is bundleID)
    end tell
    do shell script "/usr/bin/open -a " & quoted form of appPath
    if not wasRunning then return "已提交 Codex 桌面应用启动请求。"
    try
        tell application "System Events"
            set codexProcess to first application process whose bundle identifier is bundleID
            tell codexProcess
                set frontmost to true
                set beforeCount to count of windows
                delay 0.3
                if not frontmost then error "无法将 Codex 切到前台，已取消快捷键发送。"
                keystroke "n" using {command down, shift down}
                repeat 25 times
                    delay 0.2
                    if (count of windows) > beforeCount then return "已检测到新的 Codex 桌面窗口。"
                end repeat
            end tell
        end tell
    on error detail number code
        error "无法操作 Codex 窗口。请检查系统设置 → 隐私与安全性 → 辅助功能／自动化，允许启动桥接程序的终端控制 System Events。原始错误：" & detail number code
    end try
    error "Codex 已在前台，但 Command+Shift+N 后未检测到新窗口。当前版本的新窗口入口尚未验证；本次不计为新窗口成功。"
end run
