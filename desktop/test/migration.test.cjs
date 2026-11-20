const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {issued, other} = require('./fixtures/identities.cjs');
const {exportSunshineProfile, restoreSunshineProfile} = require('../lib/migration.cjs');

const storage = {
  encryptString: text => Buffer.from(text),
  decryptString: bytes => bytes.toString('utf8')
};
const paired = {uniqueid: 'test-host-uuid', named_devices: [{name: 'Tablet', cert: other.cert}]};
const apps = {env: {}, apps: [{name: 'Custom Desktop', 'image-path': 'config/covers/example.png'}]};
const fakeCover = Buffer.from('89504e470d0a1a0aacab', 'hex');

test('migration preserves host identity, pairings and covers while leaving credentials out', async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'sl-migration-test-'));
  t.after(() => fs.rm(root, {recursive: true, force: true}));
  const source = path.join(root, 'sunshine', 'config');
  const backup = path.join(root, 'backup.slbak');
  const target = path.join(root, 'restored');
  await fs.mkdir(path.join(source, 'credentials'), {recursive: true});
  await fs.mkdir(path.join(source, 'covers'));
  await fs.writeFile(path.join(source, 'credentials/cacert.pem'), issued.cert);
  await fs.writeFile(path.join(source, 'credentials/cakey.pem'), issued.key);
  await fs.writeFile(path.join(source, 'covers/example.png'), fakeCover);
  await fs.writeFile(path.join(source, 'sunshine.conf'), 'max_bitrate = 35\n');
  await fs.writeFile(path.join(source, 'sunshine_state.json'), JSON.stringify({root: paired, username: 'old-user', password: 'old-credentials'}));
  await fs.writeFile(path.join(source, 'apps.json'), JSON.stringify(apps));
  assert.deepEqual(await exportSunshineProfile({source, backup, storage}), {applications: 1, covers: 1, warnings: []});
  assert.doesNotMatch(await fs.readFile(backup, 'utf8'), /old-credentials|old-user|max_bitrate/);
  await restoreSunshineProfile({backup, target, storage});
  assert.deepEqual(JSON.parse(await fs.readFile(path.join(target, 'sunshine_state.json'), 'utf8')), {root: paired});
  assert.equal(await fs.readFile(path.join(target, 'credentials/cakey.pem'), 'utf8'), issued.key);
  const restored = JSON.parse(await fs.readFile(path.join(target, 'apps.json'), 'utf8'));
  assert.equal(path.dirname(restored.apps[0]['image-path']), path.join(target, 'covers'));
  assert.match(path.basename(restored.apps[0]['image-path']), /^sl-import-[0-9a-f]+\.png$/);
  assert.deepEqual(await fs.readFile(path.join(target, 'covers', path.basename(restored.apps[0]['image-path']))), fakeCover);
  await assert.rejects(restoreSunshineProfile({backup, target, storage}), /不存在的目标目录/);
  assert.equal(await fs.readFile(path.join(source, 'sunshine.conf'), 'utf8'), 'max_bitrate = 35\n');
});

test('a mismatched key aborts before writing a backup', async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'sl-migration-invalid-'));
  t.after(() => fs.rm(root, {recursive: true, force: true}));
  await fs.mkdir(path.join(root, 'credentials'));
  await fs.writeFile(path.join(root, 'credentials/cacert.pem'), issued.cert);
  await fs.writeFile(path.join(root, 'credentials/cakey.pem'), other.key);
  const backup = path.join(root, 'backup.slbak');
  await assert.rejects(exportSunshineProfile({source: root, backup, storage}), /证书与私钥不匹配/);
  await assert.rejects(fs.access(backup), {code: 'ENOENT'});
});
