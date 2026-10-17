// Gives the page a small, safe API to the native JAJA Engine (nothing else from Node is exposed)
const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("jajaEngine", {
  available: () => ipcRenderer.invoke("engine:available"),
  start: () => ipcRenderer.invoke("engine:start"),
  stop: () => ipcRenderer.invoke("engine:stop"),
  send: (msg) => ipcRenderer.send("engine:send", JSON.stringify(msg)),
  onEvent: (fn) => ipcRenderer.on("engine:event", (_e, line) => { let m; try { m = JSON.parse(line); } catch (e) { return; } fn(m); }),
  putAudio: (id, data) => ipcRenderer.invoke("engine:putAudio", id, data),
  readFile: (p) => ipcRenderer.invoke("engine:readFile", p),
  savePath: (name) => ipcRenderer.invoke("engine:savePath", name),
});

// project files on disk (save / open / autosave / recent)
contextBridge.exposeInMainWorld("jajaFS", {
  writeAudio: (id, data) => ipcRenderer.invoke("fs:writeAudio", id, data),
  readAudio: (p) => ipcRenderer.invoke("fs:readAudio", p),
  saveProject: (args) => ipcRenderer.invoke("fs:saveProject", args),
  openProject: (file) => ipcRenderer.invoke("fs:openProject", file),
  recent: () => ipcRenderer.invoke("fs:recent"),
  autosave: (data) => ipcRenderer.invoke("fs:autosave", data),
  loadAutosave: () => ipcRenderer.invoke("fs:loadAutosave"),
  showInFolder: (p) => ipcRenderer.invoke("fs:showInFolder", p),
  pickFolder: (title) => ipcRenderer.invoke("fs:pickFolder", title),
  writeFileIn: (dir, name, data) => ipcRenderer.invoke("fs:writeFileIn", dir, name, data),
  saveFile: (name, data, ext) => ipcRenderer.invoke("fs:saveFile", name, data, ext),
  setDirty: (v) => ipcRenderer.send("fs:setDirty", !!v),
  closeNow: () => ipcRenderer.send("fs:closeNow"),
  settingsGet: () => ipcRenderer.sendSync("settings:get"),
  settingsSet: (k, v) => ipcRenderer.send("settings:set", k, v),
  onSaveAndClose: (fn) => ipcRenderer.on("app:saveAndClose", () => fn()),
});
