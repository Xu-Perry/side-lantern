const { createHash, X509Certificate } = require('node:crypto');
const https = require('node:https');

const LOOPBACK = '127.0.0.1';
const READ_TIMEOUT_MS = 5000;
const PAIR_TIMEOUT_MS = 30000;
const MAX_RESPONSE_BYTES = 4 * 1024 * 1024;
const MAX_BITRATE_KBPS = 2147483647;
const ENCODERS = new Set(['auto', 'nvenc', 'quicksync', 'amdvce', 'software']);
const FORMAT_MODES = new Set(['auto', 'disabled']);
const BOOLEAN_DISABLED = new Set(['off', 'false', 'disabled', 'no', '0']);
const PAIRING_ID = /^[a-f0-9]{32}$/i;
const DEVICE_UUID = /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/i;
const DISPLAY_ID = /^\{[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}\}$/i;
const CONFIG_METADATA = new Set(['status', 'version', 'platform']);
const APP_FIELDS = {
  command: 'cmd',
  workingDirectory: 'working-dir',
  imagePath: 'image-path',
};

class SunshineResponseError extends Error {
  constructor(statusCode) {
    super(`Sunshine 管理请求失败（HTTP ${statusCode}）。`);
    this.statusCode = statusCode;
  }
}

function requestJson(endpoint, options) {
  const { path, method = 'GET', authorized = true, data, timeoutMs = READ_TIMEOUT_MS } = options;
  const signal = AbortSignal.timeout(timeoutMs);
  const payload = data === undefined ? undefined : JSON.stringify(data);
  const headers = { Accept: 'application/json' };
  if (authorized) headers.Authorization = endpoint.authorization;
  if (payload !== undefined) {
    headers['Content-Type'] = 'application/json';
    headers['Content-Length'] = Buffer.byteLength(payload);
  }

  return new Promise((resolve, reject) => {
    function fail(error) {
      if (signal.aborted) {
        reject(new Error('Sunshine 管理请求超时。'));
      } else if (typeof error.code === 'string' && /CERT|SELF_SIGNED/.test(error.code)) {
        reject(new Error('Sunshine 管理证书与私有实例不匹配或无效。'));
      } else {
        reject(new Error('无法连接 Sunshine 管理服务。'));
      }
    }

    const request = https.request({
      hostname: LOOPBACK,
      port: endpoint.port,
      method,
      path,
      headers,
      signal,
      agent: false,
      ca: endpoint.cert.toString(),
      rejectUnauthorized: true,
      checkServerIdentity(_hostname, peer) {
        if (!peer.raw?.equals(endpoint.cert.raw)) {
          const error = new Error('Unexpected Sunshine certificate');
          error.code = 'SIDELANTERN_CERT_MISMATCH';
          return error;
        }
      },
    }, response => {
      response.on('error', fail);
      if (response.statusCode < 200 || response.statusCode >= 300) {
        reject(new SunshineResponseError(response.statusCode));
        response.destroy();
        return;
      }

      const chunks = [];
      let received = 0;
      response.on('data', chunk => {
        received += chunk.length;
        if (received > MAX_RESPONSE_BYTES) {
          reject(new Error('Sunshine 管理响应过大。'));
          response.destroy();
          return;
        }
        chunks.push(chunk);
      });
      response.on('end', () => {
        try {
          const result = JSON.parse(Buffer.concat(chunks).toString('utf8'));
          if (!result || typeof result !== 'object' || Array.isArray(result)) {
            throw new TypeError('Expected a JSON object');
          }
          resolve(result);
        } catch {
          reject(new Error('Sunshine 返回了无效的管理响应。'));
        }
      });
    });
    request.on('error', fail);
    request.end(payload);
  });
}

function readSettings(config) {
  const bitrate = Number(config.max_bitrate);
  return {
    encoder: ENCODERS.has(config.encoder) ? config.encoder : 'auto',
    maxBitrateMbps: Number.isInteger(bitrate) && bitrate >= 0 && bitrate <= MAX_BITRATE_KBPS ? bitrate / 1000 : 0,
    hevc: Number(config.hevc_mode) === 1 ? 'disabled' : 'auto',
    av1: Number(config.av1_mode) === 1 ? 'disabled' : 'auto',
    audio: !BOOLEAN_DISABLED.has(String(config.stream_audio).toLowerCase()),
  };
}

function mergeSettings(config, settings) {
  if (!settings || !ENCODERS.has(settings.encoder) || !FORMAT_MODES.has(settings.hevc)
      || !FORMAT_MODES.has(settings.av1) || typeof settings.audio !== 'boolean') {
    throw new TypeError('串流设置无效。');
  }
  const mbps = settings.maxBitrateMbps;
  const kbps = Math.round(mbps * 1000);
  if (!Number.isFinite(mbps) || mbps < 0 || kbps > MAX_BITRATE_KBPS || kbps / 1000 !== mbps) {
    throw new RangeError('码率必须为非负数字，最多三位小数。');
  }

  const owned = {
    encoder: settings.encoder === 'auto' ? undefined : settings.encoder,
    max_bitrate: kbps || undefined,
    hevc_mode: settings.hevc === 'disabled' ? 1 : undefined,
    av1_mode: settings.av1 === 'disabled' ? 1 : undefined,
    stream_audio: settings.audio ? undefined : 'disabled',
  };
  const merged = Object.fromEntries(Object.entries(config).filter(([key]) => !CONFIG_METADATA.has(key)));
  for (const [key, value] of Object.entries(owned)) {
    if (value === undefined) delete merged[key];
    else merged[key] = value;
  }
  return merged;
}

function requireSuccess(response, message) {
  if (response.status !== true) throw new Error(message);
}

function validIdentifier(value, expression) {
  return typeof value === 'string' && expression.test(value);
}

function applicationRevision(app) {
  return createHash('sha256').update(JSON.stringify(app)).digest('hex');
}

function createSunshineClient({ port, username, password, certificate }) {
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new RangeError('Sunshine 管理端口无效。');
  }
  if (typeof username !== 'string' || !username || username.includes(':')
      || typeof password !== 'string' || !password) {
    throw new TypeError('Sunshine 管理凭据无效。');
  }

  const endpoint = {
    port,
    authorization: `Basic ${Buffer.from(`${username}:${password}`).toString('base64')}`,
    cert: new X509Certificate(certificate),
  };
  const read = path => requestJson(endpoint, { path });
  const write = (path, method, data, timeoutMs) => requestJson(endpoint, { path, method, data, timeoutMs });
  let configWrite = Promise.resolve();
  let legacyPairing = false;
  let hasSessionEndpoint = true;
  let hasDisplayEndpoint = true;

  async function getConfig() {
    const config = await read('/api/config');
    requireSuccess(config, '无法读取 Sunshine 串流设置。');
    return config;
  }

  async function readApps() {
    const result = await read('/api/apps');
    if (!Array.isArray(result.apps) || result.apps.some(app => typeof app?.name !== 'string')) {
      throw new Error('Sunshine 返回了无效的串流应用列表。');
    }
    return result.apps;
  }

  async function currentApplication({index, revision} = {}) {
    if (!Number.isSafeInteger(index) || index < 0 || !validIdentifier(revision, /^[a-f0-9]{64}$/)) {
      throw new TypeError('串流应用标识无效。');
    }
    const app = (await readApps())[index];
    if (!app || applicationRevision(app) !== revision) {
      throw new Error('串流应用列表已变更，请刷新后再试。');
    }
    return app;
  }

  return {
    async ping() {
      const result = await requestJson(endpoint, { path: '/api/configLocale', authorized: false });
      requireSuccess(result, 'Sunshine 管理服务尚未就绪。');
      return true;
    },

    async getSettings() {
      return readSettings(await getConfig());
    },

    async getApplications() {
      return (await readApps()).map((app, index) => ({
        index,
        revision: applicationRevision(app),
        name: app.name,
        ...Object.fromEntries(Object.entries(APP_FIELDS).map(([key, field]) =>
          [key, typeof app[field] === 'string' ? app[field] : ''])),
      }));
    },

    async saveApplication(input) {
      if (typeof input?.name !== 'string' || !input.name.trim() || input.name.includes('\0')) {
        throw new TypeError('请输入串流应用名称。');
      }
      const fields = {};
      for (const [key, field] of Object.entries(APP_FIELDS)) {
        const value = input[key];
        if (typeof value !== 'string' || value.includes('\0')) throw new TypeError('串流应用设置无效。');
        fields[field] = value.trim() ? value : null;
      }
      const previous = input.index === -1 ? {} : await currentApplication(input);
      const result = await write('/api/apps', 'POST', {...previous, ...fields, name: input.name.trim(), index: input.index});
      requireSuccess(result, '无法保存串流应用。');
    },

    async deleteApplication(input) {
      await currentApplication(input);
      const result = await write(`/api/apps/${input.index}`, 'DELETE');
      requireSuccess(result, '无法删除串流应用。');
    },

    async saveSettings(settings, outputId) {
      if (outputId !== undefined && outputId !== '' && !validIdentifier(outputId, DISPLAY_ID)) {
        throw new TypeError('串流显示器标识无效。');
      }
      const writeThisConfig = configWrite.then(async () => {
        const config = mergeSettings(await getConfig(), settings);
        if (outputId === '') delete config.output_name;
        else if (outputId !== undefined) config.output_name = outputId;
        const response = await write('/api/config', 'POST', config);
        requireSuccess(response, '无法保存 Sunshine 串流设置。');
        return readSettings(config);
      });
      configWrite = writeThisConfig.catch(() => {});
      return writeThisConfig;
    },

    async getDevices() {
      const result = await read('/api/clients/list');
      requireSuccess(result, '无法读取已配对设备。');
      if (!Array.isArray(result.named_certs)) throw new Error('Sunshine 返回了无效的设备列表。');
      return result.named_certs.filter(device => validIdentifier(device?.uuid, DEVICE_UUID))
        .map(device => ({
          id: device.uuid,
          name: typeof device.name === 'string' ? device.name : '',
          enabled: device.enabled !== false,
        }));
    },

    async getSessions() {
      if (!hasSessionEndpoint) return null;
      let result;
      try {
        result = await read('/api/sessions');
      } catch (error) {
        if (!(error instanceof SunshineResponseError) || error.statusCode !== 404) throw error;
        hasSessionEndpoint = false;
        return null;
      }
      requireSuccess(result, '无法读取当前串流会话。');
      if (!Array.isArray(result.sessions)) throw new Error('Sunshine 返回了无效的串流会话列表。');
      return result.sessions.filter(session => validIdentifier(session?.device_id, DEVICE_UUID))
        .map(session => ({
          deviceId: session.device_id,
          name: typeof session.name === 'string' ? session.name : '',
          source: typeof session.source === 'string' ? session.source : null,
        }));
    },

    async getOutput() {
      const config = await getConfig();
      return typeof config.output_name === 'string' ? config.output_name : '';
    },

    async getDisplays() {
      if (!hasDisplayEndpoint) return null;
      let result;
      try {
        result = await read('/api/displays');
      } catch (error) {
        if (!(error instanceof SunshineResponseError) || error.statusCode !== 404) throw error;
        hasDisplayEndpoint = false;
        return null;
      }
      requireSuccess(result, '无法读取引擎显示器列表。');
      if (!Array.isArray(result.displays)) throw new Error('Sunshine 返回了无效的显示器列表。');
      return result.displays;
    },

    async getPairings() {
      let result;
      try {
        result = await read('/api/pin');
      } catch (error) {
        if (error instanceof SunshineResponseError && error.statusCode === 404) {
          legacyPairing = true;
          return null;
        }
        throw error;
      }
      legacyPairing = false;
      if (!Array.isArray(result.pairings)) throw new Error('Sunshine 返回了无效的配对请求列表。');
      return result.pairings.filter(pairing => validIdentifier(pairing?.id, PAIRING_ID))
        .map(pairing => ({
          id: pairing.id,
          name: typeof pairing.name === 'string' ? pairing.name : '',
          address: typeof pairing.address === 'string' ? pairing.address : '',
        }));
    },

    async approvePairing({ id, pin, name }) {
      const useLegacy = id === null && legacyPairing;
      if (!useLegacy && !validIdentifier(id, PAIRING_ID)) throw new TypeError('需要选择有效的配对请求。');
      if (!validIdentifier(pin, /^[0-9]{4}$/)) throw new TypeError('PIN 必须是四位数字。');
      if (typeof name !== 'string' || !name.trim() || Buffer.byteLength(name) > 128) {
        throw new TypeError('设备名称必须在 1 到 128 字节之间。');
      }
      const data = { pin, name };
      if (!useLegacy) data.pairing_id = id;
      const result = await write('/api/pin', 'POST', data, PAIR_TIMEOUT_MS);
      requireSuccess(result, '配对未完成。请确认 PIN 或重新发起配对。');
    },

    async cancelPairing(id) {
      if (!validIdentifier(id, PAIRING_ID)) throw new TypeError('需要选择有效的配对请求。');
      const result = await write('/api/pin', 'DELETE', { pairing_id: id });
      requireSuccess(result, '无法取消配对请求。该请求可能已结束。');
    },

    async unpair(id) {
      if (!validIdentifier(id, DEVICE_UUID)) throw new TypeError('设备标识无效。');
      const result = await write('/api/clients/unpair', 'POST', { uuid: id });
      requireSuccess(result, '无法解除设备配对。');
    },
  };
}

module.exports = { createSunshineClient };
