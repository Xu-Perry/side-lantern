const { fork } = require('node:child_process');
const dgram = require('node:dgram');
const { readFile } = require('node:fs/promises');
const net = require('node:net');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');

async function checkPort(port, udp) {
  const socket = udp ? dgram.createSocket('udp4') : net.createServer();
  try {
    await new Promise((resolve, reject) => {
      socket.once('error', reject);
      if (udp) socket.bind({port, address: '0.0.0.0', exclusive: true}, resolve);
      else socket.listen({port, host: '0.0.0.0', exclusive: true}, resolve);
    });
  } catch (error) {
    throw new Error(`测试端口 ${port}/${udp ? 'UDP' : 'TCP'} 无法使用（${error.code}）。`);
  } finally {
    try { socket.close(); } catch (error) {
      if (error.code !== 'ERR_SOCKET_DGRAM_NOT_RUNNING') throw error;
    }
  }
}

async function ensurePortsAvailable(base) {
  await checkPort(base + 1, false);
  await Promise.all([
    ...[-5, 0, 21].map(offset => checkPort(base + offset, false)),
    ...[9, 10, 11, 13].map(offset => checkPort(base + offset, true))
  ]);
}

function engineArguments(profile, port) {
  return [
    profile.config, `port=${port}`, 'origin_web_ui_allowed=pc', 'system_tray=disabled', 'upnp=disabled',
    'credentials_file=sl-management.json', 'file_state=sunshine_state.json',
    'file_apps=apps.json', 'pkey=credentials/cakey.pem', 'cert=credentials/cacert.pem', 'log_path=sunshine.log',
    'install_steam_audio_drivers=disabled', 'nvenc_opengl_vulkan_on_dxgi=disabled'
  ];
}

function launchEngine(profile, port, options = {}) {
  const worker = fork(path.join(__dirname, 'engine-worker.cjs'), [], {
    env: {...process.env, ELECTRON_RUN_AS_NODE: '1'},
    stdio: ['ignore', 'ignore', 'ignore', 'ipc'],
    execPath: options.execPath ?? process.execPath,
    detached: true, windowsHide: true
  });
  let engineExit;
  let workerError;
  worker.on('message', message => {
    if (message.type === 'error' || message.type === 'exit') engineExit = message;
  });
  const exited = new Promise(resolve => {
    worker.once('error', error => { workerError = error; resolve(); });
    worker.once('exit', resolve);
  });

  worker.send({
    executable: profile.executable,
    args: engineArguments(profile, port),
    cwd: profile.root,
    consoleLog: profile.consoleLog,
    terminationHelper: profile.terminationHelper,
    disableGlobalNvidiaPreferences: true
  }, error => { if (error) workerError = error; });

  return {
    exited,
    failure() {
      return workerError?.code ?? engineExit?.code ?? engineExit?.signal ?? '未知原因';
    },
    async stop() {
      if (worker.connected) worker.disconnect();
      const marker = Symbol('timeout');
      if (await Promise.race([exited, delay(15000, marker, {ref: false})]) === marker) {
        throw new Error('等待测试串流引擎退出超时。');
      }
    }
  };
}

async function waitForEngine({engine, profile, port, createClient, timeout = 20000}) {
  const expiredAt = Date.now() + timeout;
  let exited = false;
  engine.exited.then(() => { exited = true; });
  let lastError;
  while (Date.now() < expiredAt && !exited) {
    try {
      const certificate = await readFile(profile.certificate);
      const client = createClient({port: port + 1, username: profile.username, password: profile.password, certificate});
      const settings = await client.getSettings();
      if (!exited) return {client, settings};
    } catch (error) {
      lastError = error;
    }
    await Promise.race([delay(400), engine.exited]);
  }
  if (exited) throw new Error(`测试串流引擎已退出（${engine.failure()}）。请查看引擎日志。`);
  throw new Error(`测试串流引擎未就绪。请查看引擎日志。`, {cause: lastError});
}

module.exports = {engineArguments, ensurePortsAvailable, launchEngine, waitForEngine};
