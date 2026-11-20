const assert = require('node:assert/strict');
const { Duplex } = require('node:stream');
const test = require('node:test');
const { launchServiceEngine } = require('../lib/windows-service.cjs');

function desktopConnection(onCommand) {
  const socket = new Duplex({
    read() {},
    write(data, _, callback) {
      onCommand(data.toString(), socket);
      callback();
    }
  });
  queueMicrotask(() => socket.emit('connect'));
  return socket;
}

function missingConnection() {
  const socket = new Duplex({read() {}, write(_, __, next) { next(); }});
  queueMicrotask(() => socket.destroy(Object.assign(new Error('missing pipe'), {code: 'ENOENT'})));
  return socket;
}

test('starts the service on demand, validates greeting, then waits for confirmed stop', async () => {
  let starts = 0;
  let attempts = 0;
  let stops = 0;
  const engine = await launchServiceEngine({
    port: 47989,
    connect(name) {
      assert.equal(name, String.raw`\\.\pipe\SideLantern.Host.v1`);
      if (++attempts === 1) return missingConnection();
      return desktopConnection((line, socket) => {
        if (line === 'OPEN 47989\n') {
          socket.push(Buffer.from('REA'));
          socket.push(Buffer.from('DY\n'));
        } else if (line === 'STOP\n') {
          stops++;
          queueMicrotask(() => {
            socket.push(Buffer.from('EXIT 0\n'));
            socket.push(null);
          });
        } else assert.fail(line);
      });
    },
    startService: async () => { starts++; }
  });
  await Promise.all([engine.stop(), engine.stop()]);
  assert.equal(await engine.exited, 0);
  assert.equal(engine.failure(), 0);
  assert.equal(starts, 1);
  assert.equal(stops, 1);
});

test('retries demand start after a previous service is still stopping', async () => {
  let attempts = 0;
  const engine = await launchServiceEngine({
    port: 47989,
    timeout: 700,
    connect: () => attempts < 2 ? missingConnection() : desktopConnection((line, socket) => {
      if (line.startsWith('OPEN')) socket.push(Buffer.from('READY\n'));
      else socket.push(Buffer.from('EXIT 0\n'));
    }),
    startService: async () => { attempts++; }
  });
  assert.equal(attempts, 2);
  await engine.stop();
});

test('rejects malformed commands and fails closed on a rejected or disconnected service', async () => {
  await assert.rejects(launchServiceEngine({port: '47989\nSTOP'}), /port/);
  await assert.rejects(launchServiceEngine({
    port: 47989,
    connect: () => desktopConnection((_, socket) => socket.push(Buffer.from('ERROR 5\n')))
  }), /5/);
  const engine = await launchServiceEngine({
    port: 47989,
    connect: () => desktopConnection((line, socket) => {
      if (line.startsWith('OPEN')) socket.push(Buffer.from('READY\n'));
      else socket.push(null);
    })
  });
  await assert.rejects(engine.stop(), /shutdown/);
  assert.equal(engine.failure(), 'SIDE_LANTERN_SERVICE_DISCONNECTED');
});
