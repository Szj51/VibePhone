import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import path from 'node:path';
import os from 'node:os';
const runFile = promisify(execFile);

export function buildLaunchCommand(action, { platform = process.platform, root, configPath, home = os.homedir() }) {
  if (!['codex', 'claude'].includes(action)) throw new Error('未知启动目标');
  if (platform === 'win32') {
    const executable = path.win32.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
    return { executable, args: ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', path.join(root, 'platform', 'launch-windows.ps1'), '-Action', action, '-ConfigPath', configPath, '-UserHome', home] };
  }
  if (platform === 'darwin') {
    return { executable: '/usr/bin/osascript', args: [path.join(root, 'platform', 'launch-macos.applescript'), action, configPath, home] };
  }
  throw new Error('目前仅支持 Windows 和 macOS。');
}

export async function launchApplication(action, options, runner = runFile) {
  const command = buildLaunchCommand(action, options);
  try {
    const { stdout, stderr } = await runner(command.executable, command.args, {
      windowsHide: true, timeout: 45000, maxBuffer: 256 * 1024, encoding: 'utf8'
    });
    const message = stdout.trim();
    if (!message) throw new Error(stderr.trim() || '系统没有返回启动结果。');
    return { message };
  } catch (error) {
    const detail = error.stderr?.trim() || error.stdout?.trim() || error.message;
    throw new Error(detail.replace(/^Command failed:.*\n/, '').slice(0, 1600));
  }
}
