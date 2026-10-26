# gmstream protocol, version 1

One WebTransport session per client (HTTP/3 over QUIC, UDP). The same layout
maps onto raw QUIC for future native clients: one bidirectional stream, one
unidirectional stream per video frame, datagrams.

All integers are little-endian. `f32` is IEEE 754 single precision.

## Discovery and security

- The host serves the web client over HTTPS (TCP, default port 8443) and the
  WebTransport endpoint on the same port number over UDP.
- `GET /config.json` → `{"port": 8443, "certHash": [32 bytes]}`: the SHA-256
  of the WebTransport certificate, for `serverCertificateHashes`. That
  certificate is self-signed, valid for 13 days, and rotated weekly.
- The client authenticates with a shared token (the URL fragment the host
  prints; `~/.local/state/gmstream/token`), sent in `Hello`.

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
| 0x01 | Hello | `u16 version, u16 width, u16 height, u32 refresh_mHz, u32 bitrate_kbps, u8 codec_mask, u8 preferred_codec, token bytes (rest)` |
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

`Hello` must be first. Codec ids: 0 = H.264, 1 = HEVC, 2 = AV1; the mask has
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

One host-opened unidirectional stream per encoded frame:

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
it could not deliver within 250 ms. The client decodes frames in `frame_id`
order; when a frame is missing and a later one has arrived, it waits 30 ms,
then drops to the next keyframe and sends `KeyframeRequest`. A size or codec
change always starts with a keyframe.

## Audio

Datagrams:

```
u8  type = 1
u32 sequence
u64 pts_ns        host CLOCK_MONOTONIC capture time of the first sample
... Opus packet   48 kHz stereo, 5 ms
```

## Clock

`Pong` gives the host clock; the client keeps the offset from the
lowest-RTT exchange and uses it to show capture→display latency.
