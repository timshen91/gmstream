# gmstream: SteamOS Game Mode streaming server

Design and handover notes. Status as of 2026-10-26: SteamOS Game Mode streams
end to end on the test machine through gmstream and a patched build of Valve's
gamescope (§6): boot animation, Steam UI, games (launch, play, exit),
keyboard, mouse, client-drawn cursor, gamepads, audio, reconnects; about
7–8 ms capture→display to Chrome on a Mac over Wi-Fi. Panel mode (the
built-in screen while nobody streams) is not done.
Wire format: [PROTOCOL.md](PROTOCOL.md).

Formerly "polaris-vd" (from when it was going to feed Polaris). It uses no
Polaris, Moonlight or GameStream code or protocol.

## 1. Goal and scope

Stream SteamOS Game Mode from a SteamOS machine (dev box: the test machine, an ASUS
ROG Flow Z13 GZ302EA, Strix Halo / Radeon 8060S, SteamOS 3.9) to a client
at the client's own resolution and refresh rate, with:

- **Stock Game Mode.** Valve's gamescope, Steam, Steam Input, the Quick
  Access Menu and MangoHud overlay all behave as on a Steam Deck. We only
  replace *where gamescope's frames go*.
- **Free per-client resolution.** A client connecting at 2560x1600@90 gets
  exactly that. Switching clients or resolutions does not restart the session.
- **VRR semantics.** Frames leave the host the moment gamescope produces them
  (no compositor tick), capped at the client's refresh rate.
- **Lowest latency** at every step (§5).
- **Valve's input handling.** Mouse, keyboard and touch enter through
  gamescope's normal Wayland seat; gamepads appear as real controllers so
  Steam Input handles them.
- **The built-in screen still works** (panel mode, §6.3). With no client
  connected, the machine is a normal handheld/laptop running Game Mode.
- **One binary, no host tools.** A single executable that runs directly on
  SteamOS: no container at run time, nothing installed on the read-only root.
- **Minimal in-house complexity.** No KDE, no desktop portals, no window
  manager, no Windows support, no multi-desktop features.

Out of scope: Desktop Mode, non-Steam desktops, Windows hosts, Moonlight
compatibility, multiple simultaneous clients.

Clients: **web first** (Chrome on Mac/PC). Native clients (macOS, iPad,
Android) come later on the same protocol.

## 2. Architecture

```
                 the test machine (SteamOS, Game Mode session)
 ┌──────────────────────────────────────────────────────────────────────┐
 │  Steam ──X11──► gamescope (Valve's, unmodified)                      │
 │                 --backend wayland --force-composition --adaptive-sync│
 │                 │ one composited dma-buf per frame    ▲ wl_seat input │
 │                 ▼                                     │              │
 │  ┌────────────────────────── gmstream (one process) ───────────────┐ │
 │  │ C core (core/), compositor thread:                              │ │
 │  │  • Wayland host for exactly one client (wlroots 0.20, headless) │ │
 │  │  • VRR emulation: present slots, max = client refresh           │ │
 │  │  • at the slot: VA-API VPP blit RGB→NV12 (the one GPU copy)     │ │
 │  │  • input → seat; cursor image → client; uinput gamepads         │ │
 │  │ encoder thread: FFmpeg h264/hevc/av1_vaapi → bitstream          │ │
 │  │ PipeWire thread: default sink monitor → Opus 5 ms               │ │
 │  │        │ in-process callbacks             ▲ gm_* commands        │ │
 │  │        ▼                                  │ (eventfd queue)      │ │
 │  │ Rust (src/), tokio: WebTransport (wtransport/quinn) + HTTPS page │ │
 │  └──────────────────────────────┬──────────────────────────────────┘ │
 └─────────────────────────────────┼────────────────────────────────────┘
                                   │ QUIC/UDP + HTTPS/TCP, port 8443, LAN
                                   ▼
          Web client (Chrome): WebCodecs → desynchronized canvas,
          native cursor, Pointer/Keyboard Lock, Gamepad API, AudioWorklet
```

Why this shape:

- **The compositor is the capture point.** gamescope's Wayland backend
  commits its finished frame to its host compositor. Being that host, we hold
  every frame the instant it exists, with exact timing, and need no capture
  protocol, no PipeWire video node, no screencopy.
- **One process.** The bitstream goes from the encoder thread to the QUIC
  stack by a function call; input goes the other way through a queue the
  compositor's event loop wakes on. No unix socket, no second daemon.
- **C for the compositor, Rust for the network.** wlroots, VA-API and
  PipeWire are C APIs; QUIC/HTTP3/WebTransport is easy in Rust
  (`wtransport`) and painful in C. The C core is a static library behind one
  header (`core/gm.h`).
- **gamescope stays Valve's binary.** SteamOS's root is read-only and we must
  not unlock it. Everything goes through gamescope's command line and the
  Wayland protocol it already speaks.

### Linking

Everything is linked statically except what has to match the host:

| linked statically (built in the image, `build/deps.sh`) | from the host |
|---|---|
| wlroots 0.20.2, wayland 1.26, xkbcommon 1.13 (+ a compiled US keymap), pixman, libdrm, libffi, libva 2.24 + libva-drm, FFmpeg 8.1.2 (libavcodec/libavutil, only the three VA-API encoders), Opus 1.6.1, all Rust crates (rustls/ring, quinn, tokio) | glibc (≥ 2.38 needed), libgcc_s, libEGL/libGLESv2 (glvnd), libgbm, the VA driver (`/usr/lib/dri/radeonsi_drv_video.so`, dlopened by our libva), libpipewire-0.3 |

The host side is the GPU and audio stack, which must match the host's
drivers and daemons anyway. The binary is ~7 MB. It is built on Debian
trixie (glibc 2.41), older than SteamOS's glibc (2.43), so it runs there.

## 3. Components

### 3.1 Compositor (`core/compositor.c`)

- **Backend:** wlroots headless backend, one output `HEADLESS-1`, GLES2
  renderer and GBM allocator (for linux-dmabuf and cursor texture reads).
- **Globals** (what gamescope's Wayland backend needs, §4.1): wl_compositor
  v6, wl_subcompositor, wl_shm, linux-dmabuf v4, xdg_wm_base v6,
  xdg-decoration (always server-side), wp_viewporter, wp_presentation v2,
  single-pixel-buffer, fractional-scale, relative-pointer, pointer-
  constraints, wl_seat (pointer + keyboard), data-device, primary-selection,
  wl_output + xdg-output. Nothing else (the capture and virtual-input
  protocols of the Polaris plan are gone).
- **One view.** The first xdg_toplevel is "the screen": configured
  fullscreen and activated at the requested size, gets `wl_surface.enter`
  (gamescope reads the VRR maximum from the output mode) and focus.
- **Frame path** (`handle_view_commit` → `try_present`):
  1. A root-surface commit with a buffer gets a present slot: `now`, or
     `last_slot + 1/refresh` if later; a commit replacing a still-queued frame
     inherits its slot.
  2. **Immediately** send `wp_presentation_feedback.presented` with the slot
     time and `refresh = 0` (VRR). See §4.3 for why this must be immediate.
  3. Wait for the dma-buf's implicit fence (checked synchronously first; in
     practice always signaled, §4.2).
  4. At the slot time (absolute `timerfd`): hand the buffer to the encoder,
     send frame callbacks, update the output mode if the buffer size
     changed. There is no output commit of the buffer at all.
- **Resize:** `gm_stream_start`/`gm_stream_configure` set the target size and
  refresh and send gamescope an xdg_toplevel configure. A refresh change is
  committed to the output mode at once (it is gamescope's VRR maximum); the
  size follows whatever buffer size gamescope actually commits.
- **Input:** a server-side `wlr_keyboard` with a keymap compiled at build
  time (no xkb data needed at run time); pointer motion produces
  relative-pointer events always and absolute motion unless a lock is
  active; the newest pointer constraint is activated unconditionally. Held
  keys and buttons are released when a client leaves.
- **Cursor:** gamescope sets its cursor with `wl_pointer.set_cursor` (shm).
  We read the cursor surface's texture on each commit, convert it to
  straight-alpha RGBA and send it (deduplicated) to the client, which makes
  it the native browser cursor: zero-latency cursor motion.
- **Commands from other threads** (`gm_*` in `core/gm.h`) go into a
  mutex-protected queue and wake the event loop through an eventfd.

### 3.2 Encoder (`core/encode.c`)

- Opens a VA display on the renderer's DRM node and wraps it as an FFmpeg
  VAAPI device. Probes H.264 High, HEVC Main and AV1 encode entrypoints.
- **Copy at present time (compositor thread):** gamescope's dma-buf is
  imported as a VA RGB surface (cached per wl_buffer: gamescope cycles three),
  and one VPP blit converts it into an NV12 surface from an FFmpeg frame pool
  (BT.709, limited range). Only the GPU work is queued here; nothing waits.
- **Encode (worker thread):** `vaSyncSurface` on the NV12 surface, then
  `avcodec_send_frame`/`receive_packet` with `async_depth=1`, no B-frames,
  GOP 2^30 (IDR only when asked), VBR (CBR fallback) with maxrate = bitrate
  and a two-frame VBV. A size, codec, bitrate or refresh change reopens the
  encoder (which starts with an IDR). At most one frame waits for the worker;
  a newer one replaces it.
- The VA-API quality level makes no measurable difference on radeonsi
  (tested 1–31: 4.5–5.1 ms at 2560x1600), so it is left at the default.
- **AV1 on the Vulkan video encode queue (`core/vkencode.c`):** gamescope's
  frame is encoded where it lies, with no copy and no conversion pass, and
  nothing on the CPU waits for the GPU. This is the AV1 path; the VA-API one
  below is the reference (`GM_AV1_VAAPI`). See §12.4.
- **AV1 in pieces (`core/vastream.c`):** for a client that asks, the frame is
  encoded in four tile groups and each piece of the bitstream goes out as the
  GPU writes it, so most of the frame is on the wire before the encode is
  finished. The bytes are the encoder's own, byte for byte. See §12.

### 3.3 Audio (`core/audio.c`)

PipeWire stream with `stream.capture.sink=true` (default sink's monitor),
F32 stereo 48 kHz, `node.latency=240/48000`; Opus
`RESTRICTED_LOWDELAY`, 128 kb/s, 5 ms frames; only while a client streams.
Local speakers keep playing.

### 3.4 Gamepads (`core/gamepad.c`)

Each client gamepad becomes a uinput "Microsoft X-Box 360 pad"
(045e:028e) with the xpad axis layout; Steam Input picks it up. Force
feedback uploads (FF_RUMBLE) are tracked and played effects are forwarded as
`Rumble` messages. `/dev/uinput` is already accessible to `deck`.

### 3.5 Network (`src/`)

- `session.rs`: wtransport server (Cubic with a 4 MB initial window and a
  16 MB send window: a keyframe should not need several RTTs of window
  growth on a LAN), one client at a time (a new one replaces the old), one
  uni stream per frame at rising priority, 250 ms frame deadline, audio as
  datagrams, control stream reader/writer. Certificate: self-signed ECDSA,
  13 days, rotated weekly, pinned by the page via `serverCertificateHashes`.
- `web.rs`: tiny HTTPS/1.1 server for `index.html`, `client.js` and
  `config.json`; a persistent self-signed certificate (800 days, SANs:
  hostname, `hostname.local`, localhost, current IPs; regenerated when
  those change) in `~/.local/state/gmstream/`.
- `main.rs`: blocks SIGCHLD/SIGINT/SIGTERM before any thread starts (the
  compositor takes them through signalfd), starts tokio, runs the compositor
  on the main thread.

### 3.6 Web client (`web/`)

- `VideoDecoder` with `optimizeForLatency`, frames drawn in the output
  callback onto a `desynchronized` 2D canvas (no requestAnimationFrame wait).
- Frames are decoded strictly in id order; a missing frame waits 30 ms
  before the client skips to the next keyframe and asks for one.
- Input: `pointerrawupdate` (absolute, or relative under Pointer Lock with
  `unadjustedMovement`), Keyboard Lock in fullscreen, evdev keycodes from
  `KeyboardEvent.code`, Gamepad API polled every 4 ms, rumble via
  `vibrationActuator`.
- Audio: `AudioDecoder` (Opus) → AudioWorklet ring buffer (10 ms prebuffer,
  trims back to 20 ms above 40 ms).
- HUD (Ctrl+Alt+Shift+H): fps, bitrate, capture→display, network, decode,
  RTT, audio buffer. `?codec=`, `?bitrate=`, `?maxheight=`, `?hud=0`;
  `?autostart&log` are for automated tests.

### 3.7 Native macOS client (`mac/`)

A reference client for latency work: plain UDP (PROTOCOL.md, "Plain UDP"),
built with the Command Line Tools' `swiftc` (`mac/build.sh`, no Xcode).

Source layout (shared with the iPad app, §3.8):
- `apple/`: everything but the app shell. Session.swift is one stream
  (connection, decoders, renderer, pacing, audio, controllers, stats, the
  input messages); it asks the app for its view and screen through
  `SessionShell`. The rest are the parts it uses (Connection, Video,
  Phase, PanelRate, Latency, Audio, Gamepads, PyroWave, Mock, Options,
  Stats, Trace), with `#if os(macOS)` where the platforms differ (vsync
  off, the audio device buffer, text drawing).
- `mac/Sources`: the AppKit app (main.swift: window, menu, keyboard
  capture, the cursor decision tree, pointer lock; Settings.swift; Keys.swift,
  macOS key codes; Display.swift, VRR detection).

Unit tests (`mac/test.sh [Class[/testMethod]]`, needs Xcode for XCTest; ~10 s):
`apple/` and `mac/Sources` except `main.swift` and `Settings.swift`,
compiled with `mac/Tests` into one XCTest bundle. They cover:
- the wire format, and Connection against a fake host on loopback (Hello,
  control reassembly out of order, video fragments, audio, PyroWave
  datagrams, the clock, keep-awake);
- the decoders, fed by this Mac's VideoToolbox, PyroWave and Opus
  encoders;
- the phase lock against a simulated display and host (sweep, lock,
  following a slower network, drain, vsync-off steering);
- the hold, PanelRate, FrameGate, the stats, the options and Pacing;
- Session against a fake host with a fake shell (the Hello's size and
  rate, Auto pacing and Steam's VRR policy, vsync off waiting for the
  grid, settings changes, keys, the click that recaptures the pointer,
  positions, scrolling, letterboxing, the host cursor's image, the forced
  client cursor).

Not covered yet: the app shells (the macOS cursor decision tree, keyboard
capture, iPad touch and pointer input), MetalRenderer, audio output,
controllers.

- Video: H.264/HEVC, and AV1 where the hardware decodes it (M3 and later:
  `Decoder.hasAV1`; apple/AV1.swift), through VideoToolbox (NV12 output,
  converted in our shader), PyroWave through its Metal port (`build/pyrowave`, same commit
  as the host). Frames are drawn on a CAMetalLayer from a render thread.
- AV1 (2026-11-01; not yet run against the live host): VideoToolbox takes a
  frame's OBUs without temporal delimiters, plus an av1C record built from
  the sequence header. Clips from the test machine's `av1_vaapi` at 2560x1600, 40
  Mb/s, paced at 120 Hz, decoded on the M4 (3 runs): AV1 2.1 / 2.4 / 4.1 ms
  p50 / p90 / p99, HEVC 2.0 / 2.3 / 2.8; frame 100's luma is bit-identical
  to dav1d. The encoder (AMD VCN) pads some heights (360 -> 362, 1080 ->
  1082; 1600 stays): the frame header carries the size asked for, and the
  renderer leaves out the rows below it. `mac/Tests/av1-640x360.obu` is
  eight frames from that encoder.
- Presentation (measured on an M4 MacBook Air, 60 Hz panel, in §5):
  - vsync on, and the private `CAMetalLayer.disableFIFO`: a newer drawable
    replaces a queued one. With FIFO, one frame that misses its refresh
    delays every later one by a refresh for good (the source runs at the
    panel's rate, so the queue never drains): the display stage sat at 24,
    39, 55 or 71 ms depending on history. Without FIFO it is dropped instead.
  - Phase lock (`Sources/Phase.swift`, protocol `DisplayGrid`): the client
    sends its refresh grid in the host's clock; gmstream reports
    gamescope's frames as presented on it, so gamescope's own just-in-time
    scheduler produces frames on that grid. The feedback still says VRR
    (refresh 0): gamescope's Wayland backend calls the display VRR-capable
    only then, and Steam hides its VRR toggle otherwise; with the toggle off
    gamescope paces to the timestamps all the same. The client sweeps
    the grid's shift through one refresh (the display stage is a sawtooth
    with one cut-off) and locks at the lowest step without drops. Each
    frame is then placed by its lead: submit → the next point in the
    refresh where flips land (the refresh interval measured, not the
    screen's nominal one: §8). Which leads get shown is learned per 0.25 ms
    bin; the lock steers the host present's lead to where the recent
    latenesses (submit − host present, no wrap) would have been shown most
    often, which also follows the drift between the two machines' clocks
    (tens of ppm). gmstream holds frames gamescope committed early to its
    usual commit time before the vblank (`grid_release`). If most frames start
    showing a refresh late with every lead fine (the window server one
    frame deeper), the renderer skips one frame to drain it.
    Settings → "Pace the host to this display's refreshes" (on by default;
    `--no-phase` for one launch) turns it off: the client then sends an
    empty grid, as on a VRR display, gamescope paces itself and frames are
    shown as they arrive. Separate from vsync ("lowest latency").
  - Full screen with "lowest latency" (Settings; vsync off only while in
    full screen): the window server stops compositing (private
    `displayCompositingInternalStatus` 0) and frames flip straight to the
    panel: display stage ~4 ms instead of ~20. It tears: Metal's presented
    times (the flips) land wherever the frame was submitted, while with vsync
    they all land at one phase (GM_DEBUG_FLIP logs each flip's phase). The
    phase lock stays on there: it steers the flips to 0.3 ms before the
    vblank (`GM_FLIP_LEAD`), where a tear shows least, on the panel's grid
    as measured from vsync-on flips (vsync stays on for the first second
    of frames, and again after the screen changes, wakes or is
    reconfigured, until it is measured). Mock on the built-in panel: 83% of
    flips within 1.75 ms before the vblank, display 3.3 ms p50. The other
    17% flip 3–4 ms later (tearing 15–22% down the screen), about one frame
    in six at irregular intervals of ~100 ms, wherever the target is: the
    GPU is done 1.3 ms after submit either way, so the delay is in macOS's
    flip path (not the stats overlay, the cursor, or the terminal
    redrawing). Not explained yet.
  - Full screen on a variable-refresh (Adaptive-Sync) display: the panel
    refreshes when a frame arrives, so vsync costs nothing. The settings
    alone decide what happens there (the app used to turn pacing and
    "lowest latency" off on such a display by itself): with pacing off and
    vsync on, it sends an empty grid (interval 0: gamescope paces itself)
    and shows frames as they come, tear-free. Measured that way on a Dell U2725QE
    (48–120 Hz) from an M4 MacBook Air: total 12.6 / 13.7 ms p50 / p90,
    display stage 3.1 / 3.6 ms, pacing p90 ~2 ms. `present(atTime:)` to
    smooth pacing made both pacing and p90 worse. In a window the desktop is
    composited at a fixed rate (22–25 ms total).
- Settings → Pacing: **Auto** (the default), **Low latency** or **Manual**.
  The two models of Auto do not mix. Auto, in full screen on an Adaptive-Sync display: the hold on, the
  phase lock off, Steam's VRR forced on (protocol VrrPolicy 2: the game runs
  free, gamescope commits each frame when it is ready, gmstream passes it on
  at once stamped with that commit, and the hold makes commit → present a
  constant). Auto otherwise (a window, a fixed-refresh display): the phase
  lock on, no hold, Steam's VRR off and its toggle greyed (VrrPolicy 1: the
  grid reported as a fixed refresh, or the stream's rate with no grid). Low
  latency: frames as they come, no phase lock, no hold, vsync off in full screen
  (may tear; the iPad keeps vsync), and gamescope's VRR on on any display, as
  that is what lets the game run free on the host; the client just shows frames
  as they come. Manual: phase lock, hold, vsync off
  and VRR as set, except that the host never times itself: frames go by the
  game (VRR) or by the client's grid (phase lock), so with VRR not forced on,
  phase lock is on. Neither would leave gamescope stepping on its own 60 Hz timer,
  and Warcraft III then alternated one and two refreshes at 43 fps. VRR is Auto, as Auto pacing does it; Off; On, a capable display
  in a window too, whatever Steam's toggle shows (`adaptive_sync` is set directly;
  the toggle stays and flipping it can undo this: hiding it while VRR runs takes
  a gamescope change); or Steam, its toggle decides. Apart from Low latency, a
  display that can't do VRR always has it off. Settings shows what the mode chose.
- macOS does VRR on the Dell U2725QE in full screen, the way the panel's
  ticks allow: it refreshes on its own fastest ticks (8.3443 ms, 119.84 Hz,
  not the nominal 1/120 s) and only waits for a frame that comes after the
  next tick. Test frames in full screen: at 100 Hz flips 10.0 ms apart (true
  VRR); at 119.6 Hz 98% of flips on the ticks, at 118 45%, at 110 9%;
  present → screen 3.3 ms p50 from 117 Hz down. So the host is capped at the
  panel's shortest refresh + 0.2 ms (117.0 Hz here; `PanelRate`, measured
  while saturated) and the hold never presents two frames closer than the
  shortest refresh + 0.1 ms. Two more things broke it: redraws on the other
  display (iTerm on the built-in panel: a late flip every ~100 ms while it
  redrew), and the draw sharing the PyroWave decoder's Metal queue (a held
  frame's draw waited behind the next frame's 4K decode). Result (Ender
  Lilies' title, 4K PyroWave 200 Mb/s over Wi-Fi): commit → present within
  ±0.4 ms inside steady stretches, the hold moving in steps of a few ms when
  the network's tail moves; 117 fps, 0.04 drops/s.
- Settings → "Even out network jitter" (`--hold`): each frame is shown a
  fixed time after the host presented it (`Hold` in `Sources/Video.swift`),
  so encode + network + decode add a constant instead of their jitter. In full
  screen on a variable-refresh display, where present → screen is ~3.3 ms
  and steady, the screen then keeps the host's frame spacing as a local VRR
  panel keeps the game's. No clock sync is needed: frames are held until
  host time + C, with C learned from (our time − host time), so the clock
  offset is part of C. C stays flat and jumps to a new edge (the 99.5th
  percentile of the last 2 s of latenesses + 0.3 ms) when the network's tail
  moves: up at once after two late frames within 0.5 s, down after the edge
  has sat 1 ms+ under C for a second. A short step, then flat: an
  edge-creeping quantile tracker (0.5 ms then 0.1 ms steps, slewed) was
  smooth but never still on the latency chart (measured with 0.5 ms steps,
  window, Wi-Fi: 28.5 → ~21 ms in ~40 s, then following the network to
  18.7 ms at 200 Mb/s, pacing p90 0.0 ms). Ping-based sync on
  busy Wi-Fi (round trip 2.2 / 8.8 / 14.6 ms min / p50 / p90) moved by
  0.5–2.6 ms however the pings were filtered, which a hold cannot absorb.
  The overlay shows a "hold" stage, the delay and late frames. On 4K at
  500 Mb/s over Wi-Fi: delay ~22–25 ms, 0–2 late frames per 5 s.
- The stats overlay is drawn into the video frame (one extra blended quad),
  not as AppKit views over the Metal layer: a view that changes (the chart,
  10 times a second) makes macOS composite the window for that frame
  instead of flipping it straight to the display. As views, on the VRR
  display: display stage p90 6.9 ms, pacing p90 7.5; drawn in, 3.6 and 2.
- Color: the Metal layer is tagged sRGB, so macOS converts the stream to
  the display (checked with a test card through the whole Mac path, and
  against gamescope's own screenshot: within ±1–3). That looks muted next
  to Windows, which does not color-manage (sRGB stretched to a wide-gamut
  panel), and the Steam Deck's screen (Steam's color vibrance). Settings →
  "Vivid colors" tags the layer with the display's own color space
  instead: the pixels go to the panel unconverted, as on Windows.
- Keyboard: macOS first, then the host, Command as Meta. The window
  server's shortcuts (Cmd+Tab, Spotlight, Mission Control, screenshots) never
  reach the app, so games don't get them. A local event monitor then offers
  each key to the app's menu and sends the rest to the host; it runs before
  AppKit's key-view handling (Ctrl+Tab / Ctrl+Shift+Tab, Steam's Quick
  Access, never reach keyDown otherwise; key-ups with Command held are
  dropped by AppKit). Modifiers come from the event's flags, not a toggle:
  focus can return with one held (Cmd+Tab). Until 2026-11-01 the private
  `CGSSetGlobalHotKeyOperatingMode` turned the window server's shortcuts off
  and the whole keyboard went to the host.
- Stats overlay (menu, or Settings): per-stage p50 / p99 / max over 5 s (encode,
  network, queue, decode, render, display, total present → on screen), a
  pacing row (|presentation interval − capture interval|), dropped frames,
  the phase state, and a 10 s latency chart.
- Input: keyboard (evdev codes from macOS key codes; Cmd stays with macOS),
  mouse, scroll, pointer capture when the game asks for relative input,
  gamescope's cursor as the native cursor, game controllers through the
  GameController framework (Home/Options released from macOS's system
  gestures), rumble through CoreHaptics. Audio: Opus through Apple's
  decoder into AVAudioEngine with a 20 ms jitter buffer.
- Socket traffic class `SO_NET_SERVICE_TYPE` = interactive video.
- Measuring switches: `--stats-log`, `--hide-stats`, `--fullscreen`,
  `--no-vsync`, `--layer key=v,…` / `--drawable key=v,…` (private
  CAMetalLayer / CAMetalDrawable properties by key), `--bgra`, `--probe N`;
  env `GM_DEBUG_PHASE`, `GM_DEBUG_DECODE`, `GM_DEBUG_NET`, `GM_NET_SERVICE`,
  `GM_DEBUG_FRAMES` (one line per frame shown or dropped, every timestamp
  from gamescope's present to the flip), `GM_DEBUG_STALL=<s>` (one 30 ms
  render hiccup).

### 3.8 Native iPadOS client (`ipad/`)

The macOS client's Session (`apple/`) in a UIKit app: same protocol,
decoders (PyroWave's Metal decoder too), pacing, stats overlay and
settings, built with Xcode through XcodeGen (`ipad/build.sh`; signing team
in `GM_TEAM`, a free Apple ID's profiles last 7 days).
`ipad/run.sh [ARGS]` installs it on the connected iPad and launches it with
the macOS client's options (`--mock`, a host, `--stats-log`) and `GM_*`
environment, printing its output.

- Display: full screen; a CADisplayLink holds ProMotion at its top rate
  (120 Hz), so the panel is a fixed refresh: Auto pacing is the phase lock
  (Steam's VRR off). No vsync off on iPadOS.
- Input: fingers are the host's touchscreen (protocol Touch, gamescope
  patch 0006; SteamOS passes touches through to Steam and the game, as
  the Deck's own touchscreen), or with the setting off, a finger is the
  pointer with the left button held; a mouse or
  trackpad moves the pointer by hover and clicks with its buttons
  (UIApplicationSupportsIndirectInputEvents); trackpad and wheel scrolling;
  the hardware keyboard by HID usage (`ipad/Sources/Keys.swift`), except
  the shortcuts iPadOS keeps. Pointer lock is `prefersPointerLocked` (only
  in full screen, when iPadOS grants it) with motion and buttons from
  GCMouse. Game controllers as on macOS.
- Cursor: iPadOS's pointer takes no image, so it is hidden over the stream
  and the host's cursor is drawn into our frames (as on a macOS VRR
  display).
- The mouse (macOS). The cursor over the stream is always ours, drawn into the
  frame after decoding: the macOS cursor is hidden wherever ours appears, since
  a cursor sprite WindowServer has to move pins the Dell at a fixed 120 Hz (§8).
  Ours is redrawn once per the display's shortest refresh while it moves, by
  drawing the last decoded frame again, so it moves at the panel's rate however
  slow the stream is (`setCursor(local:)`, Video.swift; the stats line's
  `cursor ... N fps` is the measured rate).
  What changes with the window is who moves the pointer. **Full screen** the
  mouse is ours: captured, so the system cursor stays still in the middle of the
  view and nothing that watches it can fire (the Dock pops up even with
  `.hideDock`, and a flick beats any warp back or margin), and the cursor is the
  added-up motion, which cannot leave the view. **Windowed** the pointer stays
  the system's and goes where it likes, in and out of the window, so the Dock,
  the menu bar and the other displays all work; ours is drawn at the pointer's
  real place while it is over the view (an `NSTrackingArea` says when it
  crosses) and macOS's own comes back outside. A game that wants motion
  (Relative) is captured either way: it has no cursor position to show.
  gamescope sends no position, only the image (none: hidden, or a game drawing
  its own, or locked, which look the same) and a one-bit lock (`lock_pointer`:
  empty image, a constraint, not Steam).
  Two earlier rules are gone: that the macOS cursor was captured through the
  whole stream in every setting (a window should behave like a window), and the
  brief attempt to make the macOS cursor itself the windowed cursor (then a clip
  can only be held by warping the system pointer, and the cursor you see is no
  longer where the mouse is). Which cursor is drawn and who moves the pointer
  are separate questions; answering them as one is what made this go back and
  forth.
- Cursor setting (`--cursor`): Auto draws ours when gamescope has an image and
  not when it has none; Show always, with an arrow where there is no image
  (`--force-client-cursor`); Hide never. A cursor the game paints itself (Ender
  Lilies) is game pixels: it stays in the video, trailing ours by the stream's
  latency if both show.
- Who may connect: a device makes an Ed25519 key on first run and shows the line
  for the host's `~/.ssh/authorized_keys` in Settings, with
  `restrict,command="/bin/false"` so that ssh gives it no shell. It works offline,
  needs no account, and the host reads the file at each connection. Control and
  input are encrypted; video and audio are not, which costs nothing per frame and
  leaves them readable on the network (PROTOCOL.md, "Plain UDP"). The web client is
  served only with `--web`, as it has no key.
- The mouse has its own datagrams (protocol version 5). On the ordered control
  stream one lost datagram held up everything behind it, so a Wi-Fi drop froze the
  aim. A motion datagram now says how far the mouse has gone since the session
  began and a position is simply the latest, so neither waits for anything and a
  lost one is covered by the next. A button stays ordered, with its position sent
  just before it on the control stream. The control stream lost the mouse's traffic
  that used to fill the host's 256-datagram limit, so the client now resends
  unacknowledged control datagrams after 30 ms instead of relying on it. Host →
  client control datagrams are not resent.
- gamescope tells a client three things about the mouse, each on its own
  (gamescope patch 0009): the cursor image (or none), whether the game wants
  motion (a lock on its surface, Relative), and where it keeps the cursor
  (CursorClip, in stream pixels). The last two travel as one pointer constraint,
  because a surface can have only one: a lock or a confinement says which, its
  region is the clip. The compositor passes them on as separate messages.
  The clip comes from Wine's ClipCursor, which maps an override-redirect InputOnly
  window at the rectangle: Xwayland tells gamescope only that the pointer is
  confined, but gamescope is the window manager and sees that window. With no such
  window, a lock or confinement of the focused surface is a clip of all of the
  window. gamescope maps it to the output with the base plane's scale and offset,
  the inverse of what clicks go through, so it holds under any scaler, zoom or
  overscan, and gives none while its window doesn't have the input focus: the
  clip window stays mapped when Steam's overlay opens, and the overlay's side
  panels lie outside a 4:3 game. A game that keeps the cursor in by warping it has
  no clip.
- Mouse setting (`--mouse`), what the game gets: Auto sends motion while the game
  wants it and positions otherwise; Absolute and Relative stand, whatever it asks.
  gamescope takes in both at all times, but a locked game ignores positions and
  a game reading the cursor's position still sees it move with motion.
- Confinement setting (`--confine`): Auto keeps the cursor to the game's clip
  while there is one; None lets it go anywhere in the view, whatever the game
  asks. Either way it only applies while the mouse is captured, which is to say
  full screen: there the clip is simply where the added-up motion stops.
  Windowed the pointer is macOS's and is not ours to stop, and pinning our
  cursor on a clip edge while the hand went somewhere else would show a cursor
  that is not where the mouse is, so no clip is held (`Session.confined` is
  false without a capture). A capture only starts with the mouse inside the view
  (the title bar stays the system's) and parks the system cursor in the middle of
  the view, hidden: clicks go to its real place, and an edge can be the title bar
  (a double click there zooms the window). Only losing focus ends a capture.
  After a warp mouse events are ignored until it settles, as queued ones still
  say where it was and a later one can carry the whole jump as its delta. The iPad has Mouse only,
  and captures for relative only, as iPadOS decides the lock; the web client
  still follows the host.
- Settings: a three-finger tap (the touches it began are cancelled on the
  host, which ends them). The stream stops in the background (no GPU
  work there) and reconnects when the app comes back.

Phase lock on the iPad (2026-10-29): with the lead pinned (`GM_HOST_LEAD=<ms>`,
--mock, zero jitter) the display stage is exactly a refresh plus the lead
(8.34 + lead) down to 1.0 ms with no drops; 0.75 ms drops ~0.75 frames/s and
below 0.5 frames miss their flip: iPadOS's cut-off is ~0.8 ms before it. The
old lock sat at 4.5 ms: each lead bin's shown share was estimated with even
odds as the prior, so the bin it sat on (thousands of frames) always
outscored lower bins seen clean (tens of frames), and it never moved down;
the 100 ms settle after each step also hid exactly the drops a step caused;
and leads wrapped round the refresh put Wi-Fi spikes (frames 5 ms late,
missed and replaced) in the early bins. Now (Phase.swift): bins unwrapped
around the median lateness, every frame counted, a frame shown a refresh
late counted as a miss, and the lowest lead that would show 99.9% of the
recent latenesses. iPad --mock display 12.8 → 9.3 ms; iPad over Wi-Fi
(PyroWave) 16.3 / 16.9 → 14.3 / 15.2 ms p50 / p90 at ~0.12 drops/s (the
drops: frames 5–7 ms later than the median; lateness p99 is +5.4 ms, which
is what the lead now covers); Mac --mock in a window 21.2 → 20.2 ms.

Wi-Fi jitter on the iPad (2026-10-29, PyroWave 400 Mb/s, GM_DEBUG_NET, 90 s):
0.5–0.6% of frames arrive 3+ ms late: their first packet or a stall inside
their burst (5–10 ms), 0.5–0.6 events/s at intervals of almost exactly
1.024 s or multiples (10 beacon intervals). The read lag is ~0.1 ms (not the
app). The router's 5 GHz network is on channel 44 (80 MHz); pinging it from
the Mac (whose AWDL is off) at the same time showed no 1.024 s pattern, so
it is likely the iPad's own radio: AWDL (AirDrop, Handoff, Universal
Control), which gave the Mac the same 1.02 s pattern before its AWDL was
turned off. An app cannot turn AWDL off; declaring the app a game with Game
Mode support (LSApplicationCategoryType games, GCSupportsGameMode) changed
nothing and was dropped. These spikes are what the phase lock's lead covers
(~6 ms of the iPad's display stage).

Vivid colors on the iPad: the 8-bit layer tagged Display P3 shows P3's
primaries (checked by eye with `GM_MOCK_COLORS=gamut`: each P3 primary beside
sRGB's in P3 values; red, green and yellow differ, blue only in brightness,
the two spaces share their blue primary). iPadOS's screenshots clip to sRGB
(pure P3 red reads as sRGB red, in a P3-tagged PNG), so they cannot check
color: a 10-bit extended-range layer looked identical in them and on screen,
and was dropped.

Measured on an iPad Pro 12.9" (M1, 2021), 2026-10-29:
- `--mock`: 120 fps, phase lock locked on a measured 8.338 ms refresh,
  display 12.8 ms p50/p90, no drops.
- Ender Lilies over Wi-Fi, H.264 at the native 2732x2048: total 27.6 /
  28.2 ms p50 / p90 (encode 8.5, network 2.9, decode 4.5, display 11.5),
  no drops or gaps, audio and an Xbox controller working. The game showed
  60 fps.

Touch on Steam's UI tried by hand (2026-10-29): works. PyroWave at
300 Mb/s ran at 120 fps. Not yet tried: a mouse or trackpad (pointer lock
and GCMouse's motion sign), the keyboard.

## 4. gamescope facts this design depends on

Verified by reading gamescope's source (`src/Backends/WaylandBackend.cpp`,
`steamcompmgr.cpp`, `vblankmanager.cpp`, `rendervulkan.cpp`) at tag
**3.16.30.1** (what SteamOS ships on the test machine) and master, and by running it.
The test image uses Arch's gamescope 3.17.0; the relevant code is the same.

### 4.1 Wayland backend requirements

- Hard-required globals: `wl_compositor` ≥ 4, `wl_subcompositor`,
  `xdg_wm_base`, `zwp_linux_dmabuf_v1` ≥ 3, `wp_viewporter`,
  `wp_presentation`, `zwp_relative_pointer_manager_v1`,
  `zwp_pointer_constraints_v1`, `wl_shm`. Optional: `wl_seat` ≥ 8, wl_output,
  single-pixel-buffer, fractional-scale, color management, data device,
  primary selection, toplevel icon.
- Its window is a libdecor frame; advertise xdg-decoration with server-side
  mode so libdecor draws nothing.
- Without `--force-composition` it may use several subsurfaces (one per
  layer) plus a black single-pixel backing. With `--force-composition`
  (or `composite_force`) it composites everything into **one buffer on the
  root surface**. gmstream only reads the root surface.
- Output size comes from the toplevel configure (`g_nOutputWidth/Height`).
- Pacing uses **only presentation feedback**, never frame callbacks. A
  `presented` event with `refresh == 0` marks the host as VRR
  (`IsVRRActive = cv_adaptive_sync && host is VRR`); the VRR maximum rate is
  the largest `wl_output` mode refresh among outputs the surface entered.
  gamescope must be run with `--adaptive-sync` for VRR to be used.
- The cursor is a host cursor surface, not part of the frame (§3.1).
- `GetModes()` is empty and HDR is reported inactive on this backend (see
  risks, §9).

### 4.2 Buffers and synchronization

- On the full-composite path gamescope calls `vulkan_wait()` on its
  composite **before** committing, so buffers arrive finished. Measured:
  zero fence waits across all runs. gmstream still checks the implicit
  fence.
- gamescope composites into a **fixed ring of three output images**
  (`nOutImage = (nOutImage + 1) % 3`) and does not wait for
  `wl_buffer.release`. A buffer we hold can be overwritten two composites
  later. Consequences:
  - Never hold a frame while gamescope keeps compositing (§4.3 solves this).
  - Zero-copy encoding straight from gamescope's buffer is unsafe; make one
    GPU copy at present time.
- Inside gamescope, clients (games, Steam) are the other way round: gamescope
  polls each client buffer's fence on the CPU and only composites commits
  whose GPU work is done.

### 4.3 VRR pacing: why feedback is sent immediately

On DRM, gamescope's next flip is blocked by the kernel while one is pending.
On the Wayland backend nothing blocks; gamescope decides it may flip again
purely from time since the last `presented` timestamp
(`IsVRRFlipReady: now + lead >= last_vblank + interval`, where `lead` is its
draw-time estimate plus a red zone and can be several ms).

Measured with an uncapped vkcube, 120 Hz max:

| host behaviour | presented/s | gamescope composites/s |
|---|---|---|
| present instantly, no cap | 440 | 440 |
| cap; feedback when actually shown | 120 | ~4700 (every app frame) |
| cap; superseded frames reported presented, not discarded | 120 | ~4700 |
| **cap; feedback immediately with the slot time (current)** | **120** | **240** |
| no `--adaptive-sync` (fixed 120 Hz) | 120 | 120 |

Telling gamescope at commit time when the frame *will* be shown resets its
flip-ready clock at once, so it does not composite during the hold and the
held image cannot be overwritten. The remaining 2x (one early composite in
the lead window, then one from its vblank timer per slot) is cheap at
these rates; a possible refinement is to report slightly later times.

### 4.4 Session

SteamOS Game Mode is SDDM autologin into a gamescope session
(`/usr/lib/steamos/gamescope-session` and the `gamescope-session` user
units/target); gamescope runs on DRM with libliftoff planes and direct
scanout. Stopping the session: `systemctl --user stop
gamescope-session.target` (SDDM then relogs).

## 5. Latency

Measured 2026-10-26 on the test machine (Radeon 8060S), nested gamescope running
vkcube, 40 Mb/s target:

| path | p50 | p99 |
|---|---|---|
| present slot → bitstream ready, H.264 2560x1600@120 | 4.0 ms | 5–7 ms |
| same, HEVC | 4.2 ms | 6.7 ms |
| same, AV1 | 3.6 ms | 4.9 ms |
| bitstream → received, probe on the host (loopback) | 0.1 ms | 0.2 ms |
| capture → displayed, Chrome on a Mac over the LAN, 1280x656@60 H.264 | 6.5 ms | ~9 ms |

(the last row is from the web client's HUD: network ≈ 3 ms incl. clock
offset error, decode ≈ 1.4 ms.) 120 fps sustained with zero gaps, zero
encoder drops and zero fence waits. Almost all host-side latency is the
VCN encode itself; the VPP copy, queueing and the network stack are
sub-millisecond.

End to end, 2026-10-27, the test machine (wired) → M4 MacBook Air (802.11ax,
5 GHz, 1200 Mb/s PHY), native client, PyroWave 1920x1080@60 at 100 Mb/s,
a game running; gamescope present → on the panel:

| client presentation | total p50 | p90 | dropped | pacing p90 |
|---|---|---|---|---|
| vsync queue (FIFO), no phase lock | 38.0 ms | 38.0 | 0 (but the queue can deepen to 55–71) | 0.0 |
| `disableFIFO` + phase lock, window | 29–30 | 30 | ~0.8/s | 0.1–0.3 |
| same, full screen | 28.0 | 28.3 | ~0.8/s | ~0 |
| full screen, vsync off ("lowest latency"; tears) | 13.6 | 16.7 | ~0 | (not measurable) |
| full screen on a VRR display (Dell U2725QE), vsync on | 12.6 | 13.7 | 0 | ~2 (p90) |

Stages in that last row: encode 1.0, network 6.7 (first packet ~4.2 after
encode: host send path + ~2 ms Wi-Fi one way; then a 264-packet burst of
~2.6 ms), decode 2.1 (of which ~1.3 ms GPU time for the Metal decoder at
1080p), render 0.2, display 3.9. Remaining levers: the host's send path
(one syscall and allocation per datagram; batch with `sendmmsg`/GSO, larger
packets on plain UDP, DSCP marking), and AWDL on the Mac (`sudo ifconfig
awdl0 down` is the usual fix for periodic Wi-Fi spikes; needs root).

Host stages, 2026-10-29, the test machine, Ender Lilies' animated title screen at
120 fps, headless probe on the host (PyroWave 1920x1080@120, 100 Mb/s, 30 s
each), forced composition. Traces: gamescope patch 0007 (`GM_GS_TIMING`),
gmstream `GM_DEBUG_GRID` + `GM_DEBUG_ENCODE`, joined by
`stages.py` (session scratchpad). p50 / p99 ms:

| stage | default (Steam VRR off, no grid) | VRR (`-v 2`) |
|---|---|---|
| game commit → its GPU work done | 0.68 / 1.68 | 0.25 / 1.02 |
| done → gamescope paint starts (waits for its next tick) | **4.80 / 6.62** | **6.69 / 7.31** |
| paint + composite + `vulkan_wait` | 0.66 / 0.77 | 0.65 / 0.77 |
| composited → handed to gmstream | 0.006 / 0.03 | 0.006 / 0.03 |
| hop: gamescope → gmstream | 0.03 / 0.10 | 0.03 / 0.12 |
| gmstream hold to the slot | **3.35 / 3.47** | 0.005 / 0.02 |
| hand-off to the PyroWave worker | 0.013 / 0.04 | 0.013 / 0.05 |
| PyroWave GPU (incl. CPU wake) | 0.35 / 1.29 | 0.66 / 0.99 |
| packetize | 0.015 / 0.05 | 0.017 / 0.05 |

What fusing composite and encode in gamescope's queue could remove is the
composite, the hop and the hand-off: ~0.7 ms p50, ~0.9 ms p99 (most of it,
the composite, also goes by dropping forced composition). The waits are
10x that: the game finishes a frame ~1 ms after the previous flip and then
waits for gamescope's next 120 Hz tick (the game runs at the cap, so it
renders ahead), and without VRR gmstream also holds each frame to its slot.
(The probe received no PyroWave frames over WebTransport, datagrams lost or
dropped on loopback, not investigated; the host-side traces do not need
them.)

Tried and dropped on the Mac (details in §8): presenting with vsync off in a
window (macOS composites such frames one or two refreshes later
unpredictably: p90 and pacing regress), scheduling presents from frame
timestamps (same reason), CAMetalDisplayLink (in a window it plans ~3
refreshes ahead even at `preferredFrameLatency` 1; in full screen its
deadline is still a whole refresh before the frame is on screen), timing
vsync-off flips to the vblank in full screen (the flip lands up to ~10 ms
after the present, so they still tear, and latency rose to 28/46 ms), `present(atTime:)`, fewer drawables or frames
in flight, the private `lowLatency`, `serverSyncEnabled`, `fenceEnabled`,
`allowsDisplayCompositingWithCopy`, `lowWorkload`, `needsLowLatency`,
`preferredLatency` (all within noise once FIFO is off), GPU queue priority
(`setGPUPriority:` 2; no change), keeping the GPU busy to hold clocks
(−0.1 ms decode), GPU performance-state assertions (need an Apple
entitlement).

Where the rest could come from, in order of expected payoff:

- Display side: the browser composites the canvas on its next vsync. A
  native client (or WebGPU/`requestVideoFrameCallback` tuning) can do better.
- Encoder: radeonsi exposes no faster preset through VA-API; sub-frame
  (slice) output would need a VA-API path FFmpeg does not offer.
- VRR pacing holds a frame until its slot; at the client's max refresh this
  is at most one refresh interval and only when gamescope runs ahead.

## 6. Game Mode integration

### 6.1 Installed (2026-10-26, the test machine)

gmstream is a user service that gamescope connects to; gamescope is not its
child. gamescope is **our patched build of Valve's own package** (§6.2),
installed beside Valve's, never over it. Files are in `session/` and
installed as:

- `~/.config/systemd/user/gmstream.service`: `gmstream -w gmstream-0 -s
  1920x1080 -r 120`, ordered before `gamescope-session.service`. It outlives
  gamescope restarts and accepts the next gamescope. The `-r` is the refresh
  Steam sees if it starts before a client connects, and Steam sets its frame
  limit from it once: with `-r 60`, `GAMESCOPE_FPS_LIMIT` stayed 60 after a
  120 Hz client connected and gamescope gave the game every other vblank
  (60 fps; Steam's Quick Access menu showed 60 Hz, frame limit "off").
- `~/.config/systemd/user/gamescope-session.service.d/50-gmstream.conf`:
  `Requires=`/`After=gmstream.service` and `ExecStart=` the fork below.
  **Delete this file to go back to stock Game Mode.**
- `~/gmstream/session/gamescope-session`: forks
  `/usr/lib/steamos/gamescope-session` into `$XDG_RUNTIME_DIR` at every start
  (so it follows SteamOS updates) and changes only the gamescope command; if
  the stock script no longer matches, it runs the stock script unchanged.
- `~/gmstream/bin/gamescope`: our gamescope build; the fork runs Valve's
  `/usr/bin/gamescope` if it is missing.

The changes to the gamescope command, and why each is needed:

| change | why |
|---|---|
| `~/gmstream/bin/gamescope` | the patched build (§6.2). |
| `WAYLAND_DISPLAY=gmstream-0` for gamescope only | gamescope's backend `auto` picks Wayland when it is set (stock script passes no `--backend`). Only gamescope gets it: the session dumps its environment into `gamescope-environment`, which Steam, mangoapp etc. load, and they must not connect to gmstream. gamescope itself gives its children `WAYLAND_DISPLAY=gamescope-0`. |
| `LIBDECOR_PLUGIN_DIR=/var/empty` | as a Wayland client gamescope makes its window with libdecor, whose GTK plugin blocks on a D-Bus call to the settings portal; at boot the portal starts only after the session is ready, so gamescope hung past the unit's 5 s start timeout (restart loop). gmstream always asks for server-side decorations, so libdecor's built-in fallback is enough. |
| `--force-composition` | one composited buffer per frame on the root surface (Steam must not undo it: see §6.2). |
| (not) `--virtual-connector-strategy` | leave gamescope's Steam-mode default (`SteamControlled`). An earlier version forced `SingleApplication`; with it, games on the second Xwayland (`:1`, where `--xwayland-count 2` puts them) never got focus and Steam's UI stayed in front. |

gmstream side, found on the way:

- Answer presentation feedback for **every** commit, also buffer-less ones:
  under VRR gamescope paints nothing while a present is unanswered.
- Treat whichever toplevel last committed as the screen (gamescope may have
  several connectors) and configure all of them; always schedule a configure
  on an initial commit.
- Non-dma-buf frames (gamescope's single-pixel black fill, shm) are uploaded
  and scaled; the output size follows the surface (viewport) size.
- Keep the last frame and re-encode it when a client joins or asks for a
  keyframe: Steam's UI is often static and gamescope only presents on change.
- A client's mouse is passed on as it came: Position as an absolute move, Motion
  as relative motion, with no conversion. gamescope hears only the one its mode
  uses, relative under a lock and absolute otherwise, so Position under a lock
  does nothing, nor does Motion without one. Choosing is the client's job.
- Cursor "hidden" is debounced by 50 ms: gamescope sets an empty cursor
  between two images (arrow ↔ hand in Steam), which a browser shows as a
  blink.
- A watchdog aborts the process if the compositor thread stalls for 10 s
  (systemd restarts it); the web client reconnects on its own (2 s retry)
  unless the host refused it or another client took over.

Restarting gmstream restarts gamescope (`Requires=`) but not Steam, which then
loses its X connection; restart the whole session instead
(`systemctl --user stop gamescope-session.target`, SDDM logs back in).

### 6.2 Our gamescope build (`gamescope/`)

Valve publishes the exact source package SteamOS ships
(`steamdeck-packages.steamos.cloud/archlinux-mirror/sources/jupiter-3.9/gamescope-3.16.30.1-2.src.tar.gz`:
PKGBUILD, Valve's patches, pinned wlroots/libliftoff/stb/glm). `build.sh`
builds that package with our patches added to its `source=()` (Valve's
`prepare()` applies every listed `*.patch`) in the `gamescope-build`
container: Arch pointed at SteamOS 3.9's own repos (`Containerfile`), so every
library and glibc match the device and the binary uses the host's shared
libraries exactly like Valve's. The binary is copied to the device.

Patches (all in the nested Wayland backend path stock Game Mode never runs:
it uses the DRM backend, where gamescope has no window and none of this
exists):

1. **`0001-wayland-keep-the-toplevel-mapped`.** With nothing focused (focus
   moving on as a game exits) gamescope's Wayland backend attached no buffer,
   which unmaps its xdg_toplevel; it then waits for a configure before
   showing anything again, but a second detach before that configure is not
   an xdg-shell initial commit, so no configure ever came: the stream froze on
   game exit. Now it shows its 1x1 black buffer instead; the window never
   unmaps.
2. **`0003-wayland-host-pointer-moves-the-cursor`.** The Wayland backend fed
   host pointer motion through `wlserver_touchmotion`, which follows the
   touch mode; SteamOS runs gamescope with `--default-touch-mode 4`
   (passthrough), so mouse motion became touchscreen motion and the cursor
   never moved (clicks landed at its old spot). Host pointer motion now always
   moves the cursor.
3. **`0004-wayland-vrr-pending-flip`.** Under VRR a committed flip counts as
   pending until the host answers it, as a pending flip does on DRM, so a
   FIFO client's queued frame is not sent right behind it.
4. **`0005-nested-relative-mouse-lock-without-hint`.** Nested relative mouse
   mode needs an empty cursor and a pointer constraint, and gamescope counted
   a constraint only when it carried a cursor position hint. Witcher 3
   (Proton) has Xwayland lock the pointer with no hint, so gamescope never
   locked the host pointer: the game got absolute positions (wild camera
   turns, no menu cursor). A lock now counts without a hint.
5. **`0006-nested-touch`.** The Wayland backend held a `wl_touch` but never
   bound it, so a host touchscreen did nothing. It now binds it when the
   host seat offers touch (gmstream's does) and feeds `wlserver_touchdown/
   motion/up` with positions normalized over the output, as for the
   pointer; they follow the touch mode (SteamOS: passthrough). A cancel
   ends every touch (wlserver has no cancel).
6. **`0007-debug-frame-timing`.** Measuring only, off unless
   `GM_GS_TIMING=<file>`: per frame handed to the host, the base plane's
   commit arrival, fence done, paint start, composite done and hand-off
   times (§5).
7. **`0008-throttle-fifo-games`** (convar `gm_throttle`, on by default).
   A FIFO game is released when a frame is shown, runs ahead by its frame
   latency, and that many finished frames then wait in gamescope for as long
   as it makes them no slower than they leave (§11.7). The patch holds
   `wp_presentation` presented events, which is what Vulkan's present wait and
   so DXVK block on, and lets go of one per refresh; while frames still wait it
   skips a release, so the backlog drains and no frame is dropped. With no
   queue a frame's phase decides everything (done just after a latch, it waits
   a whole refresh), so each release is nudged until frames are done 2 ms
   before the latch. Held events survive a client exiting (each has a destroy
   listener). Tested: Warcraft III killed with `kill` and `kill -9` while
   held, capped at 30 fps (30.09 fps, no stall), Witcher 3 (59.6 fps, no gap
   over 100 ms, latency not measured).
8. **`0009-nested-clip-and-relative-apart`.** The nested backend now gives its
   host the cursor image, the clip and absolute or relative as three
   independent things (§3.8, the mouse notes). A visible cursor no longer blocks
   relative, and relative no longer hides the image: a lock on the focused
   surface is relative, Steam's own and filtered app IDs excepted. Positions and
   motion are taken in at all times, not chosen by the lock.
9. **`0010-absolute-motion-carries-a-delta`.** A position from the host also sends
   the delta, as compositors do. With positions alone Xwayland uses its absolute
   device, the X master pointer takes on absolute axes, and raw-only motion under
   a later lock can't change them. SDL reads XI_RawMotion by the master's axes,
   so a native game (CS2) took each delta for a position: the camera moved a
   little and came back. Wine was not affected.

Not a patch: Steam writes the root property `GAMESCOPE_COMPOSITE_FORCE` from
its Settings → Developer → **Force Composite** toggle at startup, and the
property overrides `--force-composition`. With the toggle off (the default) it
writes 0, and gamescope puts Steam's picture on a subsurface with only black
on the root surface (black boot animation, black after every Steam restart).
The toggle must be on; it is a per-device Steam setting. (An earlier patch
made the command-line flag win over the property; dropped in favour of the
setting.)

Debugging aids: gamescope's log channels are runtime convars
(`WAYLAND_DISPLAY=gamescope-0 gamescopectl log_focus debug`, `log_xdg_backend`),
and our build keeps symbols, so gdb breakpoints on its input handlers work.

### 6.3 Panel mode (no client: this machine's own screen)

With no client connected, gmstream shows gamescope's frames on this machine's
screen (core/compositor.c, "panel"): gamescope stays nested on gmstream-0
exactly as when streaming, so connecting is a switch of display, not of
session.

- **Screen:** wlroots' DRM backend through a logind session (libseat, the
  seat0 session in XDG_SESSION_ID, as native gamescope takes it). An external
  screen if one is connected, else the built-in panel (native Game Mode's
  choice), at its preferred mode, adaptive sync on when supported.
- **Frames:** each committed buffer goes to the screen as it is (direct
  scanout) or, if the display engine cannot take it, is copied into the
  screen's own buffer; one flip at a time, the newest frame committed during
  a flip going next; presentation feedback answered at the flip (real
  timestamps, refresh 0 under VRR), so gamescope's scheduler runs on the
  screen's real vblanks as on its DRM backend. gamescope picks its modifier
  from linux-dmabuf v3's flat list, not feedback, so with a screen only the
  formats its primary plane scans out are offered: every frame copied
  before, none after. The encoder imports the same buffers.
- **Input:** libinput (keyboard, touchpad, mouse, touchscreen) into the same
  seat the network's input uses; touch through patch 0006, so SteamOS's
  touch mode applies. gamescope's cursor goes on the screen's cursor plane.
- **Switching:** a client connecting turns the screen off (as native Game
  Mode does when another display is connected) and gamescope's output takes
  the client's mode; when the last client leaves, the screen comes back at
  its native mode. No seat, or `GM_NO_PANEL`: streams only, as before.
- Measured 2026-10-29 on the test machine (ROG Flow Z13 GZ302EA, eDP 2560x1600 at
  180 Hz, VRR), a test instance with nested gamescope + vkcube: 180 frames
  presented a second, 0 copied, 0 superseded; a client (probe, 1920x1080@60,
  PyroWave) turned the panel off and got 60 fps (encode ~1 ms); 10 s after it
  vanished the panel came back at 180 Hz.

What native Game Mode does that panel mode does not (yet):
- gamescope composites every frame (`--force-composition`): native can scan
  a game's buffer out directly and put overlays (MangoHud, Steam's) on
  planes. Layered frames (gamescope's planes as subsurfaces, mapped to KMS
  planes) would close that.
- Display colour management (gamma/CTM LUTs, per-panel calibration) and HDR:
  none; frames are shown as sRGB.
- One screen, chosen by gmstream: Steam sees one display; its own display
  choice and mode/refresh switching (e.g. a 60 Hz mode) do not apply.
- Idle screen-off (DPMS from Steam's power settings) is not implemented.
- The keyboard uses gmstream's fixed US keymap, not the system's layout.

### 6.4 Not done

1. Upstreaming the gamescope patches; layered (non-composited) frames from
   gamescope's planes, which also opens per-layer and pre-upscale streaming.

Gamepads are unaffected by the compositor: Steam reads controllers directly
(hidraw/evdev).

## 7. Development and test workflow

Builds run in containers on a Linux build box, never on the device: the image in
`build/Containerfile` (Debian with the static C dependencies and Rust) for gmstream,
`gamescope/Containerfile` for the patched gamescope (`gamescope/build.sh`). Copy
the two binaries to the device, as `~/gmstream/gmstream` and
`~/gmstream/bin/gamescope`, and set `cap_sys_nice=eip` on both (a copy loses it). A
first install also needs `session/`: see §6.1. Clients: `mac/build.sh`, `mac/test.sh`
and `ipad/build.sh`, which needs `GM_TEAM`.

## 8. Lessons from paths we tried and dropped

**Polaris (Sunshine fork) as the host.** Built from source with tracked
patches in this repo, CI-published image, Quadlet. It works, but it manages
far more than this use case needs and fights it:
- Game Mode detection forces "mirror desktop" semantics, and Private Stream
  is refused while Game Mode runs.
- Its private-session paths assume KDE or a desktop portal; the encoder probe
  needs a portal display name (patched: patch 0001).
- The labwc Private Stream runtime is a full 46k-line window manager whose
  headless output renders on a fixed timer: delivery to the encoder is
  event-driven but quantized to labwc's tick, and there is no VRR. Polaris's
  wlroots grabber is only event-driven ("source_driven") for its own private
  compositor and uses a fixed timer otherwise.
- VA-API DMA-BUF capture is opt-in (`POLARIS_PORTAL_DMABUF=1`) on the
  PipeWire route and forced off on the wlroots route.
- AV1 through FFmpeg's Vulkan encoder (8.1.2 / 9.0.2) fails with Vulkan
  validation errors, so Polaris disables it.
- Resume commands ran asynchronously alongside capture; a resolution change
  then renegotiated the stream mid-flight and produced a solid purple picture
  until reconnect (fixed with patch 0002: run them before capture starts).
  Lesson: settle the mode before the first frame is captured.

**Moonlight/GameStream.** A compatible host must implement pairing,
RTSP, an encrypted control channel, RTP with Reed-Solomon FEC and client
quirks. Wolf and Moonshine show it is doable from scratch, but it is weeks of
protocol work for compatibility we do not need. Replaced by our own protocol.

**Mirror Desktop (gamescope's PipeWire node).** Event-driven and nearly
zero-copy, but it captures the *panel's* session: the stream resolution is
whatever the panel runs, and changing it needs a session restart. gamescope's
PipeWire path also leaves out the MangoHud overlay (`paint_pipewire` skips
external overlays such as mangoapp). Our Wayland-backend path includes it,
because the overlay is part of the composite.

**Headless gamescope** (`--backend headless`, own session per stream).
Fixed resolution chosen at start, `IsVRRActive` hard-coded false, vblank on a
timer. Resolution changes mean a new session. Needed the SteamOS session
script forked (`polaris-gamescope-session`) and SDDM juggling.

**Fake display via EDID override** (debugfs `edid_override` + forcing an
HDMI connector on, root helper + udev rule + idle unplug timer). It works in
principle and gives stock Game Mode DRM features, but on amdgpu/Strix Halo
it repeatedly wedged the display stack: flip_done timeouts, stuck
`pflip_status`, DMCUB errors, Panel Replay warnings, DP timing-generator
failures, recoverable only by reboot. EDID detail pitfalls: the 6-bit
vertical front porch field overflows at high modes, and an HDMI 2.0 HF-VSDB
broke the kernel's CEA parser. Abandoned as too driver-specific and fragile.

**Kernel virtual displays (EVDI etc.).** Need out-of-tree kernel modules;
not acceptable on read-only SteamOS.

**KDE / Desktop Mode as a base.** plasmashell sat at 100% CPU and the
portal/desktop requirements were the root of most Polaris friction. Game Mode
does not need any of it.

**Operational lessons.**
- Steam corrupted itself when `/home` filled up (truncated files; the
  updater then deadlocked because gamescope never showed its
  override-redirect window). Keep disk headroom on the device
  (the test machine's `/home` is ~96% full); build images elsewhere.
- Container builds: copy only the built package into the runtime stage
  (a 6 GB image became 1.5 GB).
- A Decky plugin (Steamcord) registered a fake portal D-Bus name that broke
  portal discovery. Third-party plugins can shadow system services.
- `pkill -f` patterns can match your own ssh command line.
- Never `steamos-readonly disable`. Root access itself is fine; use `/etc`
  and user units.
- Build on the build box in a container; ship
  single binaries to the device, not images.

**Building gmstream.**
- Build on a distro with an *older* glibc than SteamOS (Debian trixie 2.41
  vs SteamOS 2.43); Arch's newer glibc produces binaries SteamOS can't load.
- libva's meson hardcodes `shared_library`; `deps.sh` patches it to
  `library` to get `libva.a`. Opus' git tree lacks the DNN model data; use
  the release tarball. Debian ships `libffi.a`; link it by name
  (`-l:libffi.a`) or the `.so` wins.
- signalfd-based signal handling (wl_event_loop_add_signal) needs the signal
  blocked in *every* thread: block before tokio starts.
- Teardown order matters: free the pending frame *and clear the pointer*
  before `wl_display_destroy_clients`, whose unmap handlers touch it.
- **Mesa (radeonsi 26.1) can hang the whole process while parsing the
  packed headers FFmpeg passes.** It mishandles emulation-prevention bytes
  in them, misparses, and spins forever in an Exp-Golomb read while holding
  the VA lock; the compositor then blocks on that lock (and ignores SIGTERM).
  Whether it happens depends on where fields fall: the SPS's 32-bit VUI
  timing values were the trigger (H.264 640x328@30 and 2928x1600@30 hung,
  640x336@30 and 640x328@60 did not). `deps.sh` patches FFmpeg to omit VUI
  timing info for H.264 and HEVC; nothing needs it. Handing Mesa no packed
  headers at all doesn't work: radeonsi then writes no SPS/PPS.
- Never destroy the VA video-processing context while a copy queued on it
  may still be pending; it is created once per encoder now.
- Under VRR, gamescope logs "Compositor released us but we were not
  acquired" a few dozen times a second once a client streams (also with
  encoding disabled). Harmless as far as seen (gamescope only skips a
  refcount decrement); not yet understood.

**macOS presentation experiments (2026-10-27).** Each measured with
per-stage p50/p90 over 30–60 s; lessons that cost time:

- The vsync queue's depth is history, not phase: once one frame misses its
  refresh, every later frame waits an extra refresh (FIFO), and a stream at
  the panel's rate never drains it. Most of the earlier "unexplained" runs
  (display 39, 55, 71 ms) were this. `disableFIFO` fixes it.
- With vsync off in a window, the window server shows a frame 0–28 ms after
  submission depending on its own composite timing, not ours; neither
  scheduling presents from frame timestamps nor phase-locking made that
  predictable. In full screen it is a different path (direct flips).
- A CADisplayLink from the screen tracks this app's own presents, and so
  does CVDisplayLink with vsync off in full screen (built-in panel: ±8 ms off
  a straight line, against 0.9 ms with vsync on; its timestamps are when its
  callback ran, and vsync flips land ~7.3 ms after them). The panel's grid
  comes from vsync-on flips instead: they lie on one line (16666.605 µs on
  the M4 MacBook Air's panel, the same in every run), and a line fitted to
  one run placed the flips of a run 149 s later within 5 µs.
- The window server's vsync cut-off is a whole refresh before the vblank, not
  the vblank (as a PC's exclusive full screen has): CAMetalDisplayLink in full
  screen at frame latency 1 reports its deadline 16.7 ms before the frame is
  on screen, and the vsync display stage (submit → screen) is ~19 ms in full
  screen as in a window. Vsync off skips that wait but cannot avoid tearing.
- Wi-Fi (M4 Air, 802.11ax 80 MHz at 1200 Mb/s, -39 dBm; host wired): frame
  arrival p50 7.2, p90 7.6, p99 8.8 ms after the host's encode. 0.8% of
  frames arrive 2+ ms late, every ~2.05 s (multiples of ~1.02 s): a
  periodic radio event, likely AWDL (GM_DEBUG_NET logs each frame's first
  packet, burst and largest gap).
- the test machine's wired port is a USB AX88179 under the generic `cdc_ncm`
  driver, which holds outgoing packets up to `tx_timer_usecs` (400 µs) to
  batch them: host → router ping 1.67 ms. With
  `echo 0 > /sys/class/net/<if>/cdc_ncm/tx_timer_usecs` (runtime, resets on
  replug or reboot): 0.46 ms; Mac → host ping min 3.5 → 2.2 ms. The video
  stage barely moved (a frame's burst fills the batches anyway).
- Host send path (GM_DEBUG_SEND logs it every 5 s): encode done → first
  sendmmsg 0.02 ms, all ~260 datagrams handed to the kernel in ~0.8 ms.
  Batched sends + DSCP AF41 together: total 12.9 / 14.8 → 12.4 / 14.4 ms
  (VRR display). The ~4 ms from encode done to the first packet's arrival
  on the Mac is past the host's NIC; part of it may be the ping-based
  clock offset (it assumes symmetric Wi-Fi delay).
- Stream stalls (every few seconds 3–5 frames arriving together 50–110 ms
  late) and ~2.5 ms of every frame's network time were the test machine's USB
  Ethernet adapter (ASIX AX88179A, 0b95:1790) running in its CDC NCM USB
  configuration under `cdc_ncm`. Kernel receive timestamps
  (SO_TIMESTAMP_MONOTONIC; GM_DEBUG_NET's "readlag") showed the packets
  reached the Mac late; DSCP, the NCM tx timer, host pacing, AWDL/AirDrop/
  Handoff/Location scans were ruled out; streaming over the test machine's Wi-Fi
  instead nearly removed the stalls. Switching the adapter to its vendor
  configuration (`echo 1 > /sys/bus/usb/devices/8-1/bConfigurationValue`,
  driver `ax88179_178a`): frame arrival p50 / p99 7.2 / 10–42 → 4.6 / 6.5 ms,
  no stalls, total 12.4 / 14.4 → 10.9 / 12.3 ms (VRR display). Kept across
  replugs and reboots by a udev rule setting bConfigurationValue to 1 for
  0b95:1790.
- Stalls with audio glitches (~10 a minute at 103 Mb/s, 30–110 ms with
  nothing arriving, then a burst at air rate; the router held ~70 ms and
  dropped the rest) were the Mac's Wi-Fi entering power save while only
  receiving. Trace (GM_DEBUG_TRACE, mac/tools/trace.py): stall ends lock to
  the access point's 102.4 ms beacon interval (the Mac wakes at the next
  beacon's TIM); stall starts and the gaps of a real scan do not. The
  firmware-sent frame count (`txFwFrames` in airportd's link-quality
  stats) rises with stalls: Null frames entering and leaving power save.
  Not BT coexistence (its PM counters stayed 0), not host scans (none
  logged), no host log line near the stalls; fewer at 310 Mb/s. Uplink
  datagrams from the Mac: 200 Hz and 60 Hz → no stall ≥ 30 ms in 3 min
  (35 and ~30 in the 3 min before); 10 Hz → no change. macOS has no
  setting for Wi-Fi power save (the APPLE80211 ioctls return EOPNOTSUPP),
  so the Mac app sends a 1-byte keep-awake datagram 60 times a second
  (Settings, on by default; PROTOCOL.md, "Plain UDP").
- macOS Wi-Fi logs without root: `log stream --predicate 'process ==
  "airportd"'` shows link-quality stats every 5 s (RSSI, rates, A-MPDU and
  reorder counters, per-access-category TX), scans and who asked for them
  (e.g. locationd), and the "realtime session" state (RT = 0x0: not active;
  scans check AWDL realtime / AirPlay / Sidecar before running).
- Stutter with pacing on, VRR off (2026-10-27, Thunderbolt, 1080p60 window):
  ~0.75 dropped frames/s, max ~40 ms. GM_DEBUG_FRAMES showed macOS's
  cut-off at a submit lead of ~1.7 ms (frames submitted later are not shown
  a refresh late but replaced by the next: a repeated frame), while the
  lock held the median lead ~0.6 ms above it; the submit time's own spread
  is p99 +0.9, p99.9 +1.2 ms (gamescope's present phase 0.58, encode 0.40,
  decode 0.36, network 0.24 at p99). The old lock took the cut-off from the
  sweep's lowest median display stage, but at that step half the frames
  were dropped and dropped frames give no display stage: ~1.5 ms too
  optimistic. Learning it from the drops' leads and holding the tail:
  p50 24.5, p99 ~25, max 25.5 ms, ~0.05 drops/s (+1 ms p50 for ~15x fewer
  stutters). Steering on frames shown a refresh late as if they were
  misses ran away (the window server had gone one frame deeper; every lead
  was fine): those now drain instead. The drops left are decodes whose GPU
  time jumps from 1.3 to 3–4 ms every 5.00 s, in step with CrossOver's
  Steam "Menu Helper" (its 5 s activity makes the window server redraw);
  not the stats overlay (same with it hidden).
- Anything in the window that changes (views over the Metal layer) costs
  the direct flip for that frame; screenshots do too.
- zsh does not split `$var` into words: test loops that passed several
  flags in one variable silently ran with defaults (`${=var}` splits).
- Frames dropped by the compositor do not show in a "presentation interval
  vs capture interval" pacing metric (both double); count them separately.
- Windowed on the Dell at 120 Hz (2026-10-27), four separate causes of
  drops, found in this order:
  - **Freeze after drops** ("0 fps shown", stream stuck for good): the
    render and receive threads looped with no autorelease pool, so every
    autoreleased drawable and PyroWave compute encoder lived for the whole
    session (38k of each after 15 min). A presented drawable's buffer comes
    back anyway; one handed back unpresented (a skipped frame, a drain)
    never did, and after maximumDrawableCount of them `nextDrawable` blocked
    for good. Each loop iteration now drains a pool. Repro: one forced
    `skipOne()` every 2 s froze the old build at the second.
  - **Other displays' redraws**: WindowServer misses whole Dell refreshes
    (every window on it repeats one; ~8/s) when a translucent, blurred
    window redraws on the built-in display (iTerm at 42% transparency with
    blur, its contents animating). Hidden, or transparency 0: ~1 drop/s.
    Nothing in the app can present around a composite that never happens.
  - **Wrong refresh interval**: in a window the Dell refreshes every
    8.3427 ms (119.866 Hz; Adaptive-Sync panel, desktop composited at a fixed
    rate), not 1/120 s. `PhaseLock.interval` counted refreshes over its whole
    span with the nominal interval, which on a long span always rounds to
    8.333 ms: the grid sent to the host ran 1100 ppm fast, the host followed
    it exactly (`GM_DEBUG_GRID` below), and the lock slipped a whole refresh
    every ~6.5 s with a burst of drops (10–17 drops/s). Counting with the
    best interval so far: ~1 drop/s. The same mistake in offline analysis
    (a period fitted by rounding to 8.333) made the host look like it ran at
    119.85 Hz on its own: fit periods from consecutive flips.
  - **gamescope's adaptive wake**: it wakes early by its rolling max draw
    time and raises that after a slow composite; on the test machine every ~8 s it
    committed 0.5 ms earlier for ~2 s (3.75 instead of 3.25 ms before the
    vblank we report). Passed on, every frame landed 0.5 ms earlier on the
    client, past the edge its lock had set them a margin from. gmstream
    now holds each frame on a client grid until the 10th percentile of
    recent commit leads before its vblank (`grid_release`): the release
    lead stays at 2.92–3.00 ms through those spells, for a 0.12 ms median
    hold (0.76 ms p90). With both fixes, 60 fps content: 0.97 drops/s,
    total p50 24.3 / p90 25.4 ms (was ~11 drops/s).
  - **The lock itself, with 120 fps content** (Ender Lilies' title screen;
    the numbers above were 60 fps content): steering on submit leads
    unwrapped around their median read every frame more than half a
    refresh late as early, and over Wi-Fi the late tail is that far behind
    (lateness, submit − host present, p50 9 / p99 17 ms). The lock went a
    whole refresh round every few seconds: 12.4 drops/s, total p50 24.1 /
    p90 25.5 ms. A sharp cut-off doesn't exist either: frames with leads
    under 1.5 ms dropped about half the time, 3% from 2.25 to 4.75 ms, ~60%
    from 5 ms up (replaced by a successor that came in before the window
    server took them). Fixed-edge models (lateness window above a learned
    cut-off, cut-off from near-misses or from a drop-rate threshold) all
    placed frames in the wrong band: 16–26 drops/s. Now the lock learns the
    shown share per lead bin and steers the host present's lead to where
    the last ~10 s of latenesses would have been shown most often (§3.7,
    `Phase.swift`): 6.4 drops/s, 113 fps shown, total p50 21.9 / p90
    22.3 ms, phase held within ~1 ms. The drops left are late arrivals
    (network 8.0 vs 7.2 ms median; the successor then arrives 6.9 ms later
    and replaces them): Wi-Fi jitter at 300 Mb/s.
  - `GM_DEBUG_GRID=<file>` on the host logs every grid update the client
    sends and every commit's reported vblank and release time.
- Steam capped the game at 60 fps with its frame limit "off": gmstream's
  service started at `-r 60`, Steam read that refresh once at startup and
  set `GAMESCOPE_FPS_LIMIT=60`, and with the client's 120 Hz output gamescope
  then gives a limited game every other vblank. The service now starts at
  `-r 120` (§6.1). Steam's Quick Access menu shows no refresh choice here:
  gamescope's Wayland backend reports no dynamic refresh rates.

## 9. Risks and open questions

- **gamescope's nested Wayland backend in Steam mode is a rarely used
  combination** (stock Game Mode uses the DRM backend); three of its bugs are
  patched (§6.2) and more may surface. Each SteamOS gamescope update needs
  the patches rebased and `gamescope/build.sh`'s `VERSION` bumped; until then
  the fork keeps running our older build against the newer Steam.
- **Game Mode features lost on gamescope's Wayland backend.** The backend
  reports no display modes and no active HDR. Expect the Quick Access Menu
  refresh-rate slider, HDR, and possibly "allow tearing"/frame-limiter
  behaviour to differ from DRM Game Mode. Needs testing in the real session;
  some may be fixable by advertising more (modes, color management).
- **Panel mode** (§6.3) is tested with a separate instance and vkcube, not yet
  as the session's own gmstream with Steam (suspend/resume, VT switches,
  hotplug, Steam's brightness and idle settings).
- **AV1 and HEVC in the browser:** both encode fine; the web client has only
  been exercised with H.264 so far.
- **Browsers:** Chrome only so far. Safari's WebTransport/WebCodecs support
  must be checked. The page needs a trusted (or clicked-through) certificate.
- **Audio:** local speakers keep playing while streaming; no microphone.
  Host/client clock drift is handled by trimming the jitter buffer (an
  occasional tiny glitch) rather than resampling.
- **Security:** no authentication (removed 2026-10-26 at the owner's
  request): anyone who can reach port 8443 can watch the stream and send
  input. LAN only; add access control before exposing it further.
- **Steam Deck-style pointer lock** in games that hide the cursor (gamescope
  relative mode → client Pointer Lock) works in Witcher 3 with patch 0005
  (camera and menu cursor); mouse sensitivity is not tuned (deltas are
  macOS-accelerated motion in stream pixels).

## 11. Scheduled pacing: one sequence, read two ways

Design, not built. `apple/Schedule.swift` is the client half and is off by
default; the host half (§11.9) is not written. This section is the formulation
both follow.

### 11.1 The invariant

Judder is a nonlinearity in the map from game-world time to display time. A
VRR panel keeps that map straight by moving the display to the frame; the same
invariant can be kept the other way round, by moving the frames to the panel's
grid and advancing the world clock to match where they land.

Index frames `n = 0, 1, 2, ...`. The decision variable is the *advancement*
`a[n]`: how much game time frame n moves the world forward.

    W[n] = sum(a[:n+1])     game-world time of frame n
    D[n] = W[n] + L         when frame n is on screen, for one constant L

Then `D[n] - D[n-1] == a[n]`: the advancement sequence and the display-interval
sequence are the same list. There is nothing to keep in sync. The engine reads
it as how far to step the world; the panel reads it as when the next picture
appears.

One `L` for the whole stream is what keeps the map affine. It also carries the
two machines' clock offset, as the hold's constant does (§3.7), so no clock
sync is needed: ping-based sync moves 0.5-2.6 ms on busy Wi-Fi (§8), which a
fixed offset cannot absorb.

The schedule need not be even. Game times 5, 15, 20, 40 shown at 5+L, 15+L,
20+L, 40+L are perfectly paced. Only the slope matters.

Nothing here commands the engine's clock, because nothing can: §11.11 is what
the interfaces actually offer, and why the invariant holds anyway.

### 11.2 The grid constraint disappears

`D[n]` has to land on a refresh of the client's panel: `anchor + k*G`. Since
`D[n] = anchor + W[n] + L` and `W[n]` is the running sum of advancements, the
constraint is satisfied by construction if every advancement is a whole number
of refreshes:

    a[n] = m[n] * G     m[n] >= 1, integer    fixed-refresh panel
    a[n] = any real                           VRR panel (the grid is continuous)

No snapping at display time, and no residual rounding at the panel. That is the
whole VRR / fixed-refresh unification: the decision variable is integer in one
case and real in the other, and the rest of the algorithm is identical.

### 11.3 A fractional rate from integer steps

`m[n]` being an integer does not quantize the rate. Carry the rounding error
forward:

```python
def step(target, carry, G):
    """One advancement, in ns, and the error to carry into the next."""
    if G == 0:
        return target, 0.0          # VRR: take it directly
    ideal = target + carry
    m = max(1, round(ideal / G))
    return m * G, ideal - m * G
```

A 22.2 ms target on an 8.33 ms grid comes out 3, 2, 3, 3, 2, 3 refreshes, mean
exactly 22.2. This is error-feedback quantization, the same trick as dithering
a gradient: round each step, remember what was thrown away, add it to the next.

With a *constant* target the recurrence has a closed form,

    sum(m[:n]) == round(n * target / G)

which is Bresenham's line, and is what `Schedule.slot(n)` computes. That the
two agree exactly is what lets the client name a frame's slot without being
sent the sequence. They diverge only when the target varies per frame (§11.8).

### 11.4 Feasibility is a queueing constraint

The histories say what is achievable. With `R[n]` the host's render latency for
frame n and `S[n]` the delivery latency (commit to on screen), frame n must
start by `D[n] - S[n] - R[n]` and cannot start before frame n-1 finished.
Chaining those, the condition is on the running sums, not per frame:

    mean(a) >= mean(R)

Frames cannot be scheduled closer together than the game can produce them on
average. Jitter around that mean is what the render-ahead buffer absorbs; using
a high quantile of `R` instead of its mean buys margin for that jitter, and
§11.7 is what the margin costs.

### 11.5 Three pointers, and the commitment boundary

    n_shown  <=  n_render  <=  n_sched

`a[:n_render]` is **committed**: the engine has already advanced the world by
those amounts, so they are immutable. `a[n_render:]` is tentative, and
re-planned freely as measurements arrive.

So re-planning never causes a discontinuity, which was the worry: only terms
nobody has rendered are ever rewritten, and realized world time never jumps. A
rate change is not a pacing event at all - the slope stays 1, only the sample
density changes - so a game dropping from 60 to 40 fps costs nothing. A change
in `L` *is* a one-time step in the map, so it is stepped, never slewed: a
slewed offset is many frames at the wrong slope, which is the judder being
avoided. (Measured, for the hold's constant: an edge-creeping tracker "was
smooth but never still".)

### 11.6 Generating ahead

The engine must never block waiting for a term, so the schedule runs ahead of
the renderer. But frames past `n_render` have no measurements yet - they do not
exist - so their terms come from predictions, including the generator's own
output fed back in:

```python
R_hat = list(R_measured)                   # truth where there is truth
carry = 0.0
a = a[:n_render]                           # drop the tentative tail
while len(a) < n_render + LOOKAHEAD:
    s, carry = step(predict(R_hat, S), carry, G)
    a.append(s)
    R_hat.append(s)                        # free-running past the measurements
```

Keep the decided advancement and the measured render cost as separate series.
They converge, since the game is throttled to the schedule, but they are not
the same signal: feasibility needs the true cost, and a history holding only
the generator's own output loses the evidence that the game is struggling.

### 11.7 Render-ahead depth is the input-latency knob

`B = n_render - n_shown`. A frame samples input when the engine starts it,
which is about `B` advancements before its slot, so

    input to photon  ~=  B * mean(a) + S

Every frame of render-ahead costs a full frame interval of input lag: two
frames at 45 fps is 44 ms added deliberately. The tension is one-dimensional -
`B` too small and render jitter overruns slots, `B` too large and input feels
mushy - which makes it the knob to expose, rather than a buffer depth.

Measured with `tools/mockgame` (2026-11-01): a Windows D3D9 program run from
Steam under Proton, which flips a strip the frame it sees the cursor move. The
Mac client's `GM_DEBUG_MOUSE=<file>` (`apple/MouseProbe.swift`, H.264, HEVC or
AV1) steps the pointer and times the flip. Built-in 60 Hz screen, HEVC, hotspot,
p50 ms, 70 steps each:

| game | mouse to game | game to gamescope present | present to on screen | total |
|---|---|---|---|---|
| vsync, as is | 13.6 | 53.6 | 16.5 | 83.4 |
| `d3d9.maxFrameLatency = 1` | 7.8 | 36.9 | 14.0 | 58.6 |
| that, vsync off, cap 60 | 9.0 | 36.9 | 16.0 | 62.7 |
| `d3d9.maxFrameRate = 59` | 13.4 | 1.9 | 14.1 | 31.8 |
| vsync, client asks for 120 Hz | 10.6 | 50.7 | 26.5 | 87.3 |
| vsync, no client grid (`--no-phase`) | 6.0 | 53.6 | 19.6 | 79.1 |
| vsync, no client grid, 120 Hz | 10.5 | 50.6 | 26.1 | 87.3 |
| `d3d9.maxFrameRate = 60` | 7.5 | 53.6 | 14.2 | 76.3 |
| `d3d9.presentInterval = 0` (still 60 fps) | 10.3 | 53.6 | 16.5 | 81.0 |

"Mouse to game" is mostly the wait for the game's next read: uniform over a
frame, with a floor of 2-3 ms. The frames between the game's read and the
screen wait in gamescope's commit queue (table below), and the stats overlay
cannot see them: its total starts when gamescope hands a frame over.

The gap as seen, `GM_DEBUG_MOUSE_SWEEP=0.4`: the real cursor swept across the
window (warped, without Accessibility) and each frame's orange block matched to
when the cursor was there, about 1200 frames each: cursor to block on screen
p50/p90 80.7/83.2 ms as is, 31.5/35.9 ms with `d3d9.maxFrameRate = 59`.

Warcraft III itself (`GM_DEBUG_MOUSE_GRID`, 48 px steps, its cursor found by
which video cells change after each step; client full screen on the Dell, HEVC,
main menu). It draws the cursor itself and runs at about 58.2 fps here, cause
unknown, so a cap has to be under that, not under 60:

| `dxvk.conf` next to the game | frames after a step | step to present | step to on screen |
|---|---|---|---|
| none | 3 | 63 ms | 88 ms |
| `d3d9.maxFrameRate = 59` | 3 | 62 ms | 88 ms |
| `d3d9.maxFrameRate = 57` | 0-1 | 26 ms | 53 ms |
| `d3d9.maxFrameRate = 30` | 0-1 | 36 ms | 62 ms |

The grid scan runs on the decoder's thread and may add to these totals.

**Cause, measured with the 0007 gamescope build** (commit arrival to paint):
in FIFO mode gamescope latches one commit per vblank (or per limiter slot) for
a window (`handle_done_commits_xwayland`), so a queue only shrinks if frames
leave faster than they arrive. DXVK lets the game run up to three frames ahead
of the last frame shown. If the slowest stage is after the game (gamescope's
vblank or limiter), the queue stays full. If it is before the game's read
(DXVK's limiter, which holds the game in `Present`), the queue is empty.

| Mock game, Dell | stream | gamescope limit | commit to paint | total |
|---|---|---|---|---|
| default | 60 Hz | Steam's 59 | 49.6 ms | 74.4 ms |
| `d3d9.maxFrameRate = 57` | 60 Hz | Steam's 59 | 0.6 ms | 27.5 ms |
| default | 120 Hz | Steam's 59 | 50.5 ms | 78.9 ms |
| default | 120 Hz | none | 0.6 ms | 24.0 ms |

| Warcraft III, Dell, no `dxvk.conf` | stream | gamescope limit | frames after a step | step to on screen |
|---|---|---|---|---|
| | 60 Hz | Steam's 59 | 3 | 88 ms |
| | 60 Hz | none | 3 | 84 ms |
| | 120 Hz | none | 0 | 32 ms |

Steam's frame limit reaches gamescope through
`gamescope_control.set_app_target_refresh_cycle`, not `GAMESCOPE_FPS_LIMIT`
(`[Gamescope WSI] Swapchain received new refresh cycle` logs it; cleared for a
test with `gamescopectl debug_set_fps_limit 0`). It is a limit after the game:
it paces the queue, so it holds latency up instead of lowering it. With no
limit, DXVK holds a game that asked for 60 Hz vsync at 60 by itself, so a 120 Hz
stream leaves the queue empty. A game as fast as the stream fills it again, at
three frames of that rate.

So gamescope now throttles the game instead (§6.2, patch 0008). Warcraft III,
Dell, HEVC, 60 Hz stream, no frame cap, step to on screen:

| `gm_throttle` | commit to paint | frames after a step | p50 | fps |
|---|---|---|---|---|
| 0 | 47.7 ms | 3-4 | 89 ms | 60.0 |
| 1, no phase lock | 0.3 ms | 0-2 | 40-58 ms, by luck | 60.0 |
| 1, phase lock | | 0-1 | 41-42 ms | 60.06 |

A release period longer than the refresh only repeats frames (about 20 a minute
at 100 us longer), so it is exactly the refresh. `GM_DEBUG_MOUSE` steps run off
the main queue: on it, full screen, they fell in step with the frames and each
run sampled one phase of the frame. They are still not spread evenly. Commit to
paint was not measured with the phase lock, and the phase-locked runs are from
the 0008 build without the timing drop-in.

### 11.8 What to minimize

    minimize   w1 * P(miss slot) + w2 * (B * mean(a) + S) + w3 * var(m)
    subject to mean(a) >= quantile(R, p),  m[n] >= 1 integer

`var(m)` is the unevenness of hold durations: a frame held 3 refreshes carries
slightly more motion smear than one held 2. Real, but far below the other two,
so a uniform `m` is preferred and not required.

The solver is not an optimizer. Set `mean(a)` from a quantile of `R`, set `B`
as small as keeps the miss rate under target, and let §11.3's carry spread `m`
as evenly as it can.

The one place learning would earn its place is **predicting `R[n]` per frame**,
not predicting the sequence. A quantile schedule pays the p99 render cost on
every frame; a predictive one pays each frame its own cost, and the difference,
`quantile(R, 0.99) - mean(R)`, is large on a system with translation and
thermal noise. Seeing a heavy frame coming - scene change, new geometry, a
shader not yet compiled - means giving that frame three refreshes and the rest
two, with the world clock following, which is exactly as smooth while running
faster and with less input lag than the worst-case schedule. This is the case
with no closed form, so it needs the slots sent explicitly.

### 11.9 Which machine owns what

The two budget terms split by machine, and neither side needs the other's
statistics:

| | owner | from |
|---|---|---|
| `L` (slot to photon) | client | its own decode and display latencies |
| release lead (game render cost) | host | gamescope's commit timing |
| `mean(a)` | client proposes | p99 of observed host frame spacing |
| the world clock, `B` | host | the schedule |

The client proposes the rate and the host refuses it implicitly, by missing
slots; the two meet through the drop rate rather than through shared estimates.
Worth one diagnostic field host to client (slots missed) purely for
attribution, so a drop can be blamed on the right end.

### 11.10 What runs today

With a uniform advancement the schedule is just `(anchor, interval)`, which is
exactly what `DisplayGrid` (protocol `0x0d`) already carries, so phase 1 is
client-only and the host needs no change: it already paces gamescope to the
grid the client sends. Restarting gmstream kills the running game, which is
reason enough to take the client-only half first.

Phase 2 - a fractional mean, and so §11.3's 3, 2, 3 - needs the host to
throttle the game to the sequence and advance the world clock by `a[n]`, near
`grid_release` in `core/compositor.c`. Phase 3 is §11.8's per-frame target,
which needs a protocol message carrying explicit slots.

### 11.11 Moving the world clock: what the interfaces actually offer

**No API says "advance by X".** Vulkan, Wayland and gamescope all expose
timing that is completion-shaped - here is when your last frame appeared - not
command-shaped. `wp_presentation`, `VK_GOOGLE_display_timing` and
`VK_KHR_present_wait` report the past. `VK_EXT_present_timing`, Reflex and
Anti-Lag 2 let the *application* say when it wants to present or start a
frame, which is the opposite direction. Nothing lets a compositor set an
engine's dt.

What exists is one lever: **when the game is let go.** A typical loop is

    dt = now() - last;  last = now();  simulate(dt);  render();  present()

and it blocks in `present` / `vkAcquireNextImageKHR` until a buffer comes
back. Holding `wl_buffer.release` therefore sets the loop's period, which is
exactly what gamescope's own FPS limiter does (§6.2), and DroidDeck's paced
release, and Armada's patch 0027. The actuator is already there and already in
the path; what it cannot do today is use a different interval per frame.

And then the clock follows for free, which is the part worth noticing. If the
loop is release-bound and samples its clock at `r[n]` (plus `e[n]`, for where
in the loop the sample falls), then

    W[n] = sum(dt) = r[n] - r[0] + e[n] - e[0]

so world time tracks the release times with a *bounded* error rather than a
drifting one. Showing each frame a constant `L` after its release then gives
`D[n] = W[n] + const`, which is the §11.1 invariant, with no clock commanding
anywhere. **The job is not to command the engine's clock; it is to make the
release schedule and the display schedule the same affine map.** §11.1-11.8
are that map, and `grid_release` in `core/compositor.c` is already half of it.

The residual, `e[n] - e[0]`, is the variation in where in its loop the engine
reads the clock. That is judder, and it cannot be removed from outside the
game. It is small for a tight release-bound loop and grows under load, so the
scheme is "as good as the engine's own loop timing", not exact.

Three cases where it is worse than that:

- **A game that renders ahead** - deep swapchain, no vsync, not blocking - has
  release times we do not set, so it has to be throttled before any of this
  applies. gamescope running at the cap already does this to it (§5).
- **A fixed-timestep engine** with interpolation (accumulator, render
  interpolated at the fractional remainder) follows wall-clock dt in the
  interpolation and is fine. One without interpolation quantizes motion to its
  own step and judders whatever the compositor does.
- **An engine that samples its clock off the render thread** breaks the
  relation between `r[n]` and dt entirely.

The exact version of "advance by X" is to interpose the clock: `LD_PRELOAD`
over `clock_gettime`, returning scheduled world time instead of real time,
which is what `libfaketime` does and what the phrase literally means. It is
exact, and it desynchronizes audio, networking and everything else that
compares against real time, so it is not a general answer - but it is the
measurement tool for finding out how large `e[n]` really is in a given game.

## 12. Streamable AV1: a frame on the wire before it is encoded

**Goal.** Each stage used to wait for the one before: a whole frame was encoded,
then sent, then decoded. Now the host sends each piece of a frame's bitstream as
the GPU writes it, so most of a frame is on the wire before the encoder has
finished it. The bytes are the encoder's own, from the same single encode call,
byte for byte: nothing is re-encoded, re-packed or reconstructed.

### 12.1 What the hardware will and will not do

VA-API says a frame's bitstream is ready only when the whole frame is: one fence
and one feedback buffer per picture, and mapping the coded buffer waits for the
GPU. Mesa's `radeon_enc_get_feedback` reads the length from a feedback buffer the
firmware writes at the end, and its segments separate the headers from the
bitstream, not tile from tile. There is no per-tile completion to wait on, and
`vlVaAcquireBufferHandle` refuses to export a coded buffer, so it cannot be
mapped behind the driver's back either.

Three facts make it work anyway, all of them measured on this machine (AMD
Radeon 8060S, strix_halo, Mesa 26.1.2; `tools/obuwatch` is the experiment, and
`tools/caps` what the encoder reports):

- The coded buffer's BO is created once and reused (`picture_av1_enc.c`:
  `if (!coded_buf->derived_surface.resource)`), so a mapping taken while the
  buffer was idle stays a live view of it. It is `PIPE_USAGE_STAGING`, and the
  CPU sees the GPU's writes as they happen.
- VCN fills it in order, in blocks of 384 bytes. At 2940x1846 and ~200 kB a
  frame, the first bytes appear 0.4 ms after the picture is submitted and the
  last at 4.3 ms.
- It does come back and change a few bytes it had already written: about nine in
  the frame header, and the three bytes of each tile group's OBU header and
  size, once that tile group's length is known. Nothing else, in any run.

`tiles`/`tile_groups` on `av1_vaapi` is what makes the pieces worth having: each
tile group becomes its own OBU, so the bitstream has real cut points. At
2940x1846, `1x4` with four tile groups costs nothing measurable - 193-196 kB a
frame either way, and encode time 4.05 ms against 4.28 ms with one tile - and the
tile groups finish at 0.53, 2.00, 2.78, 3.55 and 4.05 ms. Eight tile rows cost
about 1 ms of encode time, so four is the default.

### 12.2 How it is built (`core/vastream.c`)

One coded buffer is kept for the encoder's lifetime and its mapping held. Before
each picture is submitted the buffer is pre-filled with a position-keyed pattern,
so where the pattern resumes is exactly how far the GPU has written, and nothing
unwritten is ever sent (thirty-two bytes of agreement is proof; by chance it
would take 2^256). While FFmpeg waits for the frame, whole chunks of what has
appeared go out. The first 128 bytes are held back, because the frame header is
patched after the fact.

What makes it exact is the last step rather than any assumption about the GPU: a
copy is kept of every byte as it was sent, and when FFmpeg hands back the frame
that copy is compared with it. The handful of bytes that changed go out again in
later pieces, then the held-back start, which is the frame's last piece. The
client lays pieces down in index order, so the later ones win. That holds however
the GPU behaves - if it ever patched something else, the comparison would catch
it and send that too.

It needs no patch to FFmpeg or Mesa: `build.rs` passes `--wrap` for the five
libva calls involved, so FFmpeg's own calls are redirected into `vastream.c`,
and the polling happens inside the wait FFmpeg already does
(`vaSyncBuffer` in short steps), on the encoder thread, with no extra thread and
nothing to race against.

The pattern covers twice the largest recent frame (at least 256 kB, at most
2 MB), laid down after the previous frame while the encoder thread is idle. A
keyframe larger than that streams only its first part early, which costs that
one frame's overlap and nothing else. `GM_CHUNK_PREFILL` forces a small window,
which is how that path is tested.

The protocol is in PROTOCOL.md ("Video chunks", datagram type 9); a client asks
for it with Hello flag bit 2, and the Mac and iPad apps have it as a setting
("Take AV1 frames as the host encodes them", `--whole-frames` to turn it off, so
the two can be compared on the same content). Anything else - the web client,
H.264, HEVC, PyroWave - gets whole frames as before, and so does the first frame
of a session, before the buffer's mapping is known.

### 12.3 Measured

Bit-identical, by hash. `GM_DEBUG_CHUNKS` logs each frame's size and FNV-1a hash
on the host; `probe -H` logs what it put back together. Over thousands of frames,
including 31 keyframes and through 30 size changes, every hash matched and no
AV1 bitstream failed to chain its OBUs - also with `GM_CHUNK_PREFILL=20000`,
where only 19 kB of each 105 kB frame is sent early and the rest afterwards.

Per frame on the host, 2940x1846 at ~105 kB (`GM_DEBUG_CHUNKS`): the first piece
leaves 1.2-3.0 ms after the picture is submitted, the bitstream is done at 8.7 ms,
80-86 pieces, 99.5% of the frame sent before it was finished, and 3-9 bytes
re-sent.

End to end, Mac over the hotspot, 2940x1846 at ~54 Mb/s, 60 Hz, vkmark's
`desktop` scene, whole frames against pieces (two alternating 16 s runs each):

| | whole frames | in pieces |
|---|---|---|
| encoded → received | 4.89, 5.02 ms | 2.63, 2.67 ms |
| present → received | 13.92, 14.05 ms | 11.78, 11.82 ms |

With the real client decoding and showing the frames, same content at ~70 Mb/s
(p50 / p99 / max): network 4.4 / 6.5 / 7.6 → 3.1 / 5.3 / 6.6, total
35.1 / 42.7 / 43.9 → 33.7 / 40.8 / 42.1. The stats overlay's new `early` row is
the overlap itself: how long before the host finished a frame its first bytes
were already here, −1.9 ms with whole frames (a whole frame cannot start arriving
until it is finished) against +1.2 ms with pieces.

The gain is larger without the decoder in the way (2.3 ms against 1.3 ms),
because the client reads datagrams and submits frames to VideoToolbox on the same
thread, so the read lag is added to both. Taking decode off the network thread is
the next thing worth doing, and it is a client matter, not an encoder one.

### 12.4 The Vulkan encoder (`core/vkencode.c`)

VA-API cannot express what this needs: it has no device-side wait, so a thread
has to sleep through a GPU round trip before the encode starts, and it wants
NV12, so gamescope's frame has to be blitted and converted first. Vulkan's
video encode queue has both. For AV1 the encoder is now `core/vkencode.c`;
`encode.c` (VA-API, through FFmpeg) stays as the reference and runs instead
when `GM_AV1_VAAPI` is set.

What it does per frame:

- The compositor thread exports the rendering fence from gamescope's dma-buf
  and hands it over; the encode submission waits on it **on the device**, so
  VCN starts the instant the frame is drawn and no thread waits for the GPU.
- The frame is encoded **from gamescope's buffer** (see the correction below:
  it has to be copied into an image the encoder can read first). The encoder does
  the RGB→YCbCr conversion itself (`VK_VALVE_video_encode_rgb_conversion`,
  VCN's EFC), so the blit and the NV12 pool are gone. gamescope composites at
  ten bits whenever we offer it a ten-bit format (`GetPreferredOutputFormat`
  in its Wayland backend), and an eight-bit AV1 profile will only take
  eight-bit input, so the session follows the buffer: a ten-bit composite is
  encoded as ten-bit AV1. The client reads the depth from the sequence header
  when it builds VideoToolbox's `av1C`, so the stream describes itself.
- The bitstream lands in **our own pages**, imported with
  `VK_EXT_external_memory_host`, so watching the GPU fill it is an ordinary
  cached read — no `--wrap` of libva, no driver internals. The pattern,
  frontier and shadow-compare of §12.2 are the same, and still make the bytes
  exact.
- The **temporal delimiter and the sequence header are ours to write** (the
  encode operation emits only the frame OBU, and the sequence header comes from
  `vkGetEncodedVideoSessionParametersKHR`). Both are known before the encode,
  so they are the frame's first piece: 0.03 ms after submission, against the
  VA-API path's hold-everything-back-128-bytes.
- **Tiles per frame, from the frame's budget.** Tiles cost bits, so a keyframe
  (four times the budget) gets many and a small inter frame gets one or two —
  `GM_AV1_TILE_BYTES` sets the bytes per tile (48 KiB). Sizing them from the
  *last* frame's size runs away: tiles make frames bigger, which asks for more
  tiles, and it pins itself to the maximum within a second.
  **Measured later (§12.6), tiles buy nothing at all** and the default should
  probably be one: cutting the picture's dependences is not what makes a frame
  streamable here, watching the buffer fill is.
- **Rate control is off; the q index is picked per frame** (`next_q`), which
  bounds the bytes per *frame* rather than the bits per second: it is one
  frame's transmit time that shows up as latency. About 58 q indices halve the
  bytes, so the controller steps a third of the measured error each frame and
  settles in two or three. CBR hangs the encode ring on this hardware, every
  time, after a handful of frames (`ring vcn_unified_0 timeout`; the kernel
  resets it and recovers), and VBR ignores the bitrate (25 Mb/s and 100 Mb/s
  both gave 59). `GM_AV1_Q` pins a q index, `GM_AV1_RC=vbr` hands rate control
  back to the hardware.
- **The quantizer only changes when the rate-control command is re-issued.**
  `constantQIndex` reaches the encoder on the frame that follows
  `vkCmdControlVideoCodingKHR` and on no other: without that command the
  quantizer stays latched at whatever the session started with, and every
  later q is ignored. Measured at 1080p on one unchanging picture, with the q
  index sweeping 1..255: frames stuck at 74 kB when the session had begun at
  q 1, at 1.2 kB when it had begun at q 255. So the command goes out again
  whenever q changes. This was worth a factor of sixty and it is why the
  stream used to ignore the client's bitrate — the controller's q did nothing,
  and a client asking for 10 Mb/s got 130-250. The runs that looked right were
  the ones whose latched q happened to suit the content, which is how it hid:
  a pinned-q sweep behaves perfectly, because a pinned q never changes.

Two more things the hardware demands, both found by hanging the ring: the two
DPB slots have to take turns (encoding into the slot being referenced hangs
it), and tile spacing has to be spelled out, since this driver reports no
settable std syntax flags and so will not take `uniform_tile_spacing_flag`.

**Claim no coding tool the encoder does not apply.** The sequence header first
said `enable_intra_edge_filter = 1`. The encoder does not apply that filter to
its own reconstruction, so every intra block inside an inter frame decoded a
little differently from the reference the encoder had predicted from, and the
reference chain carried the error forward for as long as the session lasted:
colour casts and rainbow confetti spreading over a minute or two, worst where
the most blocks are coded. It survives nothing: not a keyframe (which clears it
until it rebuilds), and no amount of looking at a short clip, because at two
seconds there is nothing to see. The giveaway was that it got *worse* at a
finer q index, which is backwards for ordinary quantization loss.

Found by encoding seventy seconds with FFmpeg's own `av1_vulkan` on the same
device and measuring it against the source: 56.2 dB at frame 100, 53.2 dB at
frame 4150, so the driver was fine and the fault was ours. Diffing our sequence
header against FFmpeg's left four candidates, and switching them one at a time
named this one. CDEF is off for the same reason -- nothing sets strengths for
it -- and `order_hint_bits_minus_1` is 7, because `order_hint` carries a whole
byte of frame id and a seven-bit field cannot hold it.

**What it took to read gamescope's buffer.** The first build encoded garbage:
a recognisable picture shredded into compression blocks. gamescope's buffer is
DCC — AMD's colour compression, the default for anything the GPU renders into,
two planes, pixels plus metadata — and the encode block reads such a surface
as raw bytes. Nothing else here notices: the 3D engine decompresses as it
samples (PyroWave), so does a VA-API blit, and the display engine scans it
out. Noise does not compress, which is also why frames stayed at 90 kB
whatever the q index said.

Vulkan is no help in finding this. `vkGetPhysicalDeviceVideoFormatPropertiesKHR`
says DRM-modifier tiling is a supported encode input;
`VkDrmFormatModifierPropertiesList2EXT` says **no** modifier carries
`VIDEO_ENCODE_INPUT`, including linear; and `vkCreateImage` with
`VIDEO_ENCODE_SRC` on gamescope's modifier succeeds. All three cannot be
right. The experiment settles it: with DCC off, encoding straight from the
dma-buf works, so the per-modifier query is simply wrong here.

The fix is in the compositor, and it is the same lever panel mode already
pulls. `create_linux_dmabuf` narrows what we advertise: gamescope picks its
buffer's modifier from that list, so dropping the compressed modifiers
(`encodable_modifier`: AMD's modifier with the DCC bit set) makes it
composite into something the encoder can read where it lies. Ninety modifiers
go unadvertised; gamescope settles on `0x200000018601b04`, one plane, and the
picture is correct. Only the buffer it shares with us loses DCC — its own
internal render targets keep it, which `RADV_DEBUG=nodcc` would not have
managed.

The same list sets the bit depth. gamescope composites at ten bits whenever it
is offered a ten-bit format, so the ten-bit formats are not advertised either
(`GM_10BIT` puts them back, for HDR or to measure); everything is then eight
bits from the composite to the client's texture, with no conversion anywhere.

**And the colour range matters more than the depth.** The client asks
VideoToolbox for eight-bit video-range NV12, so a full-range stream makes it
convert every frame on the way out of the decoder: decode 4.7 ms against
1.7 ms for the same content in video range, at 2560x1600, and the cost does
not move with the bitrate (4.7 ms at 1 Mb/s too) or with the depth. So the
encoder asks VCN for narrow range and says so in the sequence header.

Measured on the device, same scene and settings for both (1920x1080 at 60 Hz,
vkmark's `desktop`, over loopback so the network is out of it):

| | VA-API | Vulkan |
| --- | --- | --- |
| commit → bitstream done, p50 / p99 | 3.82 / 4.05 ms | **1.77 / 1.85 ms** |
| commit → received, p50 / p99 | 3.98 / 4.18 ms | **1.88 / 2.00 ms** |

601 frames each, no gaps, no malformed bitstreams, and every frame's hash the
same on both ends. On the Mac at 2560x1600 over Wi-Fi the host stage is 3.7 ms
against 4.3, and decode 1.7 against 1.6 — the advantage narrows with
resolution, because what the VA-API path wastes (one GPU round trip, FFmpeg)
is roughly fixed while the encode itself scales.

### 12.6 HEVC, and what a streamable unit actually costs

The same encoder takes HEVC (`VK_KHR_video_encode_h265`), and everything in
§12.4 that is not a header transfers unchanged: the dma-buf import, the
encoder's own RGB conversion, the bitstream buffer in our pages, gamescope's
fence as a device semaphore, the two-slot DPB, and the whole pattern /
frontier / shadow-compare path. Watching a buffer fill is a byte-level
business and knows nothing about codecs.

What differs is the headers (a VPS, SPS and PPS instead of one sequence
header, with the colour description in the SPS's VUI), the per-picture syntax
(reference lists, a short-term reference picture set and a picture order count
instead of order hints), a qp in 0..51 instead of a q index in 0..255, and how
a frame may be cut up. This driver reports **no HEVC tiles at all** (`maxTiles`
is 1x1) and up to **128 slice segments**, which may start part way along a
coding tree block row. It does not report `dependent_slice_segment_flag` as
ours to set, so every segment is independent: an entropy context reset and a
slice header of its own.

**And the cutting turns out to be worthless.** At a fixed q on vkmark's
`desktop` scene, 2940x1846 at 60 Hz over loopback:

| slice segments | bitrate | bytes/frame | first piece before the encode ended |
| --- | --- | --- | --- |
| 1 | **7.9 Mb/s** | 16.9 k | 3.33 ms |
| 31 | 9.2 Mb/s | 19.8 k | 3.36 ms |
| 128 | 10.0 Mb/s | 22.3 k | 3.60 ms |

Twenty-seven per cent of the bitrate for an overlap that does not move. The
reason is that VCN's bitstream writer flushes as it goes whatever the slice
structure is, so the frontier walks just as smoothly through one segment as
through a hundred. The default is one segment; `GM_HEVC_SLICE_BYTES` sizes
them by the frame's budget, which is how the above was measured.

The same question, asked of AV1 at last, gets the same answer — 1x1 tiles
6.3 Mb/s and 13.6 kB a frame, 2x16 tiles 7.1 Mb/s and 16.1 kB, with the first
piece 4.60 ms against 4.65 ms before the encode ended, and 36 back-patched
bytes a frame against none. So the tiling on the live path is cost without
benefit too, at a size that depends on the operating point (2.6% at a high
bitrate, 13% here) but never earning any of it back.

The general point, which is worth stating because it was got wrong first:
a streamable unit does not have to be a cut in the picture's spatial
dependences. The frame is in memory and can be scanned any number of times;
what is needed is only *some* unit that can leave before the rest is ready,
and here that unit is "whatever bytes the GPU has written", which is free.
Severing prediction and entropy context to buy it is paying for something
already owned. (A wavelet codec makes this structural rather than incidental:
its units are spectral, independent by construction because the transform did
the decorrelation globally, and they can arrive out of order or not at all.
That is PyroWave, and it is why PyroWave has no analogue of this table.)

HEVC also back-patches **nothing** — 0 bytes a frame across every run —
because Annex B frames its NAL units with start codes and carries no table of
sizes to fill in afterwards. AV1's tile sizes have to be written once the
tiles are done, which is what the held-back 128-byte prefix and the
shadow-compare resend are for.

Measured, 2940x1846 at 60 Hz over loopback, one slice segment, our rate
control: 5 Mb/s asked gives 5.8, 20 gives 19.9, 50 gives 48.4, at qp 33, 19
and 11, with the first piece leaving 3.07, 3.71 and 3.95 ms before the encode
finished and no gaps. Ten-bit Main 10 encodes from gamescope's `XB30`
composite with the encoder's own conversion and no copy, tagged PQ and
BT.2020, coded 2944x1856 and cropped back to 2940x1846 by the conformance
window — which is better than the AV1 path, where the padding is coded.

The client needed almost nothing: the pieces carry their codec, so
reassembly was already codec-agnostic, and HEVC parameter-set extraction and
`CMVideoFormatDescriptionCreateFromHEVCParameterSets` were already there for
the VA-API path. Only HDR detection was AV1-only, and for HEVC it reads the
transfer function off the format description VideoToolbox has already built
from the parameter sets, rather than parsing a VUI; the depth comes from
`general_profile_idc`, which sits at a fixed offset in the SPS.

**Why bother, given AV1 is better per bit.** `VTIsHardwareDecodeSupported`
says AV1 needs an M3 or later, so the M1 iPad cannot decode it in hardware
and was stuck on H.264 with none of this. HEVC Main 10 gives it the pieces,
HDR10 and the rate control, at a bitrate penalty that does not matter at
50 Mb/s into a 700 Mb/s link.

Coding tools follow the rule §12.4 learned the hard way: claim nothing the
encoder does not apply. `strong_intra_smoothing_enabled_flag` is HEVC's own
version of the intra edge filter that drifted the AV1 picture, and is left
off; SAO is an in-loop filter and waits behind `GM_HEVC_SAO` until it has
been checked against a reference decode; temporal MVP is off because the
driver does not report it as ours to set. Unlike the AV1 path, RADV's HEVC
path does program these into the hardware and clamps them against its caps,
so the trap is guarded there — but the rule costs nothing to keep.

**The setting.** Since every part costs bits and buys nothing here, how many
there are is the client's to choose: Settings -> Pipelining parallelism, Auto or
a count (protocol 0x11 `Parallelism`, `vkencode_set_parallelism`). Auto is each
codec's default -- AV1 as many tiles as the hardware allows, HEVC one segment.
The host clamps the count to the hardware and the picture and reports what it
settled on, which is the number in the stats line's `host in` row (`32 tiles
(2x16)`, `8 slice segments`), so the setting always answers for itself; `probe
-P N` does the same from the command line.

Measured on the live host, vkmark's desktop scene at 1920x1080, 60 Hz, 50 Mb/s
asked, 6 s each, over loopback:

| parts | HEVC | AV1 |
| --- | --- | --- |
| 1 | 39.9 Mb/s | 26.5 Mb/s |
| 8 | 49.1 Mb/s | 24.7 Mb/s |
| 32 | 52.4 Mb/s | 27.4 Mb/s |

HEVC pays 31% more bits for 32 segments, as §12.6's fixed-q table said it
would. AV1's numbers are flat because its rate control is holding the asked
bitrate and paying for the tiles in quantizer instead -- the same cost, moved
from the bitrate to the picture. Either way nothing is bought: the overlap comes
from watching the buffer fill, not from cutting the picture up.

### 12.5 Not done

- **Decoding as it arrives.** VideoToolbox takes whole frames, so the pieces only
  overlap the network today. The tile groups are there for it; what is missing is
  a decoder that can start on tile 0, which is the GPU AV1 decoder below.
- **Loss of one piece still loses the frame**, as before. Tiles make it possible
  to lose only a tile, but a decoder that accepts a frame with a tile missing has
  to conceal it, which VideoToolbox will not do.
- **A GPU compute AV1 encoder.** Motion search, transform, quantization and
  reconstruction parallelize in compute shaders. Entropy coding is serial within
  a tile, so tiles set the parallelism and the granularity of streaming. An
  intra-only or low-delay-P subset is enough, and the output must be legal AV1.
  Budget: well under today's encode time at 2940x1846, on a GPU the game is also
  using. PyroWave shows both what compute encoding costs and how it contends with
  the game. Worth it only if the hardware encoder's own time becomes the problem.
- **A GPU AV1 decoder on the client.** Metal compute for reconstruction and CPU
  threads per tile for entropy decoding, handling only what our encoder emits.
  This is what would make decode overlap possible. Weigh power on a MacBook and
  iPad against the hardware decoder.
- **A fresh baseline on a real game.** Everything above is vkmark's `desktop`
  scene on a second gmstream instance (`systemd-run --user --unit gmtest`), which
  does not disturb the session. The numbers §12 started from (Warcraft III's
  menu, 100 Mb/s: encode 9.9 / 12.4, network 4.7 / 12.0, decode 2.9 / 3.9,
  display 3.6 / 10.0, total 21.5 / 34.9) have not been re-taken with this in
  place.

PyroWave may still be the cheaper answer at high bitrate: it already sends
independently decodable packets as they are produced. Compare against it before
going further.

## 13. PyroWave inter frames

PyroWave stays a fixed-size-per-frame GPU wavelet codec, but a frame can now be
coded as its difference from a prediction from the previous decoded frame
(motion compensated, per 32x32 block on or off). There are no keyframes: every
frame is the same size, and the first frame (or one after the host lost track
of the client) is intra at that size. The codec work, step by step with each
step measured, is in PyroWave's tree under `INTER.md` (the patched source this
repository builds: `build/pyrowave-inter.patch` over upstream 79a3232, applied
by `build/pyrowave.sh` for the host and `apple/pyrowave.sh` for the clients).
On the AOM gaming clips it needs 37% fewer bits than intra PyroWave for the same
PSNR (Dota -70%, StarCraft -63%, Euro Truck -44%, Witcher -7%, Minecraft -1%).

### 13.1 How it runs

- Protocol: Hello bit 4 (plain UDP clients that ask for PyroWave), datagram type
  10, loss reports type 11 (PROTOCOL.md, "PyroWave inter frames").
- Stripes (13.3): the frame is coded as horizontal bands, each an image of its
  own that predicts from the whole previous frame, so encode, network and
  decode overlap stripe by stripe.
- Host (`core/pyrowave.c`): `pyrowave_encoder_enable_inter` with the stripe
  layout. The encoder decodes each stripe it hands out (its reference is what
  the client has when nothing is lost) and keeps the last 8 frames. The stripes
  are GPU submissions one after another; `pyrowave_encoder_packetize_stripe`
  waits only for its own and the stripe goes out at once
  (`gm_callbacks.pyrowave`), then `pyrowave_encoder_decode_stripe` queues its
  own decode behind the frame's encode.
- Client (`apple/PyroWave.swift`): a decoder per stripe; decodes every frame in
  order from its own previous output, each stripe straight into its rows of the
  frame (`pyrowave_decoder_decode_gpu_buffer_rows`). Once a stripe's inter
  packets are in it forms that stripe's prediction
  (`pyrowave_decoder_prepare_prediction`) while its coefficients arrive. A
  stripe is decoded when complete, when its last packet is in (packets come in
  order: the missing ones are lost), or when a later stripe or frame starts; a
  stripe or frame of which nothing came is decoded from nothing. What it missed
  goes back (type 11, repeated once a frame until a frame's `repaired` reaches
  it); the host replays the client's decode with exactly that and predicts from
  the result. Too old to replay, or a client with no reference (joined late, a
  gap longer than the host keeps): an intra frame (`KeyframeRequest`).
- Both GPUs are kept awake while frames flow (13.4): `core/gpuwarm.c` on the
  host, `apple/GPUWarm.swift` on the client.
- Settings: "PyroWave inter frames" (Mac and iPad, default on; `--intra-only`),
  only usable with PyroWave; the AV1/HEVC "frames in pieces" setting likewise
  only with those. The Hello carries each flag only with its codec.

### 13.2 Measured (2026-11-04)

Cross-GPU (`tools/pwenc.c` on the host's AMD GPU, `tools/pwdec.mm` on the Mac):
the Mac's decode of the AMD encoder's frames differs from the encoder's own
decode in 0.01% of luma samples (fp16 transform rounding), not growing over 30
frames; frame 10's inter packets dropped on the Mac with the loss reported, frame
11 is back within 0.02%; unreported, 23.8 dB by frame 29 (29.4 with).

Live, the host's test instance (vkmark's desktop scene, gamescope nested) over
the hotspot, the Mac app in `--probe` (decoding, not presenting: this Mac's
display was asleep), present -> decoded p50 / p99:

| | host encode (start -> packets) | present -> decoded |
|---|---|---|
| 1920x1080 @60, 50 Mb/s, intra | 0.77 ms | 6.6 / 8.5 ms |
| same, inter | 2.09 (inter packets at 1.62) | 8.6 / 10.4 |
| 2560x1600 @120, 100 Mb/s, intra | 1.08 | 8.0 / 10.8 |
| same, inter | 3.23 (2.58) | 10.7 / 13.3 |
| 3840x2160 @120, 100 Mb/s, intra | 1.70 | 11.0 / 13.4 |
| same, inter | 6.11 (5.03) | 14.9 / 17.1 |

With every 500th datagram dropped (`GM_DEBUG_DROP=500`, about one frame in six
damaged): every loss reported and replayed, no intra frame needed, 1080p p99
11.1 ms (it was 23.9 before a frame was decoded as soon as its last packet
arrived instead of when the next one began).

So on this GPU inter frames cost 2-4 ms of latency at the same bitrate, almost
all the host's motion search (six passes over the frame, ~0.7 ms each at 4K):
the inter packets are ready only ~0.3-1 ms before the rest, so the overlap they
buy on the client is small next to it. Inter frames buy bits (37%) or quality,
not latency, until the search is cheaper.

The measurements above are of whole frames, before stripes (13.3).

### 13.3 Stripes: encode, network and decode overlapped

A whole frame overlapped nothing: the motion search, the transform, the rate
control and the packing each finish the frame before the next starts, and the
client's inverse transform needs every packet (its coarsest level spans the
frame). So the frame is cut into horizontal stripes, each coded as a PyroWave
image of its own: its own transform, rate control, packets and inter packets,
predicting from anywhere in the whole previous frame (the inter shaders take the
stripe's row in the reference). The host's GPU does the stripes one after
another and each goes out as soon as it is done; the client decodes each as it
lands, straight into its rows of the frame. Bit-exact on both ends as before
(the Mac tests check the client's decode against the encoder's, stripe by
stripe), loss replay per stripe.

- Layout: a first stripe of 128 rows (out sooner), then 384, the last taking the
  rest (128 rows or more); stripe boundaries on 64 rows so every grid lines up
  (`GM_PYROWAVE_STRIPE_ROWS`, `GM_PYROWAVE_FIRST_ROWS`; 0 rows: one stripe).
- Rate: each stripe gets the share of the frame's bytes that the rate control
  would have given it across the whole frame, from the last frame's statistics
  (the same distortion-per-byte cut in every stripe), at least a quarter of its
  share by area; the frame's total is exact as before.
- Cost in bits, Euro Truck at 1080p (PSNR-Y): 384-row stripes -0.01 dB, 256 rows
  -0.05, 128 rows -0.15. More at low rates on big frames (4K at 104 KB a frame,
  256 rows: -0.24 dB).
- Cost in GPU time: a fixed amount per stripe on both ends, so at a light load a
  frame of 7 stripes takes about what one stripe did, and under a GPU-bound game
  each stripe's chain of passes waits on the game's work (13.5).

### 13.4 Both GPUs kept awake

Decoding and encoding a stripe at a time are short bursts, and both GPUs' power
management reads that as a light load and lowers the clock: on the M4 the same
stripe decoded 4.4x slower in streaming than back to back (inverse transform
0.39 ms a stripe against 0.12), on the host the first stripe came out after
1.04 ms against 0.45 with the clock forced up. Neither clock can be set from an
ordinary process (Metal's performance state assertion needs a private
entitlement; amdgpu's `power_dpm_force_performance_level` needs root), but both
follow how busy the GPU is, so each end submits a tiny kernel back to back on a
low priority queue of its own while frames flow, and stops a second after the
last: the GPU is never idle in its eyes, the filler does almost nothing, and the
codec's work goes first. Any gap in the filler (even 0.5 ms) lets the clock
drop again.
- Client: `apple/GPUWarm.swift` (256 threads x 100 FMAs a dispatch, two in
  flight, Metal queue priority 0), setting "Keep the GPU awake for PyroWave"
  (default on, PyroWave only; `--no-gpu-awake`).
- Host: `core/gpuwarm.c` (its own Vulkan device, a compute queue at low global
  priority, 4 workgroups a dispatch), on whenever PyroWave frames are encoded
  (`GM_PYROWAVE_GPU_WARM=0` turns it off). Under a GPU-bound load it costs
  ~0.15 ms of encode time; under a light one it saves ~0.9.
Like Keep Wi-Fi awake, power for latency, only while streaming.

### 13.5 Measured with stripes (2026-11-04)

The host's test instance at 2560x1600 @120, the Mac app in `--probe` (decoding,
not presenting: this Mac's display was off), present -> decoded p50 / p99, with
vkcube (a light GPU load) or vkmark's desktop scene (the GPU at 100%):

| | whole frames (13.2) | stripes | stripes, both GPUs awake |
|---|---|---|---|
| vkcube, 100 Mb/s | 11.6 (vkmark) | 8.3-9.7 | 4.9-5.5 / 6.0-7.6 |
| vkcube, 300 Mb/s | | | 6.6 / 9.1 |
| vkmark, 100 Mb/s | 11.6 / 16.8 | 9.0-9.9 | 7.5-8.1 / 8.9-9.1 |
| vkmark, 300 Mb/s | 14-14.7 | 10.8 | 8.4-8.6 / 10.0 |

With stripes and both GPUs awake at 100 Mb/s under vkcube: the first stripe out
0.45 ms after the frame starts on the host, the whole frame 1.65 ms; the last
packet to the frame decoded 0.6 ms (it was 2.3 for a whole frame). Under vkmark
the host's encode is the longest stage (the frame's 5 stripes 4-5 ms, against
2 ms for one stripe on the GPU alone): the game takes the GPU, and every pass of
every stripe waits for its share. Things tried that did not change that: one
submission for every stripe, a completion mark polled instead of the fence,
fewer barriers, the per-band packing and rate analysis in one dispatch each, the
stripes alternating over two compute queues (all measured, all reverted).

### 13.6 Next

- The host's encode under a GPU-bound game: the motion search over the whole
  frame at once (fewer, fuller dispatches) before the stripes' backs, or fewer
  stripes when the GPU is contended.
- The hotspot carries a frame's stripes slower than one burst: at 100 Mb/s under
  vkcube a 104 KB frame arrives over ~3.2 ms in stripes (about 280 Mb/s; the
  host sends each stripe 0.01 ms after it is ready, `GM_DEBUG_SEND`), against
  1.7-2.2 ms as one burst, and the client sees each stripe land at once and
  stripes 1-3 together (`GM_DEBUG_TRACE` now records type 10's packet and
  stripe). Small bursts aggregate worse on the air, which is likely why longer
  stripes measured better. Keeping the Mac's Wi-Fi awake at 500-2000 Hz instead
  of 60 changed nothing. Untried: the hotspot's own aggregation settings
  (mt76), carefully, from the wired network.
- The client's display stage (decoded -> on screen) was 22 ms in a window on
  2026-11-04 before the stripes, which is now the biggest part of the total; not
  looked at here (pacing, DESIGN.md §3.7).
- The iPad app builds but is not installed (not connected).
