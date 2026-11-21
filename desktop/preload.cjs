const { contextBridge, ipcRenderer } = require('electron');

const invoke = (action, input) => ipcRenderer.invoke(`side-lantern:${action}`, input);

contextBridge.exposeInMainWorld('sideLantern', {
  getState: () => invoke('getState'),
  start: () => invoke('start'),
  stop: () => invoke('stop'),
  saveSettings: value => invoke('saveSettings', value),
  setPreferences: value => invoke('setPreferences', value),
  approvePairing: value => invoke('approvePairing', value),
  cancelPairing: id => invoke('cancelPairing', id),
  unpair: id => invoke('unpair', id),
  getApplications: () => invoke('getApplications'),
  saveApplication: input => invoke('saveApplication', input),
  deleteApplication: input => invoke('deleteApplication', input),
  openLogs: () => invoke('openLogs'),
  pickCoverImage: current => invoke('pickCoverImage', current),
  onState(callback) {
    const listener = (_event, state) => callback(state);
    ipcRenderer.on('side-lantern:state', listener);
    return () => ipcRenderer.removeListener('side-lantern:state', listener);
  }
});
