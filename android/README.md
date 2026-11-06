# gmstream for Android

A native client for Android 13+ (arm64), built for and measured on an AYN Odin 3
(Snapdragon 8 Elite class, Adreno 830, 1080x1920 120 Hz panel, Android 15). It
speaks the same plain-UDP protocol as the Mac and iPad apps (PROTOCOL.md) and
does what they do, minus what Android or the hardware does not offer (below).
No root.

## Build and install

```sh
android/build.sh                 # -> android/build/gmstream.apk
adb install -r android/build/gmstream.apk
```

Needs the Android SDK (build-tools, platforms;android-36, an NDK r27+) and a
JDK; Riptide also needs cmake and ninja, and checks out Granite the first time
(`riptide/checkout_granite.sh`). No Gradle: `build.sh` drives clang, javac, d8,
aapt2 and apksigner itself, and signs with a local debug key.
`tests/run.sh` runs the portable core's tests on the build machine (crypto
known-answer tests) and builds `probe`, a headless client from the same core:
`probe <dir with device.key> <host> [port] [codec] [seconds]`.

## Using it

Open it: the settings come up the first time (later: a three-finger tap, or
Back). Set the host and port (plain UDP, 8444 by default), then add the line
under "This device's key" to `~/.ssh/authorized_keys` on the host ("Copy key
line" puts it on the clipboard); the host refuses the device until then.

For scripts, the launch intent takes the settings and saves them:
`adb shell am start -n org.gmstream/.MainActivity --es host H --ei port P
--ei codec 0|1|2|4 --ei mbps M --ei pacing 0|1 --ez stats true|false
--es size WxH --ei stripes N --ei parallelism N --ez pieces true|false
--ez scanout true|false`. Debug switches: `adb shell setprop
debug.gmstream.phase 1` (the phase lock's decisions to logcat),
`debug.gmstream.trace 1` (Riptide's first steps), `debug.gmstream.planes 1`
or `debug.gmstream.red 1` (Riptide's output pass shows the raw planes, or a
clear).
`--ez overLock true` (testing from adb) shows this window over the lock screen.

## What it does

- **Protocol**: the handshake (X25519, Ed25519 host key remembered per host,
  AES-256-GCM control stream: `jni/crypto.cpp` with Monocypher), Auth, the
  sealed control stream with acknowledgements and resends, mouse and Riptide
  loss datagrams, pings and the clock offset, reconnects (`jni/conn.cpp`).
  Every datagram's kernel arrival time (SO_TIMESTAMPNS) feeds the stats.
- **Video, H.264 / HEVC / AV1**: MediaCodec, Qualcomm's `c2.qti.*.decoder.low_latency`
  where present, with the keys Moonlight sets on Qualcomm (low latency, decode
  order, the software output fence). AV1 and HEVC frames are taken in pieces as
  the host encodes them (`jni/video.cpp`).
- **Riptide**: the host's own Vulkan decoder (`riptide/`) on the device's GPU,
  each stripe decoded as it lands, every frame decoded in order from whatever
  arrived, losses reported, missing frames bridged, an intra frame asked for
  when there is nothing to predict from (`jni/riptide_client.cpp`, after
  `apple/Riptide.swift` and the host's replay in `riptide_c.cpp`). The GPU is
  kept awake while frames come, by a tiny kernel on a low-priority compute
  queue of its own (`jni/gpuwarm.cpp`).
- **Presenting**: every decoded buffer goes to a SurfaceControl of our own
  (a child of the SurfaceView) in its own transaction, cropped to the picture
  (the AV1 path's coded height is 1088), with its acquire fence; the
  transaction's present fence gives the time it reached the screen, and a frame
  replaced before it was latched counts as dropped (`jni/presenter.cpp`).
  MediaCodec renders into an AImageReader; Riptide renders RGB into
  AHardwareBuffers imported into Vulkan.
- **Pacing**: Auto is the phase lock (`jni/phase.cpp`, a port of
  `apple/Phase.swift`) on the panel's measured refresh, from those present
  times, with Steam's VRR off; Low latency shows frames as they come and lets
  the game run free. On Android the cut-off a frame must make is
  SurfaceFlinger's latch, not the refresh (it latches 10.3 ms before the
  refresh it shows a buffer at, so a frame just past a latch waits a whole
  period more): the lock sees each frame as presented at its latch point plus a
  period (`Session::to_latch_grid`), so it steers frames to arrive just before
  a latch. Frames waiting for the latch: 4.6 -> 2.5 ms p50.
- **Scanout mode** (Riptide, experimental, "write frames into the buffer on
  screen"): one front buffer (`AHARDWAREBUFFER_USAGE_FRONT_BUFFER`) stays on
  a display plane, a commit goes in every refresh so the command-mode panel is
  sent the buffer at each refresh, and each frame is written into it in place;
  it goes out with the first transfer that starts after it is written, without
  SurfaceFlinger's 10.3 ms. The phase lock aims at the transfer's start.
- **On a display plane, never composited**: Riptide draws its RGB already
  turned to the panel's portrait orientation and tags the buffer with the
  inverse turn, so the display engine scans it out unrotated (it rotates only
  compressed buffers: an RGB buffer it has to rotate is composited by the GPU,
  every frame, which also breaks scanout mode). RGB buffers are tagged sRGB,
  the decoder's YUV BT.709. `dumpsys SurfaceFlinger --hwclayers` shows the
  layer as DEVICE with no transform.
- **When the panel reads a buffer** (Qualcomm's display driver for this chip,
  `sde_encoder_phys_cmd.c`, command mode): the present fence is signalled by
  the write-pointer interrupt, the start of the transfer to the panel; the
  release fence at the transfer's end (`pp_done`). Measured: the previous
  buffer is released 0.62-0.63 ms before the next present, every frame, so a
  transfer takes ~7.7 ms of the 8.33 ms period and reads the buffer row by row
  (in the panel's order: across the landscape picture) from the present fence
  on. "On screen" in the stats is that start.
- **Audio**: Opus through the platform's decoder (MediaCodec) into AAudio
  (low latency, exclusive), with the Apple clients' jitter buffer.
- **Wi-Fi**: "Keep Wi-Fi awake" holds Android's low-latency Wi-Fi lock
  (power save, scans and roaming off while streaming) and sends the 60 Hz
  keep-awake datagram.
- **Input**: touch as a touchscreen on the host (or, with the setting off, a
  finger as the mouse); game controllers, the Odin's own included, as W3C
  standard gamepads, with rumble on the pad's motor or the device's; keyboards
  as evdev codes; a mouse as positions, buttons and scrolling, captured for
  relative motion when the game asks, with the host's cursor image as the
  system pointer.
- **Stats**: the Mac's overlay: per-stage p50 / p99 / max over 5 s (encode,
  network, overlap, read, decode, to Android, display, total, spacing), the
  phase lock, what the host's encoder and this decoder take in, rtt, drops,
  gaps and audio.

Not here: PyroWave (codec 3; no Android decoder built), HDR, cursor
confinement to the game's clip, a client-drawn cursor, scanout mode for the
MediaCodec codecs (it needs the decoder's YUV buffers sampled into the front
buffer by the GPU).

What root would add, and the independent root service to hold it: ROOT.md.

## Measured (2026-11-06, Odin 3 over the router to the host's wired port, vkcube 1920x1080)

| p50 / p99, ms | AV1 40 Mb/s, phase lock | AV1, low latency | Riptide 100 Mb/s, phase lock | Riptide, scanout mode |
|---|---|---|---|---|
| network | 2.5 / 3.5 | 2.7 / 3.5 | 3.4 / 4.2 | 2.2 / 3.2 |
| decode | 0.5 / 0.8 | 0.5 / 0.8 | 4.2 / 5.2 (first stripe -> GPU done) | 4.2 / 5.1 |
| to latch | 2.5 / 3.0 | | 3.3 / 3.7 (from the CPU's submit) | - |
| latch -> transfer starts | 10.2 / 10.3 | | 10.2 / 10.3 | - |
| display (handed over -> transfer starts) | 12.7 / 13.2 | 13.7 / 19.3 | 13.5 / 13.9 | 3.9 / 4.1 |
| total (host present -> transfer starts) | 16.3 / 26 | 17.1 / 22.7 | 17.7 / 17.8 | **6.9 / 7.1** |

(The test host ran vkcube at 60 fps in a nested gamescope; the AV1 p99 total
includes its encode spikes. Before the lock aimed at the latch, AV1 frames
waited 4.6 ms for it: display 14.9, total 18.4. Riptide was at 19.9-23.9 total
while its buffers were GPU-composited and the lock timed them from the CPU.
A row of the picture leaves for the panel up to ~7.7 ms after the transfer
starts.)

- The display stage is SurfaceFlinger's: on this device it latches a buffer
  10.33 ms before the refresh it is shown at (`dumpsys SurfaceFlinger`: SF
  duration 10333333 ns at 120 Hz), so ~10.3 ms plus the margin before its
  latch is the floor on the latched path without root (ROOT.md, item 1).
  Android drives the panel in command mode (`/sys/class/drm/card0-DSI-1/modes`:
  `1080x1920x120cmd`), and the video layer is on a display plane (`dumpsys
  SurfaceFlinger --hwclayers`: DEVICE, rotated 90° inline).
- Riptide's decode on Adreno 830 is ~10x the M4's: inter frames need the
  compute path (PyroWave's Qualcomm fragment path does intra only). Keeping the
  GPU awake took it from 7.6 to 4.5 ms p50.
- HEVC and H.264 decode in 0.7 ms.
