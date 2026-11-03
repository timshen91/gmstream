# gmstream protocol, version 3

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
- The web client is served only with `--web`, and has no authentication: anyone
  who can reach the port can connect (and take over from the current client).
  Version 1 and 2 clients sent a token after the Hello fields; the host ignores
  any such trailing bytes.

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
| 0x01 | Hello | `u16 version, u16 width, u16 height, u32 refresh_mHz, u32 bitrate_kbps, u8 codec_mask, u8 preferred_codec, u8 flags (version 2+), u16 max_datagram (version 3+)` |
| 0x02 | Configure | `u16 width, u16 height, u32 refresh_mHz` (window resize, fullscreen, monitor change) |
| 0x03 | KeyframeRequest | — |
| 0x04 | Bitrate | `u32 kbps` |
| 0x05 | Key | `u16 evdev_code, u8 pressed` |
| 0x06 | Button | `u16 evdev_code (BTN_LEFT=0x110 …), u8 pressed` |
| 0x07 | Motion | `f32 dx, f32 dy` relative, in stream pixels; only reaches a game while the pointer is locked |
| 0x08 | Position | `f32 x, f32 y` absolute, normalized 0..1 |
| 0x09 | Scroll | `f32 dx, f32 dy` (Wayland axis units, 15 ≈ one notch), `i16 v120_x, i16 v120_y` |
| 0x0a | Gamepad | `u8 index, u32 buttons, i16 lx, i16 ly, i16 rx, i16 ry, u8 lt, u8 rt` |
| 0x0b | GamepadRemove | `u8 index` |
| 0x0c | Ping | `u64 client_time_us` |
| 0x0e | VrrPolicy | `u8 policy`: what the client wants of Steam's VRR. 0 = leave Steam's toggle alone (the default); 1 = off: the host reports the client's grid as a fixed refresh, or the stream's rate with no grid, so gamescope calls the display VRR-incapable and Steam greys its toggle; 2 = on: the host sets gamescope's `adaptive_sync` (what Steam's toggle sets) and hands each frame to the encoder at once, stamped with gamescope's commit time (the client holds frames itself). Leaving 2, the host restores `adaptive_sync` from Steam's preference (`GAMESCOPE_VRR_ENABLED`). Cleared when a stream starts or stops. |
| 0x0f | Touch | `u8 id, u8 phase, f32 x, f32 y`: a touchscreen. Phase 0 = down, 1 = moved, 2 = up, 3 = cancel (every touch ends; id and position ignored); x, y normalized 0..1 over the video. Ids are the client's, reused after up. The host's gamescope applies its touch mode (SteamOS: passed through to Steam and the game as a touchscreen). |
| 0x0d | DisplayGrid | `u64 anchor_ns, u32 interval_ns`: the client display's refresh grid in host CLOCK_MONOTONIC (a vblank and the interval), shifted by the client to set where frames land. The host then reports gamescope's frames as presented on that grid (still as a VRR-capable display, so Steam's VRR toggle stays available; with it off, gamescope paces to the grid). Native clients send it every 500 ms; it is cleared when a stream starts, and by an interval of 0 (a client on a variable-refresh display, which shows frames as they arrive). |

`Hello` must be first. Hello flags: bit 0 = one video stream, bit 1 = video
datagrams (see Video; if both are set, datagrams win), bit 2 = video chunks
(AV1 only; see "Video chunks" below), bit 3 = the client can show HDR10 (PQ,
BT.2020, ten-bit). Bit 3 states what the client is able to display, not what
it wants: whether the session is HDR at all is the host's own setting (on
SteamOS, Steam's display setting, which drives gamescope). The host sends an
HDR stream only when both agree, and an eight-bit BT.709 one otherwise, so a
client that cannot show HDR is never made to convert a ten-bit stream it
has no use for.
`max_datagram`: the largest UDP payload the client's link carries unfragmented (its interface MTU minus the IP and UDP headers); 0 = unknown. Version 1 clients (no flags byte) and version 2 clients (no `max_datagram`) are still accepted. Codec ids: 0 = H.264, 1 = HEVC, 2 = AV1, 3 = PyroWave (see below); the mask has
bit `1 << id` set for each codec the client can decode. Gamepad buttons follow
the W3C standard mapping (bit i = `buttons[i]`, 0..16); stick y is positive
down; triggers are 0..255. Keys are not repeated by the client: gamescope does
key repeat itself.

### Host → client

| type | name | payload |
|---|---|---|
| 0x81 | HelloAck | `u8 codec` |
| 0x82 | Cursor | `u16 width, u16 height, i16 hotspot_x, i16 hotspot_y, RGBA (straight alpha)`; width = 0: hidden |
| 0x83 | Relative | `u8 relative` (the game wants motion: Mouse on Auto sends Motion, not Position) |
| 0x84 | Rumble | `u8 index, u16 strong, u16 weak, u16 duration_ms`; duration 0: stop |
| 0x85 | Pong | `u64 client_time_us (echoed), u64 host_time_ns (CLOCK_MONOTONIC)` |
| 0x86 | Error | UTF-8 text; the host closes the session after it |
| 0x88 | EncodeFormat | UTF-8 text: what the encoder is taking in and what it makes of it (e.g. `XB30 10-bit BT.2020, no copy → AV1 10-bit PQ`), sent whenever it changes. For the client's stats line only; the wording is not part of the protocol. |
| 0x87 | CursorClip | `u16 left, top, right, bottom` in stream pixels: where the game keeps the cursor, while it has the mouse; all 0: nowhere in particular |

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

## Video chunks

With the Hello flag *video chunks* and the AV1 codec, the host does not wait for
a frame to finish encoding: it sends each piece of the bitstream as the GPU
writes it, so most of a frame is on the wire before the encoder is done with it
(core/vastream.c, DESIGN.md §12). The bytes are the encoder's own, byte for
byte; a frame put back together from its pieces is exactly what a frame sent
whole would have been. Any other codec, and the first frame of a session, still
come whole.

```
u8  type = 9
u32 frame_id
u16 index          0, 1, 2 … in the order the host sent them
u16 count          how many pieces the frame has; 0 except in the last
u32 offset         where these bytes go in the frame's bitstream
u32 total          the bitstream's size; 0 except in the last
u8  flags          bit 0: keyframe (only meaningful in the last piece);
                   bit 1: this is the frame's last piece
u8  codec
u16 width
u16 height
u64 slot_ns        host CLOCK_MONOTONIC time the frame was presented
u64 encoded_ns     the bitstream was ready; 0 except in the last piece
u32 overlap_ns     how long before that the frame's first piece was sent, so
                   the client can start the network's share there instead of
                   at the end of the encode; 0 except in the last piece
... the frame's bytes at `offset`
```

The client keeps a `total`-byte buffer and lays each piece at its `offset`, and
has the whole frame once it has all `count` of them. Pieces are laid down in
**index order**: the GPU goes back and changes a few bytes it has already
written (the frame header, and each tile group's OBU header and size), and the
host sends those again in later pieces, which must win over the earlier ones.
A piece may run past `total`, because the GPU writes in blocks; those bytes are
padding and are dropped.

Only the last piece knows `count`, `total`, `encoded_ns`, `overlap_ns` and
whether the frame
turned out to be a keyframe, so losing it loses the frame, as losing any other
piece does. Nothing is retransmitted; the usual gap handling applies.

A frame's pieces are at most `max_datagram` minus this 43-byte header.

## PyroWave

Codec 3 is [PyroWave](https://github.com/Themaister/pyrowave): an intra-only GPU
wavelet codec whose rate control caps every frame at bitrate / refresh rate
bytes, so each frame's transmit time is bounded. YCbCr 4:2:0, BT.709 full
range. Its frames always go as datagrams, whatever the Hello flags, one
PyroWave packet per datagram. A packet is at most 1100 bytes, except over
plain UDP from a version 3 client: then up to `max_datagram` (clamped to the
host's own route MTU) minus the 29-byte header. Each datagram costs about
the same whatever its size on some links (~40 µs over a Mac's Thunderbolt
bridge), so with a ~64 KB MTU a frame arrives in ~0.5 ms instead of ~8.

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
retransmission: it assumes a clean LAN. The control stream is encrypted and the
client must be authorized; video and audio are plain. Every datagram starts with a
type byte:

| type | contents |
|---|---|
| 1 | audio, as above |
| 2 | video fragment, as in datagram mode above (always used over plain UDP; the Hello flags are ignored) |
| 3 | control: `u32 sequence` then bytes of the control stream, sealed (below) |
| 5 | keep-awake (client → host): no payload; the host drops it |
| 6 | handshake (below) |
| 7 | mouse (client → host): `u32 sequence` then a sealed payload (below) |
| 8 | acknowledgement (host → client): `u32 sequence` then a sealed `u32` |
| 9 | a piece of a frame, as in "Video chunks" above |

A session starts with a handshake. Client → host: type 6, an ephemeral X25519
public key, then zeros up to 129 bytes in all (so a forged request gets back no
more than it sent), repeated until answered. Host → client: type 6, its ephemeral
key (32), its long-term Ed25519 public key (32), and its signature (64) over
"gmstream host" + T, where T is the SHA-256 of "gmstream-udp-1", the client's
ephemeral key, the host's ephemeral key and the host's long-term key. The host
answers a repeat with the same reply. The client remembers the host's long-term key
and refuses a different one, as ssh does.

Both sides take HKDF-SHA256 of the X25519 secret with T as salt: 32 bytes with info
"c2h" and 32 with info "h2c", AES-256-GCM keys for each direction. A control
datagram is sealed with its 5-byte header as additional data and 8 zero bytes then
the big-endian sequence number as nonce, the 16-byte tag last, so its payload is
at most 1379 bytes. A datagram that doesn't open is dropped.

The nonce's first byte says what the number counts, each with its own numbers: 0
control, 1 mouse, 2 acknowledgement.

Mouse datagrams are not ordered, nor resent: a client sends one per mouse event
and the host ignores any older than the newest it has seen. A payload of `u8 0, f64
x, f64 y` says how far the mouse has moved since the session began, in stream
pixels, so a lost datagram is contained in the next. A payload of `u8 1, f32 x, f32
y` is its position, normalized over the video. The client sends the last one a
second time, as nothing may follow it. They count from when the host has accepted
`Auth`. `Motion` and `Position` still work on the control stream; a client sends
`Position` there just before a button, which is ordered.

The host answers every control datagram with type 8, whose sealed value is the next
control sequence it wants. A client sends again whatever isn't acknowledged within
30 ms.

The first control message is `Auth` (0x10): the client's Ed25519 public key (32)
and its signature (64) over "gmstream client" + T. That key must be in the host
user's `~/.ssh/authorized_keys` as `ssh-ed25519`, with or without options; if it
isn't, or the signature is wrong, the host sends `Error` and ends the session.
`Hello` follows. A client's line uses
`restrict,command="/bin/false"`, so ssh itself gives that key no shell.

The control stream (the same messages as over WebTransport, both
directions) is cut into type-3 datagrams numbered from 0 in each direction;
the receiver puts them back in order and reads messages from the bytes. A
control datagram never carries more than 1400 bytes, so nothing is fragmented
by IP; PyroWave datagrams follow `max_datagram` (above).

A client is identified by its address and port. Its control datagram 0
(which starts with `Auth`), sealed with the handshake's keys, begins a session; a
later session from another address replaces it, as over WebTransport. The host ends a session after
10 s without a control datagram from the client, so clients send `Ping`
every 500 ms. A lost client → host control datagram is sent again; one lost the other way
stalls the stream behind it, and once 256 later ones are waiting the client
reconnects.

A client on Wi-Fi should send keep-awake datagrams at about 60 Hz. A client
that only receives lets its Wi-Fi enter power save during the stream (seen
on a MacBook's Broadcom chip about 10 times a minute); the access point then
holds its packets until the next beacon, up to ~102 ms, and drops what it
can't hold. 10 Hz was not enough; 60 Hz stopped every stall.
