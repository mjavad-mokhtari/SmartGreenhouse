const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const fs = require('fs');
const path = require('path');
const session = require('express-session');
const crypto = require('crypto');

// --------------- Config ---------------
const PORT = process.env.PORT || 3000;
const DATA_DIR = path.join(__dirname, 'data');
const EVENTS_FILE = path.join(DATA_DIR, 'events.json');
const DEVICES_FILE = path.join(DATA_DIR, 'devices.json');
const USERS_FILE = path.join(DATA_DIR, 'users.json');
const FIRMWARE_DIR = path.join(__dirname, 'firmware');
const SESSION_SECRET = process.env.SESSION_SECRET || crypto.randomBytes(32).toString('hex');
const MAX_EVENTS = 20000;

// --------------- Store ---------------
let events = [];
let deviceStates = {};
let users = [];

function initStore() {
  [DATA_DIR, FIRMWARE_DIR].forEach(d => { if (!fs.existsSync(d)) fs.mkdirSync(d, { recursive: true }); });
  try { events = JSON.parse(fs.readFileSync(EVENTS_FILE, 'utf8') || '[]'); } catch(e) { events = []; }
  try { deviceStates = JSON.parse(fs.readFileSync(DEVICES_FILE, 'utf8') || '{}'); } catch(e) { deviceStates = {}; }
  try { users = JSON.parse(fs.readFileSync(USERS_FILE, 'utf8') || '[]'); } catch(e) { users = []; }
  console.log(`[Store] ${events.length} events, ${Object.keys(deviceStates).length} devices, ${users.length} users`);
}

function saveEvents() {
  if (events.length > MAX_EVENTS) events = events.slice(-MAX_EVENTS);
  try { fs.writeFileSync(EVENTS_FILE, JSON.stringify(events), 'utf8'); } catch(e) {}
}

function saveDevices() {
  try { fs.writeFileSync(DEVICES_FILE, JSON.stringify(deviceStates), 'utf8'); } catch(e) {}
}

function saveUsers() {
  try { fs.writeFileSync(USERS_FILE, JSON.stringify(users), 'utf8'); } catch(e) {}
}

function addEvent(deviceId, type, state, ts) {
  const e = { id: events.length + 1, device_id: deviceId, event_type: type, state, ts: ts || Date.now(), received_at: Date.now() };
  events.push(e);
  if (events.length % 20 === 0) saveEvents();
  return e;
}

function getStats() {
  const todayStart = new Date(); todayStart.setHours(0,0,0,0);
  const today = events.filter(e => e.received_at >= todayStart.getTime());
  const irrigationMinutes = today.filter(e => e.event_type === 'zone' && e.state.startsWith('start')).length * 15;
  return {
    devices: Object.keys(deviceStates).length,
    todayEvents: today.length,
    totalEvents: events.length,
    irrigationMinutes,
    lastIrrigation: today.filter(e => e.event_type === 'pump' && e.state === 'on').slice(-1)[0] || null
  };
}

// --------------- 2FA / TOTP ---------------
let otplib, QRCode;
try {
  otplib = require('otplib');
  QRCode = require('qrcode');
} catch(e) { console.log('[Auth] otplib/qrcode not available, 2FA disabled'); }

function hashPassword(pw) {
  const salt = crypto.randomBytes(16).toString('hex');
  return salt + ':' + crypto.pbkdf2Sync(pw, salt, 10000, 64, 'sha512').toString('hex');
}

function checkPassword(pw, stored) {
  const [salt, hash] = stored.split(':');
  return crypto.pbkdf2Sync(pw, salt, 10000, 64, 'sha512').toString('hex') === hash;
}

function findUser(username) { return users.find(u => u.username === username); }

// --------------- Express ---------------
const app = express();
app.use(express.json({ limit: '2mb' }));
app.use(express.urlencoded({ extended: true }));
app.use(session({
  secret: SESSION_SECRET,
  resave: false,
  saveUninitialized: false,
  cookie: { httpOnly: true, sameSite: 'lax', maxAge: 12 * 3600000 },
  proxy: true
}));
app.use((req, res, next) => {
  // Set secure cookie when behind HTTPS proxy
  if (req.session && req.session.cookie) {
    req.session.cookie.secure = req.secure || req.get('X-Forwarded-Proto') === 'https';
  }
  next();
});

app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET,POST,PUT,DELETE,OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, X-API-Key');
  if (req.method === 'OPTIONS') return res.sendStatus(200);
  next();
});

function requireAuth(req, res, next) {
  if (req.session && req.session.user) return next();
  if (req.path === '/login' || req.path === '/setup' || req.path === '/api/events' || req.path === '/api/health') return next();
  if (req.path.startsWith('/api/')) return res.status(401).json({ error: 'unauthorized' });
  return res.redirect('/login');
}

initStore();

// =========== ROUTES ===========

// Health
app.get('/api/health', (req, res) => {
  res.json({ status: 'ok', uptime: process.uptime(), devices: Object.keys(deviceStates).length, events: events.length, nodejs: process.version });
});

// ESP32 posts events
app.post('/api/events', (req, res) => {
  const apiKey = process.env.API_KEY;
  if (apiKey) {
    const provided = req.headers['x-api-key'] || '';
    if (provided !== apiKey) return res.status(401).json({ error: 'invalid api key' });
  }
  const { deviceId, uptime, events: evts, status: devStatus } = req.body;
  if (!deviceId) return res.status(400).json({ error: 'deviceId required' });
  deviceStates[deviceId] = { lastSeen: Date.now(), uptime: uptime || 0, ip: req.ip, status: devStatus || {} };
  saveDevices();
  let received = 0;
  if (evts && evts.length > 0) { for (const e of evts) { addEvent(deviceId, e.type, e.state, e.ts); received++; } }
  broadcast({ type: 'events', deviceId, events: (evts || []).slice(-10) });
  broadcastDeviceStatus();
  res.json({ ok: true, received });
});

// Get events
app.get('/api/events', (req, res) => {
  const deviceId = req.query.device, limit = parseInt(req.query.limit) || 100, since = parseInt(req.query.since) || 0;
  let filtered = deviceId ? events.filter(e => e.device_id === deviceId) : events;
  if (since > 0) filtered = filtered.filter(e => e.ts > since);
  res.json({ events: filtered.slice(-limit) });
});

// Get devices
app.get('/api/devices', (req, res) => {
  const list = Object.entries(deviceStates).map(([id, info]) => ({ deviceId: id, lastSeen: info.lastSeen, uptime: info.uptime, online: (Date.now() - info.lastSeen) < 120000 }));
  res.json({ devices: list, count: list.length });
});

// Stats
app.get('/api/stats', (req, res) => { res.json(getStats()); });

// Irrigation timeline data
app.get('/api/irrigation/timeline', (req, res) => {
  const todayStart = new Date(); todayStart.setHours(0,0,0,0);
  const today = events.filter(e => e.received_at >= todayStart.getTime());
  // find pump on/off pairs
  const periods = [];
  let pumpOn = null;
  for (const e of today) {
    if (e.event_type === 'pump') {
      if (e.state === 'on' && !pumpOn) pumpOn = e.received_at;
      else if (e.state === 'off' && pumpOn) {
        periods.push({ type: 'pump', start: pumpOn, end: e.received_at, duration: Math.round((e.received_at - pumpOn)/60000) });
        pumpOn = null;
      }
    }
    if (e.event_type === 'zone') {
      const match = e.state.match(/(?:start|on).*?(\d+)\s*min/i);
      const dur = match ? parseInt(match[1]) : 15;
      periods.push({ type: 'zone', zoneId: e.state, start: e.received_at, end: e.received_at + dur * 60000, duration: dur });
    }
  }
  if (pumpOn) periods.push({ type: 'pump', start: pumpOn, end: Date.now(), duration: Math.round((Date.now() - pumpOn)/60000), active: true });
  res.json({ periods, count: periods.length });
});

// Delete old events
app.delete('/api/events', (req, res) => {
  const days = parseInt(req.query.older_than) || 30;
  const cutoff = Date.now() - (days * 86400000);
  const before = events.length;
  events = events.filter(e => e.received_at >= cutoff);
  saveEvents();
  res.json({ ok: true, deleted: before - events.length });
});

// Deploy notify (Telegram via n8n)
app.post('/api/deploy/notify', (req, res) => {
  const { project, message, time, author, branch } = req.body;
  const n8nUrl = process.env.N8N_WEBHOOK_URL || 'http://localhost:5678/webhook/deploy';
  const payload = JSON.stringify({ project, message: (message||'').substring(0,200), time: time || new Date().toISOString(), author, branch });
  const url = new URL(n8nUrl);
  const opts = { hostname: url.hostname, port: url.port, path: url.pathname, method: 'POST', headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) }, timeout: 5000 };
  const req2 = http.request(opts, (r2) => {
    let body = ''; r2.on('data', d => body += d); r2.on('end', () => console.log('[Deploy] n8n:', r2.statusCode, body.substring(0,100)));
  });
  req2.on('error', e => console.log('[Deploy] n8n unreachable:', e.message));
  req2.write(payload); req2.end();
  res.json({ ok: true, n8n: n8nUrl });
});

// Firmware upload
app.post('/api/firmware', requireAuth, (req, res) => {
  // For raw binary upload via multipart, we use a simple approach
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', () => {
    const buf = Buffer.concat(chunks);
    const ts = Date.now();
    const fname = `firmware_${ts}.bin`;
    fs.writeFileSync(path.join(FIRMWARE_DIR, fname), buf);
    // Update latest symlink
    const latestPath = path.join(FIRMWARE_DIR, 'latest.json');
    fs.writeFileSync(latestPath, JSON.stringify({ file: fname, size: buf.length, ts, version: ts }));
    res.json({ ok: true, file: fname, size: buf.length });
  });
});

app.get('/api/firmware/latest', (req, res) => {
  try {
    const meta = JSON.parse(fs.readFileSync(path.join(FIRMWARE_DIR, 'latest.json'), 'utf8'));
    res.json(meta);
  } catch(e) { res.json({ file: null, version: 0 }); }
});

app.get('/api/firmware/download/:file', (req, res) => {
  const fpath = path.join(FIRMWARE_DIR, path.basename(req.params.file));
  if (fs.existsSync(fpath)) res.sendFile(fpath);
  else res.status(404).json({ error: 'not found' });
});

// =========== AUTH ROUTES ===========

app.get('/login', (req, res) => {
  if (req.session.user) return res.redirect('/');
  res.type('html').send(loginPage());
});

app.post('/login', (req, res) => {
  const { username, password, totp } = req.body;
  const user = findUser(username);
  if (!user) return res.type('html').send(loginPage('کاربر یافت نشد'));
  if (!checkPassword(password, user.passwordHash)) return res.type('html').send(loginPage('رمز عبور اشتباه'));
  if (otplib && user.totpSecret) {
    try {
      const valid = otplib.authenticator.check(totp || '', user.totpSecret);
      if (!valid) return res.type('html').send(loginPage('کد TOTP نامعتبر'));
    } catch(e) { return res.type('html').send(loginPage('خطا در بررسی کد')); }
  }
  req.session.user = { username: user.username };
  res.redirect('/');
});

app.get('/setup', (req, res) => {
  if (users.length > 0) return res.redirect('/login');
  res.type('html').send(setupPage());
});

app.post('/setup', async (req, res) => {
  if (users.length > 0) return res.redirect('/login');
  const { username, password } = req.body;
  if (!username || !password || username.length < 3 || password.length < 4) {
    return res.type('html').send(setupPage('نام کاربری (حداقل ۳) و رمز (حداقل ۴) الزامی'));
  }
  let totpSecret = '';
  let qrDataUrl = '';
  if (otplib) {
    totpSecret = otplib.authenticator.generateSecret();
    const otpauth = otplib.authenticator.keyuri(username, 'SmartGreenhouse', totpSecret);
    try {
      qrDataUrl = await QRCode.toDataURL(otpauth, { width: 250 });
    } catch(err) { console.log('[Setup] QR generation failed:', err.message); }
  }
  users.push({ username, passwordHash: hashPassword(password), totpSecret, created: Date.now() });
  saveUsers();
  if (otplib && qrDataUrl) {
    res.type('html').send(setupDonePage(qrDataUrl, totpSecret));
  } else {
    req.session.user = { username };
    res.redirect('/');
  }
});

app.get('/logout', (req, res) => { req.session.destroy(); res.redirect('/login'); });

// =========== DASHBOARD (after auth) ===========
app.get('/', requireAuth, (req, res) => {
  res.type('html').send(dashboardPage());
});

// =========== HTTP + WS ===========
const server = http.createServer(app);
const wss = new WebSocket.Server({ server, path: '/ws' });

function broadcast(msg) { const d = JSON.stringify(msg); wss.clients.forEach(c => { if (c.readyState === WebSocket.OPEN) c.send(d); }); }

function broadcastDeviceStatus() {
  const list = Object.entries(deviceStates).map(([id, info]) => ({ deviceId: id, lastSeen: info.lastSeen, uptime: info.uptime, online: (Date.now() - info.lastSeen) < 120000 }));
  wss.clients.forEach(c => { if (c.readyState === WebSocket.OPEN && c.deviceId === 'browser') c.send(JSON.stringify({ type: 'devices', devices: list })); });
}

wss.on('connection', (ws, req) => {
  const params = new URLSearchParams((req.url || '').split('?')[1] || '');
  ws.deviceId = params.get('device') || 'browser';
  ws.send(JSON.stringify({ type: 'connected', serverTime: Date.now(), clients: wss.clients.size }));
  broadcastDeviceStatus();
  ws.on('message', data => { try { const m = JSON.parse(data); if (m.type === 'ping') ws.send(JSON.stringify({ type:'pong', ts: Date.now() })); } catch(e) {} });
  ws.on('close', () => broadcastDeviceStatus());
});

// =========== PAGES ===========

function loginPage(error) {
  const err = error ? `<div style="background:rgba(239,68,68,.15);color:#ef4444;padding:10px;border-radius:8px;margin-bottom:16px;font-size:13px">${error}</div>` : '';
  return `<!DOCTYPE html><html lang="fa" dir="rtl"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ورود | گلخانه هوشمند</title>
<style>:root{--bg:#0f172a;--card:#1e293b;--accent:#10b981;--text:#f1f5f9;--sub:#94a3b8;--radius:14px}
*{margin:0;padding:0;box-sizing:border-box}body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px}
.box{background:var(--card);border-radius:var(--radius);padding:28px 22px;width:100%;max-width:380px}
.box h2{text-align:center;margin-bottom:6px;font-size:20px}.box .sub{text-align:center;color:var(--sub);font-size:12px;margin-bottom:20px}
.inp{width:100%;padding:12px;margin-bottom:10px;background:#0f172a;border:1px solid rgba(255,255,255,.1);border-radius:10px;color:var(--text);font-size:14px;outline:none}
.inp:focus{border-color:var(--accent)}
.btn{width:100%;padding:12px;background:var(--accent);color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;margin-top:6px}
.btn:hover{opacity:.9}
</style></head><body><div class="box"><h2>🌿 گلخانه هوشمند</h2><div class="sub">ورود به داشبورد</div>${err}
<form method="POST"><input class="inp" name="username" placeholder="نام کاربری" required><input class="inp" type="password" name="password" placeholder="رمز عبور" required><input class="inp" name="totp" placeholder="کد ۶ رقمی Google Authenticator" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" autocomplete="one-time-code"><button class="btn" type="submit">ورود</button></form></div></body></html>`;
}

function setupPage(error) {
  const err = error ? `<div style="background:rgba(239,68,68,.15);color:#ef4444;padding:10px;border-radius:8px;margin-bottom:16px;font-size:13px">${error}</div>` : '';
  return `<!DOCTYPE html><html lang="fa" dir="rtl"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>راه‌اندازی | گلخانه هوشمند</title>
<style>:root{--bg:#0f172a;--card:#1e293b;--accent:#10b981;--text:#f1f5f9;--sub:#94a3b8;--radius:14px}
*{margin:0;padding:0;box-sizing:border-box}body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px}
.box{background:var(--card);border-radius:var(--radius);padding:28px 22px;width:100%;max-width:380px}
.box h2{text-align:center;margin-bottom:6px}.box .sub{text-align:center;color:var(--sub);font-size:12px;margin-bottom:20px}
.inp{width:100%;padding:12px;margin-bottom:10px;background:#0f172a;border:1px solid rgba(255,255,255,.1);border-radius:10px;color:var(--text);font-size:14px;outline:none}
.inp:focus{border-color:var(--accent)}
.btn{width:100%;padding:12px;background:var(--accent);color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer}
</style></head><body><div class="box"><h2>🌿 راه‌اندازی اولیه</h2><div class="sub">تنظیم حساب کاربری مدیر</div>${err}
<form method="POST"><input class="inp" name="username" placeholder="نام کاربری (حداقل ۳ کاراکتر)" required minlength="3"><input class="inp" type="password" name="password" placeholder="رمز عبور (حداقل ۴ کاراکتر)" required minlength="4"><button class="btn" type="submit">ایجاد حساب</button></form></div></body></html>`;
}

function setupDonePage(qrDataUrl, secret) {
  return `<!DOCTYPE html><html lang="fa" dir="rtl"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>اسکن QR | گلخانه هوشمند</title>
<style>:root{--bg:#0f172a;--card:#1e293b;--accent:#10b981;--text:#f1f5f9;--sub:#94a3b8;--radius:14px}
*{margin:0;padding:0;box-sizing:border-box}body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px}
.box{background:var(--card);border-radius:var(--radius);padding:28px 22px;width:100%;max-width:380px;text-align:center}
.box h2{margin-bottom:6px}.box .sub{color:var(--sub);font-size:12px;margin-bottom:16px}
.qr{border-radius:12px;margin-bottom:14px}
.secret{background:#0f172a;padding:10px;border-radius:8px;font-family:monospace;font-size:14px;letter-spacing:2px;margin-bottom:14px;word-break:break-all}
.note{color:var(--sub);font-size:12px;margin-bottom:18px}
.btn{display:inline-block;padding:12px 28px;background:var(--accent);color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;text-decoration:none}
</style></head><body><div class="box"><h2>📱 اسکن QR کد</h2><div class="sub">با Google Authenticator اسکن کنید</div><img class="qr" src="${qrDataUrl}" alt="QR"><div class="secret">${secret}</div><div class="note">اگر اسکن نشد، کد بالا را دستی در Google Authenticator وارد کنید</div><a class="btn" href="/login">رفتن به صفحه ورود</a></div></body></html>`;
}

function dashboardPage() {
  return `<!DOCTYPE html><html lang="fa" dir="rtl"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no"><title>SmartGreenhouse</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--accent:#10b981;--blue:#3b82f6;--danger:#ef4444;--warn:#f59e0b;--text:#f1f5f9;--sub:#94a3b8;--muted:#64748b;--radius:12px;--gap:8px}
*{margin:0;padding:0;box-sizing:border-box}
body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;min-height:100vh;padding-bottom:40px;font-size:13px}
.container{max-width:560px;margin:0 auto;padding:10px}
.hdr{background:linear-gradient(135deg,#0f766e,#10b981);border-radius:var(--radius);padding:14px;margin-bottom:var(--gap);display:flex;justify-content:space-between;align-items:center}
.hdr h1{font-size:17px;font-weight:700}.hdr .dot{width:8px;height:8px;border-radius:50%;display:inline-block}.hdr .dot.g{background:#86efac;animation:pulse 2s infinite}.hdr .dot.r{background:var(--danger)}.hdr .time{font-size:11px;opacity:.8}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}

.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-bottom:var(--gap)}
.st{background:var(--card);border-radius:10px;padding:10px 6px;text-align:center}
.st .v{font-size:18px;font-weight:700}.st .l{font-size:9px;color:var(--sub);margin-top:2px}
.st.g .v{color:var(--accent)}.st.b .v{color:var(--blue)}.st.y .v{color:var(--warn)}.st.r .v{color:var(--danger)}

.timeline{background:var(--card);border-radius:var(--radius);padding:12px;margin-bottom:var(--gap)}
.tl-title{font-size:13px;font-weight:600;margin-bottom:8px;display:flex;align-items:center;gap:6px}
.tl-title::before{content:'';width:3px;height:14px;background:var(--accent);border-radius:2px}
.tl-canvas{width:100%;height:60px;display:block;border-radius:6px}

.tabs{display:flex;gap:4px;margin-bottom:var(--gap)}
.tab{flex:1;padding:9px 0;background:var(--card);border:none;color:var(--sub);font-size:12px;border-radius:8px;cursor:pointer;text-align:center;font-family:inherit}
.tab.on{background:var(--accent);color:#fff;font-weight:600}

.card{background:var(--card);border-radius:var(--radius);padding:12px;margin-bottom:6px}
.card .hdr{background:none;padding:0;display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.card .ttl{font-weight:600;font-size:14px}
.card .st-on{color:var(--accent);font-size:12px;font-weight:600}.card .st-off{color:var(--danger);font-size:12px}
.fr{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-top:8px}
.fr2{display:flex;justify-content:space-between;gap:6px;margin-top:8px}

.bt{border:none;padding:7px 14px;border-radius:8px;font-size:12px;font-weight:600;cursor:pointer;font-family:inherit;color:#fff}
.bt-g{background:var(--accent)}.bt-r{background:var(--danger)}.bt-o{background:var(--blue)}.bt-w{background:var(--warn);color:#000}
.bt-sm{padding:5px 10px;font-size:11px}
.in{background:#0f172a;border:1px solid rgba(255,255,255,.1);border-radius:7px;color:var(--text);padding:6px 8px;font-size:12px;outline:none}
.in:focus{border-color:var(--accent)}
.sel{background:#0f172a;border:1px solid rgba(255,255,255,.1);border-radius:7px;color:var(--text);padding:6px 8px;font-size:12px}

.log-list{max-height:400px;overflow-y:auto}
.log-row{padding:9px 12px;border-bottom:1px solid rgba(255,255,255,.04);display:flex;justify-content:space-between;align-items:center;gap:8px}
.log-row:last-child{border:none}
.log-t{font-size:12px;font-weight:500}
.log-s{font-size:10px;color:var(--sub);padding:2px 8px;background:rgba(255,255,255,.05);border-radius:5px;max-width:120px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.log-m{font-size:10px;color:var(--muted);white-space:nowrap}

.empty{padding:20px;text-align:center;color:var(--muted);font-size:12px}

.bar{display:flex;justify-content:space-between;align-items:center;padding:4px 0;margin-bottom:var(--gap)}
.bar span{font-size:10px;color:var(--muted)}
.bar a{color:var(--danger);text-decoration:none;font-size:11px}
.hidden{display:none!important}
@media(max-width:380px){.stats{grid-template-columns:repeat(2,1fr)}}
</style></head><body>
<div class="container">
<div class="hdr"><h1>🌿 SmartGreenhouse</h1><span style="display:flex;align-items:center;gap:8px"><span class="time" id="clock">--</span><span class="dot g" id="dot"></span></span></div>
<div class="stats">
<div class="st g"><div class="v" id="s1">0</div><div class="l">دستگاه</div></div>
<div class="st b"><div class="v" id="s2">0</div><div class="l">رویداد امروز</div></div>
<div class="st y"><div class="v" id="s3">0</div><div class="l">دقیقه آبیاری</div></div>
<div class="st r"><div class="v" id="s4">--</div><div class="l">آخرین آبیاری</div></div>
</div>
<div class="timeline"><div class="tl-title">⏱ نمودار آبیاری ۲۴ ساعته</div><canvas class="tl-canvas" id="tlCanvas" width="512" height="60"></canvas><div style="text-align:center;font-size:10px;color:var(--muted);margin-top:4px" id="tlLegend"></div></div>
<div class="tabs" id="tabs">
<button class="tab on" data-pg="irrigation">💧 آبیاری</button>
<button class="tab" data-pg="lighting">💡 نور</button>
<button class="tab" data-pg="logs">📋 لاگ</button>
<button class="tab" data-pg="settings">⚙️ تنظیمات</button>
</div>
<div id="pg-irrigation"></div><div id="pg-lighting" class="hidden"></div><div id="pg-logs" class="hidden"></div><div id="pg-settings" class="hidden"></div>
<div class="bar"><span id="lu">--</span><a href="/logout">🚪 خروج</a></div></div>
<script>
const A=location.origin,W=(A.startsWith('https')?'wss://':'ws://')+location.host+'/ws?device=browser';
let ws,page='irrigation',clockTimer;
function $(id){return document.getElementById(id)}
function F(ts){return new Date(ts).toLocaleTimeString('fa-IR',{hour:'2-digit',minute:'2-digit'})}
function G(id,v){$(id).textContent=v}

function startWS(){
  ws=new WebSocket(W);ws.onopen=()=>{$('dot').className='dot g'};
  ws.onclose=()=>{$('dot').className='dot r';setTimeout(startWS,3000)};
  ws.onerror=()=>ws.close();
  ws.onmessage=e=>{try{const m=JSON.parse(e.data);if(m.type==='events')refreshWithDelay(1000);if(m.type==='devices')refreshWithDelay(500)}catch(ex){}}
}
startWS();

function refreshWithDelay(d){setTimeout(refresh,d)}

function pad(n){return(n<10?'0':'')+n}

async function refresh(){
  try{
    const r=await fetch(A+'/api/status-full'),d=await r.json();
    updateStats(d.stats||{});
    if(page==='irrigation'&&d.irrigation)renderIrrigation(d.irrigation);
    if(page==='lighting'&&d.lighting)renderLighting(d.lighting);
    if(page==='logs')loadLogs();
    if(page==='irrigation'||page==='lighting')drawTimeline(d.timeline||[]);
  }catch(e){}
}
async function loadLogs(){try{const r=await fetch(A+'/api/events?limit=50'),d=await r.json();renderLogs(d.events||[])}catch(e){}}
async function loadTimeline(){try{const r=await fetch(A+'/api/irrigation/timeline'),d=await r.json();drawTimeline(d.periods||[])}catch(e){}}
function updateStats(s){
  G('s1',s.devices||0);
  G('s2',s.todayEvents||0);
  G('s3',s.irrigationMinutes||0);
  const last = s.lastIrrigation;
  G('s4',last ? F(last.received_at||last.ts) : '--');
  G('lu','به‌روز: '+F(Date.now()));
}

function renderIrrigation(d){
  const el=$('pg-irrigation');
  const zones=d.zones||[];
  if(!zones.length){el.innerHTML='<div class="card"><span class="ttl">آبیاری</span><div class="empty">داده‌ای از دستگاه دریافت نشده</div></div>';return}
  let h='';
  zones.forEach(z=>{
    h+='<div class="card"><div class="hdr"><span class="ttl">💧 زون '+z.id+' • GPIO'+z.pin+'</span><span class="'+(z.active?'st-on':'st-off')+'">'+(z.active?'فعال':'خاموش')+'</span></div>';
    h+='<div class="fr"><button class="bt bt-g bt-sm" onclick="act(\'zone/on\',{id:'+z.id+',dur:'+(z.duration||15)+'})">ON</button>';
    h+='<button class="bt bt-r bt-sm" onclick="act(\'zone/off\',{id:'+z.id+'})">OFF</button></div>';
    h+='<div class="fr"><input class="in" type="time" id="st'+z.id+'" value="'+pad(z.hour||0)+':'+pad(z.minute||0)+'"><input class="in" type="number" id="sd'+z.id+'" value="'+(z.duration||15)+'" min="1" max="120" style="width:45px"><span style="font-size:10px;color:var(--sub)">min</span>';
    h+='<button class="bt bt-o bt-sm" onclick="setSch('+z.id+')">Set</button></div></div>';
  });
  h+='<div class="fr2"><button class="bt bt-g" onclick="act(\'pump/on\')">⛽ پمپ ON</button><button class="bt bt-r" onclick="act(\'pump/off\')">⛽ پمپ OFF</button></div>';
  h+='<div class="fr2" style="margin-top:6px"><button class="bt bt-r" style="width:100%" onclick="act(\'e\')">🛑 E-STOP (همه قطع)</button></div>';
  el.innerHTML=h;
}

function renderLighting(d){
  const el=$('pg-lighting');
  const chs=d.channels||[];
  if(!chs.length){el.innerHTML='<div class="card"><span class="ttl">نور</span><div class="empty">کانالی تعریف نشده</div></div>';return}
  let h='<div class="fr2"><button class="bt bt-g" onclick="act(\'lighting/all-on\')">💡 All ON</button><button class="bt bt-r" onclick="act(\'lighting/all-off\')">🌑 All OFF</button></div>';
  chs.forEach(c=>{
    h+='<div class="card"><div class="hdr"><span class="ttl">💡 کانال '+c.id+'</span><span class="'+(c.state?'st-on':'st-off')+'">'+(c.state?'ON':'OFF')+'</span></div>';
    h+='<button class="bt '+(c.state?'bt-r':'bt-g')+' bt-sm" onclick="act(\'lighting/toggle\',{id:'+c.id+'})">Toggle</button></div>';
  });
  el.innerHTML=h;
}

function renderLogs(evts){
  const el=$('pg-logs');
  if(!evts.length){el.innerHTML='<div class="card"><div class="empty">هنوز رویدادی ثبت نشده</div></div>';return}
  let h='<div class="card"><div style="max-height:420px;overflow-y:auto">';
  evts.slice().reverse().forEach(e=>{
    const state=e.state||'';
    const clr=e.event_type==='pump'?'#3b82f6':e.event_type==='zone'?'#10b981':e.event_type==='schedule'?'#f59e0b':e.event_type==='light'?'#a78bfa':'#94a3b8';
    h+='<div class="log-row"><span class="log-t">'+e.event_type+'</span><span class="log-s" style="border-left:2px solid '+clr+'">'+state+'</span><span class="log-m">'+F(e.ts||e.received_at)+'</span></div>';
  });
  h+='</div></div>';
  el.innerHTML=h;
}

function renderSettings(){
  $('pg-settings').innerHTML='<div class="card"><div class="ttl">📶 همگام‌سازی با سرور</div><div class="fr"><input class="in" id="syncUrl" value="'+A+'/api/events" style="flex:1"><button class="bt bt-o bt-sm" onclick="saveSync()">ذخیره</button></div></div>'+
    '<div class="card"><div class="ttl">📡 Firmware Update</div><div class="fr"><input type="file" id="fwFile" accept=".bin" class="in" style="flex:1"><button class="bt bt-o bt-sm" onclick="uploadFW()">آپلود</button></div><div style="font-size:10px;color:var(--sub);margin-top:4px" id="fwStatus"></div></div>'+
    '<div class="card"><div class="ttl">⏰ RTC</div><div class="fr"><input class="in" id="rtcY" value="2026" style="width:50px" placeholder="Y"><input class="in" id="rtcM" value="1" style="width:38px" placeholder="M"><input class="in" id="rtcD" value="1" style="width:38px" placeholder="D"><input class="in" id="rtcH" value="12" style="width:38px" placeholder="H"><input class="in" id="rtcMi" value="0" style="width:38px" placeholder="M"><button class="bt bt-o bt-sm" onclick="setRTC()">Set</button></div></div>'+
    '<div class="card"><div class="ttl">🌐 WiFi</div><div class="fr"><input class="in" id="wSsid" placeholder="SSID" style="flex:1"><input class="in" id="wPass" type="password" placeholder="Password" style="flex:1"><button class="bt bt-o bt-sm" onclick="saveWifi()">ذخیره</button></div></div>';
}

function setSch(id){
  const t=$('st'+id).value,d=$('sd'+id).value;
  if(!t||!d){alert('زمان و مدت را تنظیم کنید');return}
  const [h,m]=t.split(':');
  act('zone/schedule',{id,hour:h,minute:m,dur:d});
}

function saveSync(){act('config/sync',{url:$('syncUrl').value})}
function setRTC(){act('config/time',{year:$('rtcY').value,month:$('rtcM').value,day:$('rtcD').value,hour:$('rtcH').value,minute:$('rtcMi').value})}
function saveWifi(){act('config/wifi',{ssid:$('wSsid').value,password:$('wPass').value})}

async function uploadFW(){
  const f=$('fwFile').files[0];
  if(!f)return;
  $('fwStatus').textContent='در حال آپلود...';
  const fd=new FormData();fd.append('firmware',f);
  try{const r=await fetch(A+'/api/firmware',{method:'POST',body:f});const j=await r.json();$('fwStatus').textContent='✓ آپلود شد: '+j.file}catch(e){$('fwStatus').textContent='خطا در آپلود'}}
}

function act(path,params){
  let url=A+'/api/'+path;
  if(params){url+='?'+Object.entries(params).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&')}
  fetch(url,{method:'POST'}).then(r=>r.json()).then(j=>{if(j.ok)refresh();else console.log('fail',j)}).catch(e=>console.log(e));
}

function drawTimeline(periods){
  const c=$('tlCanvas');
  if(!c)return;
  const ctx=c.getContext('2d');
  const w=c.width,h=c.height;
  ctx.clearRect(0,0,w,h);

  // Background grid
  ctx.strokeStyle='rgba(255,255,255,0.05)';
  for(let i=0;i<=24;i+=4){const x=i*w/24;ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,h);ctx.stroke()}
  // Hour labels
  ctx.fillStyle='#64748b';ctx.font='9px sans-serif';ctx.textAlign='center';
  for(let i=0;i<=24;i+=4){ctx.fillText(i+':00',i*w/24,h-3)}

  const now=new Date();const dayStart=new Date();dayStart.setHours(0,0,0,0);const nowX=(now-dayStart)/86400000*w;
  // Now line
  ctx.strokeStyle='rgba(239,68,68,0.6)';ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(nowX,0);ctx.lineTo(nowX,h);ctx.stroke();

  if(!periods.length){ctx.fillStyle='#64748b';ctx.font='11px sans-serif';ctx.fillText('امروز آبیاری انجام نشده',w/2,20);$('tlLegend').textContent='';return}

  const colors={pump:'#3b82f6',zone1:'#10b981',zone2:'#f59e0b',zone3:'#a78bfa',zone4:'#ec4899'};

  ctx.globalAlpha=0.85;
  periods.forEach(p=>{
    const x1=(p.start-dayStart)/86400000*w, x2=(p.end-dayStart)/86400000*w;
    const cid=p.type==='pump'?'pump':('zone'+(p.zoneId||'1'));
    ctx.fillStyle=colors[cid]||colors.zone1;
    const bh=16,by=p.type==='pump'?2:20;
    ctx.fillRect(Math.max(0,x1),by,Math.max(1,x2-x1),bh);
    if(x2-x1>30&&p.duration){
      ctx.fillStyle='#fff';ctx.font='9px sans-serif';ctx.fillText(p.duration+'m',x1+4,by+12);
    }
  });
  ctx.globalAlpha=1;

  // Legend
  const types=[...new Set(periods.map(p=>p.type==='pump'?'⛽ پمپ':'💧 زون '+(p.zoneId||'1')))];
  $('tlLegend').innerHTML=types.map(t=>'<span style="display:inline-block;margin:0 6px;font-size:10px">'+t+'</span>').join('');
}

// Tab switching
$('tabs').addEventListener('click',e=>{
  const tb=e.target.closest('.tab');
  if(!tb)return;
  page=tb.dataset.pg;
  document.querySelectorAll('.tab').forEach(t=>t.classList.toggle('on',t===tb));
  document.querySelectorAll('[id^="pg-"]').forEach(p=>p.classList.toggle('hidden',p.id!=='pg-'+page));
  if(page==='settings')renderSettings();
  refresh();
});

// Clock
function tick(){const d=new Date();G('clock',d.toLocaleTimeString('fa-IR',{hour:'2-digit',minute:'2-digit',second:'2-digit'}))}
tick();clockTimer=setInterval(tick,1000);

refresh();
setInterval(refresh,5000);
setInterval(loadTimeline,30000);
setTimeout(loadTimeline,1000);
</script></body></html>`;
}


// =========== COMBINED STATUS ===========
app.get('/api/status-full', (req, res) => {
  res.json({
    stats: getStats(),
    timeline: getTimelineData(),
    irrigation: getIrrigationData(),
    lighting: getLightingData()
  });
});

function getTimelineData() {
  const todayStart = new Date(); todayStart.setHours(0,0,0,0);
  const today = events.filter(e => e.received_at >= todayStart.getTime());
  const periods = [];
  let pumpOn = null;
  for (const e of today) {
    if (e.event_type === 'pump') {
      if (e.state === 'on' && !pumpOn) pumpOn = e.received_at;
      else if (e.state === 'off' && pumpOn) {
        periods.push({ type: 'pump', start: pumpOn, end: e.received_at });
        pumpOn = null;
      }
    }
    if (e.event_type === 'zone') {
      const m = e.state.match(/(\d+)\s*min/i);
      const dur = m ? parseInt(m[1]) : 15;
      const zid = e.state.match(/zone\s*(\d+)/i);
      periods.push({ type: 'zone', zoneId: zid ? zid[1] : '?', start: e.received_at, end: e.received_at + dur * 60000, duration: dur });
    }
  }
  if (pumpOn) periods.push({ type: 'pump', start: pumpOn, end: Date.now(), active: true });
  return periods;
}

function getIrrigationData() {
  // Build irrigation state from real events
  const zoneMap = {};
  let pumpOn = false;
  for (const e of events) {
    if (e.event_type === 'pump') {
      pumpOn = (e.state === 'on');
    }
    if (e.event_type === 'zone') {
      const zidMatch = e.state.match(/zone\s*(\d+)/i);
      const zid = zidMatch ? zidMatch[1] : '?';
      if (!zoneMap[zid]) zoneMap[zid] = { id: zid, active: false, duration: 15, hour: 0, minute: 0, pin: '?' };
      if (e.state.includes('on') || e.state.includes('start') || e.state.includes('manual')) zoneMap[zid].active = true;
      if (e.state.includes('off') || e.state.includes('completed') || e.state.includes('stop')) zoneMap[zid].active = false;
    }
  }
  const zones = Object.values(zoneMap);
  return { zones, pumpOn };
}

function getLightingData() {
  const channels = {};
  for (let i = events.length - 1; i >= 0; i--) {
    const e = events[i];
    if (e.event_type === 'light') {
      const m = e.state.match(/ch(\d+)/i);
      const ch = m ? m[1] : null;
      if (ch && !channels[ch]) channels[ch] = e.state.includes('on');
    }
  }
  const list = Object.entries(channels).map(([id, state]) => ({ id: parseInt(id), state }));
  return { channels: list };
}

// =========== START ===========
server.listen(PORT, '0.0.0.0', () => {
  console.log(`[Server] SmartGreenhouse v2 running on port ${PORT}`);
  console.log(`[Server] Dashboard: http://0.0.0.0:${PORT}`);
});

process.on('SIGINT', () => { saveEvents(); saveDevices(); saveUsers(); process.exit(); });
process.on('SIGTERM', () => { saveEvents(); saveDevices(); saveUsers(); process.exit(); });
setInterval(() => { saveEvents(); saveDevices(); }, 30000);
