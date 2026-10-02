'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { TelemetryStore, extractSamples, buildInsights } = require('./analytics');

test('extracts normalized telemetry for irrigation and future home modules', () => {
  const points = extractSamples('board-a', {
    irrigation: { soilPercent: 27, pumpOn: false, zones: [{ id: 1, running: false }] },
    lighting: { channels: [{ id: 2, state: true }] },
    wifi: { rssi: -40 }
  }, 1000);
  assert.ok(points.some(p => p.metric === 'soilPercent' && p.unit === '%' && p.value === 27));
  assert.ok(points.some(p => p.metric === 'zones.1.running' && p.value === 0));
  assert.ok(points.some(p => p.module === 'lighting' && p.metric === 'channels.2.state' && p.value === 1));
  assert.ok(!points.some(p => p.module === 'wifi'));
});

test('stores periodic samples and records discrete state changes immediately', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sgh-analytics-'));
  const file = path.join(dir, 'telemetry.json');
  try {
    const store = new TelemetryStore(file, 100, 300000);
    assert.equal(store.record('board-a', { irrigation: { soilPercent: 40, pumpOn: false } }, 1000), true);
    assert.equal(store.record('board-a', { irrigation: { soilPercent: 42, pumpOn: false } }, 2000), false);
    assert.equal(store.record('board-a', { irrigation: { soilPercent: 42, pumpOn: true } }, 3000), true);
    const restored = new TelemetryStore(file, 100, 300000);
    restored.load();
    assert.equal(restored.points.length, 3);
    assert.equal(restored.points.at(-1).metric, 'pumpOn');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('creates explainable advisory insights without issuing commands', () => {
  const now = 50 * 60 * 1000;
  const points = [
    { deviceId: 'a', module: 'irrigation', metric: 'soilPercent', unit: '%', value: 20, ts: now - 20 * 60000 },
    { deviceId: 'a', module: 'irrigation', metric: 'soilPercent', unit: '%', value: 21, ts: now - 10 * 60000 },
    { deviceId: 'a', module: 'irrigation', metric: 'soilPercent', unit: '%', value: 22, ts: now }
  ];
  const result = buildInsights(points, [], { a: { online: false } }, now);
  assert.ok(result.insights.some(i => i.id === 'soil-consistently-low:a'));
  assert.ok(result.insights.some(i => i.id === 'device-offline:a'));
  assert.ok(result.insights.every(i => i.action === 'recommendation-only'));
  assert.equal(result.mode, 'advisory-only');
});

test('reports weak post-irrigation soil response only after three comparable cycles', () => {
  const minute = 60000;
  const points = [];
  const events = [];
  for (let cycle = 0; cycle < 3; cycle++) {
    const start = (cycle + 1) * 180 * minute;
    points.push(
      { deviceId: 'a', module: 'irrigation', metric: 'soilPercent', value: 40, ts: start - 5 * minute },
      { deviceId: 'a', module: 'irrigation', metric: 'soilPercent', value: 41, ts: start + 20 * minute }
    );
    events.push(
      { device_id: 'a', event_type: 'zone', state: 'start zone1 10min', received_at: start },
      { device_id: 'a', event_type: 'zone', state: 'stop zone1', received_at: start + 10 * minute }
    );
  }
  const result = buildInsights(points, events, {}, 1000 * minute);
  const insight = result.insights.find(i => i.id === 'irrigation-low-response:a:1');
  assert.ok(insight);
  assert.equal(insight.evidence.cycles, 3);
  assert.equal(insight.evidence.medianSoilDeltaPercent, 1);
});
