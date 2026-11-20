const { spawn } = require('node:child_process');
const { closeSync, openSync } = require('node:fs');

let child;
let terminationHelper;
let executable;
let stopping = false;

function tell(message) {
  if (process.connected) process.send(message, () => {});
}

function finish() {
  if (process.connected) process.disconnect();
}

function stop() {
  if (stopping || !child?.pid || child.exitCode !== null || child.signalCode !== null) return;
  stopping = true;
  const force = () => {
    if (child.exitCode === null && child.signalCode === null) child.kill();
  };
  if (process.platform !== 'win32' || !terminationHelper) {
    force();
    return;
  }

  let helper;
  try {
    helper = spawn(terminationHelper, [String(child.pid), executable], {
      windowsHide: true, stdio: 'ignore'
    });
  } catch {
    force();
    return;
  }
  helper.once('error', force);
  helper.once('exit', code => { if (code !== 0) force(); });
  const timer = setTimeout(() => {
    if (helper.exitCode === null) helper.kill();
    force();
  }, 12000);
  child.once('exit', () => {
    clearTimeout(timer);
    if (helper.exitCode === null) helper.kill();
  });
}

process.on('message', message => {
  if (message.type === 'stop') {
    stop();
    return;
  }
  if (child) return;
  try {
    const log = openSync(message.consoleLog, 'a');
    try {
      const env = {...process.env};
      delete env.ELECTRON_RUN_AS_NODE;
      if (message.disableGlobalNvidiaPreferences) {
        // Sunshine skips all NVIDIA profile changes when ProgramData is unavailable.
        for (const key of Object.keys(env)) if (key.toLowerCase() === 'programdata') delete env[key];
      }
      terminationHelper = message.terminationHelper;
      executable = message.executable;
      child = spawn(executable, message.args, {
        cwd: message.cwd, env, stdio: ['ignore', log, log], windowsHide: true
      });
    } finally {
      closeSync(log);
    }
    child.once('spawn', () => tell({type: 'started', pid: child.pid}));
    child.once('error', error => {
      tell({type: 'error', code: error.code ?? 'ENGINE_ERROR'});
      finish();
    });
    child.once('exit', (code, signal) => {
      tell({type: 'exit', code, signal});
      finish();
    });
  } catch (error) {
    tell({type: 'error', code: error.code ?? 'ENGINE_ERROR'});
    finish();
  }
});
process.once('disconnect', stop);
