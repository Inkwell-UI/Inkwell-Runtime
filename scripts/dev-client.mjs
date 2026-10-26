#!/usr/bin/env node
// Tiny dev-mode client for the Inkwell Runtime debug server (SPEC-CHANGES S6), no dependencies: Node's built-in `net` and `crypto` only
// (chosen so that no Origin header is sent: the server refuses any request that carries one).
//
//   node scripts/dev-client.mjs --token <64 hex> [--port 8788] [--subscribe MenuName] [--watch] [--pins] [--reload [MenuName]] [--seconds 0]
//   node scripts/dev-client.mjs --ini "C:\\...\\Data\\SKSE\\Plugins\\Inkwell\\Inkwell.ini" --subscribe MyMenu --watch --pins
//
// --token / --ini   the token (or an Inkwell.ini to read `[Dev] Token` from)
// --subscribe M     subscribe to menu M (with --watch and/or --pins); several --subscribe allowed
// --reload [M]      send a reload request once connected (all menus when no name follows)
// --seconds N       exit after N seconds (default 0 = run until Ctrl+C)
// --expect-fail     self-test mode: exit 0 only if the server refuses (used to check the Origin/Host/token rejections)
// --origin X / --host X   send an Origin / Host header on purpose, to see the 403 (the real editor never does)
import net from "node:net";
import crypto from "node:crypto";
import fs from "node:fs";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(name);
  return i < 0 ? dflt : args[i + 1] && !args[i + 1].startsWith("--") ? args[i + 1] : true;
};
const all = (name) => args.flatMap((a, i) => (a === name && args[i + 1] && !args[i + 1].startsWith("--") ? [args[i + 1]] : []));

const port = Number(opt("--port", 8788));
let token = opt("--token", "");
const iniPath = opt("--ini", "");
if (!token && iniPath) {
  const m = /^\s*Token\s*=\s*([0-9a-f]{64})\s*$/im.exec(fs.readFileSync(iniPath, "utf8"));
  if (m) token = m[1];
}
if (!/^[0-9a-f]{64}$/.test(token) && !args.includes("--expect-fail")) {
  console.error("need --token <64 lower-case hex digits> or --ini <path to Inkwell.ini with [Dev] Token>");
  process.exit(2);
}
const subscribe = all("--subscribe");
const watch = args.includes("--watch"), pins = args.includes("--pins");
const reloadArg = opt("--reload", false);
const seconds = Number(opt("--seconds", 0));
const expectFail = args.includes("--expect-fail");
const host = opt("--host", `127.0.0.1:${port}`);
const origin = opt("--origin", "");

const key = crypto.randomBytes(16).toString("base64");
const sock = net.connect(port, "127.0.0.1");
let buf = Buffer.alloc(0), upgraded = false, nextReload = 1;

const send = (obj) => {
  const payload = Buffer.from(JSON.stringify(obj));
  const mask = crypto.randomBytes(4);
  const n = payload.length;
  const head = n < 126 ? Buffer.from([0x81, 0x80 | n]) : Buffer.from([0x81, 0x80 | 126, n >> 8, n & 255]);
  const body = Buffer.from(payload.map((b, i) => b ^ mask[i & 3]));
  sock.write(Buffer.concat([head, mask, body]));
};
const frame = (op, payload) => {  // client frame with any opcode (pong / close)
  const mask = crypto.randomBytes(4);
  const body = Buffer.from(payload.map((b, i) => b ^ mask[i & 3]));
  sock.write(Buffer.concat([Buffer.from([0x80 | op, 0x80 | payload.length]), mask, body]));
};

sock.on("connect", () => {
  sock.write(
    `GET / HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n` +
      (origin ? `Origin: ${origin}\r\n` : "") + "\r\n"
  );
});

const onMessage = (m) => {
  const t = new Date().toISOString().slice(11, 23);
  switch (m.type) {
    case "helloAck":
      console.log(`${t} helloAck ok=${m.ok}${m.error ? " error=" + m.error : ""} runtime ${m.runtimeVersionString} (packed ${m.runtimeVersion}) protocol ${m.protocolVersion} spec ${m.specVersion} game ${m.gameVersion}`);
      for (const mm of m.menus ?? []) console.log(`        menu ${mm.name}  ${mm.swf}  open=${mm.open}`);
      if (!m.ok) return;
      for (const name of subscribe) send({ type: "subscribe", menu: name, watch, pins });
      if (reloadArg) send({ type: "reload", id: nextReload++, ...(reloadArg === true ? {} : { menu: reloadArg }) });
      break;
    case "trace":
      console.log(`${t} trace [${m.level}] ${m.source}${m.menu ? " " + m.menu : ""}${m.node ? " node=" + m.node : ""}: ${m.text}`);
      break;
    case "ping":
      send({ type: "pong", ...(m.id !== undefined ? { id: m.id } : {}) });
      break;
    default:
      console.log(`${t} ${m.type} ${JSON.stringify(m)}`);
  }
};

sock.on("data", (d) => {
  buf = Buffer.concat([buf, d]);
  if (!upgraded) {
    const end = buf.indexOf("\r\n\r\n");
    if (end < 0) return;
    const head = buf.subarray(0, end).toString();
    buf = buf.subarray(end + 4);
    console.log(head.split("\r\n")[0]);
    if (!head.includes(" 101 ")) {
      sock.destroy();
      process.exit(expectFail ? 0 : 1);
    }
    const accept = crypto.createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    if (!head.includes(accept)) {
      console.error("Sec-WebSocket-Accept does not match");
      process.exit(1);
    }
    upgraded = true;
    send({ type: "hello", token, editorVersion: "dev-client.mjs", protocolVersion: 1, specVersion: 6 });
  }
  for (;;) {  // server frames are unmasked; no fragmentation is produced by the server
    if (buf.length < 2) return;
    const op = buf[0] & 15;
    let len = buf[1] & 127, off = 2;
    if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
    else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
    if (buf.length < off + len) return;
    const payload = buf.subarray(off, off + len);
    buf = buf.subarray(off + len);
    if (op === 1) onMessage(JSON.parse(payload.toString("utf8")));
    else if (op === 9) frame(10, payload);
    else if (op === 8) {
      console.log(`server closed: code ${payload.length >= 2 ? payload.readUInt16BE(0) : "none"} ${payload.subarray(2).toString()}`);
      sock.destroy();
      process.exit(expectFail ? 0 : 1);
    }
  }
});
sock.on("error", (e) => {
  console.error("socket error:", e.message);
  process.exit(expectFail ? 0 : 1);
});
sock.on("close", () => process.exit(expectFail ? 0 : 1));
if (seconds > 0) setTimeout(() => { console.log("done"); process.exit(expectFail ? 1 : 0); }, seconds * 1000);
