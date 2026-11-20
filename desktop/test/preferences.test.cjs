const assert = require('node:assert/strict');
const test = require('node:test');
const {validatePreferences, languageFromWindows} = require('../lib/preferences.cjs');

test('uses the first supported Windows language and falls back to English', () => {
  assert.equal(languageFromWindows(['fr-FR', 'zh-Hans-CN', 'en-US']), 'zh-CN');
  assert.equal(languageFromWindows(['ko-KR']), 'en');
});

test('restores an explicit language and upgrades existing preferences without a language', () => {
  const base = {theme: 'light', launchAtLogin: true};
  assert.deepEqual(validatePreferences(base, 'zh-CN'), {...base, language: 'zh-CN'});
  assert.deepEqual(validatePreferences({...base, language: 'en'}, 'zh-CN'), {...base, language: 'en'});
  assert.throws(() => validatePreferences({...base, language: 'fr'}, 'en'), /设置/);
});
