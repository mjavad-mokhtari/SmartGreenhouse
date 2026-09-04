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
const COMMANDS_FILE = path.join(DATA_DIR, 'commands.json');
const SESSION_SECRET = process.env.SESSION_SECRET || crypto.randomBytes(32).toString('hex');
const MAX_EVENTS = 20000;
const COMMAND_TTL_MS = 24 * 60 * 60 * 1000;
const COMMAND_POLL_LIMIT = 50;

// --------------- Store ---------------
let events = [];
let deviceStates = {};
let users = [];
let commands = {};

function initStore() {
  [DATA_DIR, FIRMWARE_DIR].forEach(d => { if (!fs.existsSync(d)) fs.mkdirSync(d, { recursive: true }); });
  try { events = JSON.parse(fs.readFileSync(EVENTS_FILE, 'utf8') || '[]'); } catch(e) { events = []; }
  try { deviceStates = JSON.parse(fs.readFileSync(DEVICES_FILE, 'utf8') || '{}'); } catch(e) { deviceStates = {}; }
  try { users = JSON.parse(fs.readFileSync(USERS_FILE, 'utf8') || '[]'); } catch(e) { users = []; }
  try {
    commands = JSON.parse(fs.readFileSync(COMMANDS_FILE, 'utf8') || '{}');
    if (typeof commands !== 'object' || commands === null) commands = {};
  } catch(e) { commands = {}; }
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
function saveCommands() {
  try { fs.writeFileSync(COMMANDS_FILE, JSON.stringify(commands), 'utf8'); } catch(e) {}
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
  const irrigationStarts = today.filter(e => e.event_type === 'zone' && String(e.state || '').startsWith('start'));
  const irrigationMinutes = irrigationStarts.reduce((total, event) => {
    const match = String(event.state || '').match(/(\d+)\s*min/i);
    return total + (match ? parseInt(match[1], 10) : 15);
  }, 0);
  return {
    devices: Object.keys(deviceStates).length,
    todayEvents: today.length,
    totalEvents: events.length,
    irrigationMinutes,
    lastIrrigation: today.filter(e => e.event_type === 'pump' && e.state === 'on').slice(-1)[0] || null
  };
}

function getPrimaryDeviceId() {
  const entries = Object.entries(deviceStates);
  if (!entries.length) return null;
  const online = entries.find(([, info]) => (Date.now() - (info.lastSeen || 0)) < 120000);
  if (online) return online[0];
  return entries.sort((a, b) => (b[1].lastSeen || 0) - (a[1].lastSeen || 0))[0][0];
}

function getPrimaryDeviceState() {
  const id = getPrimaryDeviceId();
  if (!id) return null;
  return deviceStates[id] || null;
}

function ensureNumber(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
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

function checkPassword(pw, hash) {
  const [salt, key] = hash.split(':');
  return key === crypto.pbkdf2Sync(pw, salt, 10000, 64, 'sha512').toString('hex');
}

// --------------- Express ---------------
const app = express();
app.use(express.json({ limit: '128kb' }));
app.use(express.urlencoded({ extended: true }));

const sessionMiddleware = session({
  secret: SESSION_SECRET,
  resave: false,
  saveUninitialized: false,
  cookie: { maxAge: 24 * 60 * 60 * 1000, sameSite: 'lax' }
});
app.use(sessionMiddleware);

const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

wss.on('connection', (ws) => {
  ws.send(JSON.stringify({ type: 'connected' }));
  ws.on('message', (msg) => { try { console.log('[WS]', msg.toString()); } catch(e) {} });
});

app.use((req, res, next) => {
  if (req.headers['x-forwarded-proto'] === 'https') {
    req.session.cookie.secure = true;
  }
  next();
});

function requireAuth(req, res, next) {
  if (req.session && req.session.user) return next();
  if (req.path === '/login' || req.path === '/setup' || req.path === '/api/events' || req.path === '/api/commands' || req.path === '/api/commands/ack' || req.path === '/api/health' || req.path === '/api/devices' || req.path === '/api/stats') return next();
  if (req.path === '/api/status-full' || req.path === '/api/irrigation/timeline') return next();
  if (req.path.startsWith('/api/')) return res.status(401).json({ error: 'unauthorized' });
  return res.redirect('/login');
}

function requireApiKey(req, res, next) {
  const apiKey = process.env.API_KEY;
  if (!apiKey) return next();
  const provided = req.headers['x-api-key'] || req.headers['x-apikey'] || '';
  if (provided !== apiKey) return res.status(401).json({ error: 'invalid api key' });
  next();
}

function requireControl(req, res, next) {
  if (req.session && req.session.user) return next();
  return requireApiKey(req, res, next);
}

function nowTs() { return Date.now(); }

function pickDeviceId(requestedId = '') {
  if (requestedId && deviceStates[requestedId]) return requestedId;
  const firstOnline = Object.entries(deviceStates).find(([, info]) => (Date.now() - (info.lastSeen || 0)) < 120000);
  if (firstOnline) return firstOnline[0];
  return requestedId || null;
}

function pruneExpiredCommands() {
  const now = nowTs();
  Object.keys(commands).forEach((deviceId) => {
    const list = commands[deviceId] || [];
    const alive = list.filter((c) => !c.createdAt || (now - c.createdAt) < COMMAND_TTL_MS);
    if (!alive.length) delete commands[deviceId];
    else commands[deviceId] = alive;
  });
}

function newCommandId() {
  return `${Date.now().toString(36)}-${Math.floor(Math.random() * 1e6)}`;
}

function pushCommand(deviceId, action, params = {}) {
  const id = newCommandId();
  if (!commands[deviceId]) commands[deviceId] = [];
  commands[deviceId].push({ id, action, params, createdAt: nowTs(), source: 'dashboard' });
  if (commands[deviceId].length > COMMAND_POLL_LIMIT) {
    commands[deviceId] = commands[deviceId].slice(-COMMAND_POLL_LIMIT);
  }
  saveCommands();
  return { id, action, params, createdAt: nowTs() };
}

function ackCommands(deviceId, ids = []) {
  if (!commands[deviceId]) return 0;
  const before = commands[deviceId].length;
  const remove = new Set(ids.map((x) => String(x)));
  commands[deviceId] = commands[deviceId].filter((c) => !remove.has(String(c.id)));
  const removed = before - commands[deviceId].length;
  if (removed > 0) saveCommands();
  return removed;
}

initStore();

// =========== ROUTES ===========

// Health
app.get('/api/health', (req, res) => {
  res.json({ status: 'ok', uptime: process.uptime(), devices: Object.keys(deviceStates).length, events: events.length, nodejs: process.version });
});

// Login page
app.get('/login', (req, res) => {
  if (users.length === 0) return res.redirect('/setup');
  res.type('html').send(loginPage());
});

app.post('/login', (req, res) => {
  const { username, password, totp } = req.body;
  const user = users.find(u => u.username === username);
  if (!user || !checkPassword(password, user.passwordHash)) {
    return res.type('html').send(loginPage('نام کاربری یا رمز عبور اشتباه است'));
  }
  if (user.totpEnabled && user.totpSecret) {
    if (!totp || totp.length < 4) return res.type('html').send(loginPage('کد ۲عاملی را وارد کنید'));
    try {
      const valid = otplib.authenticator.check(totp, user.totpSecret);
      if (!valid) return res.type('html').send(loginPage('کد ۲عاملی نامعتبر است'));
    } catch(e) { return res.type('html').send(loginPage('خطا در بررسی کد')); }
  }
  req.session.user = { username: user.username };
  req.session.save(() => res.redirect('/'));
});

// Setup (first run)
app.get('/setup', (req, res) => {
  if (users.length > 0) return res.redirect('/login');
  res.type('html').send(setupPage());
});

app.post('/setup', async (req, res) => {
  if (users.length > 0) return res.redirect('/login');
  const username = (req.body.username || 'admin').trim();
  const password = req.body.password || '';
  if (!password || password.length < 4) return res.type('html').send(setupPage('رمز عبور حداقل ۴ کاراکتر باشد'));
  let totpSecret = '';
  let qrDataUrl = '';
  if (otplib) {
    totpSecret = otplib.authenticator.generateSecret();
    const otpauth = otplib.authenticator.keyuri(username, 'خانه سبز هوشمند', totpSecret);
    try {
      qrDataUrl = await QRCode.toDataURL(otpauth, { width: 250 });
    } catch(err) { console.log('[Setup] QR generation failed:', err.message); }
  }
  users.push({ username, passwordHash: hashPassword(password), totpEnabled: !!totpSecret, totpSecret });
  saveUsers();
  if (qrDataUrl) return res.type('html').send(setupDonePage(qrDataUrl, totpSecret));
  req.session.user = { username };
  req.session.save(() => res.redirect('/'));
});

// Emergency QR recovery (uses first user)
app.get('/emergency-qr', async (req, res) => {
  if (!users.length) return res.redirect('/setup');
  const user = users[0];
  if (!user.totpSecret || !user.totpEnabled) return res.type('html').send(recoverPage('2FA برای این کاربر فعال نیست'));
  try {
    const otpauth = otplib.authenticator.keyuri(user.username, 'خانه سبز هوشمند', user.totpSecret);
    const qrDataUrl = await QRCode.toDataURL(otpauth, { width: 250 });
    res.type('html').send(setupDonePage(qrDataUrl, user.totpSecret));
  } catch(e) { res.type('html').send(recoverPage('خطا در تولید QR')); }
});

app.get('/recover-2fa', (req, res) => {
  if (!users.length) return res.redirect('/setup');
  const user = users[0];
  if (!user.totpSecret || !user.totpEnabled) return res.type('html').send(recoverPage('2FA برای این کاربر فعال نیست'));
  try {
    const otpauth = otplib.authenticator.keyuri(user.username, 'خانه سبز هوشمند', user.totpSecret);
    QRCode.toDataURL(otpauth, { width: 250 }).then(qrDataUrl => {
      res.type('html').send(setupDonePage(qrDataUrl, user.totpSecret));
    }).catch(err => {
      res.type('html').send(recoverPage('خطا در تولید QR: ' + err.message));
    });
  } catch(e) { res.type('html').send(recoverPage('خطا: ' + e.message)); }
});

// Emergency password reset — bypass 2FA, set new password
app.get('/emergency-reset', (req, res) => {
  res.type('html').send(emergencyResetPage(''));
});

app.post('/emergency-reset', (req, res) => {
  if (users.length === 0) return res.redirect('/setup');
  const token = (req.body.token || '').trim();
  if (token !== 'smarthome-reset') return res.type('html').send(emergencyResetPage('توکن اشتباه است'));
  const newPass = (req.body.password || '').trim();
  if (newPass.length < 4) return res.type('html').send(emergencyResetPage('رمز عبور حداقل ۴ کاراکتر باشد'));
  users[0].passwordHash = hashPassword(newPass);
  users[0].totpEnabled = false;
  users[0].totpSecret = '';
  saveUsers();
  res.type('html').send(emergencyResetPage('✓ رمز عبور بازنشانی شد. ۲FA غیرفعال شد. به صفحه لاگین بروید.', true));
});

// Logout
app.get('/logout', (req, res) => {
  req.session.destroy(() => res.redirect('/login'));
});

// Receive events from ESP32
app.post('/api/events', (req, res) => {
  if (!req.body) return res.status(400).json({ error: 'invalid json' });
  const { deviceId, uptime, events: evts, status: devStatus } = req.body;
  if (!deviceId) return res.status(400).json({ error: 'deviceId required' });
  deviceStates[deviceId] = {
    lastSeen: Date.now(),
    uptime: uptime || 0,
    ip: req.ip,
    status: devStatus && typeof devStatus === 'object' ? devStatus : (devStatus || {})
  };
  saveDevices();
  let received = 0;
  if (evts && evts.length > 0) { for (const e of evts) { addEvent(deviceId, e.type, e.state, e.ts); received++; } }
  if (received > 0) saveEvents();
  res.json({ ok: true, received });
});

// Get events
app.get('/api/events', (req, res) => {
  const deviceId = req.query.device, limit = parseInt(req.query.limit) || 100, since = parseInt(req.query.since) || 0;
  let result = events;
  if (deviceId) result = result.filter(e => e.device_id === deviceId);
  if (since) result = result.filter(e => (e.received_at || e.ts) > since);
  result = result.sort((a,b) => (b.received_at||b.ts) - (a.received_at||a.ts)).slice(0, limit);
  res.json(result);
});

// Devices
app.get('/api/devices', (req, res) => {
  const list = Object.entries(deviceStates).map(([id, info]) => ({
    id, lastSeen: info.lastSeen, uptime: info.uptime, ip: info.ip,
    online: (Date.now() - info.lastSeen) < 120000
  }));
  res.json(list);
});

// Stats
app.get('/api/stats', (req, res) => { res.json(getStats()); });

// Control endpoints (UI or board bridge)
app.post('/api/zone/on', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const zoneId = parseInt(req.body.id || req.query.id || req.body.zoneId || req.query.zoneId, 10);
  const duration = parseInt(req.body.dur || req.body.duration || req.query.duration || 15, 10);
  if (!Number.isFinite(zoneId) || zoneId <= 0) return res.status(400).json({ error: 'invalid zone id' });
  const command = pushCommand(deviceId, 'zone/on', { id: zoneId, dur: duration });
  res.json({ ok: true, command });
});

app.post('/api/zone/off', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const zoneId = parseInt(req.body.id || req.query.id || req.body.zoneId || req.query.zoneId, 10);
  if (!Number.isFinite(zoneId) || zoneId <= 0) return res.status(400).json({ error: 'invalid zone id' });
  const command = pushCommand(deviceId, 'zone/off', { id: zoneId });
  res.json({ ok: true, command });
});

app.post('/api/zone/schedule', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const zoneId = parseInt(req.body.id || req.query.id || req.body.zoneId || req.query.zoneId, 10);
  const hour = parseInt(req.body.hour || req.query.hour, 10);
  const minute = parseInt(req.body.minute || req.query.minute, 10);
  const dur = parseInt(req.body.dur || req.body.duration || req.query.dur || req.query.duration || req.query.min, 10);
  if (!Number.isFinite(zoneId) || zoneId <= 0) return res.status(400).json({ error: 'invalid zone id' });
  if (!Number.isFinite(hour) || !Number.isFinite(minute) || hour < 0 || hour > 23 || minute < 0 || minute > 59)
    return res.status(400).json({ error: 'invalid schedule time' });
  if (!Number.isFinite(dur) || dur < 1 || dur > 480) return res.status(400).json({ error: 'invalid duration' });
  const command = pushCommand(deviceId, 'zone/schedule', { id: zoneId, hour, minute, dur });
  res.json({ ok: true, command });
});

app.post('/api/pump/on', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const command = pushCommand(deviceId, 'pump/on', {});
  res.json({ ok: true, command });
});

app.post('/api/pump/off', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const command = pushCommand(deviceId, 'pump/off', {});
  res.json({ ok: true, command });
});

app.post('/api/e', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const command = pushCommand(deviceId, 'e', {});
  res.json({ ok: true, command });
});

app.post('/api/lighting/toggle', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const id = parseInt(req.body.id || req.query.id, 10);
  if (!Number.isFinite(id) || id < 0) return res.status(400).json({ error: 'invalid channel id' });
  const command = pushCommand(deviceId, 'lighting/toggle', { id });
  res.json({ ok: true, command });
});

app.post('/api/lighting/all-on', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const command = pushCommand(deviceId, 'lighting/all-on', {});
  res.json({ ok: true, command });
});

app.post('/api/lighting/all-off', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const command = pushCommand(deviceId, 'lighting/all-off', {});
  res.json({ ok: true, command });
});

app.post('/api/config/sync', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const url = String(req.body.url || req.query.url || '').trim();
  if (!url) return res.status(400).json({ error: 'url required' });
  const command = pushCommand(deviceId, 'config/sync', { url });
  res.json({ ok: true, command });
});

app.post('/api/config/time', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const p = req.body || {};
  const year = parseInt(p.year || req.query.year, 10);
  const month = parseInt(p.month || req.query.month, 10);
  const day = parseInt(p.day || req.query.day, 10);
  const hour = parseInt(p.hour || req.query.hour, 10);
  const minute = parseInt((p.minute || p.mi || req.query.minute || req.query.mi), 10);
  if (![year, month, day, hour, minute].every((v) => Number.isFinite(v))) {
    return res.status(400).json({ error: 'invalid time fields' });
  }
  const command = pushCommand(deviceId, 'config/time', { year, month, day, hour, minute });
  res.json({ ok: true, command });
});

app.post('/api/config/wifi', requireControl, (req, res) => {
  const deviceId = pickDeviceId(req.body.deviceId || req.query.deviceId);
  if (!deviceId) return res.status(400).json({ error: 'device not found' });
  const ssid = (req.body.ssid || req.query.ssid || '').trim();
  const password = String(req.body.password || req.query.password || '');
  if (!ssid) return res.status(400).json({ error: 'ssid required' });
  const command = pushCommand(deviceId, 'config/wifi', { ssid, password });
  res.json({ ok: true, command });
});

app.get('/api/commands', requireApiKey, (req, res) => {
  pruneExpiredCommands();
  const deviceId = String(req.query.device || req.headers['x-device-id'] || 'esp32-irrigation');
  const list = commands[deviceId] || [];
  const limit = Math.min(Math.max(1, parseInt(req.query.limit || COMMAND_POLL_LIMIT, 10)), COMMAND_POLL_LIMIT);
  const payload = list.slice(0, Math.max(1, limit));
  res.json({ ok: true, deviceId, count: payload.length, commands: payload, pending: list.length });
});

// Device ACK consumed commands
app.post('/api/commands/ack', requireApiKey, (req, res) => {
  const deviceId = req.body.deviceId || req.headers['x-device-id'] || 'esp32-irrigation';
  const ids = Array.isArray(req.body.ids) ? req.body.ids : [];
  const removed = ackCommands(deviceId, ids);
  pruneExpiredCommands();
  res.json({ ok: true, removed, remaining: (commands[deviceId] || []).length });
});

// Irrigation timeline data
app.get('/api/irrigation/timeline', (req, res) => {
  const todayStart = new Date(); todayStart.setHours(0,0,0,0);
  const today = events.filter(e => e.received_at >= todayStart.getTime());
  res.json({ periods: getTimelineData() });
});

// === FULL STATUS for new dashboard ===
app.get('/api/status-full', (req, res) => {
  const deviceInfo = {};
  for (const [id, s] of Object.entries(deviceStates)) {
    deviceInfo[id] = Object.assign({}, s, { wifi: s.status ? (s.status.wifi || {}) : {}, rtc: s.status ? (s.status.rtc || {}) : {}, health: s.status ? (s.status.health || {}) : {}, irrigation: s.status ? getIrrigationDataFor(s.status.irrigation) : null, lighting: s.status ? getLightingDataFor(s.status.lighting) : null });
  }
  res.json({
    devices: Object.keys(deviceStates),
    deviceInfo,
    allEvents: events,
    timeline: getTimelineData(),
    stats: getStats()
  });
});

function getIrrigationDataFor(irrigation) {
  if (!irrigation) return null;
  const result = Object.assign({}, irrigation);
  result.zoneCount = ensureNumber(result.zoneCount, 0);
  result.pumpOn = !!result.pumpOn;
  result.pumpManualOverride = !!result.pumpManualOverride;
  result.pumpGpio = ensureNumber(result.pumpGpio, -1);
  result.soilRaw = ensureNumber(result.soilRaw, 0);
  result.soilPercent = ensureNumber(result.soilPercent, 0);
  if (Array.isArray(result.zones)) {
    result.zones = result.zones.map((z) => ({
      id: ensureNumber(z.id, 0),
      pin: ensureNumber(z.pin, 0),
      enabled: !!z.enabled,
      running: !!z.running,
      active: !!z.running,
      hour: ensureNumber(z.hour, 0),
      minute: ensureNumber(z.minute, 0),
      duration: ensureNumber(z.duration, 15),
      ranToday: !!z.ranToday,
      nextRun: z.nextRun || '--',
      remainingSec: ensureNumber(z.remainingSec, 0)
    }));
  }
  return result;
}

function getLightingDataFor(lighting) {
  if (!lighting) return null;
  const result = Object.assign({}, lighting);
  if (Array.isArray(result.state)) {
    result.channels = result.state.map((c) => ({
      id: ensureNumber(c.id, 0),
      pin: ensureNumber(c.pin, 0),
      enabled: !!c.enabled,
      state: !!c.state,
      scheduleEnabled: !!c.scheduleEnabled,
      onTime: c.onTime || '00:00',
      offTime: c.offTime || '00:00'
    }));
    delete result.state;
  } else if (!result.channels) {
    result.channels = [];
  }
  return result;
}

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

// Dashboard
app.get('/', requireAuth, (req, res) => {
  res.type('html').send(dashboardPage());
});

// Firmware upload
const multer = require('multer');
const upload = multer({ dest: FIRMWARE_DIR });
app.post('/api/firmware', upload.single('firmware'), (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'no file' });
  const ext = path.extname(req.file.originalname) || '.bin';
  const dest = path.join(FIRMWARE_DIR, `firmware-${Date.now()}${ext}`);
  fs.renameSync(req.file.path, dest);
  res.json({ ok: true, file: path.basename(dest) });
});

// ============ PAGES ============

function loginPage(err = '') {
  const msg = err ? `<div style="color:#ef4444;margin-bottom:12px;font-size:.85rem">${err}</div>` : '';
  return `<!doctype html><html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ورود</title><style>body{background:#0b0e11;color:#e4e6ea;font:14px system-ui;display:flex;justify-content:center;align-items:center;min-height:100dvh;margin:0}.box{background:#15191e;border:1px solid #1f2937;border-radius:12px;padding:24px;width:100%;max-width:340px}.box h2{text-align:center;margin:0 0 16px;color:#10b981}input{width:100%;background:#0b0e11;border:1px solid #1f2937;border-radius:8px;color:#e4e6ea;padding:10px;margin-bottom:10px;font-size:.85rem;box-sizing:border-box}button{width:100%;background:#10b981;border:none;border-radius:8px;color:#000;padding:10px;font-size:.9rem;font-weight:600;cursor:pointer}</style></head><body><div class="box"><h2>🏠 خانه سبز هوشمند</h2>${msg}<form method="post"><input name="username" placeholder="نام کاربری" required><input name="password" type="password" placeholder="رمز عبور" required><input name="totp" placeholder="کد ۲عاملی (در صورت فعال بودن)" style="letter-spacing:4px;text-align:center"><button>ورود</button></form><div style="text-align:center;margin-top:12px"><a href="/recover-2fa" style="color:#6b7280;font-size:.75rem">دریافت مجدد QR کد</a> | <a href="/emergency-reset" style="color:#ef4444;font-size:.75rem">بازنشانی اضطراری</a></div></div></body></html>`;
}

function setupPage(err = '') {
  const msg = err ? `<div style="color:#ef4444;margin-bottom:12px;font-size:.85rem">${err}</div>` : '';
  return `<!doctype html><html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>راه‌اندازی</title><style>body{background:#0b0e11;color:#e4e6ea;font:14px system-ui;display:flex;justify-content:center;align-items:center;min-height:100dvh;margin:0}.box{background:#15191e;border:1px solid #1f2937;border-radius:12px;padding:24px;width:100%;max-width:360px}.box h2{text-align:center;margin:0 0 8px;color:#10b981}.box p{font-size:.8rem;color:#6b7280;text-align:center;margin:0 0 16px}input{width:100%;background:#0b0e11;border:1px solid #1f2937;border-radius:8px;color:#e4e6ea;padding:10px;margin-bottom:10px;font-size:.85rem;box-sizing:border-box}button{width:100%;background:#10b981;border:none;border-radius:8px;color:#000;padding:10px;font-size:.9rem;font-weight:600;cursor:pointer}</style></head><body><div class="box"><h2>🏠 راه‌اندازی اولیه</h2><p>یک ادمین بسازید. کد QR برای Google Authenticator نمایش داده می‌شود.</p>${msg}<form method="post"><input name="username" placeholder="نام کاربری (پیش‌فرض: admin)" value="admin"><input name="password" type="password" placeholder="رمز عبور" required minlength="4"><button>ایجاد و ادامه</button></form></div></body></html>`;
}

function setupDonePage(qrDataUrl, secret) {
  return `<!doctype html><html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>راه‌اندازی کامل شد</title><style>body{background:#0b0e11;color:#e4e6ea;font:14px system-ui;display:flex;justify-content:center;align-items:center;min-height:100dvh;margin:0}.box{background:#15191e;border:1px solid #1f2937;border-radius:12px;padding:20px;width:100%;max-width:360px;text-align:center}.box h2{color:#10b981;margin:0 0 4px}.box p{font-size:.8rem;color:#6b7280;margin:4px 0 12px}img{border-radius:10px;background:#fff;padding:8px;max-width:220px}.secret{background:#0b0e11;border:1px solid #1f2937;border-radius:8px;padding:8px;font-family:monospace;font-size:.8rem;color:#f59e0b;margin:10px 0;word-break:break-all}a{display:block;background:#10b981;color:#000;text-decoration:none;padding:10px;border-radius:8px;font-weight:600;margin-top:12px}</style></head><body><div class="box"><h2>✅ راه‌اندازی کامل شد</h2><p>این QR را با Google Authenticator اسکن کنید:</p><img src="${qrDataUrl}" alt="QR Code"><div class="secret">${secret}</div><p style="font-size:.7rem;color:#6b7280">کد مخفی را یادداشت کنید یا اسکرین‌شات بگیرید.</p><a href="/login">ورود به داشبورد</a></div></body></html>`;
}

function recoverPage(msg = '') {
  return `<!doctype html><html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>بازیابی</title><style>body{background:#0b0e11;color:#e4e6ea;font:14px system-ui;display:flex;justify-content:center;align-items:center;min-height:100dvh;margin:0}.box{background:#15191e;border:1px solid #1f2937;border-radius:12px;padding:20px;width:100%;max-width:360px;text-align:center}.box h2{color:#ef4444;margin:0 0 8px}.box p{font-size:.8rem;color:#6b7280;margin:4px 0 12px}a{display:block;background:#1f2937;color:#e4e6ea;text-decoration:none;padding:10px;border-radius:8px;font-weight:600;margin-top:12px}</style></head><body><div class="box"><h2>⚠️ ${msg || 'بازیابی ۲FA'}</h2><p>اگر دسترسی به اپ احراز هویت ندارید، از لینک زیر استفاده کنید:</p><a href="/emergency-reset">🔄 بازنشانی اضطراری رمز عبور</a></div></body></html>`;
}

function emergencyResetPage(msg, done = false) {
  const clr = done ? '#10b981' : '#ef4444';
  return `<!doctype html><html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>بازنشانی</title><style>body{background:#0b0e11;color:#e4e6ea;font:14px system-ui;display:flex;justify-content:center;align-items:center;min-height:100dvh;margin:0}.box{background:#15191e;border:1px solid #1f2937;border-radius:12px;padding:20px;width:100%;max-width:360px;text-align:center}.box h2{color:${clr};margin:0 0 8px}.box p{font-size:.8rem;color:#6b7280;margin:4px 0 12px}input{width:100%;background:#0b0e11;border:1px solid #1f2937;border-radius:8px;color:#e4e6ea;padding:10px;margin-bottom:10px;font-size:.85rem;box-sizing:border-box}button{width:100%;background:#ef4444;border:none;border-radius:8px;color:#fff;padding:10px;font-size:.9rem;font-weight:600;cursor:pointer}a{display:block;background:#1f2937;color:#e4e6ea;text-decoration:none;padding:10px;border-radius:8px;font-weight:600;margin-top:12px}</style></head><body><div class="box"><h2>🔄 بازنشانی اضطراری</h2><p>${msg || 'توکن: smarthome-reset'}</p>${done?'<a href="/login">رفتن به صفحه ورود</a>':'<form method="post"><input name="token" placeholder="توکن اضطراری"><input name="password" type="password" placeholder="رمز عبور جدید"><button>بازنشانی</button></form>'}</div></body></html>`;
}

function dashboardPage() {
  return `<!doctype html><html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no"><title>خانه سبز هوشمند</title>
<style>:root{--bg:#0b0e11;--card:#15191e;--accent:#10b981;--blue:#3b82f6;--danger:#ef4444;--warn:#f59e0b;--fg:#e4e6ea;--sub:#9ca3af;--muted:#6b7280;--border:#1f2937;--r:10px}
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--fg);font:14px/1.6 system-ui,-apple-system,sans-serif;min-height:100dvh;-webkit-tap-highlight-color:transparent}
main{max-width:680px;margin:0 auto;padding:12px}
.top{display:flex;justify-content:space-between;align-items:center;padding:4px 0 10px;border-bottom:1px solid var(--border);margin-bottom:8px}
.top h1{font:bold 1.2rem system-ui;display:flex;align-items:center;gap:6px}
.top .ri{display:flex;align-items:center;gap:10px}
.top .clk{font-size:.78rem;color:var(--sub)}
.top .dsel{background:var(--card);border:1px solid var(--border);color:var(--fg);border-radius:6px;padding:4px 8px;font-size:.72rem;outline:none}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.dot.g{background:var(--accent);box-shadow:0 0 6px var(--accent)}.dot.r{background:var(--danger)}
.insights{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:7px;margin:10px 0}
.in{border:1px solid var(--border);border-radius:var(--r);padding:10px 8px;text-align:center;background:var(--card)}
.in .nv{font-size:1.25rem;font-weight:700;line-height:1.2}
.in .nv.g{color:var(--accent)}.in .nv.r{color:var(--danger)}.in .nv.y{color:var(--warn)}.in .nv.b{color:var(--blue)}
.in .lb{font-size:.65rem;color:var(--muted);margin-top:2px;text-transform:uppercase;letter-spacing:.2px}
.tabs{display:flex;gap:3px;overflow-x:auto;scrollbar-width:none;margin:8px 0}
.tabs::-webkit-scrollbar{display:none}
.tab{flex-shrink:0;padding:7px 15px;border:1px solid var(--border);border-radius:20px;font-size:.78rem;cursor:pointer;background:var(--bg);color:var(--sub);transition:all .15s}
.tab.on{background:var(--accent);color:#000;border-color:var(--accent);font-weight:600}
.card{background:var(--card);border:1px solid var(--border);border-radius:var(--r);padding:12px;margin-bottom:8px}
.card .ch{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.card .ct{font-size:.8rem;color:var(--sub)}
.stg{font-size:.72rem;font-weight:600;border-radius:10px;padding:2px 10px}
.stg.on{background:#065f46;color:var(--accent)}.stg.off{background:#1f2937;color:var(--muted)}.stg.run{background:#7c2d12;color:var(--warn)}
.fr{display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin-top:8px}
.bt{display:inline-flex;align-items:center;justify-content:center;gap:4px;border:none;padding:8px 16px;border-radius:8px;font-size:.8rem;font-weight:600;cursor:pointer;color:#fff;transition:opacity .12s}
.bt:active{opacity:.7}
.bt.g{background:#065f46}.bt.r{background:#991b1b}.bt.b{background:#1e3a5f}.bt.y{background:#92400e}
.bt.sm{padding:5px 10px;font-size:.72rem}
input[type=time],input[type=number],input[type=text],input[type=password],input[type=file],select{background:var(--bg);border:1px solid var(--border);border-radius:6px;color:var(--fg);padding:7px 9px;font-size:.78rem;outline:none}
input:focus,select:focus{border-color:var(--accent)}
input[type=number]{width:48px}input[type=time]{width:92px}
.logs{max-height:360px;overflow-y:auto;font-size:.7rem;font-family:'SF Mono',SFMono-Regular,Consolas,monospace;line-height:1.7;background:#06080a;border-radius:8px;padding:10px}
.logs .le{padding:3px 0;border-bottom:1px solid rgba(255,255,255,.02);display:flex;justify-content:space-between}
.logs .le .lt{color:var(--accent);font-weight:600}.logs .le .ls{color:var(--sub);margin:0 8px}.logs .le .tm{color:var(--muted)}
.setc{background:var(--card);border:1px solid var(--border);border-radius:var(--r);padding:12px;margin-bottom:8px}
.setc h3{font-size:.82rem;color:var(--sub);margin-bottom:8px;font-weight:500}
.estop{text-align:center;margin:16px 0}
.estop .bt{font-size:.95rem;padding:12px 40px;border-radius:24px;background:var(--danger);animation:pls 2s infinite}
@keyframes pls{0%,100%{opacity:1}50%{opacity:.55}}
.cnv-wrap{background:var(--card);border:1px solid var(--border);border-radius:var(--r);padding:10px;margin-bottom:8px}
.cnv-wrap .cnv-t{font-size:.75rem;color:var(--sub);margin-bottom:6px;font-weight:500}
canvas.cnv{width:100%;height:70px;display:block;border-radius:6px}
.hidden{display:none!important}
.bar{display:flex;justify-content:space-between;align-items:center;padding:4px 0;margin-top:8px;border-top:1px solid var(--border)}
.bar span{font-size:.7rem;color:var(--muted)}
.bar a{color:var(--danger);text-decoration:none;font-size:.7rem}
@media(min-width:768px){.insights{grid-template-columns:repeat(4,1fr)}main{max-width:860px}}</style></head>
<body><main>
<div class="top"><h1>🏠 خانه سبز هوشمند</h1>
<div class="ri"><select class="dsel" id="devSel"></select><span class="clk" id="clock">--:--:--</span><span class="dot g" id="dot"></span></div></div>
<div class="insights" id="insights"></div>
<nav class="tabs" id="tabs">
<button class="tab" data-pg="overview">نمای کلی</button>
<button class="tab on" data-pg="irrigation">آبیاری</button>
<button class="tab" data-pg="lighting">نور</button>
<button class="tab" data-pg="logs">لاگ‌ها</button>
<button class="tab" data-pg="settings">تنظیمات</button></nav>
<div id="pg-overview" class="hidden"></div>
<div id="pg-irrigation"></div>
<div id="pg-lighting" class="hidden"></div>
<div id="pg-logs" class="hidden"></div>
<div id="pg-settings" class="hidden"></div>
<div class="estop"><button class="bt" onclick="act('e')">⚠ توقف اضطراری</button></div>
<div class="bar"><span>نسخه ۲.۱</span><a href="/logout">خروج</a></div>
</main>
<script>
var page='irrigation',st=null,did='',dids=[];
function $(id){return document.getElementById(id)}
function pad(n){return n<10?'0'+n:''+n}

function setDid(id){did=id;updateDot();refresh()}

async function refresh(){
  try{var r=await fetch('/api/status-full');st=await r.json();if(!did&&st.devices&&st.devices.length)did=st.devices[0];renderAll()}catch(e){}
}
function renderAll(){
  if(!st)return;
  var saved={};
  (function(){
    var ae=document.activeElement;
    if(ae&&(ae.tagName==='INPUT'||ae.tagName==='TEXTAREA'||ae.tagName==='SELECT')){
      if(ae.id)saved[ae.id]={v:ae.value,selStart:ae.selectionStart,selEnd:ae.selectionEnd}
    }
  })();
  renderDevSel();renderInsights();renderIrrigation();renderLighting();
  if(page==='logs')renderLogs();
  if(page==='overview')renderOverview();
  drawCharts()

  // restore saved input values after refresh
  (function(){
    for(var k in saved){
      var e=$(k);
      if(e){e.value=saved[k].v;try{e.setSelectionRange(saved[k].selStart||0,saved[k].selEnd||0)}catch(_){}}
    }
  })();
}
function renderDevSel(){
  dids=st.devices||[];var sel=$('devSel'),cur=sel.value;
  sel.innerHTML=dids.map(function(d){return'<option value="'+d+'">'+d+'</option>'}).join('');
  if(dids.indexOf(did)<0&&dids.length)did=dids[0];
  if(dids.indexOf(did)>=0)sel.value=did;
  updateDot();
  sel.onchange=function(){setDid(sel.value)}
}
function updateDot(){
  var dot=$('dot'),d=st&&st.deviceInfo?st.deviceInfo[did]:null;
  var online=d&&Date.now()-(d.lastSeen||0)<120000;
  dot.className='dot '+(online?'g':'r');
}
function renderInsights(){
  var d=st&&st.deviceInfo?st.deviceInfo[did]:null,w={connected:false},irr={},lt={},rt={valid:false},hl={freeHeapKB:0,uptimeMin:0};
  if(d){w=d.wifi||w;irr=d.irrigation||irr;lt=d.lighting||lt;rt=d.rtc||rt;hl=d.health||hl}
  var actZ=0;if(irr.zones)irr.zones.forEach(function(z){if(z.running)actZ++});
  var ltOn=0;var chs=lt.channels||lt.state||[];chs.forEach(function(c){if(c.state)ltOn++});
  var soil=irr.soilPercent!=null?irr.soilPercent:'--';
  var items=[
    {v:w.connected?'متصل':'قطع',l:'Wi-Fi',c:w.connected?'g':'r'},
    {v:irr.pumpOn?'روشن':'خاموش',l:'پمپ',c:irr.pumpOn?'g':'r'},
    {v:actZ,l:'زون فعال',c:actZ?'y':''},
    {v:soil+'%',l:'رطوبت خاک',c:soil!='--'&&soil<30?'r':'g'},
    {v:ltOn,l:'نور روشن',c:''},
    {v:hl.freeHeapKB+' KB',l:'رم آزاد',c:''},
    {v:rt.valid?'معتبر':'نامعتبر',l:'RTC',c:rt.valid?'g':'r'},
    {v:hl.uptimeMin+' دقیقه',l:'آپتایم',c:''}
  ];
  var h='';items.forEach(function(it){h+='<div class="in"><div class="nv '+it.c+'">'+it.v+'</div><div class="lb">'+it.l+'</div></div>'});
  $('insights').innerHTML=h
}
function renderOverview(){
  var s=st.stats||{},d=st.devices||[],ol=d.map(function(id){var di=st.deviceInfo[id];return di?'✓ '+id+' (آنلاین)':'✗ '+id}).join('<br>');
  var h='<div class="card"><span class="ct">📊 وضعیت سیستم</span><div style="font-size:.8rem;color:var(--sub);line-height:1.8">';
  h+='دستگاه‌ها: '+s.devices+'<br>رویداد امروز: '+s.todayEvents+'<br>کل رویدادها: '+s.totalEvents+'<br>دقیقه آبیاری: '+(s.irrigationMinutes||0)+'</div></div>';
  h+='<div class="card"><span class="ct">🖥 دستگاه‌ها</span><div style="font-size:.78rem;color:var(--sub);line-height:1.8">'+ol+'</div></div>';
  $('pg-overview').innerHTML=h
}
function renderIrrigation(){
  var d=st&&st.deviceInfo?st.deviceInfo[did]:null,irr=d&&d.irrigation?d.irrigation:null;
  if(!irr||!irr.zones){$('pg-irrigation').innerHTML='<div class="card"><div class="ct">ماژول آبیاری فعال نیست</div></div>';return}
  var h='';
  h+='<div class="card"><div class="ch"><span class="ct">⛽ پمپ • GPIO '+irr.pumpGpio+'</span><span class="stg '+(irr.pumpOn?'run':'off')+'">'+(irr.pumpOn?'فعال':'خاموش')+'</span></div><div class="fr"><button class="bt g" onclick="act(\'pump/on\')">روشن</button><button class="bt r" onclick="act(\'pump/off\')">خاموش</button></div></div>';
  if(irr.soilPercent!=null)h+='<div class="card"><span class="ct">🌱 سنسور رطوبت خاک</span><div style="font-size:1.5rem;font-weight:700;color:'+(irr.soilPercent<30?'var(--danger)':'var(--accent)')+'">'+irr.soilPercent+'%</div><div style="font-size:.7rem;color:var(--muted)">خام: '+irr.soilRaw+'</div></div>';
  h+='<div class="cnv-wrap"><div class="cnv-t">📊 مصرف آب ۲۴ ساعته</div><canvas class="cnv" id="cvWater" width="600" height="70"></canvas><div style="text-align:center;font-size:.65rem;color:var(--muted);margin-top:4px" id="cvLeg"></div></div>';
  var zones=irr.zones||[];
  zones.forEach(function(z){
    h+='<div class="card"><div class="ch"><span class="ct">💧 زون '+z.id+' • GPIO'+z.pin+'</span><span class="stg '+(z.running?'run':'off')+'">'+(z.running?'فعال ('+z.remainingSec+'ث)':'خاموش')+'</span></div><div style="font-size:.7rem;color:var(--muted);margin-bottom:5px">بعدی: '+(z.nextRun||'--')+' | فعال: '+(z.enabled?'بله':'خیر')+' | اجرا امروز: '+(z.ranToday?'بله':'خیر')+'</div><div class="fr"><button class="bt g sm" onclick="act(\'zone/on\',{id:'+z.id+',dur:'+(z.duration||15)+'})">روشن</button><button class="bt r sm" onclick="act(\'zone/off\',{id:'+z.id+'})">خاموش</button></div><div class="fr"><input type="time" id="st'+z.id+'" value="'+pad(z.hour)+':'+pad(z.minute)+'"><input type="number" id="sd'+z.id+'" value="'+(z.duration||15)+'" min="1" max="480"><span style="font-size:.7rem;color:var(--muted)">دقیقه</span><button class="bt b sm" onclick="setSch('+z.id+')">تنظیم</button></div></div>'
  });
  $('pg-irrigation').innerHTML=h
}
function renderLighting(){
  var d=st&&st.deviceInfo?st.deviceInfo[did]:null,lt=d&&d.lighting?d.lighting:null;
  if(!lt){$('pg-lighting').innerHTML='<div class="card"><div class="ct">ماژول نور فعال نیست</div></div>';return}
  var h='';
  h+='<div class="card"><span class="ct">💡 کنترل کلی</span><div class="fr"><button class="bt g sm" onclick="act(\'lighting/all-on\')">همه روشن</button><button class="bt r sm" onclick="act(\'lighting/all-off\')">همه خاموش</button></div></div>';
  h+='<div class="cnv-wrap"><div class="cnv-t">📊 وضعیت نور ۲۴ ساعته</div><canvas class="cnv" id="cvLight" width="600" height="70"></canvas></div>';
  var chs=lt.channels||lt.state||[];
  chs.forEach(function(c){
    h+='<div class="card"><div class="ch"><span class="ct">کانال '+c.id+' • GPIO'+c.pin+'</span><span class="stg '+(c.state?'run':'off')+'">'+(c.state?'روشن':'خاموش')+'</span></div><div style="font-size:.7rem;color:var(--muted);margin-bottom:5px">زمانبندی: '+(c.scheduleEnabled?c.onTime+' تا '+c.offTime:'غیرفعال')+'</div><button class="bt '+(c.state?'r':'g')+' sm" onclick="act(\'lighting/toggle\',{id:'+c.id+'})">تغییر وضعیت</button></div>'
  });
  $('pg-lighting').innerHTML=h
}
function renderLogs(){
  var ev=st&&st.allEvents?st.allEvents:[];var f=ev.filter(function(e){return e.device_id===did}).slice(0,60);
  var h='<div class="logs">';
  if(!f.length)h+='<div style="color:var(--muted)">رویدادی ثبت نشده</div>';
  f.forEach(function(e){
    var d=new Date(e.received_at||e.ts),t=pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());
    h+='<div class="le"><span><span class="lt">'+e.event_type+'</span><span class="ls">'+e.state+'</span></span><span class="tm">'+t+'</span></div>'
  });
  h+='</div><div style="font-size:.65rem;color:var(--muted);margin-top:3px">'+ev.length+' رویداد کل | '+f.length+' نمایش</div>';
  $('pg-logs').innerHTML=h
}
function renderSettings(){
  $('pg-settings').innerHTML=
  '<div class="setc"><h3>📡 همگام‌سازی</h3><div class="fr"><input type="text" id="syncUrl" placeholder="آدرس سرور" style="flex:1"><button class="bt b sm" onclick="saveSync()">ذخیره</button></div></div>'+
  '<div class="setc"><h3>⏰ تنظیم ساعت</h3><div class="fr"><input type="number" id="rtcY" value="'+new Date().getFullYear()+'" placeholder="سال" style="width:58px"><input type="number" id="rtcM" value="'+(new Date().getMonth()+1)+'" placeholder="ماه" style="width:44px"><input type="number" id="rtcD" value="'+new Date().getDate()+'" placeholder="روز" style="width:44px"><input type="number" id="rtcH" value="'+new Date().getHours()+'" placeholder="ساعت" style="width:44px"><input type="number" id="rtcMi" value="'+new Date().getMinutes()+'" placeholder="دقیقه" style="width:48px"><button class="bt b sm" onclick="setRTC()">تنظیم</button></div></div>'+
  '<div class="setc"><h3>📶 وای‌فای</h3><div class="fr"><input type="text" id="wSsid" placeholder="SSID" style="flex:1"><input type="password" id="wPass" placeholder="Password" style="flex:1"><button class="bt b sm" onclick="saveWifi()">ذخیره</button></div></div>'+
  '<div class="setc"><h3>➕ افزودن زون</h3><div class="fr"><input type="number" id="azId" value="1" min="1" placeholder="ID"><input type="number" id="azPin" value="26" placeholder="GPIO"><button class="bt b sm" onclick="addZone()">افزودن</button></div></div>'+
  '<div class="setc"><h3>📦 فریمور OTA</h3><div class="fr"><input type="file" id="fwFile" accept=".bin"><button class="bt b sm" onclick="uploadFW()">آپلود</button></div><div style="font-size:.7rem;color:var(--muted);margin-top:6px" id="fwStatus"></div></div>'
}

function setSch(id){
  var t=$('st'+id),d=$('sd'+id);if(!t||!d)return;
  var v=t.value.split(':');act('zone/schedule',{id:id,hour:parseInt(v[0]),minute:parseInt(v[1]),dur:parseInt(d.value)})
}
function saveSync(){act('config/sync',{url:$('syncUrl').value})}
function setRTC(){act('config/time',{year:$('rtcY').value,month:$('rtcM').value,day:$('rtcD').value,hour:$('rtcH').value,minute:$('rtcMi').value})}
function saveWifi(){act('config/wifi',{ssid:$('wSsid').value,password:$('wPass').value})}
function addZone(){var i=$('azId'),p=$('azPin');if(i&&p)act('zone/add',{id:i.value,pin:p.value})}
async function uploadFW(){
  var f=$('fwFile').files[0];if(!f)return;
  $('fwStatus').textContent='در حال آپلود...';
  var fd=new FormData();fd.append('firmware',f);
  try{var r=await fetch('/api/firmware',{method:'POST',body:fd}),j=await r.json();$('fwStatus').textContent='آپلود شد: '+j.file}catch(e){$('fwStatus').textContent='خطا در آپلود'}
}

function act(path,params){
  var url='/api/'+path;
  if(params){url+='?'+Object.entries(params).map(function(e){return e[0]+'='+encodeURIComponent(e[1])}).join('&')}
  fetch(url,{method:'POST'}).then(function(r){return r.json()}).then(function(j){if(j.ok)refresh()}).catch(function(){})
}

function drawCharts(){
  drawWaterTimeline();drawLightTimeline()
}

function drawWaterTimeline(){
  var c=$('cvWater'),ctx=c?c.getContext('2d'):null;if(!ctx)return;
  var w=c.width,h=c.height;ctx.clearRect(0,0,w,h);
  ctx.fillStyle='#06080a';ctx.fillRect(0,0,w,h);
  for(var i=0;i<=24;i+=4){var x=i*w/24;ctx.strokeStyle='rgba(255,255,255,0.06)';ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,h);ctx.stroke()}
  ctx.fillStyle='#6b7280';ctx.font='9px monospace';ctx.textAlign='center';
  for(var i=0;i<=24;i+=4)ctx.fillText(i,Math.max(8,i*w/24),h-4);
  var now=new Date(),ds=new Date();ds.setHours(0,0,0,0);var nx=(now-ds)/86400000*w;
  ctx.strokeStyle='rgba(239,68,68,0.5)';ctx.beginPath();ctx.moveTo(nx,0);ctx.lineTo(nx,h);ctx.stroke();
  var tl=st&&st.timeline?st.timeline:[];if(!tl.length){ctx.fillStyle='#6b7280';ctx.font='10px sans-serif';ctx.fillText('امروز آبیاری انجام نشده',w/2,28);if($('cvLeg'))$('cvLeg').textContent='';return}
  var cols={pump:'#3b82f6',z1:'#10b981',z2:'#f59e0b',z3:'#a78bfa',z4:'#ec4899'};
  ctx.globalAlpha=0.85;
  tl.forEach(function(p){
    var x1=(p.start-ds)/86400000*w,x2=(p.end-ds)/86400000*w;
    var ck=p.type==='pump'?'pump':('z'+(p.zoneId||'1'));
    ctx.fillStyle=cols[ck]||cols.z1;
    ctx.fillRect(Math.max(0,x1),p.type==='pump'?4:26,Math.max(1.5,x2-x1),16);
    if(x2-x1>25&&p.duration){ctx.fillStyle='#fff';ctx.font='9px sans-serif';ctx.fillText(p.duration+'د',x1+3,p.type==='pump'?16:38)}
  });
  ctx.globalAlpha=1;
  var leg=[];tl.forEach(function(p){var t=p.type==='pump'?'⛽ پمپ':'💧 زون '+(p.zoneId||'1');if(leg.indexOf(t)<0)leg.push(t)});
  if($('cvLeg'))$('cvLeg').innerHTML=leg.map(function(t){return'<span style="margin:0 5px;font-size:.65rem">'+t+'</span>'}).join('')
}

function drawLightTimeline(){
  var c=$('cvLight'),ctx=c?c.getContext('2d'):null;if(!ctx)return;
  var w=c.width,h=c.height;ctx.clearRect(0,0,w,h);
  ctx.fillStyle='#06080a';ctx.fillRect(0,0,w,h);
  for(var i=0;i<=24;i+=4){var x=i*w/24;ctx.strokeStyle='rgba(255,255,255,0.06)';ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,h);ctx.stroke()}
  ctx.fillStyle='#6b7280';ctx.font='9px monospace';ctx.textAlign='center';
  for(var i=0;i<=24;i+=4)ctx.fillText(i,Math.max(8,i*w/24),h-4);
  var now=new Date(),ds=new Date();ds.setHours(0,0,0,0);var nx=(now-ds)/86400000*w;
  ctx.strokeStyle='rgba(239,68,68,0.5)';ctx.beginPath();ctx.moveTo(nx,0);ctx.lineTo(nx,h);ctx.stroke();
  var d=st&&st.deviceInfo?st.deviceInfo[did]:null,lt=d&&d.lighting?d.lighting:null;
  var chs=lt?lt.channels||lt.state||[]:[];if(!chs.length){ctx.fillStyle='#6b7280';ctx.font='10px sans-serif';ctx.fillText('داده\u200cای برای نمایش نیست',w/2,30);return}
  var cols=['#f59e0b','#3b82f6','#10b981','#a78bfa'];chs.forEach(function(c,i){var y=8+i*15;ctx.fillStyle=cols[i%cols.length];ctx.fillRect(4,y,8,8);ctx.fillText('Ch'+c.id+(c.state?' ON':' OFF'),16,y+8);ctx.fillStyle='#e4e6ea';ctx.font='8px sans-serif';ctx.textAlign='left'})
}

$('tabs').addEventListener('click',function(e){
  var tb=e.target.closest('.tab');if(!tb)return;
  page=tb.dataset.pg;
  document.querySelectorAll('.tab').forEach(function(t){t.classList.toggle('on',t===tb)});
  document.querySelectorAll('[id^="pg-"]').forEach(function(p){p.classList.toggle('hidden',p.id!=='pg-'+page)});
  if(page==='settings')renderSettings();
  if(page==='overview')renderOverview();
  if(page==='logs')renderLogs();
  refresh()
});

$('devSel').addEventListener('change',function(){setDid(this.value)});

function tick(){var d=new Date();$('clock').textContent=d.toLocaleTimeString('fa-IR',{hour:'2-digit',minute:'2-digit',second:'2-digit'})}
tick();setInterval(tick,1000);
renderSettings();refresh();setInterval(refresh,4000);
</script></body></html>`;
}

// =========== START ===========
server.listen(PORT, '0.0.0.0', () => {
  console.log(`[Server] خانه سبز هوشمند v2 running on port ${PORT}`);
  console.log(`[Server] Dashboard: http://0.0.0.0:${PORT}`);
});

process.on('SIGINT', () => { saveEvents(); saveDevices(); saveUsers(); process.exit(); });
process.on('SIGTERM', () => { saveEvents(); saveDevices(); saveUsers(); process.exit(); });
setInterval(() => { saveEvents(); saveDevices(); }, 30000);
