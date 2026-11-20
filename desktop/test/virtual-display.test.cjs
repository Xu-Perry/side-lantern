const assert = require('node:assert/strict');
const test = require('node:test');
const {createVirtualDisplay} = require('../lib/virtual-display.cjs');

const displayId = '{5eb52002-659f-5729-bdd8-9cdc4efd1bf5}';
const anotherId = '{4d845f49-cfdb-59e3-9da4-529ceb2a8293}';
const firstMode = {width: 1920, height: 1080, refreshRate: 60};
const secondMode = {width: 2560, height: 1440, refreshRate: 120};

test('a Windows-attached MttVDD monitor resolves to the current Sunshine device ID and supported modes', async () => {
  const adapter = createVirtualDisplay({
    probe: async () => ({
      detected: true,
      outputs: [{
        windowsName: String.raw`\\.\DISPLAY2`,
        mode: firstMode,
        modes: [secondMode, firstMode, secondMode, {width: 1, height: 1, refreshRate: 0}]
      }]
    }),
    getEngineOutputs: async () => [
      {device_id: anotherId, display_name: String.raw`\\.\DISPLAY1`},
      {device_id: displayId, display_name: String.raw`\\.\display2`}
    ]
  });

  const status = await adapter.getStatus();
  assert.equal(status.detected, true);
  assert.equal(status.enabled, true);
  assert.equal(status.available, false);
  assert.equal(status.controllable, false);
  assert.equal(status.outputId, displayId);
  assert.deepEqual(status.modes, [firstMode, secondMode]);
  assert.deepEqual(status.outputs[0].mode, firstMode);
});

test('a shared installed driver is detected even when Windows has no attached extended output', async () => {
  const adapter = createVirtualDisplay({
    probe: async () => ({detected: true, outputs: []}),
    getEngineOutputs: async () => [{device_id: displayId, display_name: ''}]
  });

  const status = await adapter.getStatus();
  assert.equal(status.detected, true);
  assert.equal(status.enabled, false);
  assert.equal(status.outputId, undefined);
  assert.deepEqual(status.modes, []);
});

test('legacy engine failures leave a shared output visible without guessing the device ID', async () => {
  const adapter = createVirtualDisplay({
    probe: async () => ({detected: true, outputs: [{windowsName: String.raw`\\.\DISPLAY2`, modes: [firstMode]}]}),
    getEngineOutputs: async () => {throw new Error('HTTP 404');}
  });

  const status = await adapter.getStatus();
  assert.equal(status.enabled, true);
  assert.equal(status.outputId, undefined);
  assert.deepEqual(status.modes, [firstMode]);
});

test('multiple shared displays have separate IDs and leave the automatic choice unset', async () => {
  const first = {windowsName: String.raw`\\.\DISPLAY2`, modes: [firstMode]};
  const second = {windowsName: String.raw`\\.\DISPLAY3`, modes: [secondMode]};
  const adapter = createVirtualDisplay({
    probe: async () => ({detected: true, outputs: [first, second]}),
    getEngineOutputs: async () => [
      {device_id: displayId, display_name: first.windowsName},
      {device_id: anotherId, display_name: second.windowsName}
    ]
  });

  const status = await adapter.getStatus();
  assert.equal(status.outputId, undefined);
  assert.deepEqual(status.outputs.map(output => output.outputId), [displayId, anotherId]);
});

test('apply and teardown never change the shared driver even when the monitor is present', async () => {
  let probes = 0;
  const adapter = createVirtualDisplay({
    probe: async () => {probes++; return {detected: true, outputs: []};}
  });

  await assert.rejects(adapter.apply({enabled: true, ...firstMode}), {code: 'DISPLAY_NOT_OWNED'});
  await assert.rejects(adapter.apply({enabled: false}), {code: 'DISPLAY_NOT_OWNED'});
  await adapter.teardown();
  assert.equal(probes, 0);
});

test('failed Windows enumeration reports unavailable rather than an invented output', async () => {
  const adapter = createVirtualDisplay({probe: async () => {throw new Error('probe failed');}});
  const status = await adapter.getStatus();
  assert.equal(status.enabled, false);
  assert.equal(status.detected, false);
  assert.equal(status.available, false);
  assert.deepEqual(status.outputs, []);
  assert.match(status.reason, /无法读取/);
});
