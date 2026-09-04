const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');

// --------------- Config ---------------
const PORT = process.env.PORT || 3000;
const API_KEY = process.env.API_KEY || 'gh-greenhouse-2025-secure-key-change-me';
const devices = new Map(); // deviceId -> last seen info

// --------------- SQLite setup ---------------
let db;
try {
  const Database = require('better-sqlite3');
  db = new Database('greenhouse.db');
  db.pragma('journal_mode = WAL');
  db.exec(`
    CREATE TABLE IF NOT EXISTS events (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      device_id TEXT NOT NULL,
      event_type TEXT NOT NULL,
      state TEXT NOT NULL,
      ts INTEGER NOT NULL,
      received_at INTEGER NOT NULL DEFAULT (strftime('%s','now')*1000)
    );
    CREATE INDEX IF NOT EXISTS idx_events_device ON events(device_id);
    CREATE INDEX IF NOT EXISTS idx_events_type ON events(event_type);
    CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts DESC);
    
    CREATE TABLE IF NOT EXISTS device_state (
      device_id TEXT PRIMARY KEY,
      state_json TEXT NOT NULL DEFAULT '{}',
      last_seen INTEGER NOT NULL DEFAULT 0,
      ip TEXT,
      uptime INTEGER DEFAULT 0
    );
  `);
  console.log('[DB] SQLite ready');
} catch(e) {
  console.error('[DB] better-sqlite3 not available, falling back to memory store:', e.message);
  // In-memory fallback
  const memEvents = [];
  db = {
    memory: true,
    events: [],
    addEvent(deviceId, type, state, ts) {
      const e = { id: memEvents.length+1, device_id: deviceId, event_type: type, state, ts, received_at: Date.now() };
      memEvents.push(e);
      if (memEvents.length > 10000) memEvents.shift();
      return e;
    },
    getEvents(deviceId, limit=100) {
      return memEvents.filter(e => e.device_id === deviceId || !deviceId).slice(-limit);
    }
  };
}

// --------------- Express + HTTP ---------------
const app = express();
app.use(express.json({ limit: '1mb' }));
app.use(express.urlencoded({ extended: true }));

// CORS for ESP32 + browser
app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET,POST,PUT,DELETE,OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, X-API-Key, Authorization');
  if (req.method === 'OPTIONS') return res.sendStatus(200);
  next();
});

// API Key middleware
function requireApiKey(req, res, next) {
  const key = req.headers['x-api-key'] || req.query.api_key;
  if (key !== API_KEY) return res.status(401).json({ error: 'invalid API key' });
  next();
}

// --------------- Static Dashboard ---------------
app.get('/', (req, res) => {
  res.type('html').send(generateDashboard());
});

// Health check
app.get('/api/health', (req, res) => {
  res.json({
    status: 'ok',
    uptime: process.uptime(),
    memory: process.memoryUsage(),
    devices: devices.size,
    nodejs: process.version
  });
});

// --------------- Device Events (from ESP32) ---------------
app.post('/api/events', (req, res) => {
  const { deviceId, uptime, events, status: deviceStatus } = req.body;
  if (!deviceId) return res.status(400).json({ error: 'deviceId required' });

  // Update device state
  const deviceInfo = {
    lastSeen: Date.now(),
    uptime: uptime || 0,
    ip: req.ip || req.connection.remoteAddress,
    status: deviceStatus || {}
  };
  devices.set(deviceId, deviceInfo);

  // Store in DB
  try {
    if (db.memory) {
      if (events) events.forEach(e => db.addEvent(deviceId, e.type, e.state, e.ts));
    } else {
      if (events && events.length > 0) {
        const insert = db.prepare('INSERT INTO events (device_id, event_type, state, ts) VALUES (?, ?, ?, ?)');
        const upsertState = db.prepare(`INSERT OR REPLACE INTO device_state (device_id, state_json, last_seen, ip, uptime) VALUES (?, ?, ?, ?, ?)`);
        
        const tx = db.transaction(() => {
          for (const e of events) {
            insert.run(deviceId, e.type, e.state, e.ts || Date.now());
          }
          upsertState.run(deviceId, JSON.stringify(deviceStatus || {}), Date.now(), req.ip, uptime || 0);
        });
        tx();
      }
    }
  } catch(e) { console.error('[DB] Error storing events:', e.message); }

  // Broadcast to WebSocket clients
  if (events && events.length > 0) {
    const msg = JSON.stringify({ type: 'events', deviceId, events: events.slice(-10) });
    wss.clients.forEach(c => { if (c.readyState === WebSocket.OPEN) c.send(msg); });
  }

  // Broadcast device status update
  broadcastDeviceStatus();

  res.json({ ok: true, received: events ? events.length : 0 });
});

// --------------- API: Get events ---------------
app.get('/api/events', (req, res) => {
  const deviceId = req.query.device;
  const limit = parseInt(req.query.limit) || 100;
  const since = parseInt(req.query.since) || 0;

  try {
    if (db.memory) {
      res.json({ events: db.getEvents(deviceId, limit) });
    } else {
      let query, params;
      if (deviceId) {
        query = 'SELECT * FROM events WHERE device_id = ? AND ts > ? ORDER BY id DESC LIMIT ?';
        params = [deviceId, since, limit];
      } else {
        query = 'SELECT * FROM events WHERE ts > ? ORDER BY id DESC LIMIT ?';
        params = [since, limit];
      }
      res.json({ events: db.prepare(query).all(...params).reverse() });
    }
  } catch(e) { res.status(500).json({ error: e.message }); }
});

// --------------- API: Get devices status ---------------
app.get('/api/devices', (req, res) => {
  const list = [];
  devices.forEach((info, id) => {
    list.push({
      deviceId: id,
      lastSeen: info.lastSeen,
      uptime: info.uptime,
      online: (Date.now() - info.lastSeen) < 120000 // 2 min timeout
    });
  });
  res.json({ devices: list, count: list.length });
});

// --------------- API: Remote Control (for future) ---------------
app.post('/api/control', requireApiKey, (req, res) => {
  const { deviceId, command } = req.body;
  if (!deviceId || !command) return res.status(400).json({ error: 'deviceId + command required' });
  // Forward command via WebSocket if client is connected
  let sent = false;
  wss.clients.forEach(c => {
    if (c.deviceId === deviceId && c.readyState === WebSocket.OPEN) {
      c.send(JSON.stringify({ type: 'command', ...command }));
      sent = true;
    }
  });
  res.json({ ok: sent, note: sent ? 'command forwarded' : 'device not connected via WebSocket' });
});

// --------------- System Stats ---------------
app.get('/api/stats', (req, res) => {
  try {
    if (db.memory) {
      res.json({ totalEvents: db.events.length, devices: devices.size });
    } else {
      const totalEvents = db.prepare('SELECT COUNT(*) as c FROM events').get().c;
      const todayEvents = db.prepare("SELECT COUNT(*) as c FROM events WHERE received_at > strftime('%s','now','start of day')*1000").get().c;
      const topTypes = db.prepare('SELECT event_type, COUNT(*) as c FROM events GROUP BY event_type ORDER BY c DESC LIMIT 10').all();
      res.json({ totalEvents, todayEvents, devices: devices.size, topTypes });
    }
  } catch(e) { res.status(500).json({ error: e.message }); }
});

// --------------- API: Delete old events ---------------
app.delete('/api/events', requireApiKey, (req, res) => {
  const days = parseInt(req.query.older_than) || 30;
  try {
    if (!db.memory) {
      const cutoff = Date.now() - (days * 86400000);
      const result = db.prepare('DELETE FROM events WHERE ts < ?').run(cutoff);
      res.json({ ok: true, deleted: result.changes });
    } else { res.json({ ok: true, deleted: 0, note: 'memory mode' }); }
  } catch(e) { res.status(500).json({ error: e.message }); }
});

// --------------- HTTP Server + WebSocket ---------------
const server = http.createServer(app);
const wss = new WebSocket.Server({ server, path: '/ws' });

wss.on('connection', (ws, req) => {
  const deviceId = req.url.includes('device=') 
    ? new URL(req.url, 'http://localhost').searchParams.get('device') 
    : 'browser';
  ws.deviceId = deviceId;
  console.log(`[WS] connected: ${deviceId} (total: ${wss.clients.size})`);

  ws.send(JSON.stringify({ type: 'connected', serverTime: Date.now(), clientCount: wss.clients.size }));

  // Send current device list
  broadcastDeviceStatus();

  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data);
      if (msg.type === 'ping') ws.send(JSON.stringify({ type: 'pong', ts: Date.now() }));
      if (msg.type === 'command_response') {
        console.log(`[CMD] Response from ${deviceId}:`, msg);
      }
    } catch(e) {}
  });

  ws.on('close', () => {
    console.log(`[WS] disconnected: ${deviceId} (total: ${wss.clients.size})`);
    broadcastDeviceStatus();
  });
});

function broadcastDeviceStatus() {
  const list = [];
  devices.forEach((info, id) => {
    list.push({
      deviceId: id,
      lastSeen: info.lastSeen,
      uptime: info.uptime,
      online: (Date.now() - info.lastSeen) < 120000
    });
  });
  wss.clients.forEach(c => {
    if (c.readyState === WebSocket.OPEN && c.deviceId === 'browser') {
      c.send(JSON.stringify({ type: 'devices', devices: list }));
    }
  });
}

// --------------- Dashboard HTML ---------------
function generateDashboard() {
  return `<!DOCTYPE html>
<html lang="fa" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>گلخانه هوشمند</title>
<style>
:root {
  --bg: #0f172a; --card: #1e293b; --accent: #10b981; --accent2: #3b82f6;
  --danger: #ef4444; --warn: #f59e0b; --text: #f1f5f9; --sub: #94a3b8;
  --radius: 16px; --gap: 12px;
}
* { margin: 0; padding: 0; box-sizing: border-box; }
body { background: var(--bg); color: var(--text); font-family: -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif; min-height: 100vh; padding-bottom: 20px; }
.container { max-width: 600px; margin: 0 auto; padding: 12px; }
/* Header */
.header { background: linear-gradient(135deg, #0f766e, #10b981); border-radius: var(--radius); padding: 20px 16px; margin-bottom: var(--gap); position: relative; overflow: hidden; }
.header::after { content:''; position:absolute; top:-40px; right:-40px; width:120px; height:120px; background:rgba(255,255,255,0.06); border-radius:50%; }
.header h1 { font-size: 20px; font-weight: 700; margin-bottom: 4px; }
.header .sub { font-size: 13px; opacity: 0.85; }
.header .status-dot { display:inline-block; width:8px; height:8px; background:#fff; border-radius:50%; margin-left:6px; animation: pulse 2s infinite; }
@keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.4} }

/* Stats row */
.stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: var(--gap); }
.stat-card { background: var(--card); border-radius: 12px; padding: 14px 10px; text-align: center; }
.stat-card .val { font-size: 24px; font-weight: 700; margin-bottom: 2px; }
.stat-card .lbl { font-size: 11px; color: var(--sub); }
.stat-card.online .val { color: var(--accent); }
.stat-card.events .val { color: var(--accent2); }
.stat-card.uptime .val { color: var(--warn); }

/* Info cards */
.info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: var(--gap); }
.info-card { background: var(--card); border-radius: 12px; padding: 14px; }
.info-card .label { font-size: 11px; color: var(--sub); margin-bottom: 4px; }
.info-card .value { font-size: 16px; font-weight: 600; }
.info-card .value.online { color: var(--accent); }
.info-card .value.offline { color: var(--danger); }

/* Section */
.section { margin-bottom: var(--gap); }
.section-title { font-size: 15px; font-weight: 600; margin-bottom: 8px; display:flex; align-items:center; gap:6px; }
.section-title::before { content:''; width:4px; height:18px; background:var(--accent); border-radius:2px; }

/* Log / Event list */
.log-list { background: var(--card); border-radius: var(--radius); overflow: hidden; }
.log-item { padding: 12px 14px; border-bottom: 1px solid rgba(255,255,255,0.05); display: flex; justify-content: space-between; align-items: center; gap: 10px; }
.log-item:last-child { border-bottom: none; }
.log-item .evt-type { font-size: 13px; font-weight: 500; }
.log-item .evt-state { font-size: 12px; color: var(--sub); padding: 3px 8px; background: rgba(255,255,255,0.06); border-radius: 6px; max-width: 160px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.log-item .evt-time { font-size: 11px; color: var(--sub); white-space: nowrap; }
.log-empty { padding: 24px; text-align: center; color: var(--sub); font-size: 13px; }

/* Device list */
.device-card { background: var(--card); border-radius: 12px; padding: 14px; margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center; }
.device-card .dev-id { font-weight: 600; font-size: 14px; }
.device-card .dev-info { font-size: 12px; color: var(--sub); }
.device-card .dev-status { font-size: 12px; padding: 4px 12px; border-radius: 20px; font-weight: 500; }
.device-card .dev-status.online { background: rgba(16,185,129,0.15); color: var(--accent); }
.device-card .dev-status.offline { background: rgba(239,68,68,0.15); color: var(--danger); }

/* Refresh indicator */
.refresh-bar { display: flex; justify-content: space-between; align-items: center; padding: 8px 0; margin-bottom: var(--gap); }
.refresh-bar .last-update { font-size: 11px; color: var(--sub); }
.refresh-bar button { background: var(--card); color: var(--text); border: 1px solid rgba(255,255,255,0.1); padding: 6px 14px; border-radius: 8px; font-size: 12px; cursor: pointer; }

/* Responsive */
@media (max-width: 380px) {
  .stats { grid-template-columns: repeat(3,1fr); gap: 6px; }
  .info-grid { grid-template-columns: 1fr; }
}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <h1>🌱 گلخانه هوشمند</h1>
    <div class="sub"><span class="status-dot"></span> <span id="connStatus">در حال اتصال...</span></div>
  </div>

  <div class="refresh-bar">
    <span class="last-update" id="lastUpdate">--</span>
    <button onclick="refreshAll()">🔄 به‌روزرسانی</button>
  </div>

  <div class="stats">
    <div class="stat-card online"><div class="val" id="statDevices">0</div><div class="lbl">دستگاه فعال</div></div>
    <div class="stat-card events"><div class="val" id="statEvents">0</div><div class="lbl">رویداد امروز</div></div>
    <div class="stat-card uptime"><div class="val" id="statUptime">0</div><div class="lbl">دقیقه uptime</div></div>
  </div>

  <div class="section">
    <div class="section-title">دستگاه‌ها</div>
    <div id="deviceList"><div class="log-empty">منتظر اتصال دستگاه...</div></div>
  </div>

  <div class="section">
    <div class="section-title">آخرین رویدادها</div>
    <div class="log-list" id="logList">
      <div class="log-empty">هنوز رویدادی ثبت نشده</div>
    </div>
  </div>
</div>

<script>
const API = window.location.origin;
const WS_URL = (API.startsWith('https') ? 'wss://' : 'ws://') + window.location.host + '/ws?device=browser';
let ws, lastUpdate = Date.now();

function initWS() {
  try {
    ws = new WebSocket(WS_URL);
    ws.onopen = () => { document.getElementById('connStatus').textContent = 'متصل'; loadStats(); loadDevices(); loadEvents(); };
    ws.onclose = () => { document.getElementById('connStatus').textContent = 'قطع - تلاش مجدد...'; setTimeout(initWS, 3000); };
    ws.onerror = () => ws.close();
    ws.onmessage = (e) => {
      try {
        const msg = JSON.parse(e.data);
        if (msg.type === 'events') appendEvents(msg.events || []);
        if (msg.type === 'devices') renderDevices(msg.devices || []);
        if (msg.type === 'connected') document.getElementById('connStatus').textContent = 'متصل';
      } catch(ex) {}
    };
  } catch(e) { setTimeout(initWS, 3000); }
}

function fmtTime(ts) {
  const d = new Date(ts);
  return d.toLocaleTimeString('fa-IR', {hour:'2-digit',minute:'2-digit',second:'2-digit'});
}

function fmtRelative(ts) {
  const diff = Date.now() - ts;
  if (diff < 60000) return 'اکنون';
  if (diff < 3600000) return Math.floor(diff/60000) + ' دقیقه پیش';
  return Math.floor(diff/3600000) + ' ساعت پیش';
}

async function loadStats() {
  try {
    const r = await fetch(API+'/api/stats');
    const d = await r.json();
    document.getElementById('statDevices').textContent = d.devices || 0;
    document.getElementById('statEvents').textContent = d.todayEvents || 0;
    document.getElementById('statUptime').textContent = Math.floor((Date.now()-lastUpdate)/60000);
  } catch(e) {}
}

async function loadDevices() {
  try {
    const r = await fetch(API+'/api/devices');
    const d = await r.json();
    renderDevices(d.devices || []);
  } catch(e) {}
}

function renderDevices(list) {
  const el = document.getElementById('deviceList');
  if (list.length === 0) { el.innerHTML = '<div class="log-empty">دستگاهی متصل نیست</div>'; return; }
  el.innerHTML = list.map(d => \`
    <div class="device-card">
      <div>
        <div class="dev-id">📡 \${d.deviceId}</div>
        <div class="dev-info">uptime: \${Math.floor((d.uptime||0)/60000)} دقیقه | \${fmtRelative(d.lastSeen)}</div>
      </div>
      <div class="dev-status \${d.online?'online':'offline'}">\${d.online?'آنلاین':'آفلاین'}</div>
    </div>\`).join('');
}

async function loadEvents() {
  try {
    const r = await fetch(API+'/api/events?limit=30');
    const d = await r.json();
    appendEvents(d.events || [], true);
  } catch(e) {}
}

function appendEvents(events, replace=false) {
  const el = document.getElementById('logList');
  if (replace) el.innerHTML = '';
  if (!events.length && replace) { el.innerHTML = '<div class="log-empty">هنوز رویدادی ثبت نشده</div>'; return; }
  const items = events.slice(-30).reverse().map(e => \`
    <div class="log-item">
      <span class="evt-type">\${e.event_type || e.type}</span>
      <span class="evt-state">\${e.state || ''}</span>
      <span class="evt-time">\${fmtTime(e.ts || e.received_at)}</span>
    </div>\`).join('');
  if (replace) { el.innerHTML = items || '<div class="log-empty">هنوز رویدادی ثبت نشده</div>'; }
  else { el.insertAdjacentHTML('afterbegin', items); }
  document.getElementById('lastUpdate').textContent = 'آخرین به‌روزرسانی: ' + fmtTime(Date.now());
  lastUpdate = Date.now();
}

function refreshAll() { loadStats(); loadDevices(); loadEvents(); }

// Auto refresh every 30s
setInterval(refreshAll, 30000);
initWS();
refreshAll();
</script>
</body>
</html>`;
}

// --------------- Start ---------------
server.listen(PORT, '0.0.0.0', () => {
  console.log(`[Server] Smart Greenhouse running on port ${PORT}`);
  console.log(`[Server] API Key: ${API_KEY.substring(0,8)}...`);
});

// Graceful shutdown
process.on('SIGINT', () => { console.log('\n[Server] Shutting down...'); if (db && db.close) db.close(); process.exit(); });
