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
