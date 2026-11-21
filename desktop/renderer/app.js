'use strict';

const ui = document;
const api = window.sideLantern;
const { normalizeLanguage, translate } = window.sideLanternI18n;
const byId = (id) => ui.getElementById(id);
const panels = [...ui.querySelectorAll('[data-panel]')];
const navButtons = [...ui.querySelectorAll('[data-page]')];
const titles = { host: 'streamingHost', apps: 'streamingApps', stream: 'streamingSettings', display: 'virtualDisplay' };
const phaseLabels = {
  stopped: 'serviceStopped',
  starting: 'serviceStarting',
  running: 'serviceRunning',
  stopping: 'serviceStopping',
  error: 'serviceError'
};
const displayReasons = new Map(['displayProbeError', 'displayDriverMissing', 'sharedDisplayExtended', 'sharedDisplayDriverDetected']
  .map((key) => [translate('zh-CN', key), key]));
const listData = new WeakMap();
const pairDialog = byId('pair-dialog');
const preferencesDialog = byId('preferences-dialog');
const confirmDialog = byId('confirm-dialog');
const applicationDialog = byId('stream-app-dialog');
const applicationFields = {
  name: byId('stream-app-name'),
  command: byId('stream-app-command'),
  workingDirectory: byId('stream-app-directory'),
  imagePath: byId('stream-app-image')
};
const fields = byId('stream-form').elements;

let state;
let busy = false;
let streamDirty = false;
let streamApplied = false;
let currentPairing;
let pairValidationFailed = false;
let confirmation;
let currentPage = 'host';
let language;
let pendingLanguage;
let applications = null;
let applicationsLoading = false;
let applicationsError = '';
let editingApplication;

function t(key, values) {
  return translate(language, key, values);
}

function updateLanguage() {
  const selected = normalizeLanguage(pendingLanguage ?? state?.preferences?.language);
  if (language === selected) return;
  language = selected;
  ui.documentElement.lang = language;
  for (const item of ui.querySelectorAll('[data-i18n]')) item.textContent = t(item.dataset.i18n);
  for (const item of ui.querySelectorAll('[data-i18n-label]')) item.setAttribute('aria-label', t(item.dataset.i18nLabel));
}

function setText(id, text) {
  const target = byId(id);
  if (target.textContent !== text) target.textContent = text;
}

function setStatusMessage(id, message) {
  setText(id, message);
  byId(id).hidden = !message;
}

function showError(message) {
  setText('action-message', message);
  byId('action-notice').hidden = !message;
}

function errorText(error) {
  const message = error instanceof Error ? error.message : String(error);
  return message.replace(/^Error invoking remote method '[^']+': (?:Error: )?/, '');
}

function row(label, detail) {
  const item = ui.createElement('li');
  const text = ui.createElement('span');
  const name = ui.createElement('strong');
  item.className = 'item-row';
  text.className = 'item-name';
  name.textContent = label;
  text.append(name);
  if (detail) {
    const subtitle = ui.createElement('small');
    subtitle.textContent = detail;
    text.append(subtitle);
  }
  item.append(text);
  return item;
}

function itemButton(label, callback, { danger = false, disabled = false } = {}) {
  const button = ui.createElement('button');
  button.type = 'button';
  button.className = danger ? 'button destructive' : 'button';
  button.textContent = label;
  button.disabled = disabled;
  button.addEventListener('click', callback);
  return button;
}

function renderList(id, data, renderItem) {
  const list = byId(id);
  const key = JSON.stringify([language, data]);
  if (listData.get(list) !== key) {
    list.replaceChildren(...data.map(renderItem));
    listData.set(list, key);
  }
  list.hidden = data.length === 0;
}

function renderHost(snapshot) {
  const running = snapshot.phase === 'running';
  const activeSessions = Array.isArray(snapshot.sessions) ? snapshot.sessions : [];
  const devices = snapshot.devices ?? [];
  const liveDevices = new Set(activeSessions.map(({ deviceId }) => deviceId).filter(Boolean));
  const pairings = running ? (snapshot.pairings ?? []) : [];

  byId('host-phase').dataset.phase = snapshot.phase;
  setText('host-phase', t(activeSessions.length ? 'streaming' : phaseLabels[snapshot.phase] ?? 'statusUnknown'));
  setText('hostname', snapshot.hostname || 'SideLantern');
  setText('address-value', snapshot.address || '');
  byId('host-address').hidden = !snapshot.address;
  setStatusMessage('host-error', snapshot.error || '');

  const serviceButton = byId('service-toggle');
  serviceButton.disabled = busy || snapshot.phase === 'starting' || snapshot.phase === 'stopping';
  serviceButton.textContent = t(running ? 'stopService' : 'startService');

  const showLegacy = running && snapshot.legacyPairing === true;
  byId('legacy-pair').hidden = !showLegacy;
  byId('legacy-pair').disabled = busy;
  byId('pair-empty').hidden = pairings.length > 0;
  setText('pair-empty', t(!running ? 'startServiceToPair' : showLegacy ? 'legacyPairHelp' : 'startPairing'));

  renderList('pairings', pairings, (pairing) => {
    const displayName = pairing.name || t('moonlightDevice');
    const item = row(displayName, pairing.address);
    const actions = ui.createElement('div');
    actions.className = 'inline-controls';
    actions.append(
      itemButton(t('enterPin'), () => openPairing(pairing)),
      itemButton(t('cancelRequest'), () => run(() => api.cancelPairing(pairing.id)))
    );
    item.append(actions);
    return item;
  });

  byId('session-empty').hidden = activeSessions.length > 0;
  byId('session-count').hidden = activeSessions.length === 0;
  setText('session-count', String(liveDevices.size || activeSessions.length));
  setText('session-empty', t(!running ? 'serviceStopped'
    : snapshot.sessions === null ? 'noLiveSessionSupport' : 'waitingForMoonlight'));
  renderList('sessions', activeSessions, (session) => row(session.name || t('moonlightDevice'), session.source));

  byId('device-count').hidden = devices.length === 0;
  setText('device-count', String(devices.length));
  byId('device-empty').hidden = devices.length > 0;
  setText('device-empty', t(running ? 'noDevices' : 'startServiceToReadDevices'));
  byId('device-cache').hidden = running || devices.length === 0;
  renderList('devices', devices.map((device) => ({ ...device, live: liveDevices.has(device.id), running })), (device) => {
    const displayName = device.name || t('moonlightDevice');
    const status = t(device.live ? 'deviceStreaming' : device.enabled === false ? 'deviceDisabled' : 'devicePaired');
    const item = row(displayName, status);
    item.append(itemButton(t('unpair'), () => ask({
      title: 'unpairTitle',
      detail: 'unpairDetail',
      values: { device: device.name },
      label: 'unpair',
      command: () => api.unpair(device.id)
    }), { danger: true, disabled: !device.running }));
    return item;
  });
  byId('approve-pin').disabled = busy || !running;
}

function renderStream(snapshot) {
  const active = snapshot.phase === 'running';
  const source = byId('source');
  const virtual = snapshot.display?.outputId;
  const configured = snapshot.outputId && snapshot.outputId.toLowerCase() !== virtual?.toLowerCase();
  const idle = Array.isArray(snapshot.sessions) && snapshot.sessions.length === 0;
  byId('source-virtual').hidden = !virtual;
  byId('source-configured').hidden = !configured;
  source.disabled = !idle || busy || !active || (!virtual && !configured);
  setText('source-note', t(idle ? 'sourceNextConnection' : 'sourceAfterStream'));
  byId('stream-fields').disabled = !active || busy || !snapshot.settings;
  byId('save-settings').disabled = !active || busy || !streamDirty;
  if (!streamDirty && snapshot.settings) {
    for (const name of ['encoder', 'maxBitrateMbps', 'hevc', 'av1']) {
      fields[name].value = snapshot.settings[name];
    }
    fields.audio.checked = snapshot.settings.audio;
    source.value = configured ? 'configured' : snapshot.outputId ? 'virtual' : 'primary';
  }
  setText('stream-feedback', t(!active ? 'startServiceForSettings' : streamDirty ? 'settingsDirty'
    : streamApplied ? 'settingsApplied' : 'settingsApplyOnSave'));
}

function renderApplications() {
  const running = state?.phase === 'running';
  const items = running ? applications ?? [] : [];
  const canEdit = running && !busy && !applicationsLoading && !applicationsError && applications !== null;
  byId('refresh-apps').disabled = !running || busy || applicationsLoading;
  byId('add-stream-app').disabled = !canEdit;
  byId('save-stream-app').disabled = !running || busy;
  if (editingApplication) setText('stream-app-heading', t(editingApplication.index === -1 ? 'addStreamApp' : 'editStreamApp'));
  const status = !running ? t('startServiceToReadApps') : applicationsLoading ? t('loadingApps')
    : applicationsError || (applications === null ? t('loadingApps') : items.length ? '' : t('noStreamApps'));
  setStatusMessage('app-list-status', status);
  renderList('stream-app-list', items.map(app => ({...app, canEdit})), (app) => {
    const item = row(app.name, app.command || t('noLaunchCommand'));
    const actions = ui.createElement('div');
    actions.className = 'inline-controls';
    actions.append(
      itemButton(t('editApp'), () => openApplication(app), {disabled: !app.canEdit}),
      itemButton(t('deleteApp'), () => ask({
        title: 'deleteStreamAppTitle',
        detail: 'deleteStreamAppDetail',
        values: {app: app.name},
        label: 'deleteApp',
        command: async () => {
          if (await run(() => api.deleteApplication({index: app.index, revision: app.revision}))) {
            await refreshApplications();
          }
        }
      }), {danger: true, disabled: !app.canEdit})
    );
    item.append(actions);
    return item;
  });
}

async function refreshApplications() {
  if (state?.phase !== 'running' || applicationsLoading) return;
  applicationsLoading = true;
  applicationsError = '';
  renderApplications();
  try {
    applications = await api.getApplications();
  } catch (error) {
    applicationsError = errorText(error);
  } finally {
    applicationsLoading = false;
    renderApplications();
  }
}

function openApplication(app = {index: -1}) {
  if (state?.phase !== 'running' || busy) return;
  editingApplication = {index: app.index, revision: app.revision};
  for (const [name, field] of Object.entries(applicationFields)) field.value = app[name] ?? '';
  setStatusMessage('stream-app-feedback', '');
  renderApplications();
  applicationDialog.showModal();
  applicationFields.name.focus();
}

function renderDisplay(snapshot) {
  const display = snapshot.display;
  setText('display-title', t(display?.enabled ? 'displayExtended'
    : display?.detected ? 'displayDriverDetected' : 'displayUnavailable'));
  const reason = display?.reason;
  const translatedReason = displayReasons.get(reason);
  setText('display-reason', translatedReason ? t(translatedReason) : reason || t('displayWaiting'));
  const mode = display?.outputs?.[0]?.mode;
  setStatusMessage('display-mode', mode ? `${mode.width} × ${mode.height} · ${mode.refreshRate} Hz` : '');
}

function renderPreferences(snapshot) {
  const preferences = snapshot.preferences;
  if (preferences) {
    const theme = preferences.theme || 'system';
    ui.documentElement.dataset.theme = theme;
    byId('theme').value = theme;
    byId('language').value = language;
    byId('launch-login').checked = preferences.launchAtLogin === true;
  }
  byId('theme').disabled = busy || !preferences;
  byId('language').disabled = busy || !preferences;
  byId('launch-login').disabled = busy || !preferences;
  setText('version', snapshot.version || t('versionUnknown'));
}

function render() {
  updateLanguage();
  setText('breadcrumb', t(titles[currentPage]));
  byId('refresh').disabled = busy;
  if (!state) return;
  renderHost(state);
  renderApplications();
  renderStream(state);
  renderDisplay(state);
  renderPreferences(state);
  renderPairing();
  renderConfirmation();
}

function receiveState(next) {
  if (!next) return;
  const phaseChanged = state?.phase !== next.phase;
  state = next;
  if (phaseChanged) {
    applications = null;
    applicationsError = '';
  }
  render();
  if (phaseChanged && currentPage === 'apps') void refreshApplications();
}

async function run(command, report = showError) {
  if (busy || !api) return false;
  busy = true;
  showError('');
  report('');
  render();
  try {
    receiveState(await command());
    return true;
  } catch (error) {
    report(errorText(error));
    return false;
  } finally {
    busy = false;
    render();
  }
}

function renderConfirmation() {
  if (!confirmation) return;
  const { title, detail, values, label } = confirmation;
  setText('confirm-heading', t(title));
  setText('confirm-detail', t(detail, values?.device === '' ? { device: t('moonlightDevice') } : values));
  setText('confirm-action', t(label));
}

function ask(action) {
  if (busy) return;
  confirmation = action;
  renderConfirmation();
  confirmDialog.showModal();
}

function renderPairing() {
  if (!currentPairing) return;
  const { address } = currentPairing;
  setText('pair-detail', t(address ? 'remotePairDetail' : 'localPairDetail', { address }));
  if (pairValidationFailed) setStatusMessage('pair-feedback', t('invalidPairing'));
}

function openPairing(pairing) {
  if (busy || state?.phase !== 'running') return;
  currentPairing = pairing;
  byId('pair-name').value = pairing.name || 'Moonlight';
  byId('pair-pin').value = '';
  pairValidationFailed = false;
  setStatusMessage('pair-feedback', '');
  renderPairing();
  pairDialog.showModal();
  byId('pair-pin').focus();
}

for (const button of navButtons) {
  button.addEventListener('click', () => {
    currentPage = button.dataset.page;
    for (const other of navButtons) other.removeAttribute('aria-current');
    button.setAttribute('aria-current', 'page');
    for (const panel of panels) panel.hidden = panel.dataset.panel !== currentPage;
    setText('breadcrumb', t(titles[currentPage]));
    if (currentPage === 'apps') void refreshApplications();
  });
}

for (const button of ui.querySelectorAll('[data-close]')) {
  button.addEventListener('click', () => button.closest('dialog').close());
}

byId('confirm-action').addEventListener('click', () => {
  const command = confirmation?.command;
  confirmDialog.close();
  if (command) void command();
});
confirmDialog.addEventListener('close', () => { confirmation = undefined; });

byId('refresh').addEventListener('click', () => run(() => api.getState()));
byId('dismiss-notice').addEventListener('click', () => showError(''));
byId('refresh-apps').addEventListener('click', refreshApplications);
byId('add-stream-app').addEventListener('click', () => openApplication());
byId('stream-app-image-browse').addEventListener('click', async () => {
  if (!api || busy) return;
  const button = byId('stream-app-image-browse');
  button.disabled = true;
  try {
    const chosen = await api.pickCoverImage(applicationFields.imagePath.value);
    if (chosen) {
      applicationFields.imagePath.value = chosen;
      setStatusMessage('stream-app-feedback', '');
    }
  } catch (error) {
    setStatusMessage('stream-app-feedback', errorText(error));
  } finally {
    button.disabled = false;
  }
});
applicationDialog.addEventListener('close', () => { editingApplication = undefined; });
byId('stream-app-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!editingApplication || state?.phase !== 'running') return;
  const changes = {...editingApplication};
  for (const [name, field] of Object.entries(applicationFields)) changes[name] = field.value;
  if (await run(() => api.saveApplication(changes), message => setStatusMessage('stream-app-feedback', message))) {
    if (applicationDialog.open) applicationDialog.close();
    await refreshApplications();
  }
});

byId('service-toggle').addEventListener('click', () => {
  if (!state || busy) return;
  if (state.phase !== 'running') {
    void run(() => api.start());
    return;
  }
  ask({
    title: 'stopServiceTitle',
    detail: 'stopServiceDetail',
    label: 'stopService',
    command: () => run(() => api.stop())
  });
});

byId('legacy-pair').addEventListener('click', () => openPairing({ id: null }));
pairDialog.addEventListener('close', () => {
  currentPairing = undefined;
  pairValidationFailed = false;
  byId('pair-pin').value = '';
});
byId('pin-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!currentPairing) return;
  const name = byId('pair-name').value.trim();
  const pin = byId('pair-pin').value;
  if (!name || !/^[0-9]{4}$/.test(pin)) {
    pairValidationFailed = true;
    renderPairing();
    return;
  }
  pairValidationFailed = false;
  const success = await run(() => api.approvePairing({
    id: currentPairing.id,
    pin,
    name
  }), (message) => setStatusMessage('pair-feedback', message));
  if (success && pairDialog.open) pairDialog.close();
});

byId('stream-form').addEventListener('input', () => {
  streamDirty = true;
  streamApplied = false;
  renderStream(state);
});
byId('stream-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const changes = {
    encoder: fields.encoder.value,
    maxBitrateMbps: Number(fields.maxBitrateMbps.value),
    hevc: fields.hevc.value,
    av1: fields.av1.value,
    audio: fields.audio.checked
  };
  const selected = byId('source');
  if (!selected.disabled && selected.value === 'primary' && state.outputId) changes.source = 'primary';
  if (!selected.disabled && selected.value === 'virtual' &&
      state.display?.outputId?.toLowerCase() !== state.outputId?.toLowerCase()) changes.source = 'virtual';
  const apply = async () => {
    if (await run(() => api.saveSettings(changes))) {
      streamDirty = false;
      streamApplied = true;
      render();
    }
  };
  if (state?.sessions === null || state?.sessions?.length) {
    ask({
      title: 'applySettingsTitle',
      detail: 'applySettingsDetail',
      label: 'saveAndApply',
      command: apply
    });
  } else {
    void apply();
  }
});

byId('open-preferences').addEventListener('click', () => {
  setStatusMessage('preferences-feedback', '');
  preferencesDialog.showModal();
});
async function updatePreferences() {
  if (busy || !state?.preferences) return;
  const changes = {
    theme: byId('theme').value,
    launchAtLogin: byId('launch-login').checked,
    language: byId('language').value
  };
  pendingLanguage = changes.language;
  try {
    return await run(() => api.setPreferences(changes), (message) => setStatusMessage('preferences-feedback', message));
  } finally {
    pendingLanguage = undefined;
    render();
  }
}
byId('theme').addEventListener('change', updatePreferences);
byId('language').addEventListener('change', updatePreferences);
byId('launch-login').addEventListener('change', updatePreferences);
byId('open-logs').addEventListener('click', () => {
  run(() => api.openLogs(), (message) => setStatusMessage('preferences-feedback', message));
});

if (api) {
  const unsubscribe = api.onState(receiveState);
  window.addEventListener('pagehide', () => unsubscribe(), { once: true });
  void run(() => api.getState());
} else {
  render();
  setText('host-phase', t('connectionFailed'));
  showError(t('desktopProcessError'));
}
