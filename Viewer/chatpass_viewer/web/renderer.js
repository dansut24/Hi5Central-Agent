"use strict";

/* -----------------------------------------
   UI elements
------------------------------------------ */

const elStatusDot    = document.getElementById("status-dot");
const elStatusLabel  = document.getElementById("status-label");
const elDeviceLabel  = document.getElementById("device-label");
const elBtnFiles     = document.getElementById("btn-files");
const elBtnChat      = document.getElementById("btn-chat");
const elBtnAudio     = document.getElementById("btn-audio");
const elBtnBlockInput = document.getElementById("btn-block-input");
const elBtnBackstage = document.getElementById("btn-backstage");
const elBtnConsole   = document.getElementById("btn-console");
const elBtnStartMenu = document.getElementById("btn-start-menu");
const elBtnCad       = document.getElementById("btn-cad");
const elBtnMonitor   = document.getElementById("btn-monitor");
const elMonitorMenu  = document.getElementById("monitor-menu");
const elBtnDisc      = document.getElementById("btn-disconnect");
const elFilesPanel   = document.getElementById("files-panel");
const elChatPanel    = document.getElementById("chat-panel");
const elFilesClose   = document.getElementById("files-close");
const elChatClose    = document.getElementById("chat-close");
const elFilePath     = document.getElementById("file-path");
const elFileRefresh  = document.getElementById("file-refresh");
const elFileUpload   = document.getElementById("file-upload");
const elFileUploadInput = document.getElementById("file-upload-input");
const elFileMobileStatus = document.getElementById("file-mobile-status");
const elFileList     = document.getElementById("file-list");
const elChatLog      = document.getElementById("chat-log");
const elChatInput    = document.getElementById("chat-input");
const elChatSend     = document.getElementById("chat-send");
const elVideo        = document.getElementById("remote-video");
const elAudio        = document.getElementById("remote-audio");
const elOverlay      = document.getElementById("overlay");
const elOverlayTitle = document.getElementById("overlay-title");
const elOverlaySub   = document.getElementById("overlay-sub");
const elSpinner      = document.getElementById("spinner");
const elErrorDetail  = document.getElementById("error-detail");
const elStatsBar     = document.getElementById("statsbar");
const elStatRes      = document.getElementById("stat-res");
const elStatState    = document.getElementById("stat-state");
const elStatCodec    = document.getElementById("stat-codec");
const elCodecDevBadge = document.getElementById("codec-dev-badge");
const elCodecDevSelect = document.getElementById("codec-dev-select");
const elDiagIceState = document.getElementById("diag-ice-state");
const elDiagConnState = document.getElementById("diag-connection-state");
const elDiagCandidatePair = document.getElementById("diag-candidate-pair");
const elDiagBitrate = document.getElementById("diag-bitrate");
const elDiagFps = document.getElementById("diag-fps");
const elDiagFrames = document.getElementById("diag-frames");
const elDiagPacketsLost = document.getElementById("diag-packets-lost");
const elDiagRtt = document.getElementById("diag-rtt");
const elDiagIceServers = document.getElementById("diag-ice-servers");

if (elVideo) {
  elVideo.autoplay = true;
  elVideo.playsInline = true;
  elVideo.muted = true;
  elVideo.defaultMuted = true;
}

/* -----------------------------------------
   State
------------------------------------------ */

let ws = null;
let pc = null;
let inputDc = null;
let controlDc = null;
let mouseMoveDc = null;
let mouseMoveSeq = 0;
let devCodecSwitchTimer = null;
let devCodecRequested = "auto";
let lastNegotiatedCodecKey = "";

let currentSession = null;
let activeDesktopMode = "console";
let desktopModePending = null;
let backgroundModeLocked = false;
let desktopModeSwitchStartedAt = 0;
let desktopModePendingFrames = 0;
let desktopModeSwitchTimer = null;
let audioEnabled = false;
let localInputBlocked = false;
let remoteDescSet = false;
let pendingRemoteIce = [];

let statsTimer = null;
let transitionWatchdogTimer = null;
let lastStats = { tsMs: 0, bytes: 0, frames: 0, packetsLost: 0, jitterDelay: 0, jitterEmitted: 0 };

let inputBound = false;
let controlActive = false;
const pressedKeys = new Set();
let remoteAltTabActive = false;

let remoteCursorEl = null;
let lastCursorNorm = null;

let remoteMonitors = [];
let currentMonitorIndex = 0;
let monitorMenuOpen = false;
let monitorMenuCloseTimer = null;
let pendingMonitorIndex = null;
let chatMessages = [];
let remoteFileEntries = [];
let remoteFilePath = "/";

/* transition state */
let hasEverRenderedFrame = false;
let lastFrameAtMs = 0;
let lastFramesDecoded = 0;
let passiveOverlayActive = false;
let secureDesktopLikely = false;
let secureDesktopActive = false;
let desktopHandoffActive = false;
let revealOnNextFrame = false;
let monitorSwitchUntilMs = 0;
let overlayMode = "hard";

/* -----------------------------------------
   TURN / ICE config
------------------------------------------ */

const FALLBACK_ICE_SERVERS = [
  { urls: "stun:stun.l.google.com:19302" },
  { urls: "stun:stun1.l.google.com:19302" },
];

function normalizeIceServers(value) {
  if (!Array.isArray(value)) return [...FALLBACK_ICE_SERVERS];
  const safe = value.filter((server) => {
    if (!server || typeof server !== "object") return false;
    const urls = Array.isArray(server.urls) ? server.urls : [server.urls];
    return urls.some((url) => typeof url === "string" && /^(stun|stuns|turn|turns):/i.test(url));
  }).map((server) => ({
    urls: server.urls,
    ...(typeof server.username === "string" ? { username: server.username } : {}),
    ...(typeof server.credential === "string" ? { credential: server.credential } : {})
  }));
  return safe.length ? safe : [...FALLBACK_ICE_SERVERS];
}

function activeIceServers() {
  return normalizeIceServers(currentSession?.iceServers);
}

const FRAME_STALL_MS = 950;
const MONITOR_SWITCH_GRACE_MS = 1800;
const NEGOTIATION_GRACE_MS = 2500;

/* -----------------------------------------
   UI helpers
------------------------------------------ */

function setStatus(dotClass, label) {
  if (elStatusDot) elStatusDot.className = dotClass || "";
  if (elStatusLabel) elStatusLabel.textContent = label || "";
}

function setOverlayMode(mode) {
  overlayMode = mode;
  if (!elOverlay) return;
  elOverlay.classList.toggle("passive", mode === "passive");
  elOverlay.classList.toggle("secure-black", mode === "secure-black");
  elOverlay.classList.toggle("silent", mode === "silent");
  elOverlay.classList.toggle("secure-black-silent", mode === "secure-black-silent");
  elOverlay.classList.toggle("transition-hold", mode === "transition-hold");
}

function showOverlay(title, sub, {
  spinner = false,
  error = "",
  keepVideo = false,
  passive = false,
  secureBlack = false,
  silent = false
} = {}) {
  if (elOverlay) elOverlay.classList.remove("hidden");

  let mode = "hard";
  if (secureBlack && silent) mode = "secure-black-silent";
  else if (secureBlack) mode = "secure-black";
  else if (silent) mode = "silent";
  else if (passive) mode = "passive";

  setOverlayMode(mode);
  passiveOverlayActive = mode === "passive";

  if (elVideo) {
    if (keepVideo) elVideo.classList.add("visible");
    else elVideo.classList.remove("visible");
  }

  if (elStatsBar) {
    if (keepVideo) elStatsBar.classList.add("visible");
    else elStatsBar.classList.remove("visible");
  }

  hideRemoteCursor();
  closeMonitorMenu();

  if (elOverlayTitle) elOverlayTitle.textContent = title || "";
  if (elOverlaySub) elOverlaySub.textContent = sub || "";

  if (elSpinner) elSpinner.style.display = spinner ? "block" : "none";
  if (elErrorDetail) {
    elErrorDetail.style.display = error ? "block" : "none";
    elErrorDetail.textContent = error || "";
  }
}

function showPassiveOverlay() {
  // Intentionally silent: keep the recovery logic, but do not show
  // transitional overlays for monitor switches, stream stalls, handshake,
  // or normal desktop return.
  passiveOverlayActive = false;
}

function showSecureBlackOverlay({ spinner = false } = {}) {
  passiveOverlayActive = false;
  secureDesktopActive = true;
  if (hasEverRenderedFrame) {
    if (elOverlay) {
      elOverlay.classList.remove("hidden", "passive", "secure-black", "secure-black-silent");
      elOverlay.classList.add("silent", "transition-hold");
    }
    if (elVideo) elVideo.classList.add("visible");
    if (elStatsBar) elStatsBar.classList.add("visible");
    hideRemoteCursor();
    return;
  }
  showOverlay("", "", { spinner, keepVideo: false, secureBlack: true, silent: true });
}

function clearSecureDesktopState() {
  secureDesktopActive = false;
  desktopHandoffActive = false;
  secureDesktopLikely = false;
  revealOnNextFrame = false;
}

function hideOverlay() {
  passiveOverlayActive = false;
  if (elOverlay) elOverlay.classList.add("hidden");
  setOverlayMode("hard");
}

function showStream() {
  hideOverlay();
  if (elVideo) elVideo.classList.add("visible");
  if (elStatsBar) elStatsBar.classList.add("visible");
}

function markFrameRendered() {
  hasEverRenderedFrame = true;
  lastFrameAtMs = Date.now();

  if (secureDesktopActive || desktopHandoffActive) {
    return;
  }

  if (desktopModePending) {
    desktopModePendingFrames += 1;
    if (desktopModePendingFrames >= 2 && Date.now() - desktopModeSwitchStartedAt >= 700) {
      completeDesktopModeTransition(desktopModePending);
      return;
    }
  }

  if (revealOnNextFrame) {
    revealOnNextFrame = false;
    secureDesktopLikely = false;
    showStream();
    setStatus("online", "Streaming");
    return;
  }

  if (secureDesktopLikely || passiveOverlayActive) {
    secureDesktopLikely = false;
    showStream();
    setStatus("online", "Streaming");
  }
}

function updateResolution() {
  if (!elVideo) return;
  if (elVideo.videoWidth && elVideo.videoHeight && elStatRes) {
    elStatRes.textContent = `${elVideo.videoWidth}×${elVideo.videoHeight}`;
  }
  refreshRemoteCursorPosition();
}
if (elVideo) elVideo.addEventListener("resize", updateResolution);

function physicalMonitorOrdinal(index) {
  const physicalMonitors = remoteMonitors.filter(m => m.index >= 0);
  return physicalMonitors.findIndex(m => m.index === index) + 1;
}

function getMonitorLabel(monitor) {
  if (!monitor) return "Monitors";
  if (monitor.index === -1) return "All Monitors";
  return `Monitor ${physicalMonitorOrdinal(monitor.index)}`;
}

function updateMonitorButton() {
  if (!elBtnMonitor) return;

  if (!currentSession || remoteMonitors.length === 0) {
    elBtnMonitor.disabled = true;
    elBtnMonitor.textContent = "Monitors";
    return;
  }

  const current = remoteMonitors.find(m => m.index === currentMonitorIndex) || remoteMonitors[0];
  if (!current) {
    elBtnMonitor.disabled = true;
    elBtnMonitor.textContent = "Monitors";
    return;
  }

  elBtnMonitor.textContent = getMonitorLabel(current);
  elBtnMonitor.title = current.name || getMonitorLabel(current);
  elBtnMonitor.disabled = remoteMonitors.length <= 1;
}

function renderMonitorMenu() {
  if (!elMonitorMenu) return;

  const items = remoteMonitors.filter(m => m.index !== currentMonitorIndex);

  elMonitorMenu.innerHTML = "";
  if (items.length === 0) return;

  for (const monitor of items) {
    const btn = document.createElement("button");
    btn.className = "monitor-item";
    btn.type = "button";

    const title = document.createElement("div");
    title.className = "monitor-title";
    title.textContent = getMonitorLabel(monitor);

    const sub = document.createElement("div");
    sub.className = "monitor-sub";
    sub.textContent = monitor.name || `${monitor.w}×${monitor.h}`;

    btn.appendChild(title);
    btn.appendChild(sub);

    btn.addEventListener("click", () => {
      if (!ws || ws.readyState !== WebSocket.OPEN || !currentSession) return;

      pendingMonitorIndex = monitor.index;
      monitorSwitchUntilMs = Date.now() + MONITOR_SWITCH_GRACE_MS;

      ws.send(JSON.stringify({
        type: "switch_monitor",
        session_id: currentSession.sessionId,
        monitor_index: monitor.index
      }));

      closeMonitorMenu();
    });

    elMonitorMenu.appendChild(btn);
  }
}

function clearMonitorMenuCloseTimer() {
  if (monitorMenuCloseTimer) {
    clearTimeout(monitorMenuCloseTimer);
    monitorMenuCloseTimer = null;
  }
}

function scheduleMonitorMenuClose() {
  clearMonitorMenuCloseTimer();
  monitorMenuCloseTimer = setTimeout(() => {
    closeMonitorMenu();
  }, 120);
}

function openMonitorMenu() {
  if (!elMonitorMenu || !currentSession) return;
  renderMonitorMenu();
  if (!elMonitorMenu.children.length) return;
  clearMonitorMenuCloseTimer();
  elMonitorMenu.classList.add("visible");
  monitorMenuOpen = true;
}

function closeMonitorMenu() {
  clearMonitorMenuCloseTimer();
  if (!elMonitorMenu) return;
  elMonitorMenu.classList.remove("visible");
  monitorMenuOpen = false;
}

/* -----------------------------------------
   Remote cursor overlay
------------------------------------------ */

function ensureRemoteCursor() {
  if (remoteCursorEl) return remoteCursorEl;

  remoteCursorEl = document.createElement("div");
  remoteCursorEl.id = "remote-cursor-overlay";
  remoteCursorEl.style.position = "fixed";
  remoteCursorEl.style.left = "0";
  remoteCursorEl.style.top = "0";
  remoteCursorEl.style.width = "28px";
  remoteCursorEl.style.height = "28px";
  remoteCursorEl.style.pointerEvents = "none";
  remoteCursorEl.style.zIndex = "99999";
  remoteCursorEl.style.display = "none";
  remoteCursorEl.style.transform = "translate3d(0, 0, 0)";
  remoteCursorEl.style.willChange = "transform";
  remoteCursorEl.style.filter = "drop-shadow(0 1px 2px rgba(0,0,0,0.5))";

  remoteCursorEl.innerHTML = `
    <svg width="28" height="28" viewBox="0 0 28 28" xmlns="http://www.w3.org/2000/svg" style="display:block;overflow:visible">
      <path d="M2 1 L2 22 L7.8 16.7 L11.6 25.4 L15.3 23.8 L11.5 15.2 L19.2 15.2 Z"
            fill="white" stroke="black" stroke-width="1.35" stroke-linejoin="round"/>
    </svg>
  `;

  document.body.appendChild(remoteCursorEl);
  return remoteCursorEl;
}

function hideRemoteCursor() {
  if (remoteCursorEl) {
    remoteCursorEl.style.display = "none";
  }
}

function moveRemoteCursorByClient(clientX, clientY) {
  const cursor = ensureRemoteCursor();
  cursor.style.display = "block";
  cursor.style.transform = `translate3d(${Math.round(clientX)}px, ${Math.round(clientY)}px, 0)`;
}

async function openFileBrowserWindow() {
  try {
    if (typeof window.hi5OpenFileBrowserWindow === "function") {
      await window.hi5OpenFileBrowserWindow("");
      return true;
    }
    if (typeof window.hi5?.openFileBrowserWindow === "function") {
      await window.hi5.openFileBrowserWindow();
      return true;
    }
  } catch (e) {
    console.warn("[files] native file browser open failed:", e);
  }

  if (elFilesPanel) elFilesPanel.classList.add("visible");
  return false;
}

async function closeFileBrowserWindow() {
  try {
    if (typeof window.hi5CloseFileBrowserWindow === "function") {
      await window.hi5CloseFileBrowserWindow("");
    } else if (typeof window.hi5?.closeFileBrowserWindow === "function") {
      await window.hi5.closeFileBrowserWindow();
    }
  } catch {}
  if (elFilesPanel) elFilesPanel.classList.remove("visible");
}

function postFileToNativeWindow(msg) {
  const json = JSON.stringify(msg || {});
  try {
    if (typeof window.hi5PostFileMessageToWindow === "function") {
      window.hi5PostFileMessageToWindow(json);
    } else if (typeof window.hi5?.postFileMessageToWindow === "function") {
      window.hi5.postFileMessageToWindow(json);
    }
  } catch (e) {
    console.warn("[files] post to native file window failed:", e);
  }
}

function toggleFilesPanel(force) {
  const open = typeof force === "boolean" ? force : true;
  if (open) openFileBrowserWindow(); else closeFileBrowserWindow();
}

function getChatBody(msg) {
  return String(
    msg?.body ??
    msg?.message ??
    msg?.text ??
    msg?.content ??
    ""
  ).trim();
}

function normalizeChatSender(sender) {
  const s = String(sender || "").toLowerCase();
  return (s === "tech" || s === "technician" || s === "viewer" || s === "me") ? "tech" : "user";
}

async function openTechChatWindow() {
  try {
    if (typeof window.hi5OpenChatWindow === "function") {
      await window.hi5OpenChatWindow("");
      syncChatWindow();
      return true;
    }
    if (typeof window.hi5?.openChatWindow === "function") {
      await window.hi5.openChatWindow();
      syncChatWindow();
      return true;
    }
  } catch (e) {
    console.warn("[chat] native open chat failed:", e);
  }

  // Fallback only if the native separate window is unavailable.
  if (elChatPanel) elChatPanel.classList.add("visible");
  renderChat();
  return false;
}

async function closeTechChatWindow() {
  try {
    if (typeof window.hi5CloseChatWindow === "function") {
      await window.hi5CloseChatWindow("");
    } else if (typeof window.hi5?.closeChatWindow === "function") {
      await window.hi5.closeChatWindow();
    }
  } catch {}
  if (elChatPanel) elChatPanel.classList.remove("visible");
}

function postChatToNativeWindow(msg) {
  const body = getChatBody(msg);
  if (!body) return;

  const payload = {
    ...msg,
    sender: normalizeChatSender(msg.sender),
    body,
    message: body,
    text: body
  };

  const json = JSON.stringify(payload);

  try {
    if (typeof window.hi5PostChatMessageToWindow === "function") {
      window.hi5PostChatMessageToWindow(json);
    } else if (typeof window.hi5?.postChatMessageToWindow === "function") {
      window.hi5.postChatMessageToWindow(json);
    }
  } catch (e) {
    console.warn("[chat] post to native chat window failed:", e);
  }
}

function syncChatWindow() {
  for (const msg of chatMessages) {
    postChatToNativeWindow(msg);
  }
}

function toggleChatPanel(force) {
  const open = typeof force === "boolean" ? force : true;
  if (open) {
    openTechChatWindow();
  } else {
    closeTechChatWindow();
  }
}

function renderChat() {
  if (!elChatLog) return;
  elChatLog.innerHTML = "";

  for (const msg of chatMessages) {
    const bodyText = getChatBody(msg);
    if (!bodyText) continue;

    const sender = normalizeChatSender(msg.sender);
    const item = document.createElement("div");
    item.className = `chat-bubble ${sender}`;

    const meta = document.createElement("div");
    meta.className = "chat-meta";
    meta.textContent = msg.display_name || msg.displayName || (sender === "tech" ? "Technician" : "Remote user");

    const body = document.createElement("div");
    body.className = "chat-body";
    body.textContent = bodyText;

    item.appendChild(meta);
    item.appendChild(body);
    elChatLog.appendChild(item);
  }

  elChatLog.scrollTop = elChatLog.scrollHeight;
}

function setMobileFileStatus(text) {
  if (elFileMobileStatus) elFileMobileStatus.textContent = text || "";
}

function joinRemoteFilePath(base, name) {
  base = String(base || "/");
  name = String(name || "").replace(/^[\\/]+/, "");
  if (!base || base === "/") return "/" + name;
  if (/[\\/]$/.test(base)) return base + name;
  return base + (base.includes("\\") ? "\\" : "/") + name;
}

function base64ToBytes(data) {
  const bin = atob(String(data || ""));
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function bytesToBase64(bytes) {
  let bin = "";
  const step = 0x8000;
  for (let i = 0; i < bytes.length; i += step) {
    bin += String.fromCharCode(...bytes.subarray(i, Math.min(bytes.length, i + step)));
  }
  return btoa(bin);
}

function saveBrowserFile(name, parts) {
  const blob = new Blob(parts, { type: "application/octet-stream" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name || "download";
  a.style.display = "none";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}

const browserDownloadPaths = new Set();
const browserChunkDownloads = new Map();

function requestBrowserDownload(entry) {
  const path = String(entry?.path || "");
  if (!path) return;
  browserDownloadPaths.add(path);
  setMobileFileStatus(`Downloading ${entry.name || "file"}…`);
  if (!sendRemoteFileRequest("remote_file_download_request", { path })) {
    browserDownloadPaths.delete(path);
    setMobileFileStatus("Could not start download.");
  }
}

async function uploadBrowserFiles(files) {
  const list = Array.from(files || []);
  if (!list.length) return;
  const inlineBytes = 384 * 1024;
  const chunkBytes = 48 * 1024;
  for (const file of list) {
    const dest = joinRemoteFilePath(remoteFilePath, file.name);
    setMobileFileStatus(`Uploading ${file.name}…`);
    if (file.size <= inlineBytes) {
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (!sendRemoteFileRequest("remote_file_upload_request", { path: dest, data: bytesToBase64(bytes) })) {
        setMobileFileStatus(`Could not upload ${file.name}.`);
        break;
      }
      continue;
    }
    const transferId = `ul-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    if (!sendRemoteFileRequest("remote_file_upload_start", { transfer_id: transferId, path: dest, name: file.name, size: file.size })) {
      setMobileFileStatus(`Could not start upload for ${file.name}.`);
      break;
    }
    let offset = 0;
    let chunkIndex = 0;
    while (offset < file.size) {
      const end = Math.min(file.size, offset + chunkBytes);
      const bytes = new Uint8Array(await file.slice(offset, end).arrayBuffer());
      if (!sendRemoteFileRequest("remote_file_upload_chunk", { transfer_id: transferId, path: dest, chunk_index: chunkIndex++, offset, size: bytes.length, data: bytesToBase64(bytes) })) {
        setMobileFileStatus(`Upload interrupted for ${file.name}.`);
        return;
      }
      offset = end;
      setMobileFileStatus(`Uploading ${file.name}… ${Math.round(offset / file.size * 100)}%`);
      if ((chunkIndex % 8) === 0) await new Promise(resolve => setTimeout(resolve, 0));
    }
    sendRemoteFileRequest("remote_file_upload_complete_request", { transfer_id: transferId, path: dest, size: file.size });
  }
}

function renderRemoteFiles() {
  if (!elFileList) return;
  elFileList.innerHTML = "";
  for (const entry of remoteFileEntries) {
    const row = document.createElement("div");
    row.className = "file-entry";
    const main = document.createElement("div");
    main.className = "file-main";
    const title = document.createElement("div");
    title.textContent = `${entry.is_dir ? "📁" : "📄"} ${entry.name}`;
    const meta = document.createElement("div");
    meta.className = "file-meta";
    meta.textContent = entry.is_dir ? "Folder" : `${entry.size || 0} bytes`;
    main.appendChild(title);
    main.appendChild(meta);
    row.appendChild(main);
    if (entry.is_dir) {
      row.addEventListener("click", () => requestRemoteFileList(entry.path || entry.name));
    } else {
      const dl = document.createElement("button");
      dl.className = "file-download";
      dl.type = "button";
      dl.textContent = "Download";
      dl.addEventListener("click", (ev) => { ev.stopPropagation(); requestBrowserDownload(entry); });
      row.appendChild(dl);
    }
    elFileList.appendChild(row);
  }
}

function sendRemoteFileRequest(fileType, payload = {}) {
  if (!ws || ws.readyState !== WebSocket.OPEN || !currentSession) return false;

  const directPayload = {
    type: fileType,
    session_id: currentSession.sessionId,
    sessionId: currentSession.sessionId,
    ...payload
  };

  // Direct file-browser path only. The control server now forwards remote_file_*
  // and file_transfer_* messages, so do not tunnel through chat_message.
  try { ws.send(JSON.stringify(directPayload)); } catch { return false; }
  return true;
}

function normalizeFileMessage(msg) {
  if (!msg) return null;
  if (msg.channel === "file_browser") {
    const nested = msg.payload && typeof msg.payload === "object" ? msg.payload : {};
    return { ...msg, ...nested, type: msg.file_type || msg.fileType || nested.type };
  }
  return msg;
}

function requestRemoteFileList(path) {
  remoteFilePath = path || "/";
  if (elFilePath) elFilePath.value = remoteFilePath;
  sendRemoteFileRequest("remote_file_list_request", { path: remoteFilePath });
}

const pendingRemoteDownloads = new Map();

window.__hi5FilesRequestRemoteList = function(path) {
  requestRemoteFileList(path || "/");
};

window.__hi5FilesUploadRemote = function(req) {
  if (!ws || ws.readyState !== WebSocket.OPEN || !currentSession || !req) return false;
  return sendRemoteFileRequest("remote_file_upload_request", { path: req.path, data: req.data || "" });
};

window.__hi5FilesDownloadRemote = function(req) {
  if (!ws || ws.readyState !== WebSocket.OPEN || !currentSession || !req) return false;
  if (req.local_dir) pendingRemoteDownloads.set(String(req.path || ""), String(req.local_dir));
  return sendRemoteFileRequest("remote_file_download_request", { path: req.path });
};

window.__hi5FilesDeleteRemote = function(path) {
  if (!ws || ws.readyState !== WebSocket.OPEN || !currentSession) return false;
  return sendRemoteFileRequest("remote_file_delete_request", { path });
};

window.__hi5FilesMkdirRemote = function(path) {
  if (!ws || ws.readyState !== WebSocket.OPEN || !currentSession) return false;
  return sendRemoteFileRequest("remote_file_mkdir_request", { path });
};

window.__hi5FilesRenameRemote = function(req) {
  if (!ws || ws.readyState !== WebSocket.OPEN || !currentSession || !req) return false;
  return sendRemoteFileRequest("remote_file_rename_request", { from: req.from, to: req.to });
};

function sendChatMessageBody(body) {
  body = String(body || "").trim();
  if (!body || !ws || ws.readyState !== WebSocket.OPEN || !currentSession) return false;

  const messageId = `chat-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const payload = {
    type: "chat_message",
    session_id: currentSession.sessionId,
    sessionId: currentSession.sessionId,
    message_id: messageId,
    messageId: messageId,
    sender: "tech",
    display_name: "Technician",
    displayName: "Technician",
    body,
    message: body,
    text: body
  };

  ws.send(JSON.stringify(payload));

  chatMessages.push(payload);
  renderChat();
  postChatToNativeWindow(payload);
  return true;
}

function sendChatMessage() {
  const body = (elChatInput?.value || "").trim();
  if (!sendChatMessageBody(body)) return;
  if (elChatInput) {
    elChatInput.value = "";
    try { elChatInput.focus(); } catch {}
  }
}

window.__hi5ChatSendFromNative = function(body) {
  return sendChatMessageBody(body);
};

function showRemoteCursor() {
  const el = ensureRemoteCursor();
  el.style.display = "block";
}

function getVideoContentRect(el) {
  const rect = el.getBoundingClientRect();
  const objectFit = String(getComputedStyle(el).objectFit || "fill").toLowerCase();

  // Stretch mode renders the decoded frame across the full element. Input
  // coordinates must use that same rectangle or clicks drift horizontally /
  // vertically even though the synthetic cursor looks correct.
  if (objectFit === "fill") {
    return { left: rect.left, top: rect.top, width: rect.width, height: rect.height };
  }

  const vw = el.videoWidth || 1;
  const vh = el.videoHeight || 1;
  const elementAspect = rect.width / rect.height;
  const videoAspect = vw / vh;

  let drawWidth, drawHeight, offsetX, offsetY;
  if (elementAspect > videoAspect) {
    drawHeight = rect.height;
    drawWidth = drawHeight * videoAspect;
    offsetX = (rect.width - drawWidth) / 2;
    offsetY = 0;
  } else {
    drawWidth = rect.width;
    drawHeight = drawWidth / videoAspect;
    offsetX = 0;
    offsetY = (rect.height - drawHeight) / 2;
  }

  return {
    left: rect.left + offsetX,
    top: rect.top + offsetY,
    width: drawWidth,
    height: drawHeight
  };
}

function moveRemoteCursorByNorm(xNorm, yNorm) {
  if (!elVideo || !elVideo.videoWidth || !elVideo.videoHeight) return;

  lastCursorNorm = {
    x_norm: Math.max(0, Math.min(1, xNorm)),
    y_norm: Math.max(0, Math.min(1, yNorm))
  };

  const r = getVideoContentRect(elVideo);
  const x = r.left + (lastCursorNorm.x_norm * r.width);
  const y = r.top + (lastCursorNorm.y_norm * r.height);

  moveRemoteCursorByClient(x, y);
}

function refreshRemoteCursorPosition() {
  if (!lastCursorNorm) return;
  moveRemoteCursorByNorm(lastCursorNorm.x_norm, lastCursorNorm.y_norm);
}

/* -----------------------------------------
   Cleanup / control mode
------------------------------------------ */

function stopStatsPoll() {
  if (statsTimer) {
    clearInterval(statsTimer);
    statsTimer = null;
  }
  if (transitionWatchdogTimer) {
    clearInterval(transitionWatchdogTimer);
    transitionWatchdogTimer = null;
  }
  lastStats = { tsMs: 0, bytes: 0, frames: 0, packetsLost: 0, jitterDelay: 0, jitterEmitted: 0 };
  lastFramesDecoded = 0;
}

function startTransitionWatchdog() {
  if (transitionWatchdogTimer) return;

  transitionWatchdogTimer = setInterval(() => {
    if (!pc || pc.connectionState !== "connected") return;
    if (!hasEverRenderedFrame) return;

    const now = Date.now();

    if (monitorSwitchUntilMs && now < monitorSwitchUntilMs) {
      return;
    }

    if (secureDesktopActive || desktopHandoffActive) {
      return;
    }

    if (now - lastFrameAtMs > FRAME_STALL_MS) {
      secureDesktopLikely = true;
    }
  }, 250);
}

function enterRemoteControlMode() {
  controlActive = true;
  if (elVideo) {
    elVideo.style.cursor = "none";
    try { elVideo.focus(); } catch {}
  }
  showRemoteCursor();
  refreshRemoteCursorPosition();
}

function sendFastMouseMove(extra = {}) {
  if (!mouseMoveDc || mouseMoveDc.readyState !== "open") return false;
  const x = Number(extra.x_norm);
  const y = Number(extra.y_norm);
  if (!Number.isFinite(x) || !Number.isFinite(y)) return false;

  try {
    const buffer = new ArrayBuffer(21);
    const view = new DataView(buffer);
    mouseMoveSeq = (mouseMoveSeq + 1) >>> 0;
    view.setUint8(0, 1);
    view.setUint32(1, mouseMoveSeq, true);
    view.setFloat32(5, Math.max(0, Math.min(1, x)), true);
    view.setFloat32(9, Math.max(0, Math.min(1, y)), true);
    view.setFloat64(13, performance.now(), true);
    mouseMoveDc.send(buffer);
    return true;
  } catch {
    return false;
  }
}

function sendInput(kind, extra = {}, force = false) {
  if (!currentSession) return;
  if (!force && !controlActive) return;

  if (kind === "mouse_move" && sendFastMouseMove(extra)) return;

  const payload = JSON.stringify({ kind, ...extra });

  if (inputDc && inputDc.readyState === "open") {
    inputDc.send(payload);
    return;
  }

  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({
      type: "input_event",
      kind,
      session_id: currentSession.sessionId,
      ...extra,
    }));
  }
}

function normalizeDesktopMode(mode) {
  const value = String(mode || "console").toLowerCase();
  return (value === "backstage" || value === "background" || value === "background_mode") ? "backstage" : "console";
}

function updateDesktopModeButtons() {
  const connected = !!currentSession;
  const pending = !!desktopModePending;
  const backstageActive = activeDesktopMode === "backstage";
  const lockedMode = normalizeDesktopMode(currentSession?.launchMode || activeDesktopMode);
  const backstageSession = lockedMode === "backstage";

  if (elBtnBackstage) {
    elBtnBackstage.hidden = connected && !backstageSession;
    elBtnBackstage.classList.toggle("session-toggle-active", backstageActive);
    elBtnBackstage.disabled = !connected || pending || true;
    elBtnBackstage.title = backstageSession ? "Background Desktop session" : "Background Desktop is not authorised for this session";
  }
  if (elBtnConsole) {
    elBtnConsole.hidden = connected && backstageSession;
    elBtnConsole.classList.toggle("session-toggle-active", !backstageActive);
    elBtnConsole.disabled = !connected || pending || true;
    elBtnConsole.title = backstageSession ? "Console Desktop is not authorised for this session" : "Console Desktop session";
  }
  if (elBtnStartMenu) {
    elBtnStartMenu.disabled = !connected || pending;
    elBtnStartMenu.innerHTML = backstageActive
      ? '▦<span class="label">Apps</span>'
      : '⊞<span class="label">Start</span>';
    elBtnStartMenu.title = backstageActive ? "Background apps" : "Start Menu";
    elBtnStartMenu.setAttribute("aria-label", backstageActive ? "Background apps" : "Start Menu");
  }
}

function setDesktopModePending(mode) {
  desktopModePending = normalizeDesktopMode(mode);
  desktopModeSwitchStartedAt = Date.now();
  desktopModePendingFrames = 0;
  if (desktopModeSwitchTimer) clearTimeout(desktopModeSwitchTimer);
  const requested = desktopModePending;
  desktopModeSwitchTimer = setTimeout(() => {
    desktopModeSwitchTimer = null;
    if (desktopModePending === requested) {
      desktopModePending = null;
      updateDesktopModeButtons();
      hideOverlay();
      setStatus("online", activeDesktopMode === "backstage" ? "Background Desktop" : "Console Desktop");
    }
  }, 7000);
  updateDesktopModeButtons();
}

function setActiveDesktopMode(mode) {
  activeDesktopMode = normalizeDesktopMode(mode);
  desktopModePending = null;
  desktopModeSwitchStartedAt = 0;
  desktopModePendingFrames = 0;
  if (desktopModeSwitchTimer) clearTimeout(desktopModeSwitchTimer);
  desktopModeSwitchTimer = null;
  updateDesktopModeButtons();
}

function completeDesktopModeTransition(mode) {
  const resolved = normalizeDesktopMode(mode);
  setActiveDesktopMode(resolved);
  revealOnNextFrame = false;
  secureDesktopLikely = false;
  hideOverlay();
  if (elVideo) elVideo.classList.add("visible");
  if (elStatsBar) elStatsBar.classList.add("visible");
  setStatus("online", resolved === "backstage" ? "Background Desktop" : "Console Desktop");
}

function reconcileDesktopModeFromDiagnostics(msg) {
  if (!msg || typeof msg.backstage !== "boolean" || !desktopModePending) return;
  const observed = msg.backstage ? "backstage" : "console";
  if (desktopModePending === observed) completeDesktopModeTransition(observed);
}

function handleDesktopSessionState(state) {
  if (state === "backstage_entering") {
    setDesktopModePending("backstage");
    hideOverlay();
    setStatus("online", "Switching to Background…");
    return true;
  }
  if (state === "backstage_ready") {
    completeDesktopModeTransition("backstage");
    return true;
  }
  if (state === "console_entering") {
    setDesktopModePending("console");
    hideOverlay();
    setStatus("online", "Returning to Console…");
    return true;
  }
  if (state === "console_ready") {
    completeDesktopModeTransition("console");
    return true;
  }
  if (state === "backstage_failed" || state === "console_failed") {
    desktopModePending = null;
    if (desktopModeSwitchTimer) clearTimeout(desktopModeSwitchTimer);
    desktopModeSwitchTimer = null;
    updateDesktopModeButtons();
    hideOverlay();
    setStatus("error", state === "backstage_failed" ? "Background switch failed" : "Console switch failed");
    return true;
  }
  return false;
}

function updateSessionToggleButtons() {
  if (elBtnAudio) {
    elBtnAudio.classList.toggle("session-toggle-active", audioEnabled);
    elBtnAudio.innerHTML = `${audioEnabled ? "🔊" : "🔇"}<span class="label">Audio</span>`;
    elBtnAudio.title = audioEnabled ? "Mute remote audio" : "Play remote audio";
  }
  if (elBtnBlockInput) {
    elBtnBlockInput.classList.toggle("session-toggle-active", localInputBlocked);
    elBtnBlockInput.innerHTML = `${localInputBlocked ? "🔒" : "🔓"}<span class="label">User input</span>`;
    elBtnBlockInput.title = localInputBlocked
      ? "Allow the local user's keyboard and mouse"
      : "Block the local user's keyboard and mouse";
  }
}

function setRemoteAudioEnabled(enabled) {
  audioEnabled = !!enabled;
  if (elAudio) {
    elAudio.muted = !audioEnabled;
    elAudio.defaultMuted = !audioEnabled;
    if (audioEnabled) elAudio.play().catch(() => {});
  }
  updateSessionToggleButtons();
}

function setLocalInputBlocked(blocked, notifyAgent = true) {
  localInputBlocked = !!blocked;
  if (notifyAgent && currentSession) {
    sendInput("local_input_block", { blocked: localInputBlocked }, true);
  }
  updateSessionToggleButtons();
}

function sendShortcut(action) {
  if (!currentSession) return false;
  enterRemoteControlMode();

  // Ctrl+Alt+Del is special: send a dedicated service-side command first,
  // then also send the normal shortcut as a nested/VM fallback.
  if (action === "ctrl_alt_del" || action === "ctrl_alt_del_service" || action === "sas") {
    const servicePayload = {
      type: "service_shortcut",
      kind: "service_shortcut",
      action: "ctrl_alt_del_service",
      session_id: currentSession.sessionId,
    };

    try {
      if (inputDc && inputDc.readyState === "open") {
        inputDc.send(JSON.stringify(servicePayload));
        return true;
      }
    } catch {}

    try {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(servicePayload));
        return true;
      }
    } catch {}

    return false;
  }

  sendInput("shortcut", { action }, true);
  return true;
}

const NON_TEXT_KEYS = new Set([
  "Alt", "AltGraph", "CapsLock", "Control", "Dead", "Delete", "End",
  "Enter", "Escape", "Fn", "FnLock", "Home", "Hyper", "Insert",
  "Meta", "NumLock", "OS", "PageDown", "PageUp", "Process",
  "ScrollLock", "Shift", "Super", "Symbol", "SymbolLock",
  "Tab", "Unidentified", "ContextMenu", "Pause", "PrintScreen"
]);

function isModifierCode(code) {
  return code === "ShiftLeft" || code === "ShiftRight" ||
    code === "ControlLeft" || code === "ControlRight" ||
    code === "AltLeft" || code === "AltRight" ||
    code === "MetaLeft" || code === "MetaRight";
}

function isPrintableKey(ev) {
  if (!ev || ev.ctrlKey || ev.altKey || ev.metaKey) return false;
  if (isModifierCode(ev.code)) return false;
  if (typeof ev.key !== "string") return false;
  if (ev.key.length === 0) return false;
  if (NON_TEXT_KEYS.has(ev.key)) return false;
  // Printable characters, including non-ASCII and composed characters.
  // Require a single Unicode character here; named keys like "Shift"/"Alt"
  // must never be injected as text.
  return Array.from(ev.key).length === 1;
}

window.__hi5NativeShortcut = function(action) {
  try {
    if (typeof action === "string" && action.length > 0) {
      releaseAllKeys();
      sendShortcut(action);
      return true;
    }
  } catch (err) {
    console.warn("[viewer] native shortcut dispatch failed", err);
  }
  return false;
};

function sendBackstageMode(enabled) {
  if (!currentSession) return false;

  const targetMode = enabled ? "backstage" : "console";
  const launchMode = normalizeDesktopMode(currentSession.launchMode || activeDesktopMode);
  if (targetMode !== launchMode) {
    console.warn("[viewer] immutable remote session mode rejected switch", { launchMode, targetMode });
    return false;
  }
  if (!enabled && backgroundModeLocked) return false;
  if (!desktopModePending && activeDesktopMode === targetMode) return false;

  const type = enabled ? "backstage_start" : "backstage_stop";
  const payload = JSON.stringify({
    type,
    kind: type,
    session_id: currentSession.sessionId,
  });

  let sent = false;

  // Prefer the established WebRTC data channel. The control server may not
  // forward new viewer->agent message types, but the data channel goes direct
  // to the agent's active session input handler.
  if (inputDc && inputDc.readyState === "open") {
    try {
      inputDc.send(payload);
      sent = true;
      console.log("[backstage] sent over datachannel", { type });
    } catch (err) {
      console.warn("[backstage] datachannel send failed", err);
    }
  }

  // Fallback for older agents/control servers.
  if (!sent && ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type, session_id: currentSession.sessionId }));
    sent = true;
    console.log("[backstage] sent over websocket fallback", { type });
  }

  if (!sent) return false;

  setDesktopModePending(targetMode);
  hideOverlay();
  if (elVideo) elVideo.classList.add("visible");
  setStatus("online", enabled ? "Switching to Background…" : "Returning to Console…");
  return true;
}

function releaseAllKeys() {
  if (!currentSession) {
    pressedKeys.clear();
    return;
  }

  for (const code of Array.from(pressedKeys)) {
    sendInput("key_up", { code }, true);
  }
  pressedKeys.clear();
}

function leaveRemoteControlMode() {
  releaseAllKeys();
  controlActive = false;

  if (elVideo) {
    elVideo.style.cursor = "default";
  }
  hideRemoteCursor();
}

function resetTransitionState() {
  hasEverRenderedFrame = false;
  lastFrameAtMs = 0;
  lastFramesDecoded = 0;
  passiveOverlayActive = false;
  secureDesktopLikely = false;
  secureDesktopActive = false;
  desktopHandoffActive = false;
  revealOnNextFrame = false;
  monitorSwitchUntilMs = 0;
  overlayMode = "hard";
}

function disconnect(reason, options = {}) {
  const silent = !!options.silent;
  const closeNative = !!options.closeNative;
  console.log("[viewer] disconnect called:", reason || "(none)", silent ? "silent" : "", closeNative ? "close-native" : "");
  stopStatsPoll();
  setViewerCodecLabel("—");

  remoteDescSet = false;
  pendingRemoteIce = [];
  lastCursorNorm = null;

  remoteMonitors = [];
  currentMonitorIndex = 0;
  pendingMonitorIndex = null;
  updateMonitorButton();
  closeMonitorMenu();

  leaveRemoteControlMode();
  hideRemoteCursor();
  resetTransitionState();

  if (localInputBlocked && currentSession) {
    try { sendInput("local_input_block", { blocked: false }, true); } catch {}
    localInputBlocked = false;
  }
  setRemoteAudioEnabled(false);
  updateSessionToggleButtons();

  if (!silent && ws && ws.readyState === WebSocket.OPEN && currentSession) {
    try {
      ws.send(JSON.stringify({
        type: "chat_close",
        session_id: currentSession.sessionId,
        close_remote: true,
        reason: reason || "viewer_disconnect"
      }));
    } catch {}
    try {
      ws.send(JSON.stringify({
        type: "viewer_disconnected",
        session_id: currentSession.sessionId
      }));
    } catch {}
  }

  try { closeTechChatWindow(); } catch {}

  if (inputDc) {
    try { inputDc.close(); } catch {}
    inputDc = null;
  }
  if (controlDc) {
    try { controlDc.close(); } catch {}
    controlDc = null;
  }
  if (mouseMoveDc) {
    try { mouseMoveDc.close(); } catch {}
    mouseMoveDc = null;
  }
  mouseMoveSeq = 0;
  if (devCodecSwitchTimer) clearTimeout(devCodecSwitchTimer);
  devCodecSwitchTimer = null;
  devCodecRequested = "auto";
  lastNegotiatedCodecKey = "";
  if (elCodecDevSelect) {
    elCodecDevSelect.value = "auto";
    elCodecDevSelect.disabled = true;
    elCodecDevSelect.title = "Development codec override";
  }

  if (pc) {
    try { pc.close(); } catch {}
    pc = null;
  }
  if (ws) {
    try { ws.onopen = null; ws.onmessage = null; ws.onerror = null; ws.onclose = null; } catch {}
    try { ws.close(); } catch {}
    ws = null;
  }

  if (elVideo) {
    try { elVideo.pause(); } catch {}
    elVideo.srcObject = null;
    elVideo.classList.remove("visible");
  }

  if (elBtnDisc) elBtnDisc.disabled = true;
  if (elBtnFiles) elBtnFiles.disabled = true;
  if (elBtnChat) elBtnChat.disabled = true;
  if (elBtnAudio) elBtnAudio.disabled = true;
  if (elBtnBlockInput) elBtnBlockInput.disabled = true;
  activeDesktopMode = "console";
  desktopModePending = null;
  backgroundModeLocked = false;
  desktopModeSwitchStartedAt = 0;
  desktopModePendingFrames = 0;
  if (desktopModeSwitchTimer) clearTimeout(desktopModeSwitchTimer);
  desktopModeSwitchTimer = null;
  updateDesktopModeButtons();
  if (elBtnStartMenu) elBtnStartMenu.disabled = true;
  if (elBtnCad) elBtnCad.disabled = true;
  if (elDeviceLabel) elDeviceLabel.textContent = "";
  currentSession = null;

  setStatus("", reason || "Disconnected");
  showOverlay(
    reason ? "Disconnected" : "Hi5Central Viewer",
    reason || "Launch this app from Hi5Central to start a remote desktop session."
  );

  if (!silent) {
    try { window.hi5?.notifyDisconnected?.(); } catch {}
  }
  if (closeNative) {
    try { window.hi5?.closeViewer?.(); } catch {}
  }
}

window.__hi5NativeCloseRequested = function() {
  disconnect("Disconnected by technician", { closeNative: true });
};

if (elBtnDisc) {
  elBtnDisc.addEventListener("click", () => disconnect("Disconnected by technician", { closeNative: true }));
}
if (elBtnAudio) {
  elBtnAudio.addEventListener("click", () => setRemoteAudioEnabled(!audioEnabled));
}
if (elBtnBlockInput) {
  elBtnBlockInput.addEventListener("click", () => setLocalInputBlocked(!localInputBlocked, true));
}

if (elBtnMonitor) {
  elBtnMonitor.addEventListener("click", (ev) => {
    ev.stopPropagation();
    if (monitorMenuOpen) {
      closeMonitorMenu();
    } else {
      openMonitorMenu();
    }
  });

  elBtnMonitor.addEventListener("mouseenter", () => {
    if (currentSession && remoteMonitors.length > 1) {
      openMonitorMenu();
    }
  });

  elBtnMonitor.addEventListener("mouseleave", () => {
    scheduleMonitorMenuClose();
  });
}

if (elMonitorMenu) {
  elMonitorMenu.addEventListener("mouseenter", () => {
    clearMonitorMenuCloseTimer();
  });

  elMonitorMenu.addEventListener("mouseleave", () => {
    scheduleMonitorMenuClose();
  });
}

document.addEventListener("click", (ev) => {
  if (!monitorMenuOpen) return;
  if (elMonitorMenu?.contains(ev.target)) return;
  if (elBtnMonitor?.contains(ev.target)) return;
  closeMonitorMenu();
});

/* -----------------------------------------
   Stats
------------------------------------------ */

function startStatsPoll() {
  if (statsTimer) return;
  statsTimer = setInterval(() => pollStatsOnce().catch(() => {}), 1000);
  startTransitionWatchdog();
}

function fmtKbps(kbps) {
  if (!isFinite(kbps) || kbps < 0) return "—";
  if (kbps >= 1000) return `${(kbps / 1000).toFixed(2)} Mbps`;
  return `${Math.round(kbps)} kbps`;
}

async function pollStatsOnce() {
  if (!pc) return;
  const stats = await pc.getStats();

  let inbound = null;
  let selectedPair = null;

  stats.forEach((r) => {
    if (r.type === "inbound-rtp") {
      const isVideo = (r.kind === "video") || (r.mediaType === "video");
      if (isVideo) inbound = r;
    }
    if (r.type === "candidate-pair" && r.nominated && (r.state === "succeeded" || r.state === "in-progress")) {
      selectedPair = r;
    }
  });

  const nowMs = Date.now();

  let bitrateKbps = NaN;
  let fps = NaN;
  let framesDecoded = null;
  let packetsLost = null;
  let jitterMs = null;
  let jitterBufferMs = null;

  if (inbound) {
    const bytesReceived = Number(inbound.bytesReceived || 0);
    framesDecoded = Number(inbound.framesDecoded || 0);
    packetsLost = Number(inbound.packetsLost || 0);
    jitterMs = Number.isFinite(Number(inbound.jitter)) ? Number(inbound.jitter) * 1000 : null;
    const jitterDelay = Number(inbound.jitterBufferDelay || 0);
    const jitterEmitted = Number(inbound.jitterBufferEmittedCount || 0);
    if (jitterEmitted > lastStats.jitterEmitted) {
      const emittedDelta = jitterEmitted - lastStats.jitterEmitted;
      const delayDelta = jitterDelay - lastStats.jitterDelay;
      if (emittedDelta > 0 && delayDelta >= 0) {
        jitterBufferMs = (delayDelta / emittedDelta) * 1000;
      }
    }

    if (framesDecoded > lastFramesDecoded) {
      lastFramesDecoded = framesDecoded;
      markFrameRendered();
      if (monitorSwitchUntilMs && nowMs >= monitorSwitchUntilMs) {
        monitorSwitchUntilMs = 0;
      }
    }

    if (lastStats.tsMs) {
      const dt = (nowMs - lastStats.tsMs) / 1000;
      if (dt > 0.2) {
        const dBytes = bytesReceived - lastStats.bytes;
        bitrateKbps = (dBytes * 8) / dt / 1000;

        const dFrames = framesDecoded - lastStats.frames;
        fps = dFrames / dt;
      }
    }

    lastStats.tsMs = nowMs;
    lastStats.bytes = bytesReceived;
    lastStats.frames = framesDecoded;
    lastStats.packetsLost = packetsLost;
    lastStats.jitterDelay = jitterDelay;
    lastStats.jitterEmitted = jitterEmitted;
  }

  const rttMs = selectedPair && isFinite(selectedPair.currentRoundTripTime)
    ? Math.round(selectedPair.currentRoundTripTime * 1000)
    : null;

  sendInput("viewer_diagnostics", {
    rtt_ms: rttMs ?? 0,
    jitter_ms: Number.isFinite(jitterMs) ? jitterMs : 0,
    jitter_buffer_ms: Number.isFinite(jitterBufferMs) ? jitterBufferMs : 0,
    bitrate_kbps: Number.isFinite(bitrateKbps) ? bitrateKbps : 0,
    fps: Number.isFinite(fps) ? fps : 0,
    packets_lost: packetsLost ?? 0,
  }, true);

  const state = pc.connectionState || pc.iceConnectionState || "—";
  const brStr  = fmtKbps(bitrateKbps);
  const fpsStr = isFinite(fps) ? `${Math.round(fps)} fps` : "— fps";
  const frmStr = (framesDecoded != null) ? `frames ${framesDecoded}` : "frames —";
  const lossStr = (packetsLost != null) ? `lost ${packetsLost}` : "lost —";
  const rttStr = (rttMs != null) ? `rtt ${rttMs}ms` : "rtt —";

  if (elStatState) {
    elStatState.textContent = `${state} · ${brStr} · ${fpsStr} · ${frmStr} · ${lossStr} · ${rttStr}`;
  }
  if (elDiagIceState) elDiagIceState.textContent = pc.iceConnectionState || "—";
  if (elDiagConnState) elDiagConnState.textContent = pc.connectionState || "—";
  if (elDiagCandidatePair) elDiagCandidatePair.textContent = selectedPair ? `${selectedPair.localCandidateId || "local"} → ${selectedPair.remoteCandidateId || "remote"}` : "—";
  if (elDiagBitrate) elDiagBitrate.textContent = brStr;
  if (elDiagFps) elDiagFps.textContent = fpsStr;
  if (elDiagFrames) elDiagFrames.textContent = String(framesDecoded ?? "—");
  if (elDiagPacketsLost) elDiagPacketsLost.textContent = String(packetsLost ?? "—");
  if (elDiagRtt) elDiagRtt.textContent = rttMs != null ? `${rttMs}ms` : "—";
  if (elDiagIceServers) elDiagIceServers.textContent = activeIceServers().map(s => Array.isArray(s.urls) ? s.urls.join(",") : s.urls).join(" | ");

  updateSelectedCodecFromStats();
  console.log("[stats]", { state, bitrate: brStr, fps, framesDecoded, packetsLost, rttMs });
}

/* -----------------------------------------
   Input helpers
------------------------------------------ */

function getNormalizedPointer(ev) {
  const r = getVideoContentRect(elVideo);
  const x = (ev.clientX - r.left) / r.width;
  const y = (ev.clientY - r.top) / r.height;

  return {
    x_norm: Math.max(0, Math.min(1, x)),
    y_norm: Math.max(0, Math.min(1, y))
  };
}

function bindRemoteInput() {
  if (inputBound || !elVideo) return;
  inputBound = true;

  elVideo.tabIndex = 0;
  elVideo.style.outline = "none";
  elVideo.style.border = "none";
  elVideo.style.cursor = "default";

  elVideo.addEventListener("mouseenter", (ev) => {
    if (!pc || pc.connectionState !== "connected") return;
    enterRemoteControlMode();
    moveRemoteCursorByClient(ev.clientX, ev.clientY);
  });

  elVideo.addEventListener("mouseleave", () => {
    leaveRemoteControlMode();
  });

  elVideo.addEventListener("mousedown", (ev) => {
    enterRemoteControlMode();
    moveRemoteCursorByClient(ev.clientX, ev.clientY);

    moveRemoteCursorByClient(ev.clientX, ev.clientY);
    const p = getNormalizedPointer(ev);
    moveRemoteCursorByNorm(p.x_norm, p.y_norm);
    sendInput("mouse_move", p);
    sendInput("mouse_down", { button: ev.button });

    ev.preventDefault();
  });

  window.addEventListener("mouseup", (ev) => {
    if (!controlActive) return;
    sendInput("mouse_up", { button: ev.button });
  });

  elVideo.addEventListener("mousemove", (ev) => {
    if (!controlActive) enterRemoteControlMode();
    moveRemoteCursorByClient(ev.clientX, ev.clientY);
    const p = getNormalizedPointer(ev);
    lastCursorNorm = p;
    sendInput("mouse_move", p);
  });

  elVideo.addEventListener("wheel", (ev) => {
    // Trackpads fire wheel events without a physical wheel click. Treat wheel
    // as an intent to control the remote/backstage surface so two-finger
    // scrolling works even before a click focuses the viewer.
    if (!controlActive) {
      enterRemoteControlMode();
    }
    if (!controlActive) return;

    const p = getNormalizedPointer(ev);
    moveRemoteCursorByNorm(p.x_norm, p.y_norm);

    sendInput("wheel", {
      ...p,
      delta_x: Math.round(ev.deltaX),
      delta_y: Math.round(ev.deltaY),
      delta_mode: ev.deltaMode || 0,
      shift: !!ev.shiftKey,
      ctrl: !!ev.ctrlKey,
      alt: !!ev.altKey
    });

    ev.preventDefault();
  }, { passive: false });

  // Mobile/tablet direct-touch controls. Mouse listeners above remain the
  // desktop path; touch/pen pointer events are handled separately so browsers
  // do not synthesize duplicate mouse clicks.
  const touchPointers = new Map();
  let touchPrimaryId = null;
  let touchStart = null;
  let touchDragging = false;
  let touchLongPressTimer = null;
  let touchLongPressFired = false;
  let twoFingerLastY = null;

  const clearTouchLongPress = () => {
    if (touchLongPressTimer) clearTimeout(touchLongPressTimer);
    touchLongPressTimer = null;
  };

  const touchDistance = (a, b) => Math.hypot(a.clientX - b.clientX, a.clientY - b.clientY);

  elVideo.addEventListener("pointerdown", (ev) => {
    if (ev.pointerType !== "touch" && ev.pointerType !== "pen") return;
    ev.preventDefault();
    try { elVideo.setPointerCapture(ev.pointerId); } catch {}
    enterRemoteControlMode();
    touchPointers.set(ev.pointerId, { clientX: ev.clientX, clientY: ev.clientY });

    if (touchPointers.size === 1) {
      touchPrimaryId = ev.pointerId;
      touchStart = { clientX: ev.clientX, clientY: ev.clientY };
      touchDragging = false;
      touchLongPressFired = false;
      const p = getNormalizedPointer(ev);
      moveRemoteCursorByNorm(p.x_norm, p.y_norm);
      sendInput("mouse_move", p);
      clearTouchLongPress();
      touchLongPressTimer = setTimeout(() => {
        if (!touchDragging && touchPointers.size === 1 && touchPrimaryId === ev.pointerId) {
          sendInput("mouse_down", { button: 2 });
          sendInput("mouse_up", { button: 2 });
          touchLongPressFired = true;
        }
      }, 650);
    } else if (touchPointers.size === 2) {
      clearTouchLongPress();
      if (touchDragging) {
        sendInput("mouse_up", { button: 0 }, true);
        touchDragging = false;
      }
      const pts = Array.from(touchPointers.values());
      twoFingerLastY = (pts[0].clientY + pts[1].clientY) / 2;
    }
  }, { passive: false });

  elVideo.addEventListener("pointermove", (ev) => {
    if (ev.pointerType !== "touch" && ev.pointerType !== "pen") return;
    if (!touchPointers.has(ev.pointerId)) return;
    ev.preventDefault();
    touchPointers.set(ev.pointerId, { clientX: ev.clientX, clientY: ev.clientY });

    if (touchPointers.size >= 2) {
      clearTouchLongPress();
      const pts = Array.from(touchPointers.values()).slice(0, 2);
      const y = (pts[0].clientY + pts[1].clientY) / 2;
      if (twoFingerLastY != null) {
        const delta = Math.round((twoFingerLastY - y) * 2.2);
        if (Math.abs(delta) >= 2) sendInput("wheel", { delta_x: 0, delta_y: delta, delta_mode: 0 }, true);
      }
      twoFingerLastY = y;
      return;
    }

    if (ev.pointerId !== touchPrimaryId) return;
    const p = getNormalizedPointer(ev);
    moveRemoteCursorByNorm(p.x_norm, p.y_norm);
    sendInput("mouse_move", p);
    if (touchStart && touchDistance(ev, touchStart) > 8) {
      clearTouchLongPress();
      if (!touchDragging && !touchLongPressFired) {
        sendInput("mouse_down", { button: 0 }, true);
        touchDragging = true;
      }
    }
  }, { passive: false });

  const finishTouchPointer = (ev) => {
    if (ev.pointerType !== "touch" && ev.pointerType !== "pen") return;
    if (!touchPointers.has(ev.pointerId)) return;
    ev.preventDefault();
    const wasPrimary = ev.pointerId === touchPrimaryId;
    touchPointers.delete(ev.pointerId);
    clearTouchLongPress();

    if (wasPrimary) {
      if (touchDragging) sendInput("mouse_up", { button: 0 }, true);
      else if (!touchLongPressFired && touchPointers.size === 0) {
        const p = getNormalizedPointer(ev);
        moveRemoteCursorByNorm(p.x_norm, p.y_norm);
        sendInput("mouse_move", p, true);
        sendInput("mouse_down", { button: 0 }, true);
        sendInput("mouse_up", { button: 0 }, true);
      }
      touchPrimaryId = null;
      touchStart = null;
      touchDragging = false;
      touchLongPressFired = false;
    }
    if (touchPointers.size < 2) twoFingerLastY = null;
  };

  elVideo.addEventListener("pointerup", finishTouchPointer, { passive: false });
  elVideo.addEventListener("pointercancel", finishTouchPointer, { passive: false });

  window.addEventListener("blur", () => {
    if (remoteAltTabActive) {
      sendShortcut("alt_tab_end");
      remoteAltTabActive = false;
    }
    leaveRemoteControlMode();
  });

  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      leaveRemoteControlMode();
    }
  });

  function shortcutFromKeyboardEvent(ev) {
    if (!ev) return "";

    // Windows-key combinations. The native viewer hook handles these when
    // maximised, but keep this path for WebView builds that also surface Meta.
    if (ev.metaKey || ev.key === "Meta" || ev.key === "OS") {
      switch (ev.code) {
        case "KeyD": return "win_d";
        case "KeyR": return "win_r";
        case "KeyE": return "win_e";
        case "KeyL": return "lock";
        case "Tab": return "win_tab";
        case "Escape": return "start_menu";
        case "MetaLeft":
        case "MetaRight":
          return "start_menu";
        default:
          break;
      }
    }

    if (ev.altKey && ev.code === "Tab") return remoteAltTabActive ? "alt_tab_next" : "alt_tab_begin";
    if (ev.altKey && ev.code === "F4") return "alt_f4";
    if (ev.ctrlKey && ev.shiftKey && ev.code === "Escape") return "ctrl_shift_esc";
    if (ev.ctrlKey && ev.code === "Escape") return "ctrl_esc";
    if (ev.ctrlKey && ev.altKey && (ev.code === "Delete" || ev.code === "End")) return "ctrl_alt_del";
    return "";
  }

  window.addEventListener("keydown", (ev) => {
    if (!controlActive || !pc || pc.connectionState !== "connected") return;

    const keyboardShortcut = shortcutFromKeyboardEvent(ev);
    if (keyboardShortcut) {
      releaseAllKeys();
      sendShortcut(keyboardShortcut);
      if (keyboardShortcut === "alt_tab_begin" || keyboardShortcut === "alt_tab_next") {
        remoteAltTabActive = true;
      }
      ev.preventDefault();
      return;
    }

    if (!ev.metaKey) {
      if (pressedKeys.has("MetaLeft")) {
        sendInput("key_up", { code: "MetaLeft" }, true);
        pressedKeys.delete("MetaLeft");
      }
      if (pressedKeys.has("MetaRight")) {
        sendInput("key_up", { code: "MetaRight" }, true);
        pressedKeys.delete("MetaRight");
      }
    }
    if (!ev.ctrlKey) {
      if (pressedKeys.has("ControlLeft")) {
        sendInput("key_up", { code: "ControlLeft" }, true);
        pressedKeys.delete("ControlLeft");
      }
      if (pressedKeys.has("ControlRight")) {
        sendInput("key_up", { code: "ControlRight" }, true);
        pressedKeys.delete("ControlRight");
      }
    }
    if (!ev.shiftKey) {
      if (pressedKeys.has("ShiftLeft")) {
        sendInput("key_up", { code: "ShiftLeft" }, true);
        pressedKeys.delete("ShiftLeft");
      }
      if (pressedKeys.has("ShiftRight")) {
        sendInput("key_up", { code: "ShiftRight" }, true);
        pressedKeys.delete("ShiftRight");
      }
    }
    if (!ev.altKey) {
      if (pressedKeys.has("AltLeft")) {
        sendInput("key_up", { code: "AltLeft" }, true);
        pressedKeys.delete("AltLeft");
      }
      if (pressedKeys.has("AltRight")) {
        sendInput("key_up", { code: "AltRight" }, true);
        pressedKeys.delete("AltRight");
      }
    }

    if (isPrintableKey(ev)) {
      sendInput("text_input", {
        code: ev.code,
        key: ev.key,
        text: ev.key,
        repeat: !!ev.repeat
      });
      ev.preventDefault();
      return;
    }

    if (!pressedKeys.has(ev.code)) {
      pressedKeys.add(ev.code);
      sendInput("key_down", {
        code: ev.code,
        key: ev.key || "",
        repeat: false,
        shift: !!ev.shiftKey,
        ctrl: !!ev.ctrlKey,
        alt: !!ev.altKey,
        meta: !!ev.metaKey
      });
    } else if (ev.repeat) {
      sendInput("key_down", {
        code: ev.code,
        key: ev.key || "",
        repeat: true,
        shift: !!ev.shiftKey,
        ctrl: !!ev.ctrlKey,
        alt: !!ev.altKey,
        meta: !!ev.metaKey
      });
    }

    if (ev.code === "MetaLeft" || ev.code === "MetaRight") {
      setTimeout(() => {
        if (pressedKeys.has(ev.code)) {
          sendInput("key_up", { code: ev.code }, true);
          pressedKeys.delete(ev.code);
        }
      }, 250);
    }

    ev.preventDefault();
  });

  window.addEventListener("keyup", (ev) => {
    if (!pc || pc.connectionState !== "connected") return;

    if (remoteAltTabActive && (ev.key === "Alt" || ev.code === "AltLeft" || ev.code === "AltRight")) {
      sendShortcut("alt_tab_end");
      remoteAltTabActive = false;
      ev.preventDefault();
      return;
    }

    if (remoteAltTabActive && ev.code === "Tab") {
      ev.preventDefault();
      return;
    }

    if (pressedKeys.has(ev.code)) {
      pressedKeys.delete(ev.code);
    }

    sendInput("key_up", {
      code: ev.code,
      key: ev.key || "",
      shift: !!ev.shiftKey,
      ctrl: !!ev.ctrlKey,
      alt: !!ev.altKey,
      meta: !!ev.metaKey
    });

    ev.preventDefault();
  });
}

/* -----------------------------------------
   ICE normalization
------------------------------------------ */

function normalizeRemoteIce(msg) {
  if (!msg) return null;

  let candidateStr = null;
  let mid = null;
  let mline = null;

  if (typeof msg.candidate === "string") {
    candidateStr = msg.candidate;
  } else if (msg.candidate && typeof msg.candidate === "object") {
    candidateStr = typeof msg.candidate.candidate === "string" ? msg.candidate.candidate : null;
    mid = msg.candidate.sdpMid ?? msg.candidate.mid ?? null;
    mline = msg.candidate.sdpMLineIndex ?? msg.candidate.mline_index ?? null;
  }

  mid = msg.mid ?? mid;
  mline = msg.mline_index ?? msg.sdpMLineIndex ?? mline;

  if (!candidateStr) return null;

  return { candidate: candidateStr, mid: mid ?? null, mline_index: (mline != null ? Number(mline) : null) };
}

async function applyRemoteIce(msg) {
  const iceMsg = normalizeRemoteIce(msg);
  if (!iceMsg) return;

  if (!pc || !remoteDescSet) {
    pendingRemoteIce.push(iceMsg);
    return;
  }

  try {
    const ice = {
      candidate: iceMsg.candidate,
      ...(iceMsg.mid != null ? { sdpMid: iceMsg.mid } : {}),
      ...(iceMsg.mline_index != null ? { sdpMLineIndex: iceMsg.mline_index } : {}),
    };
    await pc.addIceCandidate(ice);
  } catch (e) {
    console.warn("[rtc] addIceCandidate failed:", e?.message || e, msg);
  }
}

async function flushPendingIce() {
  if (!pc || !remoteDescSet || pendingRemoteIce.length === 0) return;
  const batch = pendingRemoteIce;
  pendingRemoteIce = [];
  for (const m of batch) await applyRemoteIce(m);
}


function extractSdpVideoCodecs(sdp) {
  const lines = String(sdp || "").split(/\r?\n/);
  const rtpmap = new Map();
  const fmtp = new Map();
  for (const line of lines) {
    let m = line.match(/^a=rtpmap:(\d+)\s+([^/\s]+)\/([^\s]+)/i);
    if (m) rtpmap.set(m[1], `${m[2]}/${m[3]}`);
    m = line.match(/^a=fmtp:(\d+)\s+(.+)$/i);
    if (m) fmtp.set(m[1], m[2]);
  }
  return Array.from(rtpmap.entries()).map(([pt, codec]) => ({ pt, codec, fmtp: fmtp.get(pt) || "" }));
}

function logSdpCodecSummary(label, sdp) {
  const codecs = extractSdpVideoCodecs(sdp);
  console.log(`[codec] ${label}`, codecs);
  return codecs;
}

function codecKeyFromLabel(value) {
  const text = String(value || "").toLowerCase();
  if (text.includes("av1")) return "av1";
  if (text.includes("vp9")) return "vp9";
  if (text.includes("vp8")) return "vp8";
  if (text.includes("h.265") || text.includes("h265") || text.includes("hevc")) return "h265";
  if (text.includes("h.264") || text.includes("h264") || text.includes("avc")) return "h264";
  return "";
}

function setViewerCodecLabel(value) {
  const detail = String(value || "—").trim() || "—";
  if (elStatCodec) elStatCodec.textContent = detail;
  const key = codecKeyFromLabel(detail);
  if (key) lastNegotiatedCodecKey = key;
  if (elCodecDevBadge) {
    let codec = detail === "—" ? "—" : detail.split(/\s+/)[0].toUpperCase();
    if (codec === "H264") codec = "H.264";
    if (codec === "H265" || codec === "HEVC") codec = "H.265";
    elCodecDevBadge.textContent = `Codec: ${codec}`;
    elCodecDevBadge.title = detail === "—"
      ? "Negotiated remote video codec"
      : `Negotiated remote video codec: ${detail}`;
  }

  if (devCodecRequested !== "auto" && key === devCodecRequested && devCodecSwitchTimer) {
    clearTimeout(devCodecSwitchTimer);
    devCodecSwitchTimer = null;
    if (elCodecDevSelect) {
      elCodecDevSelect.disabled = false;
      elCodecDevSelect.title = `Development codec override · active ${detail}`;
    }
  }
}

function handleDevCodecSwitchResult(msg) {
  const status = String(msg?.status || "");
  const requested = String(msg?.requested || devCodecRequested || "auto").toLowerCase();
  const detail = String(msg?.detail || "");

  if (status === "queued") {
    if (elCodecDevSelect) elCodecDevSelect.title = detail || `Switching to ${requested}…`;
    return;
  }

  if (status === "failed") {
    if (devCodecSwitchTimer) clearTimeout(devCodecSwitchTimer);
    devCodecSwitchTimer = null;
    if (elCodecDevSelect) {
      elCodecDevSelect.disabled = false;
      const activeKey = codecKeyFromLabel(msg?.active || "");
      if (activeKey) elCodecDevSelect.value = activeKey;
      elCodecDevSelect.title = `Codec switch failed: ${detail || "unsupported codec"}`;
    }
    if (elCodecDevBadge) elCodecDevBadge.title = `Codec switch failed: ${detail || "unsupported codec"}`;
    console.warn("[codec] dev switch failed", msg);
    return;
  }

  if (status === "accepted") {
    if (elCodecDevSelect) elCodecDevSelect.title = detail || `Switch accepted: ${requested}`;
    if (requested === "auto" || lastNegotiatedCodecKey === requested) {
      if (devCodecSwitchTimer) clearTimeout(devCodecSwitchTimer);
      devCodecSwitchTimer = null;
      if (elCodecDevSelect) elCodecDevSelect.disabled = false;
    }
    setTimeout(updateSelectedCodecFromStats, 250);
    setTimeout(updateSelectedCodecFromStats, 1000);
  }
}

function handleAgentControlData(raw) {
  if (typeof raw !== "string") return;
  try {
    const msg = JSON.parse(raw);
    if (msg?.type === "dev_codec_switch_result") {
      handleDevCodecSwitchResult(msg);
      return;
    }
    if (msg?.type === "session_state") {
      handleDesktopSessionState(msg.state || "");
      return;
    }
    if (msg?.type === "stream_diagnostics") {
      reconcileDesktopModeFromDiagnostics(msg);
    }
  } catch {}
}

function sendDevCodecSwitch(requested) {
  const codec = String(requested || "auto").toLowerCase();
  const channel = controlDc && controlDc.readyState === "open"
    ? controlDc
    : (inputDc && inputDc.readyState === "open" ? inputDc : null);
  if (!currentSession || !channel) {
    if (elCodecDevSelect) {
      elCodecDevSelect.disabled = false;
      elCodecDevSelect.title = "Codec control channel is not open yet";
    }
    return false;
  }

  devCodecRequested = codec;
  if (elCodecDevSelect) {
    elCodecDevSelect.disabled = true;
    elCodecDevSelect.title = `Switching to ${codec === "auto" ? "Auto" : codec.toUpperCase()}…`;
  }
  try {
    channel.send(JSON.stringify({ kind: "dev_codec_switch", codec }));
  } catch (e) {
    if (elCodecDevSelect) {
      elCodecDevSelect.disabled = false;
      elCodecDevSelect.title = `Codec switch send failed: ${e?.message || e}`;
    }
    return false;
  }

  if (devCodecSwitchTimer) clearTimeout(devCodecSwitchTimer);
  devCodecSwitchTimer = setTimeout(() => {
    devCodecSwitchTimer = null;
    if (elCodecDevSelect) {
      elCodecDevSelect.disabled = false;
      if (codec !== "auto" && lastNegotiatedCodecKey !== codec) {
        elCodecDevSelect.title = `No ${codec.toUpperCase()} video arrived within 5 seconds`;
        if (elCodecDevBadge) elCodecDevBadge.title = `Dev test warning: no ${codec.toUpperCase()} video arrived after the switch`;
        console.warn("[codec] dev switch watchdog expired", { requested: codec, active: lastNegotiatedCodecKey });
      }
    }
  }, 5000);
  return true;
}

async function updateSelectedCodecFromStats() {
  if (!pc) return;
  try {
    const stats = await pc.getStats();
    let inbound = null;
    stats.forEach((r) => {
      if (r.type === "inbound-rtp" && ((r.kind === "video") || (r.mediaType === "video"))) inbound = r;
    });
    if (!inbound || !inbound.codecId) return;
    const codec = stats.get(inbound.codecId);
    if (codec) {
      const mime = codec.mimeType || codec.mime || "";
      const label = `${mime.replace(/^video\//i, "").toUpperCase()} pt=${codec.payloadType ?? "?"}`;
      setViewerCodecLabel(label);
      console.log("[codec] selected inbound codec", { mimeType: codec.mimeType, payloadType: codec.payloadType, clockRate: codec.clockRate, sdpFmtpLine: codec.sdpFmtpLine });
    }
  } catch (e) {
    console.warn("[codec] selected codec stats failed", e?.message || e);
  }
}

/* -----------------------------------------
   WebRTC negotiation
------------------------------------------ */

async function handleOffer(msg) {
  const offerSdp = msg?.sdp;
  if (!offerSdp) return;

  logSdpCodecSummary("remote offer", offerSdp);

  setStatus("", "Negotiating…");

  pc = new RTCPeerConnection({ iceServers: activeIceServers() });

  try {
    mouseMoveDc = pc.createDataChannel("viewer-mouse-move", { ordered: false, maxRetransmits: 0 });
    mouseMoveDc.binaryType = "arraybuffer";
    mouseMoveDc.onopen = () => console.log("[dc] fast mouse channel open");
    mouseMoveDc.onclose = () => { mouseMoveDc = null; };
    mouseMoveDc.onerror = () => {};
  } catch (e) {
    mouseMoveDc = null;
    console.warn("[dc] fast mouse channel unavailable:", e?.message || e);
  }

  let transceiver = null;
  try {
    transceiver = pc.addTransceiver("video", { direction: "recvonly" });
  } catch (e) {
    console.warn("[rtc] addTransceiver failed:", e?.message || e);
  }

  try {
    const caps = RTCRtpReceiver.getCapabilities?.("video");
    const codecs = caps?.codecs || [];

    if (transceiver && transceiver.setCodecPreferences && codecs.length) {
      // Prefer modern codecs, but keep every browser-supported fallback. The Agent
      // makes the final selection using endpoint hardware and live encode health.
      const primaryOrder = ["video/av1", "video/vp9", "video/h265", "video/hevc", "video/h264", "video/vp8"];
      const primary = [];
      for (const wanted of primaryOrder) {
        primary.push(...codecs.filter(c => String(c.mimeType).toLowerCase() === wanted));
      }
      const rest = codecs.filter(c => !primaryOrder.includes(String(c.mimeType).toLowerCase()));
      console.log("[codec] viewer codec preference", primary.map(c => c.mimeType));
      transceiver.setCodecPreferences([...primary, ...rest]);
    }
  } catch (e) {
    console.warn("[webrtc] codec preference step failed:", e?.message || e);
  }

  pc.ondatachannel = (ev) => {
    if (!ev.channel) return;

    if (ev.channel.label === "input" || ev.channel.label === "input-control") {
      const channel = ev.channel;
      if (channel.label === "input") inputDc = channel;
      else controlDc = channel;

      channel.onopen = () => {
        if (elCodecDevSelect) {
          elCodecDevSelect.disabled = false;
          elCodecDevSelect.title = "Development codec override · switches live without reconnecting";
        }
      };
      channel.onmessage = (messageEvent) => handleAgentControlData(messageEvent.data);
      channel.onclose = () => {
        if (channel.label === "input") inputDc = null;
        else controlDc = null;
        if (elCodecDevSelect && !(inputDc?.readyState === "open") && !(controlDc?.readyState === "open")) {
          elCodecDevSelect.disabled = true;
        }
      };
      channel.onerror = () => {};
    }
  };

  pc.ontrack = async (ev) => {
    console.log("[rtc] ontrack", { trackKind: ev.track?.kind, streams: ev.streams?.length || 0 });
    setTimeout(updateSelectedCodecFromStats, 500);
    setTimeout(updateSelectedCodecFromStats, 1500);
    const stream = (ev.streams && ev.streams[0])
      ? ev.streams[0]
      : new MediaStream([ev.track]);

    if (ev.track?.kind === "audio") {
      if (!elAudio) return;
      elAudio.autoplay = true;
      elAudio.muted = !audioEnabled;
      elAudio.defaultMuted = !audioEnabled;
      elAudio.srcObject = stream;
      if (audioEnabled) {
        try { await elAudio.play(); } catch {}
      }
      return;
    }

    if (ev.track?.kind !== "video" || !elVideo) return;

    elVideo.autoplay = true;
    elVideo.playsInline = true;
    elVideo.muted = true;
    elVideo.defaultMuted = true;
    elVideo.controls = false;

    elVideo.onloadedmetadata = null;
    elVideo.onloadeddata = null;
    elVideo.oncanplay = null;
    elVideo.onplaying = null;
    elVideo.onpause = null;
    elVideo.onerror = null;

    elVideo.onloadedmetadata = async () => {
      updateResolution();
      refreshRemoteCursorPosition();
      try {
        await elVideo.play();
      } catch {}
    };

    elVideo.onloadeddata = () => {
      markFrameRendered();
    };

    elVideo.oncanplay = async () => {
      try {
        await elVideo.play();
      } catch {}
    };

    elVideo.onplaying = () => {
      clearSecureDesktopState();
      markFrameRendered();
      updateResolution();
      refreshRemoteCursorPosition();
    };

    elVideo.onerror = () => {
      console.error("[video] error", elVideo.error);
    };

    try {
      elVideo.pause();
    } catch {}

    elVideo.srcObject = null;
    elVideo.srcObject = stream;

    showStream();
    bindRemoteInput();
    refreshRemoteCursorPosition();
    setStatus("online", "Streaming");

    try {
      await elVideo.play();
    } catch (err) {
      console.error("[video] immediate play() failed", err);
    }

    try { window.hi5?.notifyConnected?.(currentSession?.deviceId); } catch {}

    startStatsPoll();

    if (!hasEverRenderedFrame) {
      lastFrameAtMs = Date.now() + NEGOTIATION_GRACE_MS;
    }
  };

  pc.onicegatheringstatechange = () => console.log("[rtc] iceGatheringState:", pc.iceGatheringState);
  pc.oniceconnectionstatechange = () => console.log("[rtc] iceConnectionState:", pc.iceConnectionState);
  pc.onconnectionstatechange = () => {
    console.log("[rtc] connectionState:", pc.connectionState);

    if (pc.connectionState === "connected") {
      if (hasEverRenderedFrame) {
        showStream();
      }
    }
  };
  pc.onsignalingstatechange = () => console.log("[rtc] signalingState:", pc.signalingState);

  pc.onicecandidate = (ev) => {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    if (ev.candidate) {
      ws.send(JSON.stringify({
        type: "ice_candidate",
        session_id: currentSession.sessionId,
        candidate: ev.candidate.candidate,
        mid: ev.candidate.sdpMid ?? null,
        mline_index: ev.candidate.sdpMLineIndex ?? null
      }));
    }
  };

  await pc.setRemoteDescription({ type: "offer", sdp: offerSdp });
  remoteDescSet = true;
  await flushPendingIce();

  const answer = await pc.createAnswer();
  await pc.setLocalDescription(answer);
  logSdpCodecSummary("local answer", answer.sdp);

  ws.send(JSON.stringify({
    type: "webrtc_answer",
    session_id: currentSession.sessionId,
    sdp: answer.sdp,
    sdp_type: answer.type
  }));
}

/* -----------------------------------------
   Signaling / session start
------------------------------------------ */

async function onSignalMessage(raw) {
  let msg;
  try {
    msg = JSON.parse(raw.data);
  } catch {
    return;
  }

  console.log("[signal] raw message:", msg);

  switch (msg.type) {
    case "viewer_connected":
      break;

    case "session_config": {
      if (currentSession) {
        currentSession.iceServers = normalizeIceServers(msg.ice_servers || msg.iceServers || []);
        if (elDiagIceServers) elDiagIceServers.textContent = activeIceServers().map((server) => Array.isArray(server.urls) ? server.urls.join(",") : server.urls).join(" | ");
      }
      break;
    }

    case "start_webrtc_sent":
      break;

    case "webrtc_offer":
      await handleOffer(msg);
      break;

    case "ice_candidate":
      await applyRemoteIce(msg);
      break;

    case "monitor_info": {
      remoteMonitors = Array.isArray(msg.monitors) ? msg.monitors : [];
      currentMonitorIndex = Number(msg.current ?? 0);

      if (pendingMonitorIndex != null && currentMonitorIndex === pendingMonitorIndex) {
        pendingMonitorIndex = null;
        monitorSwitchUntilMs = 0;
        setStatus("online", "Streaming");
        showStream();
      }

      updateMonitorButton();
      if (monitorMenuOpen) {
        renderMonitorMenu();
      }
      break;
    }

    case "shortcut_result": {
      const action = String(msg.action || "").toLowerCase();
      if (action === "ctrl_alt_del" || action === "cad" || action === "sas") {
        if (!msg.ok) {
          const text = msg.message || "Ctrl+Alt+Del could not be sent by Windows.";
          console.warn("[viewer] CAD unavailable", msg.code || "sas_failed", text);
          setStatus("error", text);
          setTimeout(() => {
            if (pc && pc.connectionState === "connected") setStatus("online", "Streaming");
          }, 4500);
        }
      }
      break;
    }

    case "agent_presence": {
      if (msg.technician_name) {
        setStatus("online", `Streaming · ${msg.technician_name}`);
      }
      break;
    }

    case "chat_message": {
      const body = getChatBody(msg);
      const incomingId = String(msg.message_id || msg.messageId || "");
      if (incomingId && chatMessages.some((item) => String(item.message_id || item.messageId || "") === incomingId)) break;
      if (body) {
        const normalized = {
          ...msg,
          sender: normalizeChatSender(msg.sender || "user"),
          body,
          message: body,
          text: body
        };
        chatMessages.push(normalized);
        renderChat();
        openTechChatWindow();
        postChatToNativeWindow(normalized);
      }
      break;
    }

    case "remote_file_list": {
      remoteFileEntries = Array.isArray(msg.entries) ? msg.entries : [];
      remoteFilePath = msg.path || "/";
      if (elFilePath) elFilePath.value = remoteFilePath;
      renderRemoteFiles();
      postFileToNativeWindow(msg);
      break;
    }

    case "remote_file_download": {
      const path = String(msg.path || "");
      if (browserDownloadPaths.has(path)) {
        browserDownloadPaths.delete(path);
        saveBrowserFile(msg.name || "download", [base64ToBytes(msg.data || "")]);
        setMobileFileStatus(`Downloaded ${msg.name || "file"}.`);
        break;
      }
      const localDir = pendingRemoteDownloads.get(path);
      if (localDir) {
        msg.local_dir = localDir;
        pendingRemoteDownloads.delete(path);
      }
      postFileToNativeWindow(msg);
      break;
    }

    case "file_transfer_start": {
      const path = String(msg.path || "");
      if (msg.direction === "download" && browserDownloadPaths.has(path)) {
        browserDownloadPaths.delete(path);
        browserChunkDownloads.set(String(msg.transfer_id || ""), { name: msg.name || "download", parts: [], received: 0, total: Number(msg.size || 0) });
        setMobileFileStatus(`Downloading ${msg.name || "file"}…`);
        break;
      }
      postFileToNativeWindow(msg);
      break;
    }
    case "file_transfer_chunk": {
      const st = browserChunkDownloads.get(String(msg.transfer_id || ""));
      if (st) {
        st.parts.push(base64ToBytes(msg.data || ""));
        st.received += Number(msg.size || 0);
        const pct = st.total ? Math.min(100, Math.round(st.received / st.total * 100)) : 0;
        setMobileFileStatus(`Downloading ${st.name}… ${pct}%`);
        break;
      }
      postFileToNativeWindow(msg);
      break;
    }
    case "file_transfer_complete": {
      const id = String(msg.transfer_id || "");
      const st = browserChunkDownloads.get(id);
      if (st) {
        browserChunkDownloads.delete(id);
        saveBrowserFile(st.name, st.parts);
        setMobileFileStatus(`Downloaded ${st.name}.`);
        break;
      }
      postFileToNativeWindow(msg);
      break;
    }
    case "file_transfer_error": {
      const id = String(msg.transfer_id || "");
      if (browserChunkDownloads.has(id)) {
        browserChunkDownloads.delete(id);
        setMobileFileStatus(msg.error || "File transfer failed.");
        break;
      }
      postFileToNativeWindow(msg);
      break;
    }
    case "remote_file_upload_complete": {
      setMobileFileStatus("Upload complete.");
      requestRemoteFileList(remoteFilePath || "/");
      postFileToNativeWindow(msg);
      break;
    }
    case "remote_file_delete_complete":
    case "remote_file_mkdir_complete":
    case "remote_file_rename_complete":
    case "remote_file_download_error":
    case "remote_file_upload_error":
    case "remote_file_delete_error":
    case "remote_file_mkdir_error":
    case "remote_file_rename_error": {
      postFileToNativeWindow(msg);
      break;
    }

    case "stream_diagnostics": {
      reconcileDesktopModeFromDiagnostics(msg);
      break;
    }

    case "session_state": {
      const state = msg.state || "";
      if (handleDesktopSessionState(state)) break;

      if (state === "secure_desktop_entering") {
        secureDesktopActive = true;
        desktopHandoffActive = false;
        revealOnNextFrame = false;
        secureDesktopLikely = false;
        showSecureBlackOverlay();
        break;
      }

      if (state === "secure_desktop_ready") {
        secureDesktopActive = false;
        desktopHandoffActive = false;
        secureDesktopLikely = false;
        revealOnNextFrame = true;
        setStatus("", "Switching…");
        break;
      }

      if (state === "secure_desktop_exited") {
        clearSecureDesktopState();
        break;
      }

      if (state === "desktop_handoff_entering") {
        desktopHandoffActive = true;
        secureDesktopActive = false;
        revealOnNextFrame = false;
        secureDesktopLikely = false;
        showSecureBlackOverlay();
        break;
      }

      if (state === "desktop_handoff_ready") {
        desktopHandoffActive = false;
        secureDesktopActive = false;
        secureDesktopLikely = false;
        revealOnNextFrame = true;
        setStatus("", "Switching…");
        break;
      }

      break;
    }

    case "viewer_disconnected":
      disconnect("Disconnected by user");
      break;

    default:
      break;
  }
}

function startSession(params) {
  console.log("[viewer] starting authorised remote session");

  disconnect(undefined, { silent: true });

  params = params || {};

  const sessionId = params.session_id || params.sessionId || "";
  const token = params.token || params.viewer_token || params.viewerToken || "";
  const deviceId = params.device_id || params.deviceId || "";
  const wssUrl = params.wss_url || params.wssUrl || params.signaling_url || params.signalingUrl || "";
  const iceServers = normalizeIceServers(params.ice_servers || params.iceServers || []);
  const viewerClient = params.viewer_client || params.viewerClient || '';
  const launchMode = normalizeDesktopMode(params.mode || params.session_mode || params.sessionMode || "console");

  if (!sessionId || !deviceId || !token || !wssUrl) {
    console.error("[viewer] invalid connection parameters");
    disconnect("Invalid connection parameters");
    return;
  }

  currentSession = {
    sessionId,
    token,
    deviceId,
    wssUrl,
    iceServers,
    viewerClient,
    launchMode
  };

  activeDesktopMode = launchMode;
  desktopModePending = null;
  backgroundModeLocked = launchMode === "backstage";

  remoteMonitors = [];
  currentMonitorIndex = 0;
  pendingMonitorIndex = null;
  updateMonitorButton();
  closeMonitorMenu();

  if (elBtnDisc) elBtnDisc.disabled = false;
  if (elBtnFiles) elBtnFiles.disabled = false;
  if (elBtnChat) elBtnChat.disabled = false;
  if (elBtnAudio) elBtnAudio.disabled = false;
  if (elBtnBlockInput) elBtnBlockInput.disabled = false;
  setRemoteAudioEnabled(false);
  setLocalInputBlocked(false, false);
  updateDesktopModeButtons();
  if (elBtnStartMenu) elBtnStartMenu.disabled = false;
  if (elBtnCad) elBtnCad.disabled = false;
  if (elDeviceLabel) elDeviceLabel.textContent = deviceId || "";

  setStatus("", "Connecting…");
  showOverlay("Connecting", "Starting remote session…", { spinner: true });

  const url =
    `${wssUrl}?session_id=${encodeURIComponent(sessionId)}` +
    `&device_id=${encodeURIComponent(deviceId)}` +
    (token ? `&token=${encodeURIComponent(token)}` : "") +
    (viewerClient ? `&client=${encodeURIComponent(viewerClient)}` : "");
  console.log(`[viewer] opening signaling socket session=${sessionId} device=${deviceId}`);

  ws = new WebSocket(url);

  ws.onopen = () => {
    setStatus("", "Connected");
  };

  ws.onmessage = onSignalMessage;

  ws.onerror = () => {
    disconnect("Connection error");
  };

  ws.onclose = () => {
    disconnect("Disconnected");
  };
}


if (elBtnBackstage) {
  elBtnBackstage.addEventListener("click", () => sendBackstageMode(true));
}
if (elBtnConsole) {
  elBtnConsole.addEventListener("click", () => sendBackstageMode(false));
}
if (elBtnStartMenu) {
  elBtnStartMenu.addEventListener("click", () => sendShortcut("start_menu"));
}
if (elBtnCad) {
  elBtnCad.addEventListener("click", () => sendShortcut("ctrl_alt_del"));
}
if (elCodecDevSelect) {
  elCodecDevSelect.addEventListener("change", () => {
    sendDevCodecSwitch(elCodecDevSelect.value || "auto");
  });
}

/* -----------------------------------------
   App entry
------------------------------------------ */

showOverlay(
  "Hi5Central Viewer",
  "Launch this app from Hi5Central to start a remote desktop session."
);
updateMonitorButton();

try {
  window.hi5?.onConnect((params) => {
    console.log("[viewer] native launch received");
    startSession(params);
  });
} catch (e) {
  console.error("[viewer] failed to bind hi5 connect hook:", e);
}

// Browser/mobile shells can start the same WebRTC viewer without the native
// WebView host. The server supplies a short-lived session token and per-session ICE credentials.
window.hi5RemoteViewer = Object.freeze({
  start: (params) => startSession(params),
  disconnect: () => disconnect("Disconnected by technician", { closeNative: true }),
  isConnected: () => !!(pc && pc.connectionState === "connected")
});

if (elBtnFiles) elBtnFiles.addEventListener("click", () => { toggleFilesPanel(); if (elFilesPanel.classList.contains("visible")) requestRemoteFileList(elFilePath?.value || "/"); });
if (elBtnChat) elBtnChat.addEventListener("click", () => toggleChatPanel(true));
if (elFilesClose) elFilesClose.addEventListener("click", () => toggleFilesPanel(false));
if (elChatClose) elChatClose.addEventListener("click", () => toggleChatPanel(false));
if (elFileRefresh) elFileRefresh.addEventListener("click", () => requestRemoteFileList(elFilePath?.value || "/"));
if (elFileUpload) elFileUpload.addEventListener("click", () => elFileUploadInput?.click());
if (elFileUploadInput) elFileUploadInput.addEventListener("change", async () => {
  await uploadBrowserFiles(elFileUploadInput.files);
  elFileUploadInput.value = "";
  requestRemoteFileList(remoteFilePath || "/");
});
if (elChatSend) elChatSend.addEventListener("click", sendChatMessage);
if (elChatInput) elChatInput.addEventListener("keydown", (e) => { if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); sendChatMessage(); } });
