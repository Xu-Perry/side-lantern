const { app, BrowserWindow, Menu, nativeImage, nativeTheme, safeStorage, shell, Tray, ipcMain } = require('electron');
const { execFile } = require('node:child_process');
const { mkdirSync } = require('node:fs');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { promisify } = require('node:util');
const { createHostController } = require('./lib/host-controller.cjs');
const { prepareProfile } = require('./lib/profile.cjs');
const { ensurePortsAvailable, launchEngine, waitForEngine } = require('./lib/engine.cjs');
const { createSunshineClient } = require('./lib/sunshine-api.cjs');
const { createVirtualDisplay } = require('./lib/virtual-display.cjs');
const { exportSunshineProfile, restoreSunshineProfile } = require('./lib/migration.cjs');
const { windowsBackupStorage } = require('./lib/windows-backup-storage.cjs');
const { languageFromWindows, validatePreferences } = require('./lib/preferences.cjs');
const { translate } = require('./renderer/i18n.js');

const installed = app.isPackaged;
const port = installed ? 47989 : Number(process.env.SIDELANTERN_TEST_PORT ?? 48989);
const rendererFile = path.join(__dirname, 'renderer', 'index.html');
const defaultPreferences = {theme: 'system', launchAtLogin: false};
const smoke = process.argv.includes('--smoke-test');
const execFileAsync = promisify(execFile);
let preferences = defaultPreferences;
let window;
let tray;
let quitJob;
let quitAllowed = false;
let timer;
let display = {available: false, detected: false, enabled: false};
let nextDisplayRefresh = 0;

app.setName('SideLantern');
app.setAppUserModelId('com.sidelantern.desktop');
if (installed && process.platform === 'win32') {
  const localProfile = path.join(process.env.LOCALAPPDATA ?? app.getPath('appData'), 'SideLantern');
  mkdirSync(localProfile, {recursive: true});
  app.setPath('userData', localProfile);
} else if (process.env.SIDELANTERN_DATA_DIR) {
  app.setPath('userData', path.resolve(process.env.SIDELANTERN_DATA_DIR));
}

const dataDir = app.getPath('userData');
const privateHost = path.join(dataDir, 'host');
const preferencesFile = path.join(dataDir, 'preferences.json');

const host = createHostController({open: openPrivateHost, onChange: publish});
const virtualDisplay = createVirtualDisplay({
  dataDirectory: dataDir,
  probeExecutable: installed ? path.join(process.resourcesPath, 'ProbeVirtualDisplay.exe') : undefined,
  getEngineOutputs: () => host.getDisplays()
});
const storage = {
  encryptString: value => safeStorage.encryptStringAsync(value),
  async decryptString(encrypted) { return (await safeStorage.decryptStringAsync(encrypted)).result; }
};

async function ensureEncryption() {
  if (!(await safeStorage.isAsyncEncryptionAvailable())) throw new Error('当前 Windows 会话无法保护管理凭据。');
}

function state() {
  const address = Object.values(os.networkInterfaces()).flat()
    .find(network => network?.family === 'IPv4' && !network.internal)?.address;
  return {...host.getState(), preferences, display, hostname: 'SideLantern', address: address ? (installed ? address : `${address}:${port}`) : '',
    managementPort: port + 1, version: app.getVersion()};
}

function publish() {
  if (window && !window.isDestroyed()) window.webContents.send('side-lantern:state', state());
}

async function openPrivateHost() {
  const source = installed ? path.join(process.resourcesPath, 'engine') : process.env.SIDELANTERN_ENGINE_DIR;
  if (process.platform !== 'win32') throw new Error('当前开发版仅支持 Windows。');
  if (!source) throw new Error('请将 SIDELANTERN_ENGINE_DIR 指向用于测试的 Sunshine 程序目录。');
  await ensureEncryption();
  const profile = await prepareProfile({source, target: privateHost, port, storage, installed});
  await ensurePortsAvailable(port);
  if (!installed) {
    const helperSource = path.join(__dirname, 'native', 'ShutdownHost.cs');
    const compiler = path.join(process.env.SystemRoot || 'C:\\Windows', 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
    try {
      await execFileAsync(compiler, ['/nologo', '/target:winexe', `/out:${profile.terminationHelper}`, helperSource],
        {windowsHide: true, timeout: 15000});
    } catch (error) {
      throw new Error('无法构建测试引擎的 Windows 退出辅助程序。', {cause: error});
    }
  }
  const engine = installed
    ? await require('./lib/windows-service.cjs').launchServiceEngine({port})
    : launchEngine(profile, port);
  try {
    const {client, settings} = await waitForEngine({engine, profile, port, createClient: createSunshineClient});
    return {engine, client, settings, outputId: await client.getOutput()};
  } catch (error) {
    await engine.stop();
    throw error;
  }
}

async function loadPreferences() {
  const defaultLanguage = languageFromWindows(app.getPreferredSystemLanguages());
  preferences = {...defaultPreferences, language: defaultLanguage};
  try {
    preferences = validatePreferences(JSON.parse(await fs.readFile(preferencesFile, 'utf8')), defaultLanguage);
  } catch (error) {
    if (error.code !== 'ENOENT') console.error('Could not read application preferences:', error.message);
  }
  nativeTheme.themeSource = preferences.theme;
}

async function setPreferences(patch) {
  const updated = validatePreferences({...preferences, ...patch}, preferences.language);
  if (updated.launchAtLogin !== preferences.launchAtLogin && !installed) {
    throw new Error('测试版本的登录启动需要通过正式安装包配置。');
  }
  const temporaryFile = `${preferencesFile}.tmp`;
  await fs.writeFile(temporaryFile, JSON.stringify(updated, null, 2));
  if (updated.launchAtLogin !== preferences.launchAtLogin) {
    const args = process.defaultApp ? [__dirname, '--tray'] : ['--tray'];
    app.setLoginItemSettings({openAtLogin: updated.launchAtLogin, path: process.execPath, args});
  }
  await fs.rename(temporaryFile, preferencesFile);
  preferences = updated;
  nativeTheme.themeSource = preferences.theme;
  updateTrayMenu();
  publish();
  return state();
}

function showWindow() {
  if (!window || window.isDestroyed()) return;
  if (window.isMinimized()) window.restore();
  window.show();
  window.focus();
}

async function openLogs() {
  const logDirectory = path.join(privateHost, 'logs');
  await fs.mkdir(logDirectory, {recursive: true});
  const failure = await shell.openPath(logDirectory);
  if (failure) throw new Error(failure);
}

async function refreshDisplay(force = false) {
  if (!force && (Date.now() < nextDisplayRefresh || !window?.isVisible())) return;
  nextDisplayRefresh = Date.now() + 8000;
  const result = await virtualDisplay.getStatus();
  if (JSON.stringify(result) === JSON.stringify(display)) return;
  display = result;
  publish();
}

function requestQuit() {
  if (!quitJob) quitJob = (async () => {
    try {
      await host.stop();
      await virtualDisplay.teardown();
      quitAllowed = true;
      clearInterval(timer);
      tray?.destroy();
      app.quit();
    } catch (error) {
      showWindow();
      throw error;
    } finally {
      quitJob = null;
    }
  })();
  return quitJob;
}

function registerIpc() {
  const actions = {
    getState: async () => {await host.refresh(); return state();},
    start: async () => {await host.start(); return state();},
    stop: async () => {await host.stop(); return state();},
    saveSettings: async value => {
      if (!value || !['primary', 'virtual', undefined].includes(value.source)) throw new Error('串流来源无效。');
      const outputId = value.source === 'virtual'
        ? (await virtualDisplay.getStatus()).outputId
        : value.source === 'primary' ? '' : undefined;
      if (value.source === 'virtual' && !outputId) throw new Error('虚拟显示器当前不可用于串流。');
      await host.saveSettings(value, outputId);
      await refreshDisplay(true);
      return state();
    },
    setPreferences,
    approvePairing: async value => {await host.approvePairing(value); return state();},
    cancelPairing: async id => {await host.cancelPairing(id); return state();},
    unpair: async id => {await host.unpair(id); return state();},
    openLogs
  };
  for (const [name, action] of Object.entries(actions)) {
    ipcMain.handle(`side-lantern:${name}`, (event, input) => {
      if (event.sender !== window?.webContents || event.senderFrame?.parent ||
          event.senderFrame?.url !== pathToFileURL(rendererFile).href) {
        throw new Error('未授权的桌面请求。');
      }
      return action(input);
    });
  }
}

async function createWindow() {
  window = new BrowserWindow({
    width: 1030, height: 775, minWidth: 650, minHeight: 540, show: false,
    icon: path.join(__dirname, 'assets', 'app.ico'),
    title: 'SideLantern', autoHideMenuBar: true, backgroundColor: nativeTheme.shouldUseDarkColors ? '#1b211e' : '#f5f7f4',
    webPreferences: {preload: path.join(__dirname, 'preload.cjs'), sandbox: true, contextIsolation: true, nodeIntegration: false}
  });
  window.webContents.setWindowOpenHandler(() => ({action: 'deny'}));
  window.webContents.on('will-navigate', event => event.preventDefault());
  window.webContents.session.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
  window.webContents.session.setPermissionCheckHandler(() => false);
  window.on('close', event => {
    if (!quitAllowed) {event.preventDefault(); window.hide();}
  });
  window.on('minimize', event => {event.preventDefault(); window.hide();});
  await window.loadFile(rendererFile);
  if (!smoke && !process.argv.includes('--tray')) showWindow();
}

function createTray() {
  const icon = nativeImage.createFromPath(path.join(__dirname, 'assets', 'tray.png'));
  tray = new Tray(icon);
  tray.setToolTip('SideLantern');
  tray.on('click', showWindow);
  updateTrayMenu();
}

function updateTrayMenu() {
  if (!tray) return;
  tray.setContextMenu(Menu.buildFromTemplate([
    {label: translate(preferences.language, 'trayOpen'), click: showWindow},
    {type: 'separator'},
    {label: translate(preferences.language, 'trayExit'), click: () => {void requestQuit().catch(console.error);}}
  ]));
}

async function migrateProfile() {
  const exportAt = process.argv.indexOf('--export-profile');
  const importAt = process.argv.indexOf('--import-profile');
  if (exportAt < 0 && importAt < 0) return false;
  if (process.platform !== 'win32' || (exportAt >= 0 && importAt >= 0)) throw new Error('请选择一种 Windows 迁移操作。');
  if (exportAt >= 0) {
    const [source, backup] = process.argv.slice(exportAt + 1);
    if (!source || !backup) throw new Error('导出需要原 Sunshine 配置目录和备份文件路径。');
    console.log(JSON.stringify(await exportSunshineProfile({source, backup, storage: windowsBackupStorage})));
  } else {
    const backup = process.argv[importAt + 1];
    if (!backup) throw new Error('导入需要备份文件路径。');
    await fs.mkdir(privateHost, {recursive: true});
    await restoreSunshineProfile({backup, target: path.join(privateHost, 'config'), storage: windowsBackupStorage});
    console.log('迁移已完成。下次启动时将使用导入的主机身份和应用列表。');
  }
  return true;
}

function requestsMigration(args) {
  return args.includes('--export-profile') || args.includes('--import-profile');
}

async function boot() {
  await fs.mkdir(dataDir, {recursive: true});
  if (await migrateProfile()) {
    app.exit(0);
    return;
  }
  await loadPreferences();
  Menu.setApplicationMenu(null);
  registerIpc();
  if (!smoke) createTray();
  await createWindow();
  if (smoke) {
    const ready = await window.webContents.executeJavaScript('Boolean(window.sideLantern && document.querySelector("[data-panel=host]"))');
    if (!ready) throw new Error('Electron renderer did not initialize.');
    await host.start();
    const snapshot = await window.webContents.executeJavaScript('window.sideLantern.getState()');
    if (snapshot.phase !== 'running' || snapshot.error) throw new Error(snapshot.error ?? 'Private host did not enter running state.');
    const themed = await window.webContents.executeJavaScript('window.sideLantern.setPreferences({theme:"dark", language:"en"})');
    const saved = JSON.parse(await fs.readFile(preferencesFile, 'utf8'));
    if (themed.preferences.theme !== 'dark' || saved.theme !== 'dark' || nativeTheme.themeSource !== 'dark' ||
        themed.preferences.language !== 'en' || saved.language !== 'en') {
      throw new Error('Renderer theme or language preference did not persist.');
    }
    const localized = await window.webContents.executeJavaScript('document.documentElement.lang');
    if (localized !== 'en' || themed.phase !== 'running') {
      throw new Error('The running renderer did not apply the language change.');
    }
    console.log(`SideLantern smoke passed: renderer, theme, language, authenticated API, private host on port ${port}.`);
    if (!process.argv.includes('--smoke-hold')) await requestQuit();
    return;
  }
  void host.start().then(() => refreshDisplay(true)).catch(error => console.error(error.message));
  timer = setInterval(() => {void host.refresh(); void refreshDisplay();}, 2000);
  timer.unref();
}

if (!app.requestSingleInstanceLock()) {
  if (requestsMigration(process.argv)) {
    console.error('请先从托盘完全退出 SideLantern，再运行迁移。');
    app.exit(1);
  } else app.quit();
} else {
  app.on('second-instance', (_event, args) => {
    if (!requestsMigration(args)) showWindow();
  });
  app.on('before-quit', event => {
    if (quitAllowed) return;
    event.preventDefault();
    void requestQuit().catch(console.error);
  });
  app.whenReady().then(boot).catch(async error => {
    console.error(error.stack);
    if (error.cause) console.error(error.cause.message);
    await host.stop().catch(console.error);
    app.exit(1);
  });
}
