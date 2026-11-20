const {execFile} = require('node:child_process');
const fs = require('node:fs/promises');
const path = require('node:path');
const {promisify} = require('node:util');

const runProcess = promisify(execFile);
const displayName = /^\\\\\.\\display[1-9]\d*$/i;
const deviceId = /^\{[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\}$/i;

async function deleteProbeFile(directory) {
  await fs.unlink(path.join(directory, 'probe.exe')).catch(() => {});
  await fs.rmdir(directory).catch(() => {});
}

function createWindowsProbe(dataDirectory, probeExecutable) {
  let helper;
  let buildDirectory;

  async function build() {
    if (probeExecutable) return probeExecutable;
    if (!dataDirectory) throw new Error('Windows display probe requires an application data directory.');
    await fs.mkdir(dataDirectory, {recursive: true});
    buildDirectory = await fs.mkdtemp(path.join(dataDirectory, 'vdd-probe-'));
    const exe = path.join(buildDirectory, 'probe.exe');
    const compiler = path.join(process.env.SystemRoot || 'C:\\Windows', 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
    try {
      await runProcess(compiler, ['/nologo', '/target:exe', '/platform:x64', `/out:${exe}`,
        path.join(__dirname, '..', 'native', 'ProbeVirtualDisplay.cs')], {windowsHide: true, timeout: 15000});
      return exe;
    } catch (error) {
      await deleteProbeFile(buildDirectory);
      buildDirectory = undefined;
      throw error;
    }
  }

  return {
    async run() {
      if (process.platform !== 'win32') return {detected: false, outputs: []};
      helper ??= build().catch(error => {helper = undefined; throw error;});
      const {stdout} = await runProcess(await helper, [], {windowsHide: true, timeout: 5000, maxBuffer: 131072});
      const [presence, ...lines] = stdout.trim().split(/\r?\n/);
      if (!/^present\t[01]$/.test(presence)) throw new Error('Windows display probe returned invalid data.');
      const outputs = new Map();
      for (const line of lines) {
        const [tag, name, width, height, refreshRate] = line.split('\t');
        if (!displayName.test(name)) continue;
        if (tag === 'display') outputs.set(name, {windowsName: name, modes: []});
        else {
          const output = outputs.get(name);
          if (!output) continue;
          const value = {width: Number(width), height: Number(height), refreshRate: Number(refreshRate)};
          if (tag === 'current') output.mode = value;
          if (tag === 'mode') output.modes.push(value);
        }
      }
      return {detected: presence.endsWith('1'), outputs: [...outputs.values()]};
    },
    async dispose() {
      await helper?.catch(() => {});
      if (buildDirectory) {
        await deleteProbeFile(buildDirectory);
        buildDirectory = undefined;
        helper = undefined;
      }
    }
  };
}

function validMode(mode) {
  return mode && [mode.width, mode.height, mode.refreshRate].every(Number.isSafeInteger) &&
    mode.width > 0 && mode.height > 0 && mode.refreshRate > 1;
}

function supportedModes(modes) {
  const unique = new Map();
  for (const mode of modes ?? []) {
    if (validMode(mode)) unique.set(`${mode.width}:${mode.height}:${mode.refreshRate}`, mode);
  }
  return [...unique.values()].sort((a, b) => a.width - b.width || a.height - b.height || a.refreshRate - b.refreshRate);
}

function createVirtualDisplay({dataDirectory, probeExecutable, probe, getEngineOutputs} = {}) {
  const nativeProbe = probe ? null : createWindowsProbe(dataDirectory, probeExecutable);
  let pending;

  async function inspect() {
    const base = {available: false, controllable: false, detected: false, enabled: false, modes: [], outputs: []};
    let observed;
    try {
      observed = await (probe ? probe() : nativeProbe.run());
    } catch {
      return {...base, reason: '无法读取 Windows 虚拟显示器状态。'};
    }
    if (!observed.detected) return {...base, reason: '未检测到 MttVDD 虚拟显示驱动。'};

    let engineOutputs;
    if (getEngineOutputs && observed.outputs.length) {
      try { engineOutputs = await getEngineOutputs(); } catch { engineOutputs = null; }
    }
    const outputs = observed.outputs.map(output => {
      const matches = new Set((Array.isArray(engineOutputs) ? engineOutputs : [])
        .filter(item => typeof item?.display_name === 'string' && typeof item.device_id === 'string' &&
          displayName.test(output.windowsName) && item.display_name.toUpperCase() === output.windowsName.toUpperCase() &&
          deviceId.test(item.device_id))
        .map(item => item.device_id.toLowerCase()));
      return {
        windowsName: output.windowsName,
        modes: supportedModes(output.modes),
        ...(validMode(output.mode) ? {mode: output.mode} : {}),
        ...(matches.size === 1 ? {outputId: [...matches][0]} : {})
      };
    });
    const singleOutput = outputs.length === 1 ? outputs[0] : undefined;
    return {
      ...base, detected: true, enabled: outputs.length > 0, outputs,
      modes: singleOutput?.modes ?? [],
      ...(singleOutput?.outputId ? {outputId: singleOutput.outputId} : {}),
      reason: outputs.length
        ? '已检测到共享虚拟显示器。请在 Windows 或驱动控制面板中启停和修改模式。'
        : '驱动已安装。支持无线副屏的客户端连接时会尝试自动启用虚拟屏。如果驱动尚未提供虚拟屏，请在驱动控制面板中启用一个。'
    };
  }

  return {
    getStatus() {
      pending ??= inspect().finally(() => {pending = undefined;});
      return pending;
    },
    async apply() {
      const error = new Error('SideLantern 无法更改由系统或其他应用管理的虚拟显示器。');
      error.code = 'DISPLAY_NOT_OWNED';
      throw error;
    },
    async teardown() {
      await pending?.catch(() => {});
      await nativeProbe?.dispose();
    }
  };
}

module.exports = {createVirtualDisplay};
