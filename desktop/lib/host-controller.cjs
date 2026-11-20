const defaultSettings = {encoder: 'auto', maxBitrateMbps: 0, hevc: 'auto', av1: 'auto', audio: true};

function createHostController({open, onChange = () => {}}) {
  let state = {
    phase: 'stopped', error: null, devices: [], pairings: [], legacyPairing: false,
    sessions: null, settings: defaultSettings, outputId: ''
  };
  let current = null;
  let queue = Promise.resolve();
  let refreshInFlight = null;

  function update(changes) {
    const next = {...state, ...changes};
    if (JSON.stringify(next) === JSON.stringify(state)) return;
    state = next;
    onChange();
  }

  async function refresh() {
    if (state.phase !== 'running' || !current) return state;
    const session = current;
    if (refreshInFlight?.session === session) return refreshInFlight.promise;
    const promise = (async () => {
      try {
        const [devices, requests, sessions] = await Promise.all([
          session.client.getDevices(), session.client.getPairings(), session.client.getSessions?.()
        ]);
        if (current === session && state.phase === 'running') {
          update({devices, pairings: requests ?? [], sessions: sessions ?? null, legacyPairing: requests === null, error: null});
        }
      } catch (error) {
        if (current === session && state.phase === 'running') update({sessions: null, error: error.message});
      } finally {
        if (refreshInFlight?.session === session) refreshInFlight = null;
      }
      return state;
    })();
    refreshInFlight = {session, promise};
    return promise;
  }

  async function start() {
    if (state.phase === 'running') return state;
    if (current) throw new Error('上一个测试引擎仍在退出。');
    update({phase: 'starting', error: null});
    try {
      const session = await open();
      current = session;
      session.engine.exited.then(() => {
        if (current !== session || state.phase === 'stopping') return;
        current = null;
        update({phase: 'error', pairings: [], sessions: null, legacyPairing: false,
          error: `测试串流引擎意外停止（${session.engine.failure()}）。请查看日志。`});
      });
      update({phase: 'running', settings: session.settings, outputId: session.outputId ?? ''});
      return await refresh();
    } catch (error) {
      update({phase: 'error', error: error.message});
      throw error;
    }
  }

  async function stop() {
    const session = current;
    if (session) {
      update({phase: 'stopping', error: null, pairings: [], sessions: null, legacyPairing: false});
      try {
        await session.engine.stop();
      } catch (error) {
        update({phase: 'error', error: error.message});
        throw error;
      }
    }
    current = null;
    update({phase: 'stopped', error: null, pairings: [], sessions: null, legacyPairing: false});
    return state;
  }

  function schedule(operation) {
    const result = queue.then(operation);
    queue = result.catch(() => {});
    return result;
  }

  function withClient(callback) {
    if (state.phase !== 'running' || !current) throw new Error('请先启动串流主机。');
    return callback(current.client);
  }

  async function changeDevice(operation) {
    await withClient(operation);
    await refreshInFlight?.promise;
    return refresh();
  }

  return {
    getState: () => state, refresh,
    start: () => schedule(start),
    stop: () => schedule(stop),
    getDisplays: () => current?.client.getDisplays?.(),
    saveSettings: (settings, outputId) => schedule(async () => {
      if (outputId !== undefined) {
        const sessions = await withClient(client => client.getSessions());
        if (!Array.isArray(sessions) || sessions.length) throw new Error('请先结束当前串流，再切换显示器。');
      }
      await withClient(client => client.saveSettings(settings, outputId));
      await stop();
      return start();
    }),
    approvePairing: payload => schedule(() => changeDevice(client => client.approvePairing(payload))),
    cancelPairing: id => schedule(() => changeDevice(client => client.cancelPairing(id))),
    unpair: id => schedule(() => changeDevice(client => client.unpair(id)))
  };
}

module.exports = {createHostController};
