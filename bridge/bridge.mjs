import http from 'node:http';
import { readFile, appendFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes, timingSafeEqual } from 'node:crypto';
import { spawn } from 'node:child_process';
import readline from 'node:readline';
import { launchApplication } from './launcher.mjs';

export const ROOT = path.dirname(fileURLToPath(import.meta.url));
export const CONFIG_PATH = path.join(ROOT, 'config.json');

export function validateConfig(c) {
  if (!c || !Number.isInteger(c.port) || c.port < 1024 || c.port > 65535) throw new Error('config.json: port 必须是 1024～65535 的整数。');
  if (!c.keys || typeof c.keys.codex !== 'string' || typeof c.keys.claude !== 'string' || c.keys.codex === c.keys.claude) throw new Error('config.json: 两个快捷键必须不同。');
  for (const key of [c.keys.codex, c.keys.claude]) {
    if (!/^(?:[0-9]|F(?:[1-9]|1[0-9]|2[0-4]))$/.test(key)) throw new Error('config.json: 快捷键只支持数字或 F1～F24。');
  }
  if (!Number.isInteger(c.cooldownMs) || c.cooldownMs < 100 || c.cooldownMs > 10000) throw new Error('config.json: cooldownMs 应为 100～10000。');
  if (!c.codex || typeof c.codex.windowsExecutable !== 'string' || typeof c.codex.macApp !== 'string' || !c.codex.macApp.trim()) throw new Error('config.json: Codex 应用设置无效。');
  if (c.codex.newWindowShortcut !== 'shift+n') throw new Error('当前只支持 Ctrl/Cmd+Shift+N 新窗口快捷键。');
  // The desktop app ships as a Store (MSIX) package whose Start Menu name and
  // process name differ from the brand: it appears as "ChatGPT" and runs as
  // ChatGPT.exe. Keep both spellings so the launcher survives either build.
  for (const [field, fallback] of [['windowsStartAppNames', ['Codex', 'ChatGPT']], ['windowsProcessNames', ['ChatGPT', 'Codex']]]) {
    const value = c.codex[field];
    if (value === undefined) { c.codex[field] = fallback; continue; }
    if (!Array.isArray(value) || value.some(name => typeof name !== 'string' || !name.trim())) {
      throw new Error(`config.json: codex.${field} 必须是非空字符串数组。`);
    }
  }
  // ESP32-S3 的串口触发源。整段可以不写——不写就完全维持只靠网页按键的行为。
  if (c.device === undefined) {
    c.device = { enabled: false, port: '', buttons: ['codex', 'claude'] };
  } else {
    if (typeof c.device !== 'object' || c.device === null) throw new Error('config.json: device 必须是对象。');
    if (typeof c.device.enabled !== 'boolean') throw new Error('config.json: device.enabled 必须填 true 或 false。');
    if (c.device.port === undefined) c.device.port = '';
    if (typeof c.device.port !== 'string') throw new Error('config.json: device.port 必须是字符串，留空表示自动发现。');
    if (!Array.isArray(c.device.buttons) || c.device.buttons.length === 0 ||
        c.device.buttons.some(a => a !== 'codex' && a !== 'claude')) {
      throw new Error('config.json: device.buttons 必须是 codex/claude 组成的非空数组，按按键编号顺序排列。');
    }
  }
  return c;
}

// PowerShell 的绝对路径。不走 PATH，避免被别的同名程序顶掉。
function powershellPath() {
  return path.win32.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
}

export async function createBridge({
  config, launch = launchApplication, platform = process.platform,
  testMode = false, root = ROOT, configPath = CONFIG_PATH, withDevice = false
} = {}) {
  config = validateConfig(config || JSON.parse(await readFile(configPath, 'utf8')));
  const token = randomBytes(32).toString('hex');
  const assets = new Map();
  for (const [url, file, type] of [
    ['/', 'index.html', 'text/html; charset=utf-8'],
    ['/style.css', 'style.css', 'text/css; charset=utf-8'],
    ['/app.mjs', 'app.mjs', 'text/javascript; charset=utf-8'],
    ['/keymap.mjs', 'keymap.mjs', 'text/javascript; charset=utf-8']
  ]) {
    let body = await readFile(path.join(ROOT, 'public', file), 'utf8');
    if (url === '/') body = body.replace('__BRIDGE_TOKEN__', token);
    assets.set(url, { body, type });
  }
  let busy = false;
  let lastLaunch = -Infinity;
  const events = [];

  // ---------------------------------------------------------------
  //  唯一的启动入口
  //  网页按键和 ESP32 串口消息都走这里，所以去重、冷却、日志、
  //  错误处理只有一份实现，两个触发源的待遇完全一致。
  // ---------------------------------------------------------------
  async function triggerLaunch(action, source = 'console') {
    if (busy || Date.now() - lastLaunch < config.cooldownMs) {
      return { status: 429, error: '正在启动，请稍候再按。', event: null };
    }
    busy = true;
    lastLaunch = Date.now();
    const event = { action, source, time: new Date().toISOString(), ok: false, message: '' };
    try {
      const result = testMode
        ? { message: `测试模式：已收到 ${action} 启动请求，未打开应用。` }
        : await launch(action, { platform, root, configPath });
      event.ok = true;
      event.message = result.message;
      return { status: 200, event };
    } catch (error) {
      event.message = error.message;
      return { status: 500, error: error.message, event };
    } finally {
      busy = false;
      events.unshift(event);
      events.splice(20);
      // Do not log keys, transcripts, credentials or user terminal content.
      if (!testMode) appendFile(path.join(root, 'bridge.log'), JSON.stringify(event) + '\n').catch(() => {});
    }
  }

  // ---------------------------------------------------------------
  //  串口触发源（ESP32-S3 的 USB CDC）
  //
  //  serial-windows.ps1 负责自动找 COM 口、读行、原样打到 stdout；
  //  这里只读它的 stdout，不碰串口本身。子进程断线会自己重连。
  // ---------------------------------------------------------------
  let serialChild = null;

  function handleDeviceLine(line) {
    let msg;
    try { msg = JSON.parse(line); } catch { return; }        // 非 JSON 行一律忽略
    if (!msg || typeof msg !== 'object') return;

    if (msg.t === 'hello') {
      console.log(`[serial] 设备已握手，按键数 ${msg.btns ?? '?'}`);
      return;
    }
    // 只在【按下】时触发；松开事件仅用于日志/将来做长按等扩展。
    if (msg.t !== 'key' || msg.ev !== 'down') return;

    const action = config.device.buttons[msg.btn - 1];
    if (!action) {
      console.log(`[serial] 收到未知按键编号 ${msg.btn}，忽略`);
      return;
    }
    triggerLaunch(action, `device:${msg.btn}`)
      .then(outcome => {
        const text = outcome.event ? outcome.event.message : outcome.error;
        console.log(`[serial] 按键 ${msg.btn} → ${action}：${text}`);
      })
      .catch(() => {});
  }

  function startSerial() {
    if (!config.device.enabled) return null;
    if (platform !== 'win32') { console.log('[serial] 当前平台不支持串口触发（仅 Windows）。'); return null; }
    const args = ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                  '-File', path.join(root, 'platform', 'serial-windows.ps1')];
    // 把自己的 PID 传过去，让监听脚本能发现父进程已退出并自杀——
    // 否则 bridge.mjs 异常死掉时可能留下孤儿进程占着串口（详见脚本里的说明）。
    args.push('-ParentPid', String(process.pid));
    if (config.device.port) args.push('-Port', config.device.port);
    const child = spawn(powershellPath(), args, { windowsHide: true });
    readline.createInterface({ input: child.stdout }).on('line', handleDeviceLine);
    child.stderr.on('data', chunk => {
      const text = chunk.toString().trim();
      if (text) console.log(text);                            // 监听脚本用 stderr 报状态
    });
    child.on('error', error => console.error(`[serial] 启动监听失败：${error.message}`));
    child.on('exit', code => { if (code) console.error(`[serial] 监听进程退出（代码 ${code}）`); });
    return child;
  }

  function stopSerial() {
    if (serialChild && !serialChild.killed) { try { serialChild.kill(); } catch {} }
    serialChild = null;
  }

  if (withDevice) serialChild = startSerial();

  const server = http.createServer(async (req, res) => {
    const origin = `http://127.0.0.1:${server.address().port}`;
    const headers = {
      'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff',
      'Referrer-Policy': 'no-referrer',
      'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
    };
    function send(status, payload, type = 'application/json; charset=utf-8') {
      res.writeHead(status, { ...headers, 'Content-Type': type });
      res.end(type.startsWith('application/json') ? JSON.stringify(payload) : payload);
    }
    if (req.headers.host !== `127.0.0.1:${server.address().port}`) return send(403, { error: 'Invalid host' });
    const url = new URL(req.url, origin);
    if (req.method === 'GET' && url.pathname === '/health') return send(200, { app: 'vibe-keyboard-bridge', version: '0.2.0', root, testMode });
    if (req.method === 'GET' && assets.has(url.pathname)) {
      if (req.headers['sec-fetch-site'] === 'cross-site') return send(403, { error: '请从本机启动脚本打开控制台。' });
      const asset = assets.get(url.pathname);
      return send(200, asset.body, asset.type);
    }
    const supplied = Buffer.from(String(req.headers['x-bridge-token'] || ''));
    const expected = Buffer.from(token);
    if (supplied.length !== expected.length || !timingSafeEqual(supplied, expected)) return send(403, { error: '控制台连接已失效，请刷新页面。' });
    if (req.method === 'GET' && url.pathname === '/api/status') return send(200, { platform, keys: config.keys, busy, testMode, events, device: config.device });
    const match = /^\/api\/launch\/(codex|claude)$/.exec(url.pathname);
    if (req.method !== 'POST' || !match) return send(404, { error: '没有这个接口。' });
    if (req.headers.origin !== origin || req.headers['content-type'] !== 'application/json') return send(403, { error: '拒绝来自其他网页的启动请求。' });
    if (Number(req.headers['content-length'] || 0) > 1024) return send(413, { error: '请求过大。' });
    let bytes = 0;
    for await (const chunk of req) {
      bytes += chunk.length;
      if (bytes > 1024) { send(413, { error: '请求过大。' }); return; }
    }
    const outcome = await triggerLaunch(match[1], 'console');
    if (outcome.event) send(outcome.status, outcome.error ? { ...outcome.event, error: outcome.error } : outcome.event);
    else send(outcome.status, { error: outcome.error });
  });
  server.requestTimeout = 10000;
  return { server, token, stopSerial, get serial() { return serialChild; } };
}

export function openBrowser(url) {
  let child;
  if (process.platform === 'win32') {
    // URL is constructed locally from a validated integer port, never from a request.
    const script = `Start-Process '${url}'`;
    child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], { windowsHide: true, stdio: 'ignore' });
  } else if (process.platform === 'darwin') child = spawn('/usr/bin/open', [url], { stdio: 'ignore' });
  else return;
  child.on('error', error => console.error(`请手动打开 ${url}：${error.message}`));
  child.unref();
}

async function main() {
  const config = validateConfig(JSON.parse(await readFile(CONFIG_PATH, 'utf8')));
  if (process.argv.includes('--check')) {
    buildCheck(config);
    return;
  }
  const testMode = process.argv.includes('--test-mode');
  if (!['win32', 'darwin'].includes(process.platform) && !testMode) throw new Error('目前仅支持 Windows 和 macOS。');
  const { server, stopSerial } = await createBridge({ config, testMode, withDevice: true });
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(config.port, '127.0.0.1', resolve); });
  const url = `http://127.0.0.1:${config.port}/`;
  const deviceLine = config.device.enabled && process.platform === 'win32'
    ? 'ESP32 串口触发：已开启（自动发现 COM 口）。'
    : 'ESP32 串口触发：未开启。';
  console.log(`Vibe Keyboard Bridge 0.2.0\n${url}\n${testMode ? '测试模式，不会启动应用。' : '网页按键需页面获得焦点。'}\n${deviceLine}\n按 Ctrl+C 停止服务。`);
  if (!process.argv.includes('--no-browser')) openBrowser(url);

  // Ctrl+C 时把串口监听子进程一并带走，避免留下孤儿进程占着 COM 口。
  const shutdown = () => { stopSerial(); server.close(() => process.exit(0)); setTimeout(() => process.exit(0), 500).unref(); };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

function buildCheck(config) {
  console.log(JSON.stringify({ node: process.version, platform: process.platform, port: config.port, keys: config.keys,
    device: config.device, config: 'OK', systemSupported: ['win32', 'darwin'].includes(process.platform),
    note: '配置检查不代表应用已经启动，也不验证 Codex 新窗口快捷键。' }, null, 2));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => { console.error(error.code === 'EADDRINUSE' ? '端口被占用。可能桥接程序已经运行，请打开控制台或更改 config.json 的 port。' : error.message); process.exitCode = 1; });
}
