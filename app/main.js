// JAJA Studio — desktop shell (Electron) + bridge to the native JAJA Engine (ASIO / VST3)
const { app, BrowserWindow, session, shell, ipcMain, dialog } = require("electron");
const { spawn } = require("child_process");
const { StringDecoder } = require("string_decoder");
const path = require("path");
const fs = require("fs");

app.setName("JAJA Studio");

let win = null;
let engine = null;

/* ------------------------------------------------------------------ engine process */
function enginePath() {
  const exe = process.platform === "win32" ? "JAJAEngine.exe" : "JAJAEngine";
  const candidates = [
    path.join(process.resourcesPath || "", "engine", exe),   // packaged: resources/engine
    path.join(__dirname, "engine", exe),
    path.join(__dirname, "..", "engine", "build", "JAJAEngine_artefacts", "Release", exe),
  ];
  return candidates.find((p) => { try { return fs.statSync(p).isFile(); } catch (e) { return false; } }) || null;
}
const toUi = (line) => { if (win && !win.isDestroyed()) win.webContents.send("engine:event", line); };

function startEngine() {
  if (engine) return true;
  const exe = enginePath();
  if (!exe) return false;
  const child = spawn(exe, [], { cwd: path.dirname(exe), stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
  const dec = new StringDecoder("utf8");
  let buf = "";
  child.stdout.on("data", (d) => {
    buf += dec.write(d);
    let i;
    while ((i = buf.indexOf("\n")) >= 0) { const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1); if (line) toUi(line); }
  });
  child.stderr.on("data", (d) => console.log("[engine]", d.toString()));
  child.on("error", (e) => toUi(JSON.stringify({ ev: "exit", code: -1, message: e.message })));
  child.on("exit", (code) => { if (engine === child) engine = null; toUi(JSON.stringify({ ev: "exit", code })); });
  child.stdin.on("error", () => {});
  engine = child;
  return true;
}
function stopEngine() {
  const child = engine; if (!child) return;
  engine = null;
  try { child.stdin.write(JSON.stringify({ cmd: "quit" }) + "\n"); child.stdin.end(); } catch (e) {}
  setTimeout(() => { try { child.kill(); } catch (e) {} }, 2500);
}

const mediaDir = () => { const d = path.join(app.getPath("userData"), "media"); fs.mkdirSync(d, { recursive: true }); return d; };
const recordingsDir = () => path.join(app.getPath("documents"), "JAJA Studio");
const inside = (p, dir) => { const r = path.relative(dir, path.resolve(p)); return !!r && !r.startsWith("..") && !path.isAbsolute(r); };

ipcMain.handle("engine:available", () => !!enginePath());
ipcMain.handle("engine:start", () => startEngine());
ipcMain.handle("engine:stop", () => { stopEngine(); return true; });
ipcMain.on("engine:send", (_e, line) => { if (engine && typeof line === "string") { try { engine.stdin.write(line + "\n"); } catch (e) {} } });
// audio that only exists inside the page (imported / opened from a project) is handed to the engine as a WAV file
ipcMain.handle("engine:putAudio", async (_e, id, data) => {
  const safe = String(id).replace(/[^a-zA-Z0-9_-]/g, "").slice(0, 40) || "audio";
  const file = path.join(mediaDir(), safe + ".wav");
  await fs.promises.writeFile(file, Buffer.from(data));
  return file;
});
// recordings made by the engine are read back so the page can draw waveforms and save them in the project
ipcMain.handle("engine:readFile", async (_e, p) => {
  if (!(inside(p, recordingsDir()) || inside(p, mediaDir()))) throw new Error("not allowed");
  return fs.promises.readFile(p);
});
ipcMain.handle("engine:savePath", async (_e, name) => {
  const r = await dialog.showSaveDialog(win, { title: "ส่งออกเสียง (Bounce)", defaultPath: path.join(app.getPath("music"), String(name || "JAJA mix.wav")), filters: [{ name: "WAV", extensions: ["wav"] }] });
  return r.canceled ? null : r.filePath;
});

/* ------------------------------------------------------------------ window */
function createWindow() {
  win = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 960,
    minHeight: 620,
    title: "JAJA Studio",
    backgroundColor: "#141416",
    icon: path.join(__dirname, "icon.png"),
    autoHideMenuBar: true,
    webPreferences: { contextIsolation: true, sandbox: true, preload: path.join(__dirname, "preload.js") },
  });
  win.removeMenu();
  win.maximize();
  win.loadFile(path.join(__dirname, "index.html"));

  // links to websites open in the normal browser, never inside the studio
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:/.test(url)) shell.openExternal(url);
    return { action: "deny" };
  });
  // Ctrl+Shift+I opens developer tools (for troubleshooting)
  win.webContents.on("before-input-event", (e, input) => {
    if (input.control && input.shift && input.key.toLowerCase() === "i") win.webContents.toggleDevTools();
  });
  win.on("closed", () => { win = null; });
}

app.whenReady().then(() => {
  // microphone (recording), MIDI keyboards and choosing the audio output device; nothing else is granted
  const allowed = new Set(["media", "midi", "midiSysex", "speaker-selection"]);
  session.defaultSession.setPermissionRequestHandler((wc, permission, cb) => cb(allowed.has(permission)));
  session.defaultSession.setPermissionCheckHandler((wc, permission) => allowed.has(permission));
  createWindow();
});
app.on("window-all-closed", () => { stopEngine(); app.quit(); });
app.on("before-quit", stopEngine);
