const { execFile } = require('node:child_process');
const net = require('node:net');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const { promisify } = require('node:util');

const pipeName = '\\\\.\\pipe\\SideLantern.Host.v1';
const serviceName = 'SideLanternService';
const unavailable = new Set(['ENOENT', 'ECONNREFUSED', 'EBUSY']);
const disconnected = 'SIDE_LANTERN_SERVICE_DISCONNECTED';
const execute = promisify(execFile);

async function startWindowsService() {
  if (process.platform !== 'win32') throw new Error('The SideLantern service requires Windows.');
  const sc = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'sc.exe');
  try {
    await execute(sc, ['start', serviceName], {windowsHide: true, timeout: 10000});
  } catch (error) {
    if (error.code !== 1056 && error.code !== 1061) throw new Error(`Unable to start the SideLantern service (${error.code}).`);
  }
}

function openConnection(connect, timeout) {
  return new Promise((resolve, reject) => {
    const socket = connect(pipeName);
    const timer = setTimeout(() => {
      socket.destroy(new Error('Timed out connecting to the SideLantern service.'));
    }, timeout);
    socket.once('error', error => {
      clearTimeout(timer);
      reject(error);
    });
    socket.once('connect', () => {
      clearTimeout(timer);
      resolve(socket);
    });
  });
}

async function startSession(socket, port, timeout) {
  let buffer = '';
  let started = false;
  let complete = false;
  let failure = disconnected;
  let stopping;
  const greeting = Promise.withResolvers();
  const exit = Promise.withResolvers();
  const startup = setTimeout(() => finish(disconnected, 'Timed out starting the SideLantern engine.'), timeout);

  function finish(code, message) {
    if (complete) return;
    complete = true;
    clearTimeout(startup);
    failure = code;
    if (!started) greeting.reject(new Error(message ?? `SideLantern service rejected the desktop (${code}).`));
    exit.resolve(code);
    socket.destroy();
  }

  socket.setEncoding('ascii');
  socket.on('data', chunk => {
    buffer += chunk;
    while (!complete) {
      const boundary = buffer.indexOf('\n');
      if (boundary < 0) break;
      const line = buffer.slice(0, boundary);
      buffer = buffer.slice(boundary + 1);
      if (!started && line === 'READY') {
        started = true;
        clearTimeout(startup);
        greeting.resolve();
      } else if (!started && /^ERROR [0-9]{1,10}$/.test(line)) {
        finish(Number(line.slice(6)));
      } else if (started && /^EXIT [0-9]{1,10}$/.test(line)) {
        finish(Number(line.slice(5)));
      } else {
        finish('SIDE_LANTERN_SERVICE_PROTOCOL', 'The SideLantern service returned an invalid response.');
      }
    }
    if (buffer.length > 32) finish('SIDE_LANTERN_SERVICE_PROTOCOL', 'The SideLantern service returned an invalid response.');
  });
  socket.on('error', () => finish(disconnected));
  socket.on('end', () => finish(disconnected));
  socket.on('close', () => finish(disconnected));
  socket.write(`OPEN ${port}\n`);
  await greeting.promise;
  return {
    exited: exit.promise,
    failure: () => failure,
    stop() {
      if (complete && !stopping) return Promise.resolve();
      if (stopping) return stopping;
      stopping = (async () => {
        socket.write('STOP\n');
        const timer = setTimeout(() => finish(disconnected), 18000);
        const code = await exit.promise;
        clearTimeout(timer);
        if (code !== 0) throw new Error(`SideLantern service shutdown was not confirmed (${code}).`);
      })();
      return stopping;
    }
  };
}

async function launchServiceEngine({
  port, timeout = 15000, connect = name => net.createConnection(name),
  startService = startWindowsService
}) {
  if (!Number.isInteger(port) || port < 1029 || port > 65514) throw new RangeError('Invalid SideLantern engine port.');
  const deadline = Date.now() + timeout;
  let socket;
  let retryStartAt = 0;
  while (!socket) {
    const remaining = deadline - Date.now();
    if (remaining <= 0) throw new Error('Timed out waiting for the SideLantern service.');
    try {
      socket = await openConnection(connect, remaining);
    } catch (error) {
      if (!unavailable.has(error.code)) throw error;
      if (Date.now() >= retryStartAt) {
        await startService();
        retryStartAt = Date.now() + 250;
      } else {
        await delay(Math.min(125, remaining));
      }
    }
  }
  return startSession(socket, port, Math.max(1, deadline - Date.now()));
}

module.exports = {launchServiceEngine};
