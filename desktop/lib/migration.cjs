const {createHash, createPrivateKey, X509Certificate} = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');

const maxBackupBytes = 64 * 1024 * 1024;
const pngHeader = Buffer.from('89504e470d0a1a0a', 'hex');
const coverName = /^sl-import-[0-9a-f]{64}\.png$/;

async function read(file, max = 8 * 1024 * 1024) {
  const info = await fs.stat(file);
  if (!info.isFile() || info.size > max) throw new Error('迁移文件类型或大小不受支持。');
  const data = await fs.readFile(file);
  if (data.length > max) throw new Error('迁移文件超过大小限制。');
  return data;
}

async function optional(operation, fallback) {
  try {
    return await operation();
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    return fallback;
  }
}

function validateIdentity(cert, key) {
  try {
    if (new X509Certificate(cert).checkPrivateKey(createPrivateKey(key))) return;
  } catch {
    // The backup must contain a valid matching host identity.
  }
  throw new Error('Sunshine 主机证书与私钥不匹配或无效。');
}

function validateManifest(bundle) {
  if (bundle?.version !== 1 || !Array.isArray(bundle.apps?.apps) || !bundle.identity?.root?.uniqueid ||
      !Array.isArray(bundle.covers) || bundle.covers.some(cover => !coverName.test(cover?.name))) {
    throw new Error('备份格式或主机配对状态无效。');
  }
  validateIdentity(bundle.cert, bundle.key);
  return bundle;
}

async function readCover(source, filename) {
  if (typeof filename !== 'string' || !/\.png$/i.test(filename)) return null;
  const candidates = [path.resolve(path.dirname(source), filename), path.resolve(source, filename)];
  for (const candidate of new Set(candidates)) {
    const data = await optional(() => read(candidate, 4 * 1024 * 1024), null);
    if (data?.subarray(0, pngHeader.length).equals(pngHeader)) return data;
  }
  return null;
}

async function exportSunshineProfile({source, backup, storage}) {
  const directory = await fs.realpath(source);
  const defaults = {cert: 'credentials/cacert.pem', pkey: 'credentials/cakey.pem', file_state: 'sunshine_state.json', file_apps: 'apps.json'};
  const conf = await optional(() => read(path.join(directory, 'sunshine.conf')), Buffer.alloc(0));
  const locations = {...defaults};
  for (const line of conf.toString('utf8').split(/\r?\n/)) {
    const match = /^\s*(cert|pkey|file_state|file_apps)\s*=\s*([^#]+?)\s*(?:#.*)?$/.exec(line);
    if (match) locations[match[1]] = match[2];
  }
  const cert = (await read(path.resolve(directory, locations.cert))).toString('utf8');
  const key = (await read(path.resolve(directory, locations.pkey))).toString('utf8');
  validateIdentity(cert, key);
  const identity = JSON.parse(await read(path.resolve(directory, locations.file_state)));
  const apps = JSON.parse(await read(path.resolve(directory, locations.file_apps)));
  const covers = new Map();
  const warnings = [];
  let coverBytes = 0;
  const bundle = validateManifest({version: 1, cert, key, identity: {root: identity.root}, apps, covers: []});

  for (const entry of apps.apps) {
    if (!entry || typeof entry !== 'object' || Array.isArray(entry)) throw new Error('应用列表格式无效。');
    const image = entry['image-path'];
    if (image && !['desktop.png', 'steam.png'].includes(image)) {
      const data = await readCover(directory, image);
      if (data) {
        const name = `sl-import-${createHash('sha256').update(data).digest('hex')}.png`;
        if (!covers.has(name)) {
          coverBytes += data.length;
          if (coverBytes > 24 * 1024 * 1024) throw new Error('应用封面总大小超过迁移限制。');
          covers.set(name, data.toString('base64'));
        }
        entry['image-path'] = `config/covers/${name}`;
      } else {
        warnings.push(`${entry.name ?? '应用'}：未找到可迁移的封面`);
      }
    }
    for (const location of [entry['working-dir'], /^"([^"]+)"|^(\S+)/.exec(entry.cmd ?? '')?.slice(1).find(Boolean)]) {
      if (typeof location === 'string' && path.isAbsolute(location) && !await optional(() => fs.stat(location), null)) {
        warnings.push(`${entry.name ?? '应用'}：路径不存在（${location}）`);
      }
    }
  }

  bundle.covers = [...covers].map(([name, data]) => ({name, data}));
  const encrypted = await storage.encryptString(JSON.stringify(bundle));
  if (encrypted.length > maxBackupBytes) throw new Error('迁移备份超过大小限制。');
  await fs.writeFile(backup, encrypted, {flag: 'wx', mode: 0o600});
  return {applications: apps.apps.length, covers: covers.size, warnings};
}

async function restoreSunshineProfile({backup, target, storage}) {
  const directory = path.resolve(target);
  if (await optional(() => fs.lstat(directory), null)) throw new Error('迁移需要尚不存在的目标目录。');
  const bundle = validateManifest(JSON.parse(await storage.decryptString(await read(backup, maxBackupBytes))));
  const parent = await fs.realpath(path.dirname(directory));
  const destination = path.join(parent, path.basename(directory));
  const staging = await fs.mkdtemp(path.join(parent, '.side-lantern-import-'));
  try {
    const credentials = path.join(staging, 'credentials');
    await fs.mkdir(credentials);
    await fs.writeFile(path.join(credentials, 'cacert.pem'), bundle.cert);
    await fs.writeFile(path.join(credentials, 'cakey.pem'), bundle.key, {mode: 0o600});
    await fs.writeFile(path.join(staging, 'sunshine_state.json'), JSON.stringify({root: bundle.identity.root}));
    const importedCoverPaths = new Set(bundle.covers.map(cover => `config/covers/${cover.name}`));
    for (const entry of bundle.apps.apps) {
      if (!entry || typeof entry !== 'object' || Array.isArray(entry)) throw new Error('备份中的应用列表格式无效。');
      if (importedCoverPaths.has(entry['image-path'])) {
        entry['image-path'] = path.join(destination, 'covers', path.basename(entry['image-path']));
      }
    }
    await fs.writeFile(path.join(staging, 'apps.json'), JSON.stringify(bundle.apps, null, 2));
    if (bundle.covers.length) {
      await fs.mkdir(path.join(staging, 'covers'));
      for (const cover of bundle.covers) {
        const bytes = Buffer.from(cover.data, 'base64');
        if (bytes.length > 4 * 1024 * 1024 || !bytes.subarray(0, pngHeader.length).equals(pngHeader)) {
          throw new Error('备份包含无效的应用封面。');
        }
        await fs.writeFile(path.join(staging, 'covers', cover.name), bytes, {flag: 'wx'});
      }
    }
    if (await optional(() => fs.lstat(directory), null)) throw new Error('迁移需要尚不存在的目标目录。');
    await fs.rename(staging, destination);
  } finally {
    await fs.rm(staging, {recursive: true, force: true});
  }
}

module.exports = {exportSunshineProfile, restoreSunshineProfile};
