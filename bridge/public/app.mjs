import { actionForKey } from './keymap.mjs';
const token = document.querySelector('meta[name="bridge-token"]').content;
const buttons = ['codex', 'claude'].map(id => document.getElementById(id));
const message = document.getElementById('message');
const connection = document.getElementById('connection');
let keys = {};
let busy = false;
let ready = false;
let activeRequest = false;

function updateButtons() { for (const button of buttons) button.disabled = !ready || busy; }
function focusStatus() {
  const active = document.hasFocus();
  const el = document.getElementById('focus-status');
  el.classList.toggle('active', active);
  el.textContent = active ? '● 页面已获得焦点，快捷键生效。' : '○ 页面不在前台，快捷键暂停。点击页面恢复。';
}
async function request(url, options = {}) {
  const response = await fetch(url, { ...options, headers: { 'X-Bridge-Token': token, ...(options.method ? { 'Content-Type': 'application/json' } : {}) }, signal: AbortSignal.timeout(options.method ? 50000 : 5000) });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || '请求失败');
  return result;
}
function renderEvents(events) {
  const list = document.getElementById('events');
  list.replaceChildren();
  for (const event of events) {
    const item = document.createElement('li');
    item.className = event.ok ? '' : 'fail';
    const time = document.createElement('time'); time.textContent = new Date(event.time).toLocaleTimeString('zh-CN', { hour12: false });
    const title = document.createElement('span'); title.textContent = event.action === 'codex' ? 'Codex' : 'Claude Code';
    const result = document.createElement('span'); result.className = 'result'; result.textContent = event.message;
    item.append(time, title, result); list.append(item);
  }
}
async function sync() {
  try {
    const state = await request('/api/status');
    keys = state.keys; ready = true; busy = state.busy || activeRequest;
    connection.textContent = state.testMode ? '已连接 · 测试模式' : '本机服务已连接'; connection.className = 'pill ready';
    document.getElementById('test-banner').hidden = !state.testMode;
    for (const action of ['codex', 'claude']) document.getElementById(`key-${action}`).textContent = keys[action];
    document.getElementById('terminal-label').textContent = state.platform === 'darwin' ? '新建 Terminal 并运行 claude' : '新建 CMD 并运行 claude';
    renderEvents(state.events);
  } catch (error) {
    ready = false; connection.textContent = '服务未连接'; connection.className = 'pill';
    if (!activeRequest) { message.textContent = `无法连接桥接服务：${error.message}。请运行启动脚本后刷新页面。`; message.className = 'error'; }
  }
  updateButtons();
}
async function trigger(action) {
  if (busy || !ready) return;
  busy = true; activeRequest = true; updateButtons(); message.className = '';
  message.textContent = action === 'codex' ? '正在打开 Codex，并检查窗口…' : '正在打开新的终端…';
  try { const result = await request(`/api/launch/${action}`, { method: 'POST', body: '{}' }); message.textContent = result.message; }
  catch (error) { message.textContent = error.message; message.className = 'error'; }
  finally { activeRequest = false; busy = false; await sync(); }
}
for (const button of buttons) button.addEventListener('click', () => trigger(button.id));
document.addEventListener('keydown', event => {
  const action = actionForKey(event, keys, document.hasFocus());
  if (!action) return;
  event.preventDefault(); trigger(action);
});
window.addEventListener('focus', focusStatus); window.addEventListener('blur', focusStatus);
document.addEventListener('visibilitychange', focusStatus);
focusStatus(); await sync(); setInterval(sync, 2500);
