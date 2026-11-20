const assert = require('node:assert/strict');
const test = require('node:test');
const {createHostController} = require('../lib/host-controller.cjs');

const settings = {encoder: 'auto', maxBitrateMbps: 0, hevc: 'auto', av1: 'auto', audio: true};

test('serializes start and stop, and applies settings by restarting only the owned host', async () => {
  const actions = [];
  const client = {
    getDevices: async () => [{id: 'one', name: 'Moonlight', enabled: true}],
    getPairings: async () => null,
    saveSettings: async value => {actions.push(['save', value]); return value;}
  };
  const host = createHostController({open: async () => {
    actions.push('start');
    return {client, settings, engine: {exited: new Promise(() => {}),
      stop: async () => {actions.push('stop');}}};
  }});
  await Promise.all([host.start(), host.start()]);
  assert.deepEqual(actions, ['start']);
  assert.equal(host.getState().legacyPairing, true);
  assert.equal(host.getState().devices.length, 1);
  await host.saveSettings(settings);
  assert.deepEqual(actions, ['start', ['save', settings], 'stop', 'start']);
  await host.stop();
  assert.equal(host.getState().phase, 'stopped');
  assert.equal(host.getState().devices.length, 1);
  await assert.rejects(host.approvePairing({id: null, pin: '1234'}), /启动/);
});

test('failed or unexpected engine exit is visible and supports a retry', async () => {
  let attempts = 0;
  const {promise, resolve} = Promise.withResolvers();
  const host = createHostController({open: async () => {
    if (++attempts === 1) throw new Error('端口冲突');
    return {settings, client: {getDevices: async () => [], getPairings: async () => []},
      engine: {exited: promise, failure: () => 1}};
  }});
  await assert.rejects(host.start(), /端口冲突/);
  assert.equal(host.getState().phase, 'error');
  await host.start();
  assert.equal(host.getState().phase, 'running');
  resolve();
  await promise;
  assert.equal(host.getState().phase, 'error');
  assert.match(host.getState().error, /意外停止/);
});

test('source selection preserves active streams and applies after the host is idle', async () => {
  const outputId = '{7419eb27-6ee2-47e1-8644-d0f8418fe92d}';
  let sessions = [{deviceId: 'one', name: 'Moonlight', source: 'display'}];
  let selected = '';
  let stops = 0;
  const client = {
    getDevices: async () => [], getPairings: async () => [], getSessions: async () => sessions,
    saveSettings: async (_settings, output) => {selected = output;}
  };
  const host = createHostController({open: async () => ({
    client, settings, outputId: selected, engine: {exited: new Promise(() => {}),
      stop: async () => {stops++;}}
  })});
  await host.start();
  assert.equal(host.getState().sessions.length, 1);
  await assert.rejects(host.saveSettings(settings, outputId), /结束当前串流/);
  assert.equal(stops, 0);
  sessions = [];
  assert.equal((await host.saveSettings(settings, outputId)).outputId, outputId);
  assert.equal(stops, 1);
});
