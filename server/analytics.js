'use strict';

const fs = require('fs');

const SAMPLE_INTERVAL_MS = 5 * 60 * 1000;
const MAX_POINTS = 50000;
const NON_MODULE_FIELDS = new Set(['wifi', 'rtc', 'health', 'sync', 'system', 'device']);

function metricUnit(metric) {
  const leaf = metric.split('.').pop().toLowerCase();
  if (leaf.includes('soilpercent') || leaf.includes('humidity')) return '%';
  if (leaf.includes('temperature') || leaf === 'temp') return '°C';
  if (leaf.includes('soilraw')) return 'raw';
  return 'state/value';
}

function flattenModule(deviceId, module, value, timestamp, prefix = '', out = []) {
  if (Array.isArray(value)) {
    value.forEach((item, index) => {
      const id = item && Number.isFinite(Number(item.id)) ? Number(item.id) : index;
      flattenModule(deviceId, module, item, timestamp, `${prefix}${prefix ? '.' : ''}${id}`, out);
    });
  } else if (value && typeof value === 'object') {
    for (const [key, child] of Object.entries(value)) {
      flattenModule(deviceId, module, child, timestamp, `${prefix}${prefix ? '.' : ''}${key}`, out);
    }
  } else if ((typeof value === 'number' && Number.isFinite(value)) || typeof value === 'boolean') {
    if (!prefix) return out;
    out.push({
      deviceId,
      module,
      metric: prefix,
      value: typeof value === 'boolean' ? Number(value) : value,
      unit: metricUnit(prefix),
      ts: timestamp
    });
  }
  return out;
}

function extractSamples(deviceId, status, timestamp = Date.now()) {
  if (!status || typeof status !== 'object') return [];
  const points = [];
  for (const [module, value] of Object.entries(status)) {
    if (!NON_MODULE_FIELDS.has(module) && value && typeof value === 'object') {
      flattenModule(deviceId, module, value, timestamp, '', points);
    }
  }
  return points;
}

class TelemetryStore {
  constructor(filePath, maxPoints = MAX_POINTS, intervalMs = SAMPLE_INTERVAL_MS) {
    this.filePath = filePath;
    this.maxPoints = maxPoints;
    this.intervalMs = intervalMs;
    this.points = [];
    this.latest = new Map();
  }

  load() {
    try {
      const parsed = JSON.parse(fs.readFileSync(this.filePath, 'utf8'));
      this.points = Array.isArray(parsed) ? parsed.slice(-this.maxPoints) : [];
    } catch (_) {
      this.points = [];
    }
    this.latest.clear();
    for (const point of this.points) this.latest.set(this.key(point), point);
  }

  key(point) {
    return `${point.deviceId}\u0000${point.module}\u0000${point.metric}`;
  }

  record(deviceId, status, timestamp = Date.now()) {
    let changed = false;
    for (const point of extractSamples(deviceId, status, timestamp)) {
      const key = this.key(point);
      const previous = this.latest.get(key);
      const discrete = point.unit === 'state/value';
      const valueChanged = previous && previous.value !== point.value;
      if (previous && timestamp - previous.ts < this.intervalMs && !(discrete && valueChanged)) continue;
      this.points.push(point);
      this.latest.set(key, point);
      changed = true;
    }
    if (!changed) return false;
    if (this.points.length > this.maxPoints) this.points = this.points.slice(-this.maxPoints);
    this.save();
    return true;
  }

  save() {
    try {
      const temporaryPath = `${this.filePath}.tmp`;
      fs.writeFileSync(temporaryPath, JSON.stringify(this.points), 'utf8');
      fs.renameSync(temporaryPath, this.filePath);
    } catch (error) {
      console.error('[Analytics] failed to persist telemetry:', error.message);
    }
  }
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function soilResponseInsights(points, events) {
  const soilByDevice = new Map();
  for (const point of points) {
    if (point.module !== 'irrigation' || point.metric !== 'soilPercent') continue;
    if (!soilByDevice.has(point.deviceId)) soilByDevice.set(point.deviceId, []);
    soilByDevice.get(point.deviceId).push(point);
  }

  const starts = (events || []).filter(event => event.event_type === 'zone' && /^start\s+zone\d+/i.test(String(event.state || '')))
    .slice().sort((a, b) => (a.received_at || a.ts) - (b.received_at || b.ts));
  const results = new Map();
  for (const start of starts) {
    const match = String(start.state).match(/^start\s+zone(\d+)/i);
    if (!match) continue;
    const startAt = Number(start.received_at || start.ts);
    const stop = (events || []).find(event => event.device_id === start.device_id && event.event_type === 'zone' &&
      new RegExp(`^stop\\s+zone${match[1]}$`, 'i').test(String(event.state || '')) &&
      Number(event.received_at || event.ts) >= startAt);
    if (!stop) continue;
    const stopAt = Number(stop.received_at || stop.ts);
    const series = soilByDevice.get(start.device_id) || [];
    const before = series.filter(p => p.ts <= startAt && p.ts >= startAt - 30 * 60 * 1000).slice(-1)[0];
    const after = series.find(p => p.ts >= stopAt + 10 * 60 * 1000 && p.ts <= stopAt + 60 * 60 * 1000);
    if (before && after) {
      const key = `${start.device_id}:${match[1]}`;
      if (!results.has(key)) results.set(key, { zone: Number(match[1]), deltas: [] });
      results.get(key).deltas.push(after.value - before.value);
    }
  }

  const insights = [];
  for (const [key, result] of results) {
    if (result.deltas.length < 3) continue;
    const response = median(result.deltas);
    const weak = response < 3;
    insights.push({
      id: `${weak ? 'irrigation-low-response' : 'irrigation-response-observed'}:${key}`,
      severity: weak ? 'warning' : 'info', module: 'irrigation',
      title: weak ? `اثر آبیاری زون ${result.zone} روی رطوبت کم دیده شده` : `پاسخ رطوبت زون ${result.zone} ثبت شده`,
      detail: weak
        ? `در ${result.deltas.length} چرخهٔ قابل‌مقایسه، افزایش میانهٔ رطوبت خاک ${response.toFixed(1)}٪ بوده است. سنسور، شیر/مسیر آب و در صورت نیاز سنسور جریان بررسی شوند.`
        : `در ${result.deltas.length} چرخهٔ قابل‌مقایسه، افزایش میانهٔ رطوبت خاک ${response.toFixed(1)}٪ بوده است؛ این خط پایه برای مقایسه‌های بعدی نگهداری می‌شود.`,
      evidence: { cycles: result.deltas.length, zone: result.zone, medianSoilDeltaPercent: Number(response.toFixed(1)) },
      action: 'recommendation-only'
    });
  }
  return insights;
}

function buildInsights(points, events, devices, now = Date.now()) {
  const insights = [];
  for (const [deviceId, device] of Object.entries(devices || {})) {
    if (device && device.online === false) {
      insights.push({
        id: `device-offline:${deviceId}`, severity: 'warning', module: 'system',
        title: `دستگاه ${deviceId} در دسترس نیست`,
        detail: 'دادهٔ تازه دریافت نمی‌شود؛ پیش از اتکا به تحلیل یا فرمان از اتصال دستگاه مطمئن شوید.',
        action: 'recommendation-only'
      });
    }
  }

  const soil = (points || []).filter(p => p.module === 'irrigation' && p.metric === 'soilPercent' && Number.isFinite(p.value))
    .sort((a, b) => a.ts - b.ts);
  const soilByDevice = new Map();
  for (const point of soil) {
    if (!soilByDevice.has(point.deviceId)) soilByDevice.set(point.deviceId, []);
    soilByDevice.get(point.deviceId).push(point);
  }
  for (const [deviceId, series] of soilByDevice) {
    const recentSoil = series.filter(p => now - p.ts <= 30 * 60 * 1000);
    if (recentSoil.length < 3 || recentSoil[recentSoil.length - 1].ts - recentSoil[0].ts < 10 * 60 * 1000 ||
        !recentSoil.slice(-3).every(p => p.value < 30)) continue;
    insights.push({
      id: `soil-consistently-low:${deviceId}`, severity: 'warning', module: 'irrigation',
      title: `رطوبت خاک دستگاه ${deviceId} به‌طور پایدار پایین است`,
      detail: 'در چند نمونهٔ اخیر رطوبت زیر ۳۰٪ بوده؛ برنامهٔ آبیاری و سلامت سنسور را بررسی کنید. موتور فعلاً آبیاری را خودکار شروع نمی‌کند.',
      action: 'recommendation-only'
    });
  }

  insights.push(...soilResponseInsights(points || [], events || []));
  const newest = (points || []).reduce((value, point) => Math.max(value, Number(point.ts) || 0), 0);
  return {
    generatedAt: now,
    mode: 'advisory-only',
    sampleCount: (points || []).length,
    oldestSampleAt: points && points.length ? points.reduce((oldest, point) => Math.min(oldest, point.ts), Infinity) : null,
    newestSampleAt: newest || null,
    insights
  };
}

module.exports = { TelemetryStore, extractSamples, buildInsights, SAMPLE_INTERVAL_MS, MAX_POINTS };
