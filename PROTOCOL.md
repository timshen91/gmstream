# gmstream protocol, version 2

Two transports carry the same messages: WebTransport (HTTP/3 over QUIC, for
browsers) and plain UDP (unencrypted, for native clients; see the last
section). Over WebTransport: one bidirectional stream, one unidirectional
stream per video frame (or one for all frames), datagrams.

All integers are little-endian. `f32` is IEEE 754 single precision.

## Discovery and security

- The host serves the web client over HTTPS (TCP, default port 8443) and the
  WebTransport endpoint on the same port number over UDP. Plain UDP is on the
  next port (default 8444).
- `GET /config.json` → `{"port": 8443, "certHash": [32 bytes]}`: the SHA-256
  of the WebTransport certificate, for `serverCertificateHashes`. That
  certificate is self-signed, valid for 13 days, and rotated weekly.
- There is no authentication: anyone who can reach the port can connect
  (and take over from the current client). Version 1 and 2 clients sent a
  token after the Hello fields; the host ignores any such trailing bytes.

## Control stream

The client opens one bidirectional stream right after the session is ready.
Every message on it, in both directions:

```
u8  type
u32 length    (of the payload)
... payload
```

Unknown types are skipped.

### Client → host

| type | name | payload |
|---|---|---|
| 0x01 | Hello | `u16 version, u16 width, u16 height, u32 refresh_mHz, u32 bitrate_kbps, u8 codec_mask, u8 preferred_codec, u8 flags (version 2+)` |
| 0x02 | Configure | `u16 width, u16 height, u32 refresh_mHz` (window resize, fullscreen, monitor change) |
| 0x03 | KeyframeRequest | — |
| 0x04 | Bitrate | `u32 kbps` |
| 0x05 | Key | `u16 evdev_code, u8 pressed` |
| 0x06 | Button | `u16 evdev_code (BTN_LEFT=0x110 …), u8 pressed` |
| 0x07 | Motion | `f32 dx, f32 dy` relative, in stream pixels (pointer locked) |
| 0x08 | Position | `f32 x, f32 y` absolute, normalized 0..1 |
| 0x09 | Scroll | `f32 dx, f32 dy` (Wayland axis units, 15 ≈ one notch), `i16 v120_x, i16 v120_y` |
| 0x0a | Gamepad | `u8 index, u32 buttons, i16 lx, i16 ly, i16 rx, i16 ry, u8 lt, u8 rt` |
| 0x0b | GamepadRemove | `u8 index` |
| 0x0c | Ping | `u64 client_time_us` |
| 0x0d | DisplayGrid | `u64 anchor_ns, u32 interval_ns`: the client display's refresh grid in host CLOCK_MONOTONIC (a vblank and the interval), shifted by the client to set where frames land. The host then reports gamescope's frames as presented on that grid (still as a VRR-capable display, so Steam's VRR toggle stays available; with it off, gamescope paces to the grid). Native clients send it every 500 ms; it is cleared when a stream starts, and by an interval of 0 (a client on a variable-refresh display, which shows frames as they arrive). |

`Hello` must be first. Hello flags: bit 0 = one video stream, bit 1 = video
datagrams (see Video; if both are set, datagrams win).
Version 1 clients (no flags byte) are still accepted. Codec ids: 0 = H.264, 1 = HEVC, 2 = AV1, 3 = PyroWave (see below); the mask has
bit `1 << id` set for each codec the client can decode. Gamepad buttons follow
the W3C standard mapping (bit i = `buttons[i]`, 0..16); stick y is positive
down; triggers are 0..255. Keys are not repeated by the client: gamescope does
key repeat itself.

### Host → client

| type | name | payload |
|---|---|---|
| 0x81 | HelloAck | `u8 codec` |
| 0x82 | Cursor | `u16 width, u16 height, i16 hotspot_x, i16 hotspot_y, RGBA (straight alpha)`; width = 0: hidden |
| 0x83 | PointerLock | `u8 locked` (the game wants relative input; use Pointer Lock) |
| 0x84 | Rumble | `u8 index, u16 strong, u16 weak, u16 duration_ms`; duration 0: stop |
| 0x85 | Pong | `u64 client_time_us (echoed), u64 host_time_ns (CLOCK_MONOTONIC)` |
| 0x86 | Error | UTF-8 text; the host closes the session after it |

The cursor is drawn by the client (gamescope gives it to its host as a
separate surface, not part of the frame), so it moves without stream latency.

## Video

By default, one host-opened unidirectional stream per encoded frame:

```
u32 frame_id      consecutive across encoder restarts
u8  flags         bit 0: keyframe
u8  codec
u16 width
u16 height
u16 reserved
u64 slot_ns       host CLOCK_MONOTONIC time the frame was presented
u64 encoded_ns    host CLOCK_MONOTONIC time the bitstream was ready
u32 reserved
... bitstream     H.264/HEVC Annex B with in-band parameter sets on
                  keyframes; AV1 OBUs (sequence header on keyframes)
```

Newer frames get higher stream priority. The host abandons (resets) a frame
it could not deliver within 250 ms.

With the Hello flag *one video stream*, the host opens a single
unidirectional stream at the start and writes every frame to it as `u32
length` followed by the frame (header and bitstream as above). Nothing is
reset; when the network falls behind, the host drops frames before sending
them, so the client sees a `frame_id` gap. Web clients use this on WebKit
(Safari, every iPad browser), which grants new streams too slowly for 60 fps
and stops granting them after the host resets one.

With the Hello flag *video datagrams*, each frame (header and bitstream as
above) is split into datagrams no larger than the connection allows:

```
u8  type = 2
u32 frame_id
u16 fragment_index
u16 fragment_count
... the frame's bytes from fragment_index * size, in order
```

Nothing is retransmitted. The client reassembles a frame when it has every
fragment; a frame missing a fragment never completes, and the usual gap
handling (below) skips to a keyframe.

The client decodes frames in `frame_id` order; when a frame is missing and a later one has arrived, it waits 30 ms,
then drops to the next keyframe and sends `KeyframeRequest`. A size or codec
change always starts with a keyframe.

## PyroWave

Codec 3 is [PyroWave](https://github.com/Themaister/pyrowave): an intra-only GPU
wavelet codec whose rate control caps every frame at bitrate / refresh rate
bytes, so each frame's transmit time is bounded. YCbCr 4:2:0, BT.709 full
range. Its frames always go as datagrams, whatever the Hello flags, one
PyroWave packet (at most 1100 bytes) per datagram:

```
u8  type = 4
u32 frame_id
u16 packet_index
u16 packet_count
u16 width
u16 height
u64 slot_ns
u64 encoded_ns
... one PyroWave packet (its bitstream format: bitstream/bitstream.md upstream)
```

Packets decode independently: a lost one only blurs its part of the frame.
The client decodes once it has `packet_count` packets of a frame; a frame
still incomplete when a newer one starts is skipped. Every frame stands
alone, so there are no keyframe requests.

## Audio

Datagrams (type 1; type 2 is video in datagram mode, 4 is PyroWave):

```
u8  type = 1
u32 sequence
u64 pts_ns        host CLOCK_MONOTONIC capture time of the first sample
... Opus packet   48 kHz stereo, 5 ms
```

## Clock

`Pong` gives the host clock; the client keeps the offset from the
lowest-RTT exchange and uses it to show capture→display latency.

## Plain UDP

For native clients, on UDP port 8444 (the WebTransport port + 1). No
encryption, no authentication, no retransmission: it assumes a clean LAN.
Every datagram starts with a type byte:

| type | contents |
|---|---|
| 1 | audio, as above |
| 2 | video fragment, as in datagram mode above (always used over plain UDP; the Hello flags are ignored) |
| 3 | control: `u32 sequence` then bytes of the control stream |

The control stream (the same messages as over WebTransport, both
directions) is cut into type-3 datagrams numbered from 0 in each direction;
the receiver puts them back in order and reads messages from the bytes. A
datagram never carries more than 1400 bytes, so nothing is fragmented by IP.

A client is identified by its address and port. Its control datagram 0
(which starts with `Hello`) begins a session; a later session from another
address replaces it, as over WebTransport. The host ends a session after
10 s without a control datagram from the client, so clients send `Ping`
every 500 ms. A lost control datagram stalls the stream behind it; once 256
later ones are waiting, the host ends the session and the client reconnects.
