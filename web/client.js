// gmstream web client: WebTransport + WebCodecs. See PROTOCOL.md.
//
// Latency choices: every video frame arrives on its own QUIC stream (or, on
// WebKit, all on one stream) and is decoded as soon as it is complete and in
// order; decoded frames are drawn to
// a desynchronized canvas from the decoder callback (no requestAnimationFrame
// wait). The mouse cursor is gamescope's image set as the native browser
// cursor, so it moves with zero stream latency.

const PROTOCOL_VERSION = 2;
const HELLO_ONE_VIDEO_STREAM = 1, HELLO_VIDEO_DATAGRAMS = 2;
const CODECS = [ // index = codec id on the wire
  { name: 'h264', string: 'avc1.640034' },
  { name: 'hevc', string: 'hev1.1.6.L156.B0' },
  { name: 'av1', string: 'av01.0.13M.08' },
];

// Client → host control messages.
const C_HELLO = 0x01, C_CONFIGURE = 0x02, C_KEYFRAME = 0x03, C_BITRATE = 0x04,
  C_KEY = 0x05, C_BUTTON = 0x06, C_MOTION = 0x07, C_POSITION = 0x08,
  C_SCROLL = 0x09, C_GAMEPAD = 0x0a, C_GAMEPAD_REMOVE = 0x0b, C_PING = 0x0c;
// Host → client.
const H_HELLO_ACK = 0x81, H_CURSOR = 0x82, H_POINTER_LOCK = 0x83,
  H_RUMBLE = 0x84, H_PONG = 0x85, H_ERROR = 0x86;

const params = new URLSearchParams(location.search);
const canvas = document.getElementById('screen');
const ctx2d = canvas.getContext('2d', { alpha: false, desynchronized: true });
const hud = document.getElementById('hud');
const hudText = document.getElementById('hud-text');
const hint = document.getElementById('hint');
const overlay = document.getElementById('overlay');
const statusEl = document.getElementById('status');

// Older versions kept an access token here and in the URL fragment.
try { localStorage.removeItem('gmstream-token'); } catch {}
if (location.hash) history.replaceState(null, '', location.pathname + location.search);

// Sends a line to the host's log (for clients without devtools, like iPad).
function remoteLog(text) {
  console.log(text);
  fetch('log', { method: 'POST', body: String(text).slice(0, 2000), keepalive: true })
    .catch(() => {});
}
window.addEventListener('error', e => remoteLog(`error: ${e.message} at ${e.filename}:${e.lineno}`));
window.addEventListener('unhandledrejection', e => remoteLog(`unhandled rejection: ${e.reason?.stack || e.reason}`));

let session = null;

/* ---- settings ----------------------------------------------------------- */

// Saved per browser; URL parameters (?codec= ?bitrate=kbps ?maxheight= ?hud=0)
// override them for one visit.
const DEFAULTS = {
  codec: 'h264',      // h264 | hevc | av1; changing it reconnects
  bitrateMbps: 40,
  scale: 100,         // % of the native (device-pixel) size
  maxHeight: 0,       // 0: no cap
  customSize: '',     // "WxH" overrides scale and max height
  refresh: 60,       // Hz
  audio: true,
  audioBufferMs: 20,  // jitter buffer target
  gapWaitMs: 30,      // how long a missing frame may hold up later ones
  rawMouse: true,     // Pointer Lock without OS acceleration
  smoothing: true,    // scale the video smoothly (else pixelated)
  videoStreams: 'auto', // 'auto', 'per-frame', 'one' or 'datagrams' (see helloVideoFlags)
  hud: true,
};
const settings = { ...DEFAULTS };
try { Object.assign(settings, JSON.parse(localStorage.getItem('gmstream-settings') || '{}')); } catch {}
if (!(Number(settings.refresh) > 0)) settings.refresh = DEFAULTS.refresh; // older "auto"
if (params.has('codec')) settings.codec = params.get('codec');
if (params.has('bitrate')) settings.bitrateMbps = Number(params.get('bitrate')) / 1000;
if (params.has('maxheight')) settings.maxHeight = Number(params.get('maxheight'));
if (params.get('hud') === '0') settings.hud = false;
function saveSettings() {
  try { localStorage.setItem('gmstream-settings', JSON.stringify(settings)); } catch {}
}

/* ---- wire helpers ------------------------------------------------------ */

function message(type, size, fill) {
  const buf = new ArrayBuffer(5 + size);
  const v = new DataView(buf);
  v.setUint8(0, type);
  v.setUint32(1, size, true);
  if (fill) fill(new DataView(buf, 5), new Uint8Array(buf, 5));
  return new Uint8Array(buf);
}

// Reads exactly n bytes from a stream reader, keeping leftovers.
class ByteReader {
  constructor(reader) { this.reader = reader; this.chunks = []; this.have = 0; }
  async read(n) {
    while (this.have < n) {
      const { value, done } = await this.reader.read();
      if (done) return null;
      this.chunks.push(value);
      this.have += value.length;
    }
    const out = new Uint8Array(n);
    let off = 0;
    while (off < n) {
      const c = this.chunks[0];
      const take = Math.min(c.length, n - off);
      out.set(c.subarray(0, take), off);
      off += take;
      if (take === c.length) this.chunks.shift();
      else this.chunks[0] = c.subarray(take);
    }
    this.have -= n;
    return out;
  }
}

/* ---- display mode ------------------------------------------------------ */

// One stream per frame lets a late frame be skipped, but WebKit (Safari and
// every iPad browser) grants new streams too slowly for 60 fps and stops
// granting them after a reset, so there all video shares one stream.
// Datagrams (every frame split into UDP-sized pieces, no retransmission) are
// opt-in.
function helloVideoFlags() {
  switch (settings.videoStreams) {
    case 'per-frame': return 0;
    case 'one': return HELLO_ONE_VIDEO_STREAM;
    case 'datagrams': return HELLO_VIDEO_DATAGRAMS;
  }
  return /AppleWebKit/.test(navigator.userAgent) && !/Chrome\//.test(navigator.userAgent) ? HELLO_ONE_VIDEO_STREAM : 0;
}

function refreshMhz() {
  return Math.round(Number(settings.refresh) * 1000);
}

function wantedSize() {
  const custom = /^(\d+)x(\d+)$/.exec(settings.customSize.trim());
  if (custom) return { w: Number(custom[1]) & ~1, h: Number(custom[2]) & ~1 };
  const dpr = window.devicePixelRatio || 1;
  const k = Math.max(10, Math.min(100, settings.scale)) / 100;
  let w = Math.round(window.innerWidth * dpr * k);
  let h = Math.round(window.innerHeight * dpr * k);
  const max = settings.maxHeight;
  if (max && h > max) { w = Math.round(w * max / h); h = max; }
  return { w: w & ~1, h: h & ~1 };
}

let resizeTimer = 0;
function scheduleConfigure() {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    if (!session) return;
    const { w, h } = wantedSize();
    remoteLog(`configure ${w}x${h}@${settings.refresh} window ${innerWidth}x${innerHeight} fullscreen ${document.fullscreenElement != null}`);
    session.send(message(C_CONFIGURE, 8, v => {
      v.setUint16(0, w, true); v.setUint16(2, h, true); v.setUint32(4, refreshMhz(), true);
    }));
  }, 250);
}
window.addEventListener('resize', scheduleConfigure);
document.addEventListener('fullscreenchange', () => {
  scheduleConfigure();
  if (document.fullscreenElement && navigator.keyboard?.lock) navigator.keyboard.lock().catch(() => {});
});

// The rectangle (CSS px) the video occupies inside the canvas (object-fit: contain).
// Fits the canvas into the visible window, keeping the stream's aspect ratio.
// Done here rather than with 100vw/100vh and object-fit: on iPad, 100vh
// includes the area behind the browser's toolbars.
function layoutCanvas() {
  const W = window.innerWidth, H = window.innerHeight;
  const vw = canvas.width || W, vh = canvas.height || H;
  const s = Math.min(W / vw, H / vh);
  const w = vw * s, h = vh * s;
  Object.assign(canvas.style, { width: `${w}px`, height: `${h}px`, left: `${(W - w) / 2}px`, top: `${(H - h) / 2}px` });
}
window.addEventListener('resize', layoutCanvas);
window.visualViewport?.addEventListener('resize', layoutCanvas);
document.addEventListener('fullscreenchange', layoutCanvas);
layoutCanvas();

function videoRect() {
  const r = canvas.getBoundingClientRect();
  const vw = canvas.width || 1, vh = canvas.height || 1;
  const s = Math.min(r.width / vw, r.height / vh);
  const w = vw * s, h = vh * s;
  return { x: r.left + (r.width - w) / 2, y: r.top + (r.height - h) / 2, w, h, scale: s };
}

/* ---- session ----------------------------------------------------------- */

class Session {
  constructor() {
    this.closed = false;
    this.offsetMs = null; // host CLOCK_MONOTONIC ms - performance.now()
    this.bestRtt = Infinity;
    this.decoder = null;
    this.decoderConfig = '';
    this.nextId = -1; // next frame id to decode; -1 = waiting for a keyframe
    this.pending = new Map();
    this.fragments = new Map(); // datagram mode: frame id → pieces so far
    this.gapTimer = 0;
    this.frameTimes = new Map(); // chunk timestamp → timing info
    this.stats = { frames: 0, recv: 0, bytes: 0, drops: 0, lat: [], net: [], dec: [] };
    this.codec = 0;
    this.locked = false;
    this.audio = null;
    this.streamErrors = 0; // video streams that failed mid-read
  }

  send(bytes) {
    if (this.closed) return;
    this.writer.write(bytes).catch(() => {});
  }

  async start() {
    const config = await (await fetch('config.json', { cache: 'no-store' })).json();
    const hash = Uint8Array.from(config.certHash);
    this.wt = new WebTransport(`https://${location.hostname}:${config.port}/`, {
      serverCertificateHashes: [{ algorithm: 'sha-256', value: hash }],
      congestionControl: 'low-latency',
    });
    await this.wt.ready;
    // A keyframe arrives as hundreds of datagrams at once; the default queue
    // would drop most of them before this page reads them.
    try { this.wt.datagrams.incomingHighWaterMark = 4096; } catch {}
    this.wt.closed.then(info => info?.reason === 'replaced'
      ? this.close('Another client connected', false, true) // final, like the Error message
      : this.close(info?.reason ? `Disconnected: ${info.reason}` : 'Disconnected'), e => this.close(`Disconnected: ${e.message || e}`));
    const ctrl = await this.wt.createBidirectionalStream();
    this.writer = ctrl.writable.getWriter();

    let mask = 0;
    for (let i = 0; i < CODECS.length; i++) {
      const s = await VideoDecoder.isConfigSupported({ codec: CODECS[i].string, hardwareAcceleration: 'prefer-hardware' })
        .catch(() => ({ supported: false }));
      if (s.supported) mask |= 1 << i;
    }
    const pref = Math.max(0, CODECS.findIndex(c => c.name === settings.codec));
    const bitrate = Math.round(settings.bitrateMbps * 1000);
    const { w, h } = wantedSize();
    remoteLog(`hello ${w}x${h}@${settings.refresh} codecs ${mask} screen ${screen.width}x${screen.height} window ${innerWidth}x${innerHeight} dpr ${devicePixelRatio} video flags ${helloVideoFlags()} ${navigator.userAgent}`);
    const videoFlags = helloVideoFlags();
    this.oneStream = videoFlags === HELLO_ONE_VIDEO_STREAM;
    this.send(message(C_HELLO, 17, (v, b) => {
      v.setUint16(0, PROTOCOL_VERSION, true);
      v.setUint16(2, w, true); v.setUint16(4, h, true);
      v.setUint32(6, refreshMhz(), true);
      v.setUint32(10, bitrate, true);
      v.setUint8(14, mask); v.setUint8(15, pref);
      v.setUint8(16, videoFlags);
    }));

    this.readControl(new ByteReader(ctrl.readable.getReader()));
    this.readFrames();
    this.readDatagrams();
    this.pingTimer = setInterval(() => this.ping(), 500);
    this.ping();
    this.hudTimer = setInterval(() => this.updateHud(), 1000);
    latencyChart.clear();
    this.chartTimer = setInterval(() => { if (settings.hud) latencyChart.draw(); }, 100);
    this.padTimer = setInterval(() => pollGamepads(this), 4);
  }

  // `final`: the host refused or replaced us; don't reconnect.
  close(reason, quiet = false, final = false) {
    if (this.closed) return;
    this.closed = true;
    remoteLog(`session closed: ${reason || '(no reason)'}${final ? ' (final)' : ''}`);
    clearInterval(this.pingTimer); clearInterval(this.hudTimer); clearInterval(this.padTimer); clearInterval(this.chartTimer);
    try { this.wt.close(); } catch {}
    try { this.decoder?.close(); } catch {}
    this.audio?.close();
    if (document.pointerLockElement) document.exitPointerLock();
    canvas.style.cursor = '';
    session = null;
    if (final) wantConnected = false;
    if (quiet) return;
    if (wantConnected) {
      showOverlay(`${reason} — reconnecting…`);
      scheduleReconnect();
    } else {
      showOverlay(reason);
    }
  }

  ping() {
    this.send(message(C_PING, 8, v => v.setBigUint64(0, BigInt(Math.round(performance.now() * 1000)), true)));
  }

  async readControl(r) {
    for (;;) {
      const head = await r.read(5);
      if (!head) return;
      const hv = new DataView(head.buffer);
      const type = hv.getUint8(0), len = hv.getUint32(1, true);
      const body = len ? await r.read(len) : new Uint8Array(0);
      if (!body) return;
      const v = new DataView(body.buffer, body.byteOffset, body.byteLength);
      switch (type) {
        case H_HELLO_ACK:
          this.codec = v.getUint8(0);
          statusEl.textContent = '';
          overlay.hidden = true;
          this.audio = new AudioPlayer();
          break;
        case H_CURSOR: setCursor(v, body); break;
        case H_POINTER_LOCK: this.setLocked(v.getUint8(0) !== 0); break;
        case H_RUMBLE: rumble(v.getUint8(0), v.getUint16(1, true), v.getUint16(3, true), v.getUint16(5, true)); break;
        case H_PONG: {
          const now = performance.now();
          const t0 = Number(v.getBigUint64(0, true)) / 1000;
          const host = Number(v.getBigUint64(8, true)) / 1e6;
          const rtt = now - t0;
          // Keep the offset from the fastest round trip seen recently.
          if (this.offsetMs === null || rtt <= this.bestRtt * 1.2) {
            this.bestRtt = Math.min(this.bestRtt, rtt);
            this.offsetMs = host - (t0 + rtt / 2);
          }
          this.rtt = rtt;
          break;
        }
        case H_ERROR: this.close(new TextDecoder().decode(body), false, true); return;
      }
    }
  }

  async readFrames() {
    const streams = this.wt.incomingUnidirectionalStreams.getReader();
    for (;;) {
      let next;
      try { next = await streams.read(); } catch (e) { next = { done: true, error: e }; }
      if (next.done) {
        // Without this loop the browser grants the host no more streams, so
        // video stops while the session stays up: reconnect instead.
        if (!this.closed) {
          remoteLog(`incoming streams ended: ${next.error?.message || next.error || 'done'} (${this.streamErrors} stream errors)`);
          this.close('Video streams ended');
        }
        return;
      }
      if (this.oneStream) this.readFrameStream(next.value);
      else this.readFrame(next.value);
    }
  }

  // One-stream mode: frames back to back, each after its u32 length.
  async readFrameStream(stream) {
    const r = new ByteReader(stream.getReader());
    try {
      for (;;) {
        const head = await r.read(4);
        if (!head) break;
        const data = await r.read(new DataView(head.buffer).getUint32(0, true));
        if (!data) break;
        this.gotFrame(data, performance.now());
      }
    } catch (e) {
      if (!this.closed) remoteLog(`video stream failed: ${e.message || e}`);
    }
    if (!this.closed) this.close('Video stream ended');
  }

  async readFrame(stream) {
    const reader = stream.getReader();
    const chunks = [];
    let size = 0;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        chunks.push(value);
        size += value.length;
      }
    } catch { this.streamErrors++; return; } // reset by the host or cancelled
    const recv = performance.now();
    const data = new Uint8Array(size);
    let off = 0;
    for (const c of chunks) { data.set(c, off); off += c.length; }
    this.gotFrame(data, recv);
  }

  // Datagram mode: collect a frame's pieces; a frame with a piece missing is
  // never completed, and the gap logic in pump() skips to a keyframe.
  videoFragment(id, index, count, part) {
    if (this.nextId >= 0 && ((id - this.nextId) | 0) < 0) return; // stale
    let f = this.fragments.get(id);
    if (!f) {
      f = { parts: new Array(count), got: 0, size: 0 };
      this.fragments.set(id, f);
      // Drop the oldest incomplete frames (Map keeps insertion order).
      for (const old of this.fragments.keys()) {
        if (this.fragments.size <= 16) break;
        this.fragments.delete(old);
      }
    }
    if (index >= count || f.parts[index]) return;
    f.parts[index] = part;
    f.got++;
    f.size += part.length;
    if (f.got < count) return;
    this.fragments.delete(id);
    const data = new Uint8Array(f.size);
    let off = 0;
    for (const p of f.parts) { data.set(p, off); off += p.length; }
    this.gotFrame(data, performance.now());
  }

  gotFrame(data, recv) {
    const size = data.length;
    if (size < 32) return;
    const v = new DataView(data.buffer);
    const f = {
      id: v.getUint32(0, true),
      key: (v.getUint8(4) & 1) !== 0,
      codec: v.getUint8(5),
      w: v.getUint16(6, true), h: v.getUint16(8, true),
      slotNs: v.getBigUint64(12, true),
      encodedNs: v.getBigUint64(20, true),
      data: data.subarray(32),
      recv,
    };
    this.stats.bytes += size;
    this.stats.recv++;
    this.onFrame(f);
  }

  onFrame(f) {
    if (this.nextId >= 0 && ((f.id - this.nextId) | 0) < 0) return; // stale
    this.pending.set(f.id, f);
    this.pump();
  }

  pump() {
    if (this.nextId < 0) {
      // Waiting for a keyframe: start at the oldest one we have.
      let best = null;
      for (const f of this.pending.values()) if (f.key && (best === null || ((f.id - best.id) | 0) < 0)) best = f;
      if (!best) { this.pending.clear(); return; }
      for (const id of [...this.pending.keys()]) if (((id - best.id) | 0) < 0) this.pending.delete(id);
      this.nextId = best.id;
    }
    while (this.pending.has(this.nextId)) {
      const f = this.pending.get(this.nextId);
      this.pending.delete(this.nextId);
      this.nextId = (this.nextId + 1) >>> 0;
      this.decode(f);
    }
    clearTimeout(this.gapTimer);
    if (this.pending.size > 0) {
      // A later frame is complete but this one is not: give it a moment
      // (streams can finish out of order), then skip to a keyframe.
      this.gapTimer = setTimeout(() => {
        if (!this.pending.has(this.nextId) && this.pending.size > 0) {
          this.stats.drops++;
          this.nextId = -1;
          this.send(message(C_KEYFRAME, 0));
          this.pump();
        }
      }, settings.gapWaitMs);
    }
  }

  decode(f) {
    const cfg = `${f.codec}:${f.w}x${f.h}`;
    if (!this.decoder || this.decoder.state === 'closed' || (f.key && cfg !== this.decoderConfig)) {
      if (!f.key) { this.nextId = -1; return; }
      if (!this.decoder || this.decoder.state === 'closed') {
        this.decoder = new VideoDecoder({
          output: frame => this.draw(frame),
          error: e => {
            remoteLog(`decoder error: ${e.message || e}`);
            this.decoderConfig = '';
            this.nextId = -1;
            this.send(message(C_KEYFRAME, 0));
          },
        });
      }
      this.decoder.configure({
        codec: CODECS[f.codec].string,
        codedWidth: f.w, codedHeight: f.h,
        optimizeForLatency: true,
        hardwareAcceleration: 'prefer-hardware',
      });
      this.decoderConfig = cfg;
    }
    const ts = Number(f.slotNs / 1000n);
    this.frameTimes.set(ts, { f, start: performance.now() });
    this.decoder.decode(new EncodedVideoChunk({ type: f.key ? 'key' : 'delta', timestamp: ts, data: f.data }));
  }

  draw(frame) {
    if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
      canvas.width = frame.displayWidth;
      canvas.height = frame.displayHeight;
      layoutCanvas();
      remoteLog(`video ${canvas.width}x${canvas.height} shown at ${canvas.style.width} x ${canvas.style.height} in window ${innerWidth}x${innerHeight}`);
    }
    ctx2d.drawImage(frame, 0, 0);
    const now = performance.now();
    const t = this.frameTimes.get(frame.timestamp);
    frame.close();
    if (!t) return;
    this.frameTimes.delete(frame.timestamp);
    if (this.frameTimes.size > 64) this.frameTimes.clear();
    this.stats.frames++;
    this.stats.dec.push(now - t.start);
    if (this.offsetMs !== null) {
      const slot = Number(t.f.slotNs) / 1e6 - this.offsetMs;
      const encoded = Number(t.f.encodedNs) / 1e6 - this.offsetMs;
      this.stats.lat.push(now - slot);
      latencyChart.push(now, now - slot);
      this.stats.net.push(t.f.recv - encoded);
    }
  }

  async readDatagrams() {
    const r = this.wt.datagrams.readable.getReader();
    for (;;) {
      const { value, done } = await r.read().catch(() => ({ done: true }));
      if (done) return;
      const v = new DataView(value.buffer, value.byteOffset, value.byteLength);
      if (value.length > 13 && value[0] === 1 && this.audio) {
        this.audio.push(Number(v.getBigUint64(5, true) / 1000n), value.subarray(13));
      } else if (value.length > 9 && value[0] === 2) {
        this.videoFragment(v.getUint32(1, true), v.getUint16(5, true), v.getUint16(7, true), value.subarray(9));
      }
    }
  }

  setLocked(locked) {
    this.locked = locked;
    if (!locked && document.pointerLockElement) document.exitPointerLock();
    if (locked && !document.pointerLockElement) requestLock();
    updateHint();
  }

  updateHud() {
    const s = this.stats;
    const avg = a => a.length ? (a.reduce((x, y) => x + y, 0) / a.length).toFixed(1) : '-';
    const max = a => a.length ? Math.max(...a).toFixed(1) : '-';
    hudText.textContent =
      `${CODECS[this.codec]?.name} ${canvas.width}x${canvas.height} ${s.frames} fps ` +
      `${(s.bytes * 8 / 1e6).toFixed(1)} Mbps\n` +
      `capture→display ${avg(s.lat)} ms (max ${max(s.lat)})\n` +
      `network ${avg(s.net)} ms  decode ${avg(s.dec)} ms  rtt ${this.rtt?.toFixed(1) ?? '-'} ms\n` +
      `audio buffer ${this.audio?.bufferedMs().toFixed(0) ?? '-'} ms  drops ${s.drops}`;
    // Frames arriving but none shown: say where they are stuck.
    if (s.recv > 0 && s.frames === 0) {
      remoteLog(`stall: received ${s.recv} frames, drew 0; decoder ${this.decoder?.state} queue ${this.decoder?.decodeQueueSize}; ` +
        `next ${this.nextId} pending ${this.pending.size} [${[...this.pending.keys()].slice(0, 4)}] drops ${s.drops}`);
    }
    this.stats = { frames: 0, recv: 0, bytes: 0, drops: s.drops, lat: [], net: [], dec: [] };
    if (params.has('log')) console.log(hudText.textContent.replaceAll('\n', ' | '));
  }
}

/* ---- latency chart ----------------------------------------------------- */

// The last 10 s of per-frame capture→display latency, under the HUD text, so
// spikes stand out. The scale grows to fit spikes, in steps (never below
// 20 ms). Pauses in the stream (a static screen sends no frames) break the
// line rather than being drawn across.
const latencyChart = {
  WINDOW_MS: 10000,
  GAP_MS: 250,
  el: document.getElementById('hud-chart'),
  points: [], // [time, latency ms]

  push(t, ms) {
    this.points.push([t, ms]);
    while (this.points.length && this.points[0][0] < t - this.WINDOW_MS) this.points.shift();
  },
  clear() { this.points = []; this.draw(); },

  draw() {
    const c = this.el, dpr = window.devicePixelRatio || 1;
    const W = c.clientWidth, H = c.clientHeight;
    if (c.width !== Math.round(W * dpr) || c.height !== Math.round(H * dpr)) {
      c.width = Math.round(W * dpr); c.height = Math.round(H * dpr);
    }
    const g = c.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);
    const now = performance.now();
    const pts = this.points.filter(p => p[0] >= now - this.WINDOW_MS);
    const peak = pts.reduce((m, p) => Math.max(m, p[1]), 0);
    const top = [20, 40, 80, 160, 320, 640, 1280].find(v => v >= peak) ?? Math.ceil(peak / 1000) * 1000;
    const y = ms => H - 1 - Math.min(ms, top) / top * (H - 12);
    const x = t => (t - (now - this.WINDOW_MS)) / this.WINDOW_MS * W;

    // Recessive grid: the top of the scale and its midpoint, labelled.
    g.font = '10px ui-monospace, Menlo, monospace';
    g.textBaseline = 'top';
    g.fillStyle = '#c3c2b7';
    g.strokeStyle = '#ffffff22';
    g.lineWidth = 1;
    for (const v of [top, top / 2]) {
      g.beginPath(); g.moveTo(0, Math.round(y(v)) + 0.5); g.lineTo(W, Math.round(y(v)) + 0.5); g.stroke();
    }
    g.fillText(`${top} ms`, 2, 0);
    g.textBaseline = 'bottom';
    g.fillText(`${top / 2}`, 2, Math.round(y(top / 2)) - 1);
    g.textBaseline = 'top';
    if (pts.length) {
      const label = `max ${peak.toFixed(1)} ms`;
      g.fillText(label, W - g.measureText(label).width - 2, 0);
    }

    g.strokeStyle = '#3987e5';
    g.lineWidth = 1.5;
    g.lineJoin = 'round';
    g.beginPath();
    let last = -Infinity;
    for (const [t, ms] of pts) {
      if (t - last > this.GAP_MS) g.moveTo(x(t), y(ms)); else g.lineTo(x(t), y(ms));
      last = t;
    }
    g.stroke();
  },
};

/* ---- audio ------------------------------------------------------------- */

const WORKLET = `
class Player extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = new Float32Array(48000 * 2); // 1 s stereo ring
    this.r = 0; this.w = 0; this.started = false; this.target = 960; // samples per channel
    this.port.onmessage = e => {
      if (e.data.targetMs !== undefined) { this.target = Math.round(e.data.targetMs * 48); return; }
      const d = e.data; // interleaved stereo
      for (let i = 0; i < d.length; i++) { this.buf[this.w] = d[i]; this.w = (this.w + 1) % this.buf.length; }
      const have = this.have();
      // Twice the target buffered: drop back to the target.
      if (have > 4 * this.target) this.r = (this.w - 2 * this.target + this.buf.length) % this.buf.length;
      this.port.postMessage(this.have() / 96);
    };
  }
  have() { return (this.w - this.r + this.buf.length) % this.buf.length; }
  process(_, outputs) {
    const out = outputs[0], n = out[0].length;
    if (!this.started && this.have() < this.target) return true; // prebuffer half the target
    this.started = true;
    for (let i = 0; i < n; i++) {
      if (this.have() < 2) { this.started = false; for (; i < n; i++) { out[0][i] = 0; if (out[1]) out[1][i] = 0; } break; }
      out[0][i] = this.buf[this.r]; if (out[1]) out[1][i] = this.buf[this.r + 1];
      this.r = (this.r + 2) % this.buf.length;
    }
    return true;
  }
}
registerProcessor('gm-player', Player);`;

let audioCtx = null, workletReady = null;
// iPadOS (and Safari) only let audio start inside a user gesture, and an
// await before it (fullscreen) can use the gesture up: so the context is
// created and resumed synchronously from taps and key presses.
function unlockAudio() {
  // Media playback rules, not the ringer's (iOS WebKit).
  try { if (navigator.audioSession) navigator.audioSession.type = 'playback'; } catch {}
  if (!audioCtx) {
    audioCtx = new AudioContext({ latencyHint: 'interactive', sampleRate: 48000 });
    const url = URL.createObjectURL(new Blob([WORKLET], { type: 'text/javascript' }));
    workletReady = audioCtx.audioWorklet.addModule(url);
  }
  if (settings.audio && audioCtx.state !== 'running') audioCtx.resume().catch(() => {});
}
for (const ev of ['pointerdown', 'keydown', 'touchend']) {
  window.addEventListener(ev, () => { if (audioCtx && settings.audio && audioCtx.state !== 'running') unlockAudio(); }, true);
}
async function initAudio() {
  unlockAudio();
  await workletReady;
}

class AudioPlayer {
  constructor() {
    this.ms = 0;
    this.node = new AudioWorkletNode(audioCtx, 'gm-player', { outputChannelCount: [2] });
    this.node.port.onmessage = e => { this.ms = e.data; };
    this.node.connect(audioCtx.destination);
    this.node.port.postMessage({ targetMs: settings.audioBufferMs });
    if (settings.audio) audioCtx.resume().catch(() => {}); else audioCtx.suspend();
    this.packets = 0; this.decoded = 0;
    if (typeof AudioDecoder === 'undefined') {
      remoteLog('audio: this browser has no WebCodecs AudioDecoder');
      this.decoder = null;
      return;
    }
    AudioDecoder.isConfigSupported({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 })
      .then(s => remoteLog(`audio: context ${audioCtx.state}, opus decoder supported ${s.supported}`))
      .catch(e => remoteLog(`audio: opus support check failed: ${e.message || e}`));
    // Packets arriving but nothing decoded, or a context that never started.
    setTimeout(() => {
      if (!this.closed && (this.decoded === 0 || audioCtx.state !== 'running')) {
        remoteLog(`audio: after 3 s, ${this.packets} packets, ${this.decoded} decoded, context ${audioCtx.state}, decoder ${this.decoder?.state}`);
      }
    }, 3000);
    this.decoder = new AudioDecoder({
      output: d => {
        this.decoded++;
        const n = d.numberOfFrames, ch = d.numberOfChannels;
        const planes = [];
        for (let c = 0; c < ch; c++) {
          const p = new Float32Array(n);
          d.copyTo(p, { planeIndex: c, format: 'f32-planar' });
          planes.push(p);
        }
        d.close();
        const out = new Float32Array(n * 2);
        for (let i = 0; i < n; i++) { out[2 * i] = planes[0][i]; out[2 * i + 1] = planes[ch > 1 ? 1 : 0][i]; }
        this.node.port.postMessage(out, [out.buffer]);
      },
      error: e => remoteLog(`audio decoder error: ${e.message || e}`),
    });
    this.decoder.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 });
  }
  push(ts, data) {
    this.packets++;
    if (this.decoder?.state === 'configured') this.decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: ts, data }));
  }
  bufferedMs() { return this.ms; }
  close() { this.closed = true; try { this.decoder?.close(); } catch {} this.node.disconnect(); }
}

/* ---- cursor ------------------------------------------------------------ */

const cursorCanvas = document.createElement('canvas');
function setCursor(v, body) {
  const w = v.getUint16(0, true), h = v.getUint16(2, true);
  const hx = v.getInt16(4, true), hy = v.getInt16(6, true);
  if (w === 0 || h === 0) { canvas.style.cursor = 'none'; return; }
  cursorCanvas.width = w; cursorCanvas.height = h;
  const c = cursorCanvas.getContext('2d');
  c.putImageData(new ImageData(new Uint8ClampedArray(body.slice(8, 8 + w * h * 4).buffer), w, h), 0, 0);
  const url = cursorCanvas.toDataURL('image/png');
  // The image is in stream pixels; show it at the size it has on the stream.
  const scale = videoRect().scale || 1;
  const res = (1 / scale).toFixed(3);
  canvas.style.cursor = `-webkit-image-set(url(${url}) ${res}x) ${Math.round(hx * scale)} ${Math.round(hy * scale)}, auto`;
}

/* ---- input ------------------------------------------------------------- */

// KeyboardEvent.code → Linux evdev key code.
const KEYS = {
  Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7, Digit7: 8, Digit8: 9, Digit9: 10,
  Digit0: 11, Minus: 12, Equal: 13, Backspace: 14, Tab: 15, KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20,
  KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24, KeyP: 25, BracketLeft: 26, BracketRight: 27, Enter: 28,
  ControlLeft: 29, KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38,
  Semicolon: 39, Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43, KeyZ: 44, KeyX: 45, KeyC: 46, KeyV: 47,
  KeyB: 48, KeyN: 49, KeyM: 50, Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, NumpadMultiply: 55,
  AltLeft: 56, Space: 57, CapsLock: 58, F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65, F8: 66, F9: 67,
  F10: 68, NumLock: 69, ScrollLock: 70, Numpad7: 71, Numpad8: 72, Numpad9: 73, NumpadSubtract: 74, Numpad4: 75,
  Numpad5: 76, Numpad6: 77, NumpadAdd: 78, Numpad1: 79, Numpad2: 80, Numpad3: 81, Numpad0: 82,
  NumpadDecimal: 83, IntlBackslash: 86, F11: 87, F12: 88, IntlRo: 89, NumpadEnter: 96, ControlRight: 97,
  NumpadDivide: 98, PrintScreen: 99, AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105,
  ArrowRight: 106, End: 107, ArrowDown: 108, PageDown: 109, Insert: 110, Delete: 111, AudioVolumeMute: 113,
  AudioVolumeDown: 114, AudioVolumeUp: 115, NumpadEqual: 117, Pause: 119, NumpadComma: 121, IntlYen: 124,
  MetaLeft: 125, MetaRight: 126, ContextMenu: 127, F13: 183, F14: 184, F15: 185, F16: 186, F17: 187, F18: 188,
  F19: 189, F20: 190, F21: 191, F22: 192, F23: 193, F24: 194,
};
const BUTTONS = [0x110, 0x112, 0x111, 0x113, 0x114]; // left, middle, right, back, forward

const held = new Set();
function sendKey(code, pressed) {
  session?.send(message(C_KEY, 3, v => { v.setUint16(0, code, true); v.setUint8(2, pressed ? 1 : 0); }));
}
function releaseAll() {
  for (const code of held) sendKey(code, false);
  held.clear();
}

window.addEventListener('keydown', e => {
  if (e.ctrlKey && e.altKey && e.shiftKey && e.code === 'KeyH') {
    settings.hud = !settings.hud; saveSettings(); applySettings(); e.preventDefault(); return;
  }
  if (e.ctrlKey && e.altKey && e.shiftKey && e.code === 'KeyS') { toggleSettings(); e.preventDefault(); return; }
  if (!session || !panel.hidden) return;
  const code = KEYS[e.code];
  if (code === undefined) return;
  e.preventDefault();
  if (e.repeat || held.has(code)) return; // gamescope repeats keys itself
  held.add(code);
  sendKey(code, true);
});
window.addEventListener('keyup', e => {
  const code = KEYS[e.code];
  if (!session || code === undefined || !held.has(code)) return;
  e.preventDefault();
  held.delete(code);
  sendKey(code, false);
});
window.addEventListener('blur', releaseAll);
document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });

function requestLock() {
  const p = canvas.requestPointerLock?.({ unadjustedMovement: settings.rawMouse });
  p?.catch?.(() => canvas.requestPointerLock()?.catch?.(() => {}));
}
function updateHint() {
  hint.hidden = !(session && session.locked && !document.pointerLockElement);
}
document.addEventListener('pointerlockchange', () => {
  updateHint();
  if (!document.pointerLockElement) releaseAll();
});

const moveEvent = 'onpointerrawupdate' in window ? 'pointerrawupdate' : 'pointermove';
canvas.addEventListener(moveEvent, e => {
  if (!session) return;
  if (document.pointerLockElement === canvas) {
    // Relative motion, in stream pixels.
    const s = 1 / (videoRect().scale || 1);
    const dx = e.movementX * s, dy = e.movementY * s;
    if (dx || dy) session.send(message(C_MOTION, 8, v => { v.setFloat32(0, dx, true); v.setFloat32(4, dy, true); }));
  } else {
    const r = videoRect();
    const x = Math.min(1, Math.max(0, (e.clientX - r.x) / r.w));
    const y = Math.min(1, Math.max(0, (e.clientY - r.y) / r.h));
    session.send(message(C_POSITION, 8, v => { v.setFloat32(0, x, true); v.setFloat32(4, y, true); }));
  }
});
canvas.addEventListener('pointerdown', e => {
  if (!session) return;
  e.preventDefault();
  if (session.locked && !document.pointerLockElement) { requestLock(); return; }
  const b = BUTTONS[e.button];
  if (b !== undefined) session.send(message(C_BUTTON, 3, v => { v.setUint16(0, b, true); v.setUint8(2, 1); }));
});
canvas.addEventListener('pointerup', e => {
  if (!session) return;
  e.preventDefault();
  const b = BUTTONS[e.button];
  if (b !== undefined) session.send(message(C_BUTTON, 3, v => { v.setUint16(0, b, true); v.setUint8(2, 0); }));
});
canvas.addEventListener('contextmenu', e => e.preventDefault());
canvas.addEventListener('wheel', e => {
  if (!session) return;
  e.preventDefault();
  // Wayland axis units are ~15 per wheel notch; Chrome reports 100 px per notch.
  const k = e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? 800 : 1;
  const dx = e.deltaX * k * 0.15, dy = e.deltaY * k * 0.15;
  const v120x = Math.round(e.deltaX * k * 1.2), v120y = Math.round(e.deltaY * k * 1.2);
  session.send(message(C_SCROLL, 12, v => {
    v.setFloat32(0, dx, true); v.setFloat32(4, dy, true);
    v.setInt16(8, v120x, true); v.setInt16(10, v120y, true);
  }));
}, { passive: false });

/* ---- gamepads ---------------------------------------------------------- */

const padState = new Map(); // index → last sent state key
function pollGamepads(s) {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  const seen = new Set();
  for (const p of pads) {
    if (!p || !p.connected) continue;
    seen.add(p.index);
    let buttons = 0;
    for (let i = 0; i < Math.min(17, p.buttons.length); i++) if (p.buttons[i].pressed) buttons |= 1 << i;
    const ax = i => Math.max(-32768, Math.min(32767, Math.round((p.axes[i] || 0) * 32767)));
    const trig = i => Math.round((p.buttons[i]?.value || 0) * 255);
    const st = [buttons >>> 0, ax(0), ax(1), ax(2), ax(3), trig(6), trig(7)];
    const k = st.join(',');
    if (padState.get(p.index) === k) continue;
    padState.set(p.index, k);
    s.send(message(C_GAMEPAD, 15, v => {
      v.setUint8(0, p.index); v.setUint32(1, st[0], true);
      v.setInt16(5, st[1], true); v.setInt16(7, st[2], true); v.setInt16(9, st[3], true); v.setInt16(11, st[4], true);
      v.setUint8(13, st[5]); v.setUint8(14, st[6]);
    }));
  }
  for (const i of [...padState.keys()]) {
    if (!seen.has(i)) {
      padState.delete(i);
      s.send(message(C_GAMEPAD_REMOVE, 1, v => v.setUint8(0, i)));
    }
  }
}

function rumble(index, strong, weak, ms) {
  const p = navigator.getGamepads?.()[index];
  const act = p?.vibrationActuator;
  if (!act) return;
  if (ms === 0 || (strong === 0 && weak === 0)) { act.reset?.(); return; }
  act.playEffect('dual-rumble', { duration: ms, strongMagnitude: strong / 65535, weakMagnitude: weak / 65535 }).catch(() => {});
}

/* ---- start ------------------------------------------------------------- */

function showOverlay(reason) {
  statusEl.textContent = reason || 'gmstream';
  overlay.hidden = false;
  hint.hidden = true;
}

// Reconnect on our own after a drop (host restart, network blip) until the
// host refuses us or another client takes over.
let wantConnected = false;
let retryTimer = 0;
function scheduleReconnect() {
  clearTimeout(retryTimer);
  retryTimer = setTimeout(() => {
    if (wantConnected && !session) connect(false);
  }, 2000);
}

async function connect(fullscreen) {
  if (session) return;
  wantConnected = true;
  clearTimeout(retryTimer);
  if (!('WebTransport' in window) || !('VideoDecoder' in window)) {
    showOverlay('This browser lacks WebTransport or WebCodecs (Chrome works).');
    return;
  }
  try {
    if (fullscreen) await document.documentElement.requestFullscreen({ navigationUI: 'hide' }).catch(() => {});
    if (document.fullscreenElement && navigator.keyboard?.lock) await navigator.keyboard.lock().catch(() => {});
    await initAudio();
    statusEl.textContent = 'Connecting…';
    session = new Session();
    await session.start();
  } catch (e) {
    remoteLog(`connect failed: ${e.stack || e}`);
    const reason = `Connection failed: ${e.message || e}`;
    if (session) session.close(reason);
    else if (wantConnected) { showOverlay(`${reason} — reconnecting…`); scheduleReconnect(); }
    else showOverlay(reason);
    session = null;
  }
}

document.getElementById('fullscreen').onclick = () => { unlockAudio(); connect(true); };
document.getElementById('window').onclick = () => { unlockAudio(); connect(false); };
/* ---- settings panel ---------------------------------------------------- */

const panel = document.getElementById('settings');
const field = name => panel.querySelector(`[name="${name}"]`);

function applySettings() {
  hud.hidden = !settings.hud;
  canvas.style.imageRendering = settings.smoothing ? 'auto' : 'pixelated';
  if (audioCtx) settings.audio ? audioCtx.resume() : audioCtx.suspend();
  session?.audio?.node.port.postMessage({ targetMs: settings.audioBufferMs });
}

function fillPanel() {
  for (const [k, v] of Object.entries(settings)) {
    const el = field(k);
    if (!el) continue;
    if (el.type === 'checkbox') el.checked = v; else el.value = v;
  }
  field('bitrateMbps').parentElement.querySelector('output').textContent = `${settings.bitrateMbps} Mb/s`;
  field('scale').parentElement.querySelector('output').textContent = `${settings.scale}%`;
  const { w, h } = wantedSize();
  document.getElementById('settings-info').textContent =
    `Requests ${w}x${h} @ ${(refreshMhz() / 1000).toFixed(0)} Hz` +
    (session ? `; streaming ${CODECS[session.codec]?.name} ${canvas.width}x${canvas.height}` : '');
}

function onSettingChange(e) {
  const el = e.target;
  const k = el.name;
  if (!(k in settings)) return;
  const old = settings[k];
  settings[k] = el.type === 'checkbox' ? el.checked
    : typeof DEFAULTS[k] === 'number' ? Number(el.value) : el.value;
  saveSettings();
  if (k === 'bitrateMbps' && session) {
    session.send(message(C_BITRATE, 4, v => v.setUint32(0, Math.round(settings.bitrateMbps * 1000), true)));
  }
  if (['scale', 'maxHeight', 'customSize', 'refresh'].includes(k)) scheduleConfigure();
  if ((k === 'codec' || k === 'videoStreams') && session && old !== settings[k]) {
    const fs = document.fullscreenElement != null;
    session.close('', true);
    connect(fs);
  }
  applySettings();
  fillPanel();
}

function toggleSettings(show = panel.hidden) {
  panel.hidden = !show;
  if (show) {
    if (document.pointerLockElement) document.exitPointerLock();
    releaseAll();
    fillPanel();
  }
}

panel.addEventListener('input', e => { if (e.target.type === 'range') onSettingChange(e); });
panel.addEventListener('change', onSettingChange);
document.getElementById('settings-close').onclick = () => toggleSettings(false);
document.getElementById('settings-reset').onclick = () => {
  Object.assign(settings, DEFAULTS);
  saveSettings();
  applySettings();
  scheduleConfigure();
  fillPanel();
};
document.getElementById('gear').onclick = () => toggleSettings();
document.getElementById('overlay-settings').onclick = () => toggleSettings(true);
document.addEventListener('pointerlockchange', () => {
  document.getElementById('gear').hidden = document.pointerLockElement != null;
});

applySettings();
if (params.has('autostart')) connect(false); // testing: no click needed
