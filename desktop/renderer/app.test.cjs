const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const markup = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');

function createElement() {
  return {
    children: [],
    handlers: new Map(),
    attributes: new Map(),
    dataset: {},
    addEventListener(name, callback) { this.handlers.set(name, callback); },
    removeAttribute(name) { this.attributes.delete(name); },
    setAttribute(name, value) { this.attributes.set(name, value); },
    append(...children) { this.children.push(...children); },
    replaceChildren(...children) { this.children = children; },
    showModal() { this.open = true; },
    close() { this.open = false; this.handlers.get('close')?.(); },
    focus() {}
  };
}

function mount(initialState, approvePairing, setPreferences) {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) elements.set(id, createElement());
    return elements.get(id);
  };
  element('stream-form').elements = Object.fromEntries(
    ['encoder', 'maxBitrateMbps', 'hevc', 'av1', 'audio'].map((name) => [name, createElement()])
  );
  let update;
  const bridge = {
    async getState() { return initialState; },
    onState(callback) { update = callback; return () => {}; },
    approvePairing,
    setPreferences
  };
  const pages = ['host', 'stream', 'display'];
  const nav = pages.map((page) => ({ ...createElement(), dataset: { page } }));
  const sections = pages.map((panel) => ({ ...createElement(), dataset: { panel }, hidden: panel !== 'host' }));
  const staticLabel = { ...createElement(), dataset: { i18n: 'appSettings' } };
  const source = fs.readFileSync(path.join(__dirname, 'app.js'), 'utf8');
  const document = {
    documentElement: createElement(),
    getElementById: (id) => markup.includes(`id="${id}"`) ? element(id) : null,
    createElement,
    querySelectorAll(selector) {
      if (selector === '[data-page]') return nav;
      if (selector === '[data-panel]') return sections;
      if (selector === '[data-i18n]') return [staticLabel];
      return [];
    }
  };
  const window = { sideLantern: bridge, addEventListener() {} };
  const translator = path.join(__dirname, 'i18n.js');
  const context = vm.createContext({ document, window });
  if (fs.existsSync(translator)) vm.runInContext(fs.readFileSync(translator, 'utf8'), context);
  vm.runInContext(source, context);
  return { element, document, nav, sections, staticLabel, publish: (state) => update(state) };
}

const getState = (overrides = {}) => ({
  phase: 'running',
  error: null,
  hostname: 'Lantern',
  address: '192.168.1.42:48989',
  devices: [],
  pairings: [],
  sessions: null,
  settings: { encoder: 'auto', maxBitrateMbps: 0, hevc: 'auto', av1: 'auto', audio: true },
  preferences: { theme: 'system', launchAtLogin: false, language: 'zh-CN' },
  display: { available: false },
  version: '0.1.0',
  legacyPairing: false,
  ...overrides
});

const nextTurn = () => new Promise((resolve) => setImmediate(resolve));
const submit = { preventDefault() {} };

test('the workspace header leaves window actions to the native chrome', () => {
  const header = markup.match(/<header class="workspace-bar">([\s\S]*?)<\/header>/)[1];
  assert.doesNotMatch(header, /最小化到托盘|退出/);
});

test('legacy PIN flows stay manual and pass a null request ID', async () => {
  const state = getState({ legacyPairing: true });
  const approvals = [];
  const renderer = mount(state, async (value) => {
    approvals.push({ ...value });
    return state;
  });
  await nextTurn();
  assert.equal(renderer.element('legacy-pair').hidden, false);
  assert.equal(renderer.element('pairings').hidden, true);
  renderer.element('legacy-pair').handlers.get('click')();
  renderer.element('pair-name').value = 'Tablet';
  renderer.element('pair-pin').value = '0123';
  await renderer.element('pin-form').handlers.get('submit')(submit);
  assert.deepEqual(approvals, [{ id: null, pin: '0123', name: 'Tablet' }]);
  assert.match(renderer.element('session-empty').textContent, /无法读取当前连接/);
});

test('rendering a state push preserves streaming settings edits', async () => {
  const state = getState();
  const renderer = mount(state);
  await nextTurn();
  const fields = renderer.element('stream-form').elements;
  assert.equal(fields.maxBitrateMbps.value, 0);
  fields.maxBitrateMbps.value = '25';
  renderer.element('stream-form').handlers.get('input')();
  renderer.publish(getState({ devices: [{ id: 'a', name: 'Tablet', enabled: true }] }));
  assert.equal(fields.maxBitrateMbps.value, '25');
  assert.equal(renderer.element('device-count').textContent, '1');
});

test('changing language immediately translates the existing page and preserves dirty values', async () => {
  const initial = getState();
  const calls = [];
  let persist;
  const saved = new Promise((resolve) => { persist = resolve; });
  const renderer = mount(initial, undefined, async (value) => {
    calls.push({ ...value });
    await saved;
    return getState({ preferences: { ...initial.preferences, language: value.language } });
  });
  await nextTurn();
  assert.equal(renderer.element('language').value, 'zh-CN');
  renderer.nav[1].handlers.get('click')();
  const fields = renderer.element('stream-form').elements;
  fields.maxBitrateMbps.value = '35';
  renderer.element('stream-form').handlers.get('input')();
  renderer.element('open-preferences').handlers.get('click')();
  renderer.element('language').value = 'en';
  const change = renderer.element('language').handlers.get('change')();
  assert.equal(renderer.document.documentElement.lang, 'en');
  assert.equal(renderer.element('breadcrumb').textContent, 'Streaming settings');
  assert.equal(renderer.staticLabel.textContent, 'App settings');
  assert.equal(renderer.element('host-phase').textContent, 'Service running');
  assert.equal(renderer.sections[1].hidden, false);
  assert.equal(renderer.element('preferences-dialog').open, true);
  assert.equal(fields.maxBitrateMbps.value, '35');
  assert.deepEqual(calls, [{ theme: 'system', launchAtLogin: false, language: 'en' }]);
  persist();
  await change;
  assert.equal(renderer.element('language').value, 'en');
});

test('an open pairing dialog translates on a locale push without changing the device or PIN', async () => {
  const renderer = mount(getState({ legacyPairing: true }));
  await nextTurn();
  renderer.element('legacy-pair').handlers.get('click')();
  renderer.element('pair-name').value = '客厅电视';
  renderer.element('pair-pin').value = '12';
  await renderer.element('pin-form').handlers.get('submit')(submit);
  renderer.publish(getState({ preferences: { language: 'en' } }));
  assert.match(renderer.element('pair-detail').textContent, /Moonlight client/);
  assert.match(renderer.element('pair-feedback').textContent, /four-digit PIN/);
  assert.equal(renderer.element('pair-dialog').open, true);
  assert.equal(renderer.element('pair-name').value, '客厅电视');
  assert.equal(renderer.element('pair-pin').value, '12');
});

test('every translatable HTML key resolves in supported locales', () => {
  const { translate } = require('./i18n.js');
  for (const [, key] of markup.matchAll(/data-i18n(?:-label)?="([^"]+)"/g)) {
    assert.notEqual(translate('en', key), key);
    assert.notEqual(translate('zh-CN', key), key);
  }
  assert.equal(translate('not-supported', 'appSettings'), 'App settings');
});
