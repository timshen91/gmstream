# What root would buy, and the service that should hold it

The client runs without root and should keep doing so. What follows is what
root unlocks on a Qualcomm Android device like the Odin 3 (SM8750 class,
command-mode panel, qcacld-3.0 Wi-Fi, msm_vidc video), each item with what it
costs today and the evidence, and then the shape of an independent root
service that the app uses when it is there and does without when it is not.

## What needs root, by stage

| stage | today, without root | what root changes | expected gain |
|---|---|---|---|
| display (latched path) | SurfaceFlinger takes a buffer 10.3 ms before the refresh that shows it (`latch->on` 10.2 ms p50 on the Odin 3; `dumpsys SurfaceFlinger`: sf duration 10.33-10.5 ms) | SurfaceFlinger's durations | ~5-7 ms on every frame of the MediaCodec path |
| decode (Riptide) | the GPU's clock follows its load; a filler kernel keeps it up (7.6 -> 4.5 ms p50) | a clock floor while streaming | the rest of the clock ramp, without the filler's power |
| network, client side | the Wi-Fi driver interrupts on a 512 us timer per receive ring (`WLAN_CFG_INT_TIMER_THRESHOLD_REO_RING`) | per-packet interrupts, or the latency-sensitive ring | up to ~0.5 ms after every burst |
| decode (AV1/HEVC) | MediaCodec takes whole frames | the video device directly: slice decode and per-row fences | most of the decode after the last byte; needs the host to send tile groups |
| scheduling | the app's threads are ordinary (SCHED_OTHER) | real-time priority, big cores, interrupt affinity | tails (p99), not medians |

### 1. SurfaceFlinger's pipeline

SurfaceFlinger wakes `sf duration` before the refresh it composes for and
latches whatever buffers are ready then. On the Odin 3 that is 10.33 ms (the
`late`/`early` sf durations are 10.5 ms in `debug.sf.*`), so a frame handed
over just after a latch waits up to a period more and then 10.3 ms. The phase
lock already times frames to arrive just before a latch (2.5 ms p50 of
margin, for 99.9% of frames to make it); the 10.3 ms is the floor.

- Live, per stream: SurfaceFlinger's debug transaction 1019 sets the duration
  of the last composite until the refresh (`frameworks/native`,
  `SurfaceFlinger::onTransact`, Android 15: `case 1019`,
  `mScheduler->setDuration(Cycle::LastComposite, n)`); 1018 the app's. It
  needs uid system or `android.permission.HARDWARE_TEST`: adb's shell is
  refused (`service call SurfaceFlinger 1020` -> "Operation not permitted",
  checked on the Odin 3). Root can call it: `service call SurfaceFlinger
  1019 i32 <ns>`, and the same with 10500000 to restore.
- How low: the composite must still finish in time. The video layer is on a
  display plane (`dumpsys SurfaceFlinger --hwclayers`: DEVICE, with the 90°
  rotation done inline), so SurfaceFlinger's own work is small; 4-5 ms is the
  first thing to try, measuring `latch->on` and dropped frames in the stats
  overlay, and checking the rest of the UI does not stutter.
- Only during a stream, restored the moment it ends (the service below does
  both, and restores on its own start in case it crashed).
- The scanout mode (Riptide: frames written into the buffer on screen,
  6.9 ms p50 host present -> transfer) does not go through the latch at all, so
  it does not need this; it is the MediaCodec path that does.

### 2. The GPU's clock

kgsl (`/sys/class/kgsl/kgsl-3d0`) runs the Adreno on a busy-time governor with
a 10 ms sample, and the GMU power-collapses the GPU after ~300 us idle;
decoding a stripe at a time reads as a light load. Without root the client
keeps it busy with a tiny kernel on a low-priority compute queue
(`jni/gpuwarm.cpp`): Riptide's decode went from 7.6 to 4.5 ms p50. With root:

- `min_pwrlevel` (or `devfreq/min_freq`) to a high level while streaming, and
  back after; `idle_timer` up, so the GPU does not slumber between frames.
- Then turn the filler off and measure: the decode should drop further (the
  filler only approximates a held clock) and power should fall.

### 3. The Wi-Fi driver's receive interrupts

From the driver source (qca-wifi-host-cmn, the SM8750 build config
`sun_gki_peach-v2_defconfig`): each receive ring interrupts on a 512 us timer
with no packet-count trigger, so the first packet after a quiet spell waits up
to ~0.5 ms and the rest of its burst with it. One ring (the latency-sensitive
ring, `LSR_DEST_RING`) interrupts per packet, but flows go there only when the
active WLM latency level has host flag bit 33, and the shipped flags for the
level the low-latency Wi-Fi lock asks for (ultralow) have no host bits. Both are
in the driver's ini (`/vendor/etc/wifi/<chip>/WCNSS_qcom_cfg.ini`; the Odin 3
has `peach`, `peach_v2`, `kiwi_v2`, `wcn7750` and `qca6750` directories: find
which one the loaded driver reads before overlaying), read when the driver
loads:

- `dp_int_timer_threshold_rx=8` and `dp_int_batch_threshold_rx=1`: an
  interrupt per packet, all traffic. Simplest; costs power whenever Wi-Fi is up.
- Or `wlm_latency_flags_ultralow` with `0x700000000` added: the latency-
  sensitive ring, no rx thread, no FISA aggregation, only while the
  low-latency lock is held (that is, only while streaming). Better.
- Both need the file overlaid (a module's mount) and the driver reloaded or the
  device rebooted; there is no runtime knob for the thresholds.
- Measure first: tools/wburst's receiver on the device (single datagrams at
  1 ms spacing, then bursts) should show a flat ~0.25 ms average extra
  delivery delay, whatever the burst size, if the timer is what it looks like.

### 4. The video device, directly

The open kernel driver for this SoC (`qualcomm-linux/video-driver`,
`platform/sun`) has slice decode for AV1 and HEVC (input buffers sharing a
timestamp are parts of one frame, each passed to the firmware as it is queued)
and early notify (a sync fence per N decoded rows). MediaCodec reaches neither
usefully. The decoder is `/dev/video32` (owner system, group camera, 0660), and
AOSP's policy forbids apps from opening video devices (`neverallow appdomain
video_device:chr_file { read write }`). With root:

- the service opens the device and hands the file descriptor to the app
  (SCM_RIGHTS), and a policy rule lets the app use it (ioctl, map);
- the app drives it as a stateful V4L2 decoder with Qualcomm's controls
  (low latency, decode order, software output fence, slice decode, early
  notify), buffers from dma-buf heaps;
- the host must send each AV1 frame's header first and its tile rows as
  separate tile-group OBUs (today it sends one tile group with the header
  last: gmstream-dev notes); HEVC slice segments are already NAL units.
- Unproven: that the closed firmware starts on the first tile group before the
  last has come. Test that before building on it.

### 5. Scheduling and interrupts

- The network, decode and present threads at SCHED_FIFO (an app cannot raise
  its own threads above SCHED_OTHER), pinned to the prime cores.
- The Wi-Fi interrupts (CE and REO rings, `/proc/interrupts`) moved to a core
  that is not running the game's work, and that core kept out of deep idle
  while streaming (`/dev/cpu_dma_latency` or the core's cpuidle states).
- These are for the tails; measure p99 before and after.

### Not worth it

- Owning the display outright (stopping SurfaceFlinger and committing to DRM
  ourselves, to race the scan): the scanout mode gets most of it within
  Android, and the panel is command mode (`1080x1920x120cmd`), so racing means
  racing the transfer into the panel's memory, not a scanout.
- A different panel mode: the panel offers 120 Hz and 60 Hz, both command mode.

## The service: gmstreamd

An independent component, installed separately (a Magisk or KernelSU module),
that the app uses when present. The app keeps working without it.

- **Install.** A module with a native daemon (`gmstreamd`, C++, root), started
  from the module's `service.sh`; `sepolicy.rule` allows `untrusted_app` to
  connect to its socket and use the video device fd it passes; an optional
  overlay of the Wi-Fi ini (item 3), off by default.
- **Socket.** An abstract Unix stream socket, `@gmstreamd`. Every peer is
  checked by SO_PEERCRED: its uid must be the uid of `org.gmstream` (from
  `/data/system/packages.list`) and the package must be signed with the key the
  module was told at install. Anything else is closed unread.
- **Requests** (length-prefixed messages, versioned):
  - `hello` -> what this device supports (each item above, found or not);
  - `stream-start {threads, sf_duration_ns, gpu_floor, irq_core}` -> applies
    what was asked of the supported items, remembers the old values;
  - `stream-stop` -> restores them;
  - `open-video` -> the decoder's fd, by SCM_RIGHTS.
- **Safety.** Every change is undone when the client's socket closes (the app
  crashed, was killed or the stream ended), and when the daemon starts (it may
  have crashed holding them). Nothing is changed outside a stream; nothing
  persists across a reboot but the module's optional ini overlay.
- **In the app.** On stream start it tries the socket; if the service is
  there it asks for what the settings allow ("Use the root service" per item)
  and shows in the stats what is in effect (`root: sf 4.0 ms, gpu floor,
  fifo`); if not, nothing changes.

## Order

1. SurfaceFlinger's duration (item 1): the largest gain for AV1/HEVC, one call,
   easy to undo; measure `latch->on` and drops.
2. The GPU floor (item 2), measured against the filler.
3. The Wi-Fi ini (item 3), after measuring the timer with wburst.
4. Scheduling (item 5), for p99.
5. The video device (item 4), last: it needs a host change and an unproven
   firmware behaviour.
