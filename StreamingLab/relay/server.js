"use strict";
/* Standalone Streaming Lab relay; never imports or calls the RMM platform. */
const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { WebSocketServer, WebSocket } = require("ws");

const PORT = 8080;
const PREFIX = "/streamlab";
const ALLOWED_ORIGINS = new Set(["https://hi5central.com", "https://streamlab.hi5central.com"]);
const CODE_TTL_MS = 10 * 60_000;
const ROOM_LIMIT = 12;
const TURN_TTL_SECS = 20 * 60;
const TURN_SECRET_FILE = process.env.HI5_LAB_TURN_SECRET_FILE || "/run/secrets/turn-secret";
const TURN_HOST = "turn.hi5central.com";
const html = fs.readFileSync(path.join(__dirname, "remote-viewer.html"), "utf8");
let turnSecret = "";
try { turnSecret = fs.readFileSync(TURN_SECRET_FILE, "utf8").trim(); } catch {}
if (turnSecret.length < 16) {
  console.error("[lab] TURN shared secret missing or invalid; refusing public pairing");
  process.exit(1);
}
const rooms = new Map();  // code -> {host,viewer,deadline,connectedAt}
const limiter = new Map();
function send(ws, data) {
  if (ws?.readyState === WebSocket.OPEN) ws.send(JSON.stringify(data));
}
function code() {
  for (let i=0;i<100;i++) {
    const s = String(crypto.randomInt(0,100000000)).padStart(8,"0");
    if (!rooms.has(s)) return s;
  }
  throw new Error("No pairing code available");
}
function removeRoom(room) {
  if (room?.code) rooms.delete(room.code);
  if (room?.host?.labRoom === room) room.host.labRoom = null;
  if (room?.viewer?.labRoom === room) room.viewer.labRoom = null;
  room.code = null; room.viewer = null;
}
function register(host) {
  if (!host || host.readyState !== WebSocket.OPEN) return;
  if (rooms.size >= ROOM_LIMIT) return send(host,{type:"error",message:"Lab is at capacity"});
  const room = {code:code(),host,viewer:null,deadline:Date.now()+CODE_TTL_MS};
  host.labRoom = room;
  rooms.set(room.code,room);
  send(host,{type:"registered",code:room.code,expiresInSeconds:CODE_TTL_MS/1000});
}
function turnConfig() {
  const expiry = Math.floor(Date.now()/1000)+TURN_TTL_SECS;
  const username = expiry+":hi5lab-"+crypto.randomBytes(5).toString("hex");
  const credential = crypto.createHmac("sha1",turnSecret).update(username).digest("base64");
  return [
    {urls:"turn:"+TURN_HOST+":3478?transport=udp",username,credential},
    {urls:"turn:"+TURN_HOST+":3478?transport=tcp",username,credential}
  ];
}
function attempt(ip) {
  const now = Date.now();
  let r = limiter.get(ip);
  if (!r || now-r.start > 60_000) r={start:now,count:0};
  r.count++; limiter.set(ip,r);
  return r.count <= 8;
}
function fail(ws,message) {send(ws,{type:"error",message});}
function receive(ws,data) {
  if (typeof data !== "object" || !data || Array.isArray(data)) return fail(ws,"Invalid message");
  if (ws.role === "host") {
    if (data.type === "hello" && !ws.labRoom) return register(ws);
    const room=ws.labRoom;
    if (!room || !room.viewer) return;
    if (["webrtc_offer","ice_candidate","stats","capture_started","diagnostic","error"].includes(data.type)) {
      send(room.viewer,data);
    }
    return;
  }
  if (data.type === "join") {
    if (ws.labRoom) return fail(ws,"Already paired");
    if (!attempt(ws.ip)) return fail(ws,"Too many pairing attempts");
    if (typeof data.code !== "string" || !/^\d{8}$/.test(data.code)) return fail(ws,"Enter the eight-digit code");
    const room=rooms.get(data.code);
    if (!room || room.viewer || Date.now()>room.deadline ||
        room.host.readyState!==WebSocket.OPEN) return fail(ws,"Invalid or expired code");
    room.viewer=ws;ws.labRoom=room;
    // One-time pairing code becomes unusable as soon as it is redeemed.
    rooms.delete(room.code);room.code=null;
    const iceServers=turnConfig();
    send(ws,{type:"paired",iceServers});
    send(room.host,{type:"paired",iceServers});
    return;
  }
  const room=ws.labRoom;
  if (!room || room.viewer!==ws) return fail(ws,"Pairing required");
  if (["webrtc_answer","ice_candidate","start","stop"].includes(data.type)) send(room.host,data);
}
function cleanup(ws) {
  const room=ws.labRoom;
  if (!room)return;
  if (ws===room.viewer) {
    send(room.host,{type:"viewer_left"});
    room.viewer=null;ws.labRoom=null;
    if (room.host?.readyState===WebSocket.OPEN) register(room.host);
    else removeRoom(room);
    return;
  }
  if (ws===room.host) {
    send(room.viewer,{type:"error",message:"Windows host disconnected"});
    room.viewer?.close(1001,"Host disconnected");
    removeRoom(room);
  }
}
const server=http.createServer((req,res)=>{
  res.setHeader("Cache-Control","no-store");
  res.setHeader("X-Content-Type-Options","nosniff");
  res.setHeader("Referrer-Policy","no-referrer");
  res.setHeader("X-Frame-Options","DENY");
  res.setHeader("Content-Security-Policy","default-src 'none'; connect-src 'self' wss://hi5central.com wss://streamlab.hi5central.com; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data:; media-src blob:; base-uri 'none'; form-action 'none'");
  // iOS in-app browsers and link previews may issue HEAD before GET.
  // Return the same status, content type and length for both; never hand iOS a
  // 9-byte 404 response that it interprets as an unknown downloadable file.
  const isHead = req.method==="HEAD";
  // Never compare raw req.url: mobile link handlers can append tracking/query parameters.
  // A 9-byte 404 made iOS treat the URL as a downloadable file.
  let pathname;
  try { pathname = new URL(req.url, "http://localhost").pathname; }
  catch { res.writeHead(400); res.end(); return; }
  if ((req.method==="GET" || isHead) &&
      ["/","/index.html",PREFIX,PREFIX+"/",PREFIX+"/index.html"].includes(pathname)) {
    res.writeHead(200,{
      "Content-Type":"text/html; charset=utf-8",
      "Content-Length":Buffer.byteLength(html),
      "Content-Disposition":"inline"
    });
    res.end(isHead ? undefined : html);return;
  }
  if ((req.method==="GET" || isHead) && (pathname===PREFIX+"/health" || pathname==="/health")) {
    const data='{"ok":true,"service":"hi5-streaming-lab","version":2}';
    res.writeHead(200,{"Content-Type":"application/json","Content-Length":Buffer.byteLength(data)});
    res.end(isHead ? undefined : data);return;
  }
  res.writeHead(404,{"Content-Type":"text/plain; charset=utf-8"});
  res.end(isHead ? undefined : "Not found");
});
const wss=new WebSocketServer({noServer:true,maxPayload:128*1024,perMessageDeflate:false});
server.on("upgrade",(req,socket,head)=>{
  const url=new URL(req.url,"http://localhost");
  const role=url.searchParams.get("role");
  const viewer=role==="viewer";
  const host=role==="host";
  const origin=req.headers.origin || "";
  if (!([PREFIX+"/ws","/ws"].includes(url.pathname)) || (!viewer&&!host) ||
      (viewer&&!ALLOWED_ORIGINS.has(origin)) || (host&&origin&&!ALLOWED_ORIGINS.has(origin))) {
    socket.write("HTTP/1.1 403 Forbidden\r\n\r\n");socket.destroy();return;
  }
  wss.handleUpgrade(req,socket,head,ws=>{
    ws.role=role;
    // Do not trust spoofable X-Forwarded-For values from clients.
    // Caddy terminates HTTPS and provides a normalized peer; limit by remote socket.
    ws.ip=req.socket.remoteAddress || "unknown";
    ws.isAlive=true;
    ws.on("pong",()=>{ws.isAlive=true});
    ws.on("message",raw=>{
      try {
        const msg=JSON.parse(String(raw));
        receive(ws,msg);
      } catch {fail(ws,"Malformed message")}
    });
    ws.on("close",()=>cleanup(ws));
    ws.on("error",()=>{});
    if (host) send(ws,{type:"hello_required"});
    else send(ws,{type:"ready"});
  });
});
setInterval(()=>{
  const now=Date.now();
  for(const room of rooms.values()) {
    if(now > room.deadline) {
      rooms.delete(room.code);
      send(room.host,{type:"error",message:"Pairing code expired; reconnect the EXE"});
      room.host?.close(1000,"Code expired");
      removeRoom(room);
    }
  }
  for(const [ip,r] of limiter)if(now-r.start>120_000)limiter.delete(ip);
  for(const ws of wss.clients) {
    if(!ws.isAlive) {ws.terminate();continue}
    ws.isAlive=false;ws.ping();
  }
},30_000).unref();
server.listen(PORT,"0.0.0.0",()=>console.log("[lab] isolated relay listening on :8080; route must be protected by TLS proxy"));
