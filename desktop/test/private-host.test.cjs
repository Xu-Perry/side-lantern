const assert = require('node:assert/strict');
const { fork } = require('node:child_process');
const { createHash } = require('node:crypto');
const { once } = require('node:events');
const fs = require('node:fs/promises');
const { createServer } = require('node:net');
const { tmpdir } = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { prepareProfile } = require('../lib/profile.cjs');
const { engineArguments, ensurePortsAvailable } = require('../lib/engine.cjs');

const storage = {
  encryptString: value => Buffer.from(value, 'utf8'),
  decryptString: value => value.toString('utf8')
};

test('stages a separate executable, config and working credentials without touching the source', async t => {
  const temp = await fs.mkdtemp(path.join(tmpdir(), 'side-lantern-test-'));
  t.after(() => fs.rm(temp, {recursive: true, force: true}));
  const source = path.join(temp, 'source');
  const target = path.join(temp, 'private');
  await fs.mkdir(path.join(source, 'assets'), {recursive: true});
  await fs.mkdir(path.join(source, 'config'));
  await fs.writeFile(path.join(source, 'sunshine.exe'), 'engine');
  await fs.writeFile(path.join(source, 'assets', 'apps.json'), '{"apps":[]}');
  await fs.writeFile(path.join(source, 'config', 'sunshine.conf'), 'original');
  await assert.rejects(prepareProfile({source, target: path.join(source, 'unwanted'), port: 48989, storage}), /独立/);
  await assert.rejects(fs.stat(path.join(source, 'unwanted')), {code: 'ENOENT'});
  const linkedTarget = path.join(temp, 'linked');
  await fs.mkdir(linkedTarget);
  await fs.symlink(path.join(source, 'config'), path.join(linkedTarget, 'config'), 'junction');
  await assert.rejects(prepareProfile({source, target: linkedTarget, port: 48989, storage}), /外部目录/);
  await assert.rejects(fs.stat(path.join(source, 'config', 'sl-management.json')), {code: 'ENOENT'});

  const inputs = {source, target, port: 48989, storage};
  const profile = await prepareProfile(inputs);
  const conf = await fs.readFile(profile.config, 'utf8');
  const auth = JSON.parse(await fs.readFile(profile.credentials, 'utf8'));
  assert.match(conf, /^port = 48989$/m);
  assert.match(conf, /^origin_web_ui_allowed = pc$/m);
  assert.match(conf, /^upnp = disabled$/m);
  assert.equal(auth.username, profile.username);
  assert.equal(auth.password, createHash('sha256').update(profile.password + auth.salt).digest().reverse().toString('hex').toUpperCase());
  assert.equal(await fs.readFile(profile.executable, 'utf8'), 'engine');
  await fs.appendFile(profile.config, 'unknown_option = keep\n');
  assert.equal((await prepareProfile(inputs)).password, profile.password);
  assert.match(await fs.readFile(profile.config, 'utf8'), /unknown_option = keep/);
  assert.equal(await fs.readFile(path.join(source, 'config', 'sunshine.conf'), 'utf8'), 'original');
  const installed = await prepareProfile({...inputs, target: path.join(temp, 'installed'), port: 47989, installed: true});
  assert.equal(installed.executable, path.join(source, 'sunshine.exe'));
  await assert.rejects(fs.access(path.join(installed.root, 'sunshine.exe')), {code: 'ENOENT'});
  assert.match(await fs.readFile(installed.config, 'utf8'), /^port = 47989$/m);
});

test('worker stops its own engine when the desktop IPC owner disappears', {timeout: 6000}, async t => {
  const temp = await fs.mkdtemp(path.join(tmpdir(), 'side-lantern-worker-'));
  t.after(() => fs.rm(temp, {recursive: true, force: true}));
  const worker = fork(path.join(__dirname, '../lib/engine-worker.cjs'), [], {stdio: ['ignore', 'ignore', 'ignore', 'ipc']});
  t.after(() => { if (worker.connected) worker.disconnect(); });
  const exit = once(worker, 'exit');
  const reply = once(worker, 'message');
  worker.send({ executable: process.execPath, args: ['-e', 'setInterval(() => {}, 1000)'],
    cwd: temp, consoleLog: path.join(temp, 'engine.log') });
  const [message] = await reply;
  assert.equal(message.type, 'started');
  assert.doesNotThrow(() => process.kill(message.pid, 0));

  worker.disconnect();
  await exit;
  assert.throws(() => process.kill(message.pid, 0), {code: 'ESRCH'});
});

test('engine startup pins writable paths to its private profile even if settings were changed on disk', () => {
  const config = 'E:\\dev\\host\\config\\sunshine.conf';
  assert.deepEqual(engineArguments({config}, 48989), [
    config, 'port=48989', 'origin_web_ui_allowed=pc', 'system_tray=disabled', 'upnp=disabled',
    'credentials_file=sl-management.json', 'file_state=sunshine_state.json',
    'file_apps=apps.json', 'pkey=credentials/cakey.pem', 'cert=credentials/cacert.pem', 'log_path=sunshine.log',
    'install_steam_audio_drivers=disabled', 'nvenc_opengl_vulkan_on_dxgi=disabled'
  ]);
});

test('port preflight rejects an existing local management listener without closing it', async t => {
  const listener = createServer();
  t.after(() => new Promise(resolve => listener.close(resolve)));
  listener.listen(0, '0.0.0.0');
  await once(listener, 'listening');
  const occupied = listener.address().port;
  await assert.rejects(ensurePortsAvailable(occupied - 1), new RegExp(`${occupied}/TCP`));
  assert.equal(listener.listening, true);
});
