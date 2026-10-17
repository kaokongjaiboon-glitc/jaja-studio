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

/* ------------------------------------------------------------------ projects on disk
   A project is a folder:  <Song>/<Song>.jaja  +  <Song>/Audio Files/*.wav
   Audio is written to disk as soon as it is recorded or imported (session folder), and copied
   into the project folder on save, so nothing lives only in memory. */
const AUDIO_EXT = /\.(wav|wave|aif|aiff|flac|mp3|ogg|m4a|aac)$/i;
const sessionAudioDir = () => { const d = path.join(app.getPath("userData"), "Session Audio"); fs.mkdirSync(d, { recursive: true }); return d; };
const safeFile = (s) => String(s || "").replace(/[\\/:*?"<>|]+/g, " ").trim().slice(0, 80) || "JAJA";
const recentFile = () => path.join(app.getPath("userData"), "recent.json");
const autosaveFile = () => path.join(app.getPath("userData"), "autosave.json");
async function writeAtomic(file, data) { const tmp = file + ".tmp"; await fs.promises.writeFile(tmp, data); await fs.promises.rename(tmp, file); }
function readRecent() { try { return JSON.parse(fs.readFileSync(recentFile(), "utf8")).filter((r) => fs.existsSync(r.file)).slice(0, 8); } catch (e) { return []; } }
function addRecent(file, name) { const list = [{ file, name, time: Date.now() }, ...readRecent().filter((r) => r.file !== file)].slice(0, 8); try { fs.writeFileSync(recentFile(), JSON.stringify(list)); } catch (e) {} return list; }

ipcMain.handle("fs:writeAudio", async (_e, id, data) => {
  const file = path.join(sessionAudioDir(), String(id).replace(/[^a-zA-Z0-9_-]/g, "").slice(0, 40) + ".wav");
  await writeAtomic(file, Buffer.from(data));
  return file;
});
ipcMain.handle("fs:readAudio", async (_e, p) => {
  if (typeof p !== "string" || !AUDIO_EXT.test(p)) throw new Error("not an audio file");
  return fs.promises.readFile(p);
});
ipcMain.handle("fs:saveProject", async (_e, { file, name, json, media, saveAs }) => {
  if (!file || saveAs) {
    fs.mkdirSync(path.join(app.getPath("documents"), "JAJA Studio"), { recursive: true });
    const r = await dialog.showSaveDialog(win, {
      title: "บันทึกโปรเจกต์ JAJA Studio",
      defaultPath: path.join(app.getPath("documents"), "JAJA Studio", safeFile(name)),
      buttonLabel: "บันทึก",
      properties: ["createDirectory", "showOverwriteConfirmation"],
    });
    if (r.canceled || !r.filePath) return null;
    const base = r.filePath.replace(/\.jaja$/i, "");
    file = path.join(base, path.basename(base) + ".jaja");   // the project gets its own folder
  }
  const dir = path.dirname(file), audioDir = path.join(dir, "Audio Files");
  await fs.promises.mkdir(audioDir, { recursive: true });
  const outMedia = {}, relMedia = {};
  for (const [id, src] of Object.entries(media || {})) {
    if (!src) continue;
    const target = path.join(audioDir, id.replace(/[^a-zA-Z0-9_-]/g, "") + (path.extname(src) || ".wav"));
    try {
      if (path.resolve(src) !== path.resolve(target) && !fs.existsSync(target)) await fs.promises.copyFile(src, target);
      outMedia[id] = target; relMedia[id] = path.relative(dir, target).split(path.sep).join("/");
    } catch (e) { outMedia[id] = src; relMedia[id] = src; }   // keep the old reference if the copy failed
  }
  const proj = JSON.parse(json); proj.media = relMedia;
  if (fs.existsSync(file)) { try { await fs.promises.copyFile(file, file + ".bak"); } catch (e) {} }
  await writeAtomic(file, JSON.stringify(proj));
  return { file, dir, media: outMedia, recent: addRecent(file, proj.name) };
});
ipcMain.handle("fs:openProject", async (_e, file) => {
  if (!file) {
    const r = await dialog.showOpenDialog(win, { title: "เปิดโปรเจกต์", defaultPath: path.join(app.getPath("documents"), "JAJA Studio"), properties: ["openFile"], filters: [{ name: "JAJA Studio", extensions: ["jaja", "zip", "json"] }] });
    if (r.canceled || !r.filePaths[0]) return null;
    file = r.filePaths[0];
  }
  if (!/\.jaja$/i.test(file)) return { kind: "bytes", file, name: path.basename(file), data: await fs.promises.readFile(file) };
  const proj = JSON.parse(await fs.promises.readFile(file, "utf8")), dir = path.dirname(file), media = {}, missing = [];
  for (const [id, rel] of Object.entries(proj.media || {})) {
    const abs = path.isAbsolute(rel) ? rel : path.join(dir, rel);
    if (fs.existsSync(abs)) media[id] = abs; else missing.push(rel);
  }
  proj.media = media;
  return { kind: "jaja", file, dir, json: JSON.stringify(proj), missing, recent: addRecent(file, proj.name) };
});
ipcMain.handle("fs:recent", () => readRecent());
ipcMain.handle("fs:autosave", async (_e, data) => { try { await writeAtomic(autosaveFile(), data); return true; } catch (e) { return false; } });
ipcMain.handle("fs:loadAutosave", async () => { try { return await fs.promises.readFile(autosaveFile(), "utf8"); } catch (e) { return null; } });
ipcMain.handle("fs:showInFolder", (_e, p) => { if (typeof p === "string" && fs.existsSync(p)) shell.showItemInFolder(p); });
ipcMain.handle("fs:pickFolder", async (_e, title) => {
  const r = await dialog.showOpenDialog(win, { title: String(title || "เลือกโฟลเดอร์"), defaultPath: app.getPath("music"), properties: ["openDirectory", "createDirectory"] });
  return r.canceled ? null : r.filePaths[0];
});
ipcMain.handle("fs:writeFileIn", async (_e, dir, name, data) => {
  if (typeof dir !== "string" || !fs.existsSync(dir)) throw new Error("folder not found");
  const file = path.join(dir, safeFile(name).replace(/\.(wav|mid)$/i, "") + (/\.mid$/i.test(name) ? ".mid" : ".wav"));
  await writeAtomic(file, Buffer.from(data));
  return file;
});
ipcMain.handle("fs:saveFile", async (_e, name, data, ext) => {
  const r = await dialog.showSaveDialog(win, { defaultPath: path.join(app.getPath("music"), safeFile(name)), filters: [{ name: ext.toUpperCase(), extensions: [ext] }] });
  if (r.canceled || !r.filePath) return null;
  await writeAtomic(r.filePath, Buffer.from(data));
  return r.filePath;
});

/* small settings (audio setup, panel state) kept in a file so they survive even if the browser storage is lost */
const settingsFile = () => path.join(app.getPath("userData"), "settings.json");
const readSettings = () => { try { return JSON.parse(fs.readFileSync(settingsFile(), "utf8")); } catch (e) { return {}; } };
ipcMain.on("settings:get", (e) => { e.returnValue = readSettings(); });
ipcMain.on("settings:set", (_e, k, v) => {
  try { const s = readSettings(); s[String(k)] = String(v); const f = settingsFile(), t = f + ".tmp"; fs.writeFileSync(t, JSON.stringify(s)); fs.renameSync(t, f); } catch (e) {}
});

/* unsaved changes: ask before closing */
let dirty = false, closing = false;
ipcMain.on("fs:setDirty", (_e, v) => { dirty = !!v; });
ipcMain.on("fs:closeNow", () => { closing = true; if (win) win.close(); });

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
  win.on("close", (e) => {
    if (closing || !dirty) return;
    const choice = dialog.showMessageBoxSync(win, { type: "question", buttons: ["บันทึก", "ไม่บันทึก", "ยกเลิก"], defaultId: 0, cancelId: 2, title: "JAJA Studio", message: "บันทึกการแก้ไขก่อนปิดไหม?", detail: "ถ้าไม่บันทึก การแก้ไขล่าสุดจะยังเปิดต่อได้ครั้งหน้า แต่ไม่ได้อยู่ในไฟล์โปรเจกต์" });
    if (choice === 1) return;
    e.preventDefault();
    if (choice === 0) win.webContents.send("app:saveAndClose");
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
