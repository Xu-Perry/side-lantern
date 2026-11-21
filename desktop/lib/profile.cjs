const { createHash, randomBytes } = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');

async function resolveDirectory(directory) {
  try {
    return await fs.realpath(directory);
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    return path.join(await resolveDirectory(path.dirname(directory)), path.basename(directory));
  }
}

function containsPath(parent, child) {
  const relative = path.relative(parent, child);
  return !relative || (!path.isAbsolute(relative) && relative !== '..' && !relative.startsWith(`..${path.sep}`));
}

async function createIfMissing(file, contents) {
  try {
    await fs.writeFile(file, contents, {flag: 'wx', mode: 0o600});
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
  }
}

async function migrateDefaultHostname(config) {
  const current = await fs.readFile(config, 'utf8');
  const updated = current.replace(/^sunshine_name = SideLantern(?:\r?\n|$)/gm, '');
  if (updated === current) return;

  // Keep the engine's hostname fallback and preserve the rest of the profile.
  const temporaryFile = `${config}.${randomBytes(8).toString('hex')}.tmp`;
  await fs.writeFile(temporaryFile, updated, {flag: 'wx', mode: 0o600});
  try {
    await fs.rename(temporaryFile, config);
  } finally {
    await fs.rm(temporaryFile, {force: true});
  }
}

async function prepareProfile({source, target, port, storage, installed = false}) {
  if (installed ? port !== 47989 : !Number.isInteger(port) || port < 1029 || port > 65514 || Math.abs(port - 47989) < 32) {
    throw new Error('测试实例需要一组与 Sunshine 默认端口独立的端口。');
  }
  const sourceDirectory = await fs.realpath(path.resolve(source));
  const root = await resolveDirectory(path.resolve(target));
  if (containsPath(sourceDirectory, root) || containsPath(root, sourceDirectory)) {
    throw new Error('SideLantern 测试配置必须位于独立目录。');
  }
  const executable = path.join(installed ? sourceDirectory : root, 'sunshine.exe');
  const configDirectory = path.join(root, 'config');
  const config = path.join(configDirectory, 'sunshine.conf');
  const credentials = path.join(configDirectory, 'sl-management.json');
  const certificate = path.join(configDirectory, 'credentials', 'cacert.pem');
  const logDirectory = path.join(root, 'logs');
  const consoleLog = path.join(logDirectory, 'engine-console.log');
  const secretFile = path.join(root, 'management.dpapi');
  const terminationHelper = installed ? null : path.join(root, 'side-lantern-shutdown.exe');
  const username = 'side-lantern';

  await fs.access(path.join(sourceDirectory, 'sunshine.exe'));
  const profileFiles = [
    config, credentials, certificate, consoleLog, secretFile, path.join(root, 'assets'),
    path.join(configDirectory, 'credentials', 'cakey.pem'),
    ...['apps.json', 'sunshine_state.json', 'sunshine.log'].map(name => path.join(configDirectory, name))
  ];
  const binaries = installed ? [] : (await fs.readdir(sourceDirectory, {withFileTypes: true}))
    .filter(entry => entry.isFile() && /^(sunshine\.exe|.+\.dll)$/i.test(entry.name));
  profileFiles.push(...binaries.map(binary => path.join(root, binary.name)));
  if (terminationHelper) profileFiles.push(terminationHelper);
  for (const entry of profileFiles) {
    if (!containsPath(root, await resolveDirectory(entry))) throw new Error('测试配置包含指向外部目录的路径。');
  }
  await fs.mkdir(configDirectory, {recursive: true});
  await fs.mkdir(logDirectory, {recursive: true});
  for (const binary of binaries) {
    await fs.copyFile(path.join(sourceDirectory, binary.name), path.join(root, binary.name));
  }
  if (!installed) {
    await fs.cp(path.join(sourceDirectory, 'assets'), path.join(root, 'assets'), {
      recursive: true, force: false, dereference: true
    });
  }

  let encrypted;
  try {
    encrypted = await fs.readFile(secretFile);
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    await createIfMissing(secretFile, await storage.encryptString(randomBytes(32).toString('base64url')));
    encrypted = await fs.readFile(secretFile);
  }
  const password = await storage.decryptString(encrypted);
  const salt = randomBytes(16).toString('hex');
  const hash = createHash('sha256').update(password + salt).digest().reverse().toString('hex').toUpperCase();
  await createIfMissing(credentials, JSON.stringify({username, salt, password: hash}));
  await createIfMissing(config, [
    `port = ${port}`,
    'origin_web_ui_allowed = pc',
    'credentials_file = sl-management.json',
    'system_tray = disabled',
    'gamepad_driver = vigembus',
    'upnp = disabled',
    ''
  ].join('\n'));
  await migrateDefaultHostname(config);
  return {root, executable, config, credentials, certificate, logDirectory, consoleLog, terminationHelper, username, password};
}

module.exports = {prepareProfile, migrateDefaultHostname};
