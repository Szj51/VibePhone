import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { createBridge, validateConfig } from '../bridge.mjs';
import { actionForKey } from '../public/keymap.mjs';
import { buildLaunchCommand, launchApplication } from '../launcher.mjs';

const config = { port: 17653, cooldownMs: 100, keys: { codex: '1', claude: '2' }, codex: { windowsExecutable: '', macApp: 'Codex', newWindowShortcut: 'shift+n' } };

// fetch()/undici silently drops the Host header (it is a forbidden header), so a
// forged-Host check can only be exercised over a raw socket. Browsers do send the
// attacker-controlled Host during a DNS-rebinding attempt, so this path matters.
function getWithHost(origin, host) {
  const { port } = new URL(origin);
  return new Promise((resolve, reject) => {
    const request = http.request({ host: '127.0.0.1', port, path: '/', method: 'GET', headers: { Host: host } }, response => {
      response.resume();
      response.on('end', () => resolve(response.statusCode));
    });
    request.on('error', reject);
    request.end();
  });
}

async function fixture(t, launch) {
  const { server, token } = await createBridge({ config, launch, testMode: false, root: 'nonexistent-test-log-directory' });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  t.after(() => new Promise(resolve => { server.close(resolve); server.closeAllConnections(); }));
  const origin = `http://127.0.0.1:${server.address().port}`;
  const headers = { 'X-Bridge-Token': token, Origin: origin, 'Content-Type': 'application/json' };
  return { origin, headers, token, post: (target, overrides = {}) => fetch(origin + '/api/launch/' + target, { method: 'POST', headers: { ...headers, ...overrides }, body: '{}' }) };
}

test('only foreground unmodified digit presses launch; repeats and text fields never launch', () => {
  for (const [key, app] of [['1', 'codex'], ['2', 'claude']]) {
    assert.equal(actionForKey({ key }, config.keys), app);
    for (const flag of ['repeat', 'isComposing', 'ctrlKey', 'altKey', 'metaKey', 'shiftKey']) assert.equal(actionForKey({ key, [flag]: true }, config.keys), null);
    assert.equal(actionForKey({ key }, config.keys, false), null);
    for (const tagName of ['INPUT', 'TEXTAREA', 'SELECT']) assert.equal(actionForKey({ key, target: { tagName } }, config.keys), null);
    assert.equal(actionForKey({ key, target: { isContentEditable: true } }, config.keys), null);
  }
  assert.equal(actionForKey({ key: '3' }, config.keys), null);
  assert.equal(actionForKey({ key: 'F13' }, { codex: 'F13', claude: 'F14' }), 'codex');
});

test('config rejects collisions and invalid ports before opening a listener', () => {
  assert.throws(() => validateConfig({ ...config, keys: { codex: '1', claude: '1' } }));
  assert.throws(() => validateConfig({ ...config, port: '17653' }));
  assert.throws(() => validateConfig({ ...config, port: 65536 }));
  assert.throws(() => validateConfig({ ...config, keys: { codex: '../run', claude: '2' } }));
});

test('the Windows launcher keeps a UTF-8 BOM so PowerShell 5.1 can parse it', async () => {
  // Without a BOM, Windows PowerShell decodes the script as ANSI. The CJK
  // comments then mangle into byte pairs that unbalance quoting, the @'...'@
  // here-string never opens, and Add-Type's C# is parsed as PowerShell -
  // 34 syntax errors and no launcher at all.
  const bytes = await readFile(new URL('../platform/launch-windows.ps1', import.meta.url));
  assert.deepEqual([...bytes.subarray(0, 3)], [0xef, 0xbb, 0xbf]);
  assert.match(bytes.toString('utf8'), /Add-Type -TypeDefinition @'/);
  assert.match(bytes.toString('utf8'), /\n'@/);
});

test('device section defaults to disabled and rejects malformed button maps', () => {
  // 不写 device 整段时，行为必须和第一步完全一致（只靠网页按键）。
  const absent = validateConfig({ ...config, codex: { ...config.codex } });
  assert.deepEqual(absent.device, { enabled: false, port: '', buttons: ['codex', 'claude'] });

  const explicit = validateConfig({ ...config, codex: { ...config.codex }, device: { enabled: true, buttons: ['claude', 'codex'] } });
  assert.equal(explicit.device.enabled, true);
  assert.equal(explicit.device.port, '');            // 缺省补空串 = 自动发现
  assert.deepEqual(explicit.device.buttons, ['claude', 'codex']);

  assert.throws(() => validateConfig({ ...config, codex: { ...config.codex }, device: { enabled: 'yes', buttons: ['codex'] } }));
  assert.throws(() => validateConfig({ ...config, codex: { ...config.codex }, device: { enabled: true, buttons: [] } }));
  assert.throws(() => validateConfig({ ...config, codex: { ...config.codex }, device: { enabled: true, buttons: ['vscode'] } }));
  assert.throws(() => validateConfig({ ...config, codex: { ...config.codex }, device: { enabled: true, port: 7, buttons: ['codex'] } }));
});

test('Store-build app and process names default in, and malformed lists are rejected', () => {
  const filled = validateConfig({ ...config, codex: { ...config.codex } });
  assert.deepEqual(filled.codex.windowsStartAppNames, ['Codex', 'ChatGPT']);
  assert.deepEqual(filled.codex.windowsProcessNames, ['ChatGPT', 'Codex']);
  assert.throws(() => validateConfig({ ...config, codex: { ...config.codex, windowsProcessNames: 'ChatGPT' } }));
  assert.throws(() => validateConfig({ ...config, codex: { ...config.codex, windowsStartAppNames: [''] } }));
});

test('HTTP trigger dispatches only the requested app and preserves errors', async t => {
  let calls = [];
  const f = await fixture(t, async action => { calls.push(action); throw new Error('Codex new window not detected'); });
  const response = await f.post('codex');
  assert.equal(response.status, 500);
  assert.match((await response.json()).error, /new window not detected/);
  assert.deepEqual(calls, ['codex']);
  const state = await (await fetch(f.origin + '/api/status', { headers: f.headers })).json();
  assert.equal(state.busy, false);
  assert.equal(state.events[0].ok, false);
});

test('cross-site, missing token, unknown actions, GET and oversized requests cannot launch apps', async t => {
  let calls = 0;
  const f = await fixture(t, async () => { calls++; return { message: 'opened' }; });
  assert.equal((await f.post('claude', { Origin: 'https://untrusted.example' })).status, 403);
  assert.equal((await f.post('claude', { 'X-Bridge-Token': '' })).status, 403);
  assert.equal((await f.post('shell')).status, 404);
  assert.equal((await fetch(f.origin + '/api/launch/claude', { headers: f.headers })).status, 404);
  assert.equal((await fetch(f.origin + '/api/launch/claude', { method: 'POST', headers: f.headers, body: 'x'.repeat(2048) })).status, 413);
  assert.equal(await getWithHost(f.origin, 'evil.example'), 403);
  assert.equal(await getWithHost(f.origin, new URL(f.origin).host), 200);
  assert.equal(calls, 0);
});

test('busy guard prevents overlapping launches', async t => {
  let release, entered;
  const started = new Promise(resolve => { entered = resolve; });
  let calls = 0;
  const f = await fixture(t, async () => { calls++; entered(); await new Promise(resolve => { release = resolve; }); return { message: 'new terminal requested' }; });
  const first = f.post('claude');
  await started;
  assert.equal((await f.post('claude')).status, 429);
  release();
  assert.equal((await first).status, 200);
  assert.equal(calls, 1);
});

test('test mode exercises UI/API without invoking native launchers', async t => {
  let calls = 0;
  const { server } = await createBridge({ config, testMode: true, launch: async () => { calls++; } });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  t.after(() => new Promise(resolve => { server.close(resolve); server.closeAllConnections(); }));
  const origin = `http://127.0.0.1:${server.address().port}`;
  const html = await (await fetch(origin)).text();
  const token = /name="bridge-token" content="([a-f0-9]+)"/.exec(html)[1];
  const response = await fetch(origin + '/api/launch/claude', { method: 'POST', headers: { Origin: origin, 'X-Bridge-Token': token, 'Content-Type': 'application/json' }, body: '{}' });
  assert.equal(response.status, 200);
  assert.match((await response.json()).message, /测试模式/);
  assert.equal(calls, 0);
});

test('native launch invocation preserves paths as arguments and never uses a shell string', async () => {
  const windows = buildLaunchCommand('claude', { platform: 'win32', root: 'C:/测试 项目', configPath: 'C:/测试 项目/config.json', home: "C:/Users/O'Brien" });
  assert.equal(windows.args[windows.args.indexOf('-UserHome') + 1], "C:/Users/O'Brien");
  assert.equal(windows.args[windows.args.indexOf('-Action') + 1], 'claude');
  const mac = buildLaunchCommand('codex', { platform: 'darwin', root: '/tmp/a b', configPath: '/tmp/a b/config.json', home: '/Users/a b' });
  assert.equal(mac.executable, '/usr/bin/osascript');
  assert.equal(mac.args.at(-1), '/Users/a b');
  assert.throws(() => buildLaunchCommand('cmd & del', { platform: 'win32' }));
  await assert.rejects(() => launchApplication('codex', { platform: 'darwin', root: '.', configPath: 'config.json' }, async () => { const e = new Error('process failed'); e.stderr = 'New window failed'; throw e; }), /New window failed/);
});
