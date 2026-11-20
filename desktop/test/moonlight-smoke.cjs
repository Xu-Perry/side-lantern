const assert = require('node:assert/strict');
const { execFile, spawn } = require('node:child_process');
const { createCipheriv, createDecipheriv, randomBytes, randomInt } = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const { promisify } = require('node:util');
const { prepareProfile } = require('../lib/profile.cjs');
const { ensurePortsAvailable, launchEngine, waitForEngine } = require('../lib/engine.cjs');
const { createSunshineClient } = require('../lib/sunshine-api.cjs');

const execute = promisify(execFile);
const port = 49189;
const address = `127.0.0.1:${port}`;

async function until(check, message, timeout = 20000) {
  const deadline = Date.now() + timeout;
  do {
    const result = await check();
    if (result) return result;
    await delay(300);
  } while (Date.now() < deadline);
  throw new Error(message);
}

function openMoonlight(executable, cwd, args) {
  const process = spawn(executable, args, {
    cwd, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']
  });
  let output = '';
  for (const stream of [process.stdout, process.stderr]) stream.on('data', part => { output += part.toString(); });
  const stopped = new Promise(resolve => {
    process.on('error', error => {
      output += `\n${error.message}`;
      resolve(-1);
    });
    process.on('exit', resolve);
  });
  return {
    process, output: () => output,
    async close() {
      if (process.exitCode === null && process.signalCode === null) process.kill();
      await Promise.race([stopped, delay(5000, undefined, {ref: false})]);
    }
  };
}

async function pair(client, moonlight, directory, name) {
  await fs.mkdir(directory);
  await fs.writeFile(path.join(directory, 'portable.dat'), '');
  const portableSettings = path.join(directory, 'Moonlight Game Streaming Project', 'Moonlight.ini');
  const pin = String(randomInt(1000, 10000));
  const instance = openMoonlight(moonlight, directory, ['pair', address, '--pin', pin]);
  try {
    await until(async () => {
      if (instance.process.exitCode !== null) throw new Error(`Moonlight pairing closed:\n${instance.output()}`);
      const pending = await client.getPairings();
      try {
        if (pending === null) await client.approvePairing({id: null, pin, name});
        else if (pending[0]) await client.approvePairing({id: pending[0].id, pin, name});
        else return false;
      } catch (error) {
        if (!/配对未完成/.test(error.message)) throw error;
        return false;
      }
      return true;
    }, 'Moonlight did not receive pairing approval.');
    const device = await until(async () => (await client.getDevices()).find(entry => entry.name === name),
      'Paired device did not appear in Sunshine.');
    await until(async () => {
      try {
        const saved = await fs.readFile(portableSettings, 'utf8');
        const encoded = /^\d+\\srvcert=.*-----BEGIN CERTIFICATE-----([^\r\n]+)-----END CERTIFICATE-----/m.exec(saved)?.[1];
        // Qt can serialize an empty certificate before the pairing handshake finishes.
        return Boolean(encoded?.replaceAll(/\\[rn]/g, '').trim());
      } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        return false;
      }
    }, `Moonlight did not save the host certificate for ${name}.`);
    console.log(`Paired ${name} using original Moonlight.`);
    return device;
  } finally {
    await instance.close();
    await fs.writeFile(path.join(directory, 'pair.log'), instance.output());
  }
}

async function run() {
  const [source, clientPath, outputDirectory] = process.argv.slice(2);
  if (!source || !clientPath || !outputDirectory || process.platform !== 'win32') {
    throw new Error('Usage on Windows: node moonlight-smoke.cjs <Sunshine dir> <Moonlight.exe> <output dir>');
  }
  const moonlight = await fs.realpath(clientPath);
  await fs.mkdir(outputDirectory, {recursive: true});
  const runDirectory = await fs.mkdtemp(path.join(path.resolve(outputDirectory), 'moonlight-'));
  await ensurePortsAvailable(port);

  const secretKey = randomBytes(32);
  const testStorage = {
    encryptString(value) {
      const iv = randomBytes(12);
      const cipher = createCipheriv('aes-256-gcm', secretKey, iv);
      const bytes = Buffer.concat([cipher.update(value, 'utf8'), cipher.final()]);
      return Buffer.concat([iv, cipher.getAuthTag(), bytes]);
    },
    decryptString(value) {
      const cipher = createDecipheriv('aes-256-gcm', secretKey, value.subarray(0, 12));
      cipher.setAuthTag(value.subarray(12, 28));
      return Buffer.concat([cipher.update(value.subarray(28)), cipher.final()]).toString('utf8');
    }
  };
  const profile = await prepareProfile({source, target: path.join(runDirectory, 'host'), port, storage: testStorage});
  if (process.env.SIDELANTERN_SMOKE_DEBUG === '1') await fs.appendFile(profile.config, 'min_log_level = debug\n');
  const compiler = path.join(process.env.SystemRoot, 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
  await execute(compiler, ['/nologo', '/target:winexe', `/out:${profile.terminationHelper}`,
    path.join(__dirname, '../native/ShutdownHost.cs')], {windowsHide: true});
  const engine = launchEngine(profile, port);
  const streams = [];
  try {
    const {client} = await waitForEngine({engine, profile, port, createClient: createSunshineClient});
    const pairs = [];
    for (const index of [1, 2]) {
      const directory = path.join(runDirectory, `client-${index}`);
      pairs.push({directory, device: await pair(client, moonlight, directory, `Moonlight smoke ${index}`)});
    }
    for (const pair of pairs) {
      const stream = openMoonlight(moonlight, pair.directory, [
        'stream', address, 'Desktop', '--720', '--fps', '30', '--bitrate', '2500',
        '--display-mode', 'windowed', '--audio-on-host', '--no-game-optimization',
        '--video-decoder', 'software'
      ]);
      streams.push(stream);
      try {
        const evidence = await until(() => /[^\r\n]*(?:first video packet|first video frame)[^\r\n]*/i.exec(stream.output())?.[0],
          'Moonlight did not report receiving desktop video.');
        console.log(evidence);
      } catch (error) {
        throw new Error(`${error.message}\n${stream.output().split('\n').slice(-28).join('\n')}`);
      }
    }
    assert.equal(streams.every(stream => stream.process.exitCode === null && !stream.process.signalCode), true);
    console.log('Two original Moonlight clients received desktop video concurrently over loopback.');
    const ids = pairs.map(({device}) => device.id).sort();
    const active = await until(async () => {
      const sessions = await client.getSessions();
      return sessions?.length === 2 && sessions.every(session => session.source) ? sessions : false;
    }, 'The engine did not report both active devices with capture sources.');
    assert.deepEqual(active.map(session => session.deviceId).sort(), ids);
    await client.unpair(pairs[0].device.id);
    await until(async () => {
      const sessions = await client.getSessions();
      return sessions?.length === 1 && sessions[0].deviceId === pairs[1].device.id;
    }, 'Revoking the first device did not leave exactly the second session active.');
    console.log('The active-session API reported capture sources and preserved the remaining session after unpair.');
  } finally {
    await Promise.allSettled(streams.map(stream => stream.close()));
    try {
      await engine.stop();
    } finally {
      await Promise.all(streams.map((stream, index) => fs.writeFile(path.join(runDirectory, `client-${index + 1}.log`), stream.output())));
    }
  }
}

run().catch(error => { console.error(error.message); process.exitCode = 1; });
