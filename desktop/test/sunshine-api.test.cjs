const assert = require('node:assert/strict');
const https = require('node:https');
const { once } = require('node:events');
const { test } = require('node:test');
const { createSunshineClient } = require('../lib/sunshine-api.cjs');

const {issued, other} = require('./fixtures/identities.cjs');

const requestId = '0123456789abcdef0123456789abcdef';
const deviceId = '7419eb27-6ee2-47e1-8644-d0f8418fe92d';
const defaults = { encoder: 'auto', maxBitrateMbps: 0, hevc: 'auto', av1: 'auto', audio: true };

async function fixture(t, respond, identity = issued) {
  const requests = [];
  const server = https.createServer(identity, async (request, response) => {
    try {
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      const payload = Buffer.concat(chunks).toString();
      const recorded = { method: request.method, url: request.url, headers: request.headers,
        body: payload ? JSON.parse(payload) : undefined };
      requests.push(recorded);
      const [statusCode, result, headers = {}] = respond(recorded);
      response.writeHead(statusCode, { 'content-type': 'application/json', ...headers });
      response.end(JSON.stringify(result));
    } catch {
      response.writeHead(500);
      response.end();
    }
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  t.after(() => new Promise(resolve => server.close(resolve)));
  const client = createSunshineClient({
    port: server.address().port, username: 'side-lantern', password: 'synthetic-test-password', certificate: issued.cert,
  });
  return { client, requests };
}

test('authenticates only the pinned endpoint and sends no credentials for ping', async (t) => {
  const { client, requests } = await fixture(t, () => [200, { status: true, locale: 'en' }]);
  assert.equal(await client.ping(), true);
  assert.deepEqual((await client.getSettings()), defaults);
  assert.equal(requests[0].url, '/api/configLocale');
  assert.equal(requests[0].headers.authorization, undefined);
  assert.equal(requests[1].headers.authorization, `Basic ${Buffer.from('side-lantern:synthetic-test-password').toString('base64')}`);
});

test('rejects a different certificate even with the same common name without sending credentials', async (t) => {
  const { client, requests } = await fixture(t, () => [200, {}], other);
  await assert.rejects(client.getSettings(), /证书/);
  assert.deepEqual(requests, []);
});

test('does not follow redirects', async (t) => {
  const { client, requests } = await fixture(t, () => [302, {}, { location: '/another-route' }]);
  await assert.rejects(client.getSettings(), /302/);
  assert.deepEqual(requests.map(request => request.url), ['/api/config']);
});

test('writes owned settings without metadata or losing advanced configuration', async (t) => {
  const existing = {
    status: true, platform: 'windows', version: 'test', encoder: 'nvenc',
    max_bitrate: '22500', hevc_mode: '1', av1_mode: '3', stream_audio: 'disabled',
    port: '48989', output_name: '{other-screen}', nvenc_preset: 'p3',
  };
  const { client, requests } = await fixture(t, ({ method }) => [200, method === 'GET' ? existing : { status: true }]);
  assert.deepEqual(await client.getSettings(), { encoder: 'nvenc', maxBitrateMbps: 22.5,
    hevc: 'disabled', av1: 'auto', audio: false });
  const selected = { ...defaults, encoder: 'software', maxBitrateMbps: 42.125, av1: 'disabled' };
  assert.deepEqual(await client.saveSettings(selected), selected);
  assert.deepEqual(requests.at(-1).body, {
    port: '48989', output_name: '{other-screen}', nvenc_preset: 'p3',
    encoder: 'software', max_bitrate: 42125, av1_mode: 1,
  });
});

test('handles scoped pairing, device UUIDs and unsuccessful actions', async (t) => {
  const { client, requests } = await fixture(t, ({ method, url }) => {
    if (url === '/api/clients/list') return [200, {status: true, named_certs: [
      { uuid: deviceId, name: 'Moonlight', enabled: false },
    ]}];
    if (method === 'GET') return [200, { pairings: [{ id: requestId, name: 'iPad', address: '192.168.1.12' }] }];
    return [200, { status: method !== 'DELETE' }];
  });
  assert.deepEqual(await client.getDevices(), [{ id: deviceId, name: 'Moonlight', enabled: false }]);
  assert.deepEqual(await client.getPairings(), [{ id: requestId, name: 'iPad', address: '192.168.1.12' }]);
  await assert.rejects(client.approvePairing({ id: 'invalid', pin: '0123', name: 'iPad' }), /配对请求/);
  await assert.rejects(client.approvePairing({ id: requestId, pin: '12345', name: 'iPad' }), /PIN/);
  assert.equal(requests.length, 2);
  await client.approvePairing({ id: requestId, pin: '0123', name: 'iPad' });
  assert.deepEqual(requests.at(-1).body, { pairing_id: requestId, pin: '0123', name: 'iPad' });
  await assert.rejects(client.cancelPairing(requestId), /取消配对/);
  assert.deepEqual(requests.at(-1).body, { pairing_id: requestId });
  await client.unpair(deviceId);
  assert.deepEqual(requests.at(-1).body, { uuid: deviceId });
});

test('allows manual PIN approval only after the legacy endpoint is detected', async (t) => {
  const { client, requests } = await fixture(t, ({ method }) => method === 'GET' ? [404, {}] : [200, { status: true }]);
  await assert.rejects(client.approvePairing({ id: null, pin: '0123', name: 'iPad' }), /配对请求/);
  assert.equal(requests.length, 0);
  assert.equal(await client.getPairings(), null);
  await client.approvePairing({ id: null, pin: '0123', name: 'iPad' });
  assert.deepEqual(requests.at(-1).body, { pin: '0123', name: 'iPad' });
});

test('reads authenticated live sessions and distinguishes an older host from idle', async (t) => {
  const active = await fixture(t, () => [200, {status: true, sessions: [
    {device_id: deviceId, name: 'Tablet', source: null},
    {device_id: 'untrusted-id', name: 'Invalid'},
  ]}]);
  assert.deepEqual(await active.client.getSessions(), [{deviceId, name: 'Tablet', source: null}]);
  assert.equal(active.requests[0].url, '/api/sessions');
  assert.match(active.requests[0].headers.authorization, /^Basic /);
  const legacy = await fixture(t, () => [404, {}]);
  assert.equal(await legacy.client.getSessions(), null);
  const idle = await fixture(t, () => [200, {status: true, sessions: []}]);
  assert.deepEqual(await idle.client.getSessions(), []);
});

test('updates an engine-resolved display ID while preserving unrelated configuration', async (t) => {
  const outputId = `{${deviceId}}`;
  const displays = [{device_id: outputId, display_name: '\\\\.\\DISPLAY3'}];
  const {client, requests} = await fixture(t, ({url, method}) => {
    if (url === '/api/displays') return [200, {status: true, displays}];
    if (method === 'POST') return [200, {status: true}];
    return [200, {status: true, output_name: outputId, nvenc_preset: 'p3'}];
  });
  assert.deepEqual(await client.getDisplays(), displays);
  assert.equal(await client.getOutput(), outputId);
  await assert.rejects(client.saveSettings(defaults, '\\\\.\\DISPLAY3'), /标识无效/);
  await client.saveSettings(defaults, '');
  assert.deepEqual(requests.at(-1).body, {nvenc_preset: 'p3'});
  await client.saveSettings(defaults, outputId);
  assert.deepEqual(requests.at(-1).body, {nvenc_preset: 'p3', output_name: outputId});
});
