# gmstream: SteamOS Game Mode streaming server

Design and handover notes. Status as of 2026-10-26: SteamOS Game Mode streams
end to end on steamtab through gmstream and a patched build of Valve's
gamescope (§6): boot animation, Steam UI, games (launch, play, exit),
keyboard, mouse, client-drawn cursor, gamepads, audio, reconnects; about
7–8 ms capture→display to Chrome on a Mac over Wi-Fi. Panel mode (the
built-in screen while nobody streams) is not done.
Wire format: [PROTOCOL.md](PROTOCOL.md).

Formerly "polaris-vd" (from when it was going to feed Polaris). It uses no
Polaris, Moonlight or GameStream code or protocol.

## 1. Goal and scope

Stream SteamOS Game Mode from a SteamOS machine (dev box: steamtab, an ASUS
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
- **The built-in screen still works** (planned, §6.2). With no client
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
                 steamtab (SteamOS, Game Mode session)
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

- Video: H.264/HEVC through VideoToolbox (NV12 output, converted in our
  shader), PyroWave through its Metal port (`build/pyrowave`, same commit
  as the host). Frames are drawn on a CAMetalLayer from a render thread.
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
    refresh where flips land. The cut-off is a fixed lead (the display's,
    not the shift's), learned from the leads of dropped frames; a servo
    holds the 1st percentile of the leads 0.4 ms above it, which also
    follows the drift between the two machines' clocks (tens of ppm, which
    otherwise crosses the cut-off within a minute). If most frames start
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
    times (the flips) land anywhere in the refresh, while with vsync they all
    land at one phase (GM_DEBUG_FLIP logs each flip's phase).
  - Full screen on a variable-refresh (Adaptive-Sync) display, detected from
    `NSScreen.minimumRefreshInterval` / `maximumRefreshInterval`: the panel
    refreshes when a frame arrives, so vsync costs nothing. The app keeps
    vsync on, sends an empty grid (interval 0: gamescope paces itself) and
    shows frames as they come, tear-free. Measured on a Dell U2725QE
    (48–120 Hz) from an M4 MacBook Air: total 12.6 / 13.7 ms p50 / p90,
    display stage 3.1 / 3.6 ms, pacing p90 ~2 ms. `present(atTime:)` to
    smooth pacing made both pacing and p90 worse. In a window the desktop is
    composited at a fixed rate (22–25 ms total).
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
- Keyboard: all of it goes to the host while the stream window has focus,
  Command as Meta. A local event monitor takes key events before AppKit's
  menus and key-view handling (Ctrl+Tab / Ctrl+Shift+Tab, Steam's Quick
  Access, never reach keyDown otherwise; key-ups with Command held are
  dropped by AppKit), and the private `CGSSetGlobalHotKeyOperatingMode`
  turns off the window server's shortcuts (Cmd+Tab, Spotlight, Mission
  Control, screenshots) until the window loses focus. The trackpad and
  mouse stay macOS's, and are how to leave: menu bar, another window, a
  Mission Control swipe. App menu items work by mouse only.
- Stats overlay (menu, or Settings): per-stage p50 / p90 / max over 5 s (encode,
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

## 4. gamescope facts this design depends on

Verified by reading gamescope's source (`src/Backends/WaylandBackend.cpp`,
`steamcompmgr.cpp`, `vblankmanager.cpp`, `rendervulkan.cpp`) at tag
**3.16.30.1** (what SteamOS ships on steamtab) and master, and by running it.
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

Measured 2026-10-26 on steamtab (Radeon 8060S), nested gamescope running
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

End to end, 2026-10-27, steamtab (wired) → M4 MacBook Air (802.11ax,
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

### 6.1 Installed (2026-10-26, steamtab)

gmstream is a user service that gamescope connects to; gamescope is not its
child. gamescope is **our patched build of Valve's own package** (§6.2),
installed beside Valve's, never over it. Files are in `session/` and
installed as:

- `~/.config/systemd/user/gmstream.service`: `gmstream -w gmstream-0 -s
  1920x1080 -r 60`, ordered before `gamescope-session.service`. It outlives
  gamescope restarts and accepts the next gamescope.
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
- Absolute pointer input is tracked even while the pointer is locked, so the
  relative motion derived from it stays true.
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
libraries exactly like Valve's. `ship.sh` builds on epyc and copies the binary
to steamtab.

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

### 6.3 Not done

1. **Panel mode:** with no client connected, gmstream drives the built-in
   panel: wlroots DRM backend through libseat (logind) and libinput for the
   built-in keyboard, touchpad and touchscreen; offer a scanout tranche in
   linux-dmabuf feedback so gamescope's buffers can be scanned out directly;
   adaptive sync on the panel when supported. Stream mode blanks the panel;
   disconnect returns to the panel's native mode. Needs wlroots built with
   `-Dbackends=drm,libinput -Dsession=enabled` (libseat, libinput, libudev
   and libdisplay-info; libudev stays a host library) and DRM-master
   handover testing. Until then the built-in screen is dark in Game Mode.
2. **Touch** (`WL_SEAT_CAPABILITY_TOUCH`; gamescope's backend supports
   wl_touch) for iPad/Android clients.
3. Upstreaming the gamescope patches; layered (non-composited) frames from
   gamescope's planes, which also opens per-layer and pre-upscale streaming.

Gamepads are unaffected by the compositor: Steam reads controllers directly
(hidraw/evdev).

## 7. Development and test workflow

- **Build host: epyc** (a NixOS box), reached through `build_host` in
  `local.env` (not checked in; see `local.env.example`). Never build on the
  NixOS host itself. Image `gmstream-build` (from `build/Containerfile`:
  Debian trixie + the static C deps in `/opt/gm` + rustup), long-running
  container `gmstream-dev` with `~/gmstream` mounted at `/src` and a cargo
  registry volume. Reference checkouts of gamescope and wlroots sources
  live in epyc `~/gmstream-ref/`.
- **`./dev-ship.sh`**: tars the sources to epyc (keeping mtimes), runs
  `cargo build --release` in `gmstream-dev`, copies only the binary to
  steamtab `~/gmstream/gmstream`.
- **Test run on steamtab** (host, no container; does not touch Game Mode):

  ```sh
  cd ~/gmstream
  ./gmstream -S -s 1920x1080 -r 120 -- gamescope --backend wayland \
    --force-composition --adaptive-sync -W 1920 -H 1080 -r 120 -- vkcube --wsi xcb
  ```

  Open `https://steamtab.local:8443/` in Chrome
  (accept the self-signed certificate once, or trust
  `~/.local/state/gmstream/cert.pem`). To keep it running after the ssh
  session ends, start it with `systemd-run --user --collect` (plain `&` or
  `setsid` gets killed when the ssh session closes).
- **Headless probe** (`examples/probe.rs`, `cargo build --release --example
  probe`): connects like the web client, prints fps, gaps and latency
  percentiles, audio packet sizes (silence vs sound) and host messages
  (cursor, pointer lock), and can save the bitstream (`-o`). It can also act:
  `-k 1,105` taps evdev keys, `-C 0.25,0.25` moves and left-clicks, `-g 15`
  taps standard-gamepad buttons on a virtual pad, `-x N`/`-X mask` stress
  bitrate/size/refresh changes. Run it on steamtab against `127.0.0.1:8443`
  for exact same-clock latencies. It replaces any connected browser.
- **gamescope:** `gamescope/ship.sh` (build in `gamescope-build` on epyc, copy
  to `~/gmstream/bin/gamescope`); restart the session to use it.
- **Browser test without clicking:** `?autostart&log` connects on load and
  logs the HUD to the console (headless Chrome + DevTools protocol works).

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
  (steamtab's `/home` is ~96% full); build images elsewhere.
- Container builds: copy only the built package into the runtime stage
  (a 6 GB image became 1.5 GB).
- A Decky plugin (Steamcord) registered a fake portal D-Bus name that broke
  portal discovery. Third-party plugins can shadow system services.
- `pkill -f` patterns can match your own ssh command line.
- Never `steamos-readonly disable`. Root access itself is fine; use `/etc`
  and user units.
- Build on epyc (direct address, not over Tailscale) in a container; ship
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
- A CADisplayLink from the screen tracks this app's own presents; use
  CVDisplayLink for the panel's refresh grid. Its timestamps are not the
  vblank itself: on the M4 MacBook Air's panel, vsync flips land ~7 ms after
  them.
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
- steamtab's wired port is a USB AX88179 under the generic `cdc_ncm`
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
  late) and ~2.5 ms of every frame's network time were steamtab's USB
  Ethernet adapter (ASIX AX88179A, 0b95:1790) running in its CDC NCM USB
  configuration under `cdc_ncm`. Kernel receive timestamps
  (SO_TIMESTAMP_MONOTONIC; GM_DEBUG_NET's "readlag") showed the packets
  reached the Mac late; DSCP, the NCM tx timer, host pacing, AWDL/AirDrop/
  Handoff/Location scans were ruled out; streaming over steamtab's Wi-Fi
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
- **Panel mode** needs DRM master handover through logind and direct-scanout
  compatible buffers from gamescope; untested.
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
  relative mode → client Pointer Lock) is implemented but not yet exercised
  with a real game.

## 10. Current state of things on disk

- This directory (`~/frame/gmstream` on the Mac): the source of truth, not
  in git yet.
- epyc: image `gmstream-build`, container `gmstream-dev`, `~/gmstream`
  (synced sources + `target/`), reference checkouts of gamescope 3.16.30.1
  (Valve's `jupiter-3.9` branch) and wlroots in `~/gmstream-ref/`. The old
  polaris-vd container, images and directory are removed.
- steamtab: `~/gmstream/gmstream` (+ `probe`), `~/gmstream/bin/gamescope`
  (patched), `~/gmstream/session/`, the installed units of §6.1, state in
  `~/.local/state/gmstream/`. The `vd-dev` build container is still there.
- epyc also has image `gamescope-build` and `~/gmstream/gamescope/` (cache,
  `out/`); `~/gmstream-ref/gs-patch` is a scratch worktree of gamescope.
- Old Polaris setup on steamtab, **disabled but installed**:
  `polaris.service` and `polaris-config-volume.service` are masked (user
  units; Quadlet files, images and volumes untouched). The fake-display
  helper's files under `/etc` are untouched; its plug template and idle
  timer are static units that only Polaris started, so nothing starts them
  now (they can't be masked: they live in `/etc/systemd/system` itself).
  Its `gamescope-session` drop-in only runs the (harmless) unplug on stop.
- `polaris-quadlet` repo branch `resume-before-capture` (pushed, not
  merged): Polaris patches 0001/0002 and the fake-display helper.
  Superseded; keep for reference or delete.
