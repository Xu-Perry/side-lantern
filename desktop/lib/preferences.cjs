function languageFromWindows(preferredLanguages) {
  for (const language of preferredLanguages) {
    const prefix = language.toLowerCase().split(/[-_]/)[0];
    if (prefix === 'zh') return 'zh-CN';
    if (prefix === 'en') return 'en';
  }
  return 'en';
}

function validatePreferences(value, defaultLanguage) {
  const language = value?.language ?? defaultLanguage;
  if (!value || !['system', 'light', 'dark'].includes(value.theme) ||
      typeof value.launchAtLogin !== 'boolean' || !['zh-CN', 'en'].includes(language)) {
    throw new Error('无效的应用设置。');
  }
  return {theme: value.theme, launchAtLogin: value.launchAtLogin, language};
}

module.exports = {languageFromWindows, validatePreferences};
