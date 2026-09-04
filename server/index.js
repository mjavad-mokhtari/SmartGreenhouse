const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const fs = require('fs');
const path = require('path');

// --------------- Config ---------------
const PORT = process.env.PORT || 3000;
const API_KEY = process.env.API_KEY || 'gh-greenhouse-2025-secure-key-change-me';
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, 'data');
const EVENTS_FILE = path.join(DATA_DIR, 'events.json');
const DEVICES_FILE = path.join(DATA_DIR, 'devices.json');
const MAX_EVENTS = 10000;

// --------------- File-Based Store ---------------
let events = [];
let deviceStates = {};
let stats = { totalEvents: 0, todayEvents: 0 };

function initStore() {
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  try { events = JSON.parse(fs.readFileSync(EVENTS_FILE, 'utf8') || '[]'); } catch(e) { events = []; }
  try { deviceStates = JSON.parse(fs.readFileSync(DEVICES_FILE, 'utf8') || '{}'); } catch(e) { deviceStates = {}; }
  stats.totalEvents = events.length;
  const todayStart = new Date(); todayStart.setHours(0,0,0,0);
  stats.todayEvents = events.filter(e => e.received_at >= todayStart.getTime()).length;
}

function saveEvents() {
  if (events.length > MAX_EVENTS) events = events.slice(-MAX_EVENTS);
  fs.writeFileSync(EVENTS_FILE, JSON.stringify(events), 'utf8');
}

function saveDevices() {
  fs.writeFileSync(DEVICES_FILE, JSON.stringify(deviceStates), 'utf8');
}

function addEvent(deviceId, type, state, ts) {
  const e = {
    id: events.length + 1,
    device_id: deviceId,
    event_type: type,
    state,
    ts: ts || Date.now(),
    received_at: Date.now()
  };
  events.push(e);
  if (events.length > MAX_EVENTS) events = events.slice(-MAX_EVENTS);
  stats.totalEvents++;
  const todayStart = new Date(); todayStart.setHours(0,0,0,0);
  if (e.received_at >= todayStart.getTime()) stats.todayEvents++;
  // Save every 10 events
  if (events.length % 10 === 0) saveEvents();
  return e;
}

initStore();
console.log(`[Store] ${events.length} events loaded from file`);

// Active device tracking (in-memory)
const activeDevices = new Map(); // deviceId -> { lastSeen, uptime, ip, status }

// --------------- Express + HTTP ---------------
const app = express();
app.use(express.json({ limit: '1mb' }));
app.use(express.urlencoded({ extended: true }));

app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Methods', 'GET,POST,PUT,DELETE,OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, X-API-Key, Authorization');
  if (req.method === 'OPTIONS') return res.sendStatus(200);
  next();
});

function requireApiKey(req, res, next) {
  const key = req.headers['x-api-key'] || req.query.api_key;
  if (key !== API_KEY) return res.status(401).json({ error: 'invalid API key' });
  next();
}

// --------------- Routes ---------------

// Dashboard
app.get('/', (req, res) => {
  res.type('html').send(generateDashboard());
});

// Health
app.get('/api/health', (req, res) => {
  res.json({
    status: 'ok',
    uptime: process.uptime(),
    memory: process.memoryUsage(),
    devices: activeDevices.size,
    nodejs: process.version
  });
});

// Receive events from ESP32
app.post('/api/events', (req, res) => {
  const { deviceId, uptime, events: evts, status: deviceStatus } = req.body;
  if (!deviceId) return res.status(400).json({ error: 'deviceId required' });

  activeDevices.set(deviceId, {
    lastSeen: Date.now(),
    uptime: uptime || 0,
    ip: req.ip || req.connection.remoteAddress,
    status: deviceStatus || {}
  });
  deviceStates[deviceId] = {
    lastSeen: Date.now(),
    uptime: uptime || 0,
    ip: req.ip,
    status: deviceStatus || {}
  };
  saveDevices();

  let received = 0;
  if (evts && evts.length > 0) {
    for (const e of evts) {
      addEvent(deviceId, e.type, e.state, e.ts);
      received++;
    }
  }

  // Broadcast to WebSocket
  if (evts && evts.length > 0) {
    broadcast({ type: 'events', deviceId, events: evts.slice(-10) });
  }
  broadcastDeviceStatus();

  res.json({ ok: true, received });
});

// Get events
app.get('/api/events', (req, res) => {
  const deviceId = req.query.device;
  const limit = parseInt(req.query.limit) || 100;
  const since = parseInt(req.query.since) || 0;

  let filtered = deviceId
    ? events.filter(e => e.device_id === deviceId)
    : events;
  if (since > 0) filtered = filtered.filter(e => e.ts > since);
  res.json({ events: filtered.slice(-limit) });
});

// Get devices
app.get('/api/devices', (req, res) => {
  const list = [];
  activeDevices.forEach((info, id) => {
    list.push({
      deviceId: id,
      lastSeen: info.lastSeen,
      uptime: info.uptime,
      online: (Date.now() - info.lastSeen) < 120000
    });
  });
  res.json({ devices: list, count: list.length });
});

// Remote control (future)
app.post('/api/control', requireApiKey, (req, res) => {
  const { deviceId, command } = req.body;
  if (!deviceId || !command) return res.status(400).json({ error: 'deviceId + command required' });
  let sent = false;
  wss.clients.forEach(c => {
    if (c.deviceId === deviceId && c.readyState === WebSocket.OPEN) {
      c.send(JSON.stringify({ type: 'command', ...command }));
      sent = true;
    }
  });
  res.json({ ok: sent, note: sent ? 'forwarded' : 'device not connected' });
});

// Stats
app.get('/api/stats', (req, res) => {
  res.json({
    totalEvents: stats.totalEvents,
    todayEvents: stats.todayEvents,
    devices: activeDevices.size
  });
});

// Delete old events
app.delete('/api/events', requireApiKey, (req, res) => {
  const days = parseInt(req.query.older_than) || 30;
  const cutoff = Date.now() - (days * 86400000);
  const before = events.length;
  events = events.filter(e => e.received_at >= cutoff);
  stats.totalEvents = events.length;
  saveEvents();
  res.json({ ok: true, deleted: before - events.length });
});

// --------------- HTTP Server + WebSocket ---------------
const server = http.createServer(app);
const wss = new WebSocket.Server({ server, path: '/ws' });

function broadcast(msg) {
  const data = JSON.stringify(msg);
  wss.clients.forEach(c => { if (c.readyState === WebSocket.OPEN) c.send(data); });
}

function broadcastDeviceStatus() {
  const list = [];
  activeDevices.forEach((info, id) => {
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

wss.on('connection', (ws, req) => {
  const params = new URLSearchParams(req.url.split('?')[1] || '');
  const deviceId = params.get('device') || 'browser';
  ws.deviceId = deviceId;
  console.log(`[WS] + ${deviceId} (${wss.clients.size} clients)`);
  ws.send(JSON.stringify({ type: 'connected', serverTime: Date.now(), clients: wss.clients.size }));
  broadcastDeviceStatus();

  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data);
      if (msg.type === 'ping') ws.send(JSON.stringify({ type: 'pong', ts: Date.now() }));
    } catch(e) {}
  });

  ws.on('close', () => {
    console.log(`[WS] - ${deviceId} (${wss.clients.size} clients)`);
    broadcastDeviceStatus();
  });
});

// --------------- Dashboard HTML (Persian, RTL, Mobile-first) ---------------
function generateDashboard() {
  return `<!DOCTYPE html>
<html lang="fa" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>> گلخانه هوشمند</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--accent:#10b981;--blue:#3b82f6;--danger:#ef4444;--warn:#f59e0b;--text:#f1f5f9;--sub:#94a3b8;--radius:14px;--gap:10px}
*{margin:0;padding:0;box-sizing:border-box}
body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;min-height:100vh;padding-bottom:30px;font-size:14px}
.container{max-width:560px;margin:0 auto;padding:10px}
.header{background:linear-gradient(135deg,#0f766e,#10b981);border-radius:var(--radius);padding:18px 14px;margin-bottom:var(--gap);display:flex;justify-content:space-between;align-items:center}
.header h1{font-size:18px;font-weight:700}
.header .badge{font-size:11px;background:rgba(255,255,255,.2);padding:4px 10px;border-radius:20px}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:var(--gap)}
.stat{background:var(--card);border-radius:12px;padding:14px 10px;text-align:center}
.stat .v{font-size:22px;font-weight:700;margin-bottom:2px}
.stat .l{font-size:10px;color:var(--sub)}
.stat.g .v{color:var(--accent)}.stat.b .v{color:var(--blue)}.stat.y .v{color:var(--warn)}
.section{margin-bottom:var(--gap)}
.stitle{font-size:14px;font-weight:600;margin-bottom:8px;display:flex;align-items:center;gap:6px}
.stitle::before{content:'';width:3px;height:16px;background:var(--accent);border-radius:2px}
.card{background:var(--card);border-radius:var(--radius);overflow:hidden}
.row{padding:12px 14px;border-bottom:1px solid rgba(255,255,255,.04);display:flex;justify-content:space-between;align-items:center;gap:8px}
.row:last-child{border-bottom:none}
.row .t{font-size:13px;font-weight:500}
.row .s{font-size:11px;color:var(--sub);padding:3px 8px;background:rgba(255,255,255,.05);border-radius:6px;max-width:140px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.row .tm{font-size:11px;color:var(--sub);white-space:nowrap}
.empty{padding:24px;text-align:center;color:var(--sub);font-size:13px}
.dev-row{padding:12px 14px;display:flex;justify-content:space-between;align-items:center}
.dev-row .n{font-weight:600;font-size:13px}
.dev-row .i{font-size:11px;color:var(--sub)}
.dev-dot{font-size:11px;padding:4px 12px;border-radius:20px;font-weight:500}
.dev-dot.on{background:rgba(16,185,129,.15);color:var(--accent)}
.dev-dot.off{background:rgba(239,68,68,.15);color:var(--danger)}
.bar{display:flex;justify-content:space-between;align-items:center;padding:6px 0;margin-bottom:var(--gap)}
.bar .lu{font-size:10px;color:var(--sub)}
.bar button{background:var(--card);color:var(--text);border:1px solid rgba(255,255,255,.1);padding:5px 12px;border-radius:8px;font-size:11px;cursor:pointer}
</style>
</head>
<body>
<div class="container">
<div class="header"><h1>🌿 گلخانه هوشمند</h1><span class="badge" id="wsBadge">اتصال...</span></div>
<div class="bar"><span class="lu" id="lu">--</span><button onclick="rf()">🔄 به‌روزرسانی</button></div>
<div class="stats">
<div class="stat g"><div class="v" id="sd">0</div><div class="l">دستگاه</div></div>
<div class="stat b"><div class="v" id="se">0</div><div class="l">رویداد امروز</div></div>
<div class="stat y"><div class="v" id="su">0</div><div class="l">دقیقه</div></div>
</div>
<div class="section"><div class="stitle">📡 دستگاه‌ها</div><div id="dl"><div class="empty">منتظر اتصال...</div></div></div>
<div class="section"><div class="stitle">📋 آخرین رویدادها</div><div class="card" id="ll"><div class="empty">بدون رویداد</div></div></div>
</div>
<script>
const A=location.origin,W=(A.startsWith('https')?'wss://':'ws://')+location.host+'/ws?device=browser';
let ws,now=Date.now();
function X(){try{ws=new WebSocket(W);ws.onopen=()=>{G('wsBadge','متصل');L();D();E()};ws.onclose=()=>{G('wsBadge','قطع');setTimeout(X,3000)};ws.onerror=()=>ws.close();ws.onmessage=e=>{try{const m=JSON.parse(e.data);if(m.type==='events')P(m.events||[]);if(m.type==='devices')R(m.devices||[])}catch(ex){}}}catch(e){setTimeout(X,3000)}}
function G(id,v){document.getElementById(id).textContent=v}
function F(ts){return new Date(ts).toLocaleTimeString('fa-IR',{hour:'2-digit',minute:'2-digit',second:'2-digit'})}
async function L(){try{const r=await fetch(A+'/api/stats'),d=await r.json();G('sd',d.devices||0);G('se',d.todayEvents||0);G('su',Math.floor((Date.now()-now)/60000))}catch(e){}}
async function D(){try{const r=await fetch(A+'/api/devices'),d=await r.json();R(d.devices||[])}catch(e){}}
function R(l){const e=document.getElementById('dl');if(!l.length){e.innerHTML='<div class="empty">دستگاهی متصل نیست</div>';return}
e.innerHTML=l.map(d=>'<div class="card" style="margin-bottom:6px"><div class="dev-row"><div><div class="n">📡 '+d.deviceId+'</div><div class="i">uptime: '+Math.floor((d.uptime||0)/60000)+' دقیقه</div></div><span class="dev-dot '+(d.online?'on':'off')+'">'+(d.online?'آنلاین':'آفلاین')+'</span></div></div>').join('')}
async function E(){try{const r=await fetch(A+'/api/events?limit=30'),d=await r.json();P(d.events||[],1)}catch(e){}}
function P(ev,re){const e=document.getElementById('ll');if(re)e.innerHTML='';if(!ev.length&&re){e.innerHTML='<div class="empty">بدون رویداد</div>';return}
const i=ev.slice(-30).reverse().map(v=>'<div class="row"><span class="t">'+v.event_type+'</span><span class="s">'+v.state+'</span><span class="tm">'+F(v.ts||v.received_at)+'</span></div>').join('');
if(re)e.innerHTML=i||'<div class="empty">بدون رویداد</div>';else e.insertAdjacentHTML('afterbegin',i);G('lu','به‌روز: '+F(Date.now()));now=Date.now()}
function rf(){L();D();E()}
setInterval(rf,30000);X();rf();
</script>
</body></html>`;
}

// --------------- Start ---------------
server.listen(PORT, '0.0.0.0', () => {
  console.log(`[Server] Greenhouse running on port ${PORT}`);
  console.log(`[Server] API: http://0.0.0.0:${PORT}`);
  console.log(`[Server] Dashboard: http://<server-ip>:${PORT}`);
});

process.on('SIGINT', () => { saveEvents(); saveDevices(); process.exit(); });
process.on('SIGTERM', () => { saveEvents(); saveDevices(); process.exit(); });

// Periodic save
setInterval(() => { saveEvents(); saveDevices(); }, 30000);
