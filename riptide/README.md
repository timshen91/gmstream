# Riptide

Riptide is a video codec for game streaming. Its one goal is the shortest
time from a frame appearing on the host to that frame being ready on the
client. It is made so that the three stages, encoding, the network and
decoding, run at the same time on different parts of one frame. The total is
then about as long as the longest of the three, normally the network.

Riptide is a GPU wavelet codec. It grew out of
[PyroWave](https://github.com/Themaister/pyrowave) (Hans-Kristian Arntzen, MIT;
see LICENSE), an intra-only wavelet codec with exact per-frame size control,
and still uses PyroWave's wavelet transform, quantizer, rate control and block
packing. It adds three things on top, and those three make it a different
codec:

1. **Prediction from the last frame.** Every frame is coded as its difference
   from a motion-compensated prediction from the previous decoded frame. There
   are no keyframes.
2. **Stripes.** A frame is horizontal bands, each coded as an image of its
   own, so each can leave the host, cross the network and be decoded while the
   next is still being worked on.
3. **Losses repaired by replay, not keyframes.** The client says which
   packets never arrived. The host replays the client's decode with exactly
   those missing, and predicts the next frame from what the client actually
   has.

It runs in [gmstream](..) between a SteamOS host (Vulkan, AMD) and Mac and
iPad clients (Metal).

## What it achieved

Measured on the host's test instance at 2560x1600, 120 Hz, over the host's
own Wi-Fi hotspot to an M4 Mac. Times are from gamescope presenting the frame
to the client holding it decoded (p50, with p99 in brackets):

| | PyroWave, whole frames | Riptide |
|---|---|---|
| light GPU load, 100 Mb/s | 11.6 ms (the inter frames before stripes) | 5.9 (6.9) |
| light GPU load, 300 Mb/s | | 6.6 (9.1) |
| GPU at 100% (a busy game), 100 Mb/s | 11.6 (16.8) | 7.7 (10.2) |
| GPU at 100%, 300 Mb/s | 14-14.7 | 8.4-8.6 (~10) |

With a light load the host sends the first stripe 0.4 ms after the frame
starts and finishes the frame at 1.6 ms. The client has the frame decoded
0.5 ms after its last packet. What is left is mostly the network: about 2 ms of
one-way Wi-Fi delay plus the frame's bytes on the air.

Compression: on the five AOM gaming test clips (Dota 2, Euro Truck 2,
Minecraft, StarCraft II, Witcher 3) prediction needs 37% fewer bits than
PyroWave for the same PSNR-Y (Dota -70%, StarCraft -63%, Euro Truck -44%,
Witcher -7%, Minecraft -1%). Stripes take back very little of that: 384-row
stripes cost 0.01 dB PSNR-Y on Euro Truck at 1080p.

## How it works

### The constraint: only GPU-shaped work

Every stage is a fixed amount of parallel work per pixel or per block, with
nothing serial and nothing adaptive. There is no arithmetic coding, no
search tree, and no decision that waits for another. This rule is why the
encoder and the decoder each take a millisecond or two, and why both can be
cut into stripes without anything carrying over between them. Ideas that broke
the rule were measured and left out (`HISTORY.md` has every step and its
numbers).

### Prediction

- **Motion.** One vector per 16x16 luma block, in quarter pels, from six passes
  of 3-D recursive search (steps of 8, 4, 2, 1, 1/2 and 1/4 pixels). In each
  pass, every block picks the best of nine candidates at once: its own
  vector, zero, last frame's, its four neighbours', and two nudges. That is
  one GPU thread per (block, candidate), with the source read once into
  shared memory. Vectors are interpolated bilinearly between block centres, so
  the prediction has no block edges.
- **Decision.** Per 32x32 block, predict, or code it intra (the DC shift),
  whichever leaves less to code. The weights are blended bilinearly between
  blocks.
- **Exact arithmetic.** The prediction is computed in integers: vectors at
  1/64 pel, weights on a 0-256 scale, rounding by shifts. So the AMD encoder and
  the Apple decoder form exactly the same prediction (`shaders/inter_predict.h`).
  The remaining difference between encoder and decoder is the wavelet's float
  rounding: 0.01% of luma samples, and it does not grow, because each frame's
  residual corrects it.
- **Side information.** The map and the vectors go in inter packets, coded as
  deltas from last frame's vectors per 64x64 tile (`bitstream/inter.md`). The
  encoder charges their exact size to the frame's budget on the GPU, so the
  frame still comes out exactly the size the rate control was given.

### Stripes

The frame is cut into horizontal stripes: a first one of 128 rows (so the
frame starts out early), then 384-row stripes, the last taking the rest. All
boundaries fall on 64 rows, so every grid lines up. Each stripe is coded as an
image of its own: its own wavelet transform, rate control, packets and inter
packets. But it predicts from anywhere in the whole previous frame, because the
inter shaders take the stripe's row in the reference.

- **Host:** the stripes are GPU submissions one after another, each with its
  own fence. `riptide_encoder_packetize_stripe` waits only for its own stripe,
  and the stripe goes out at once. `riptide_encoder_decode_stripe` then queues
  the encoder's own decode of that stripe behind the frame: that decode is the
  next frame's reference.
- **Client:** each stripe has a decoder. Once a stripe's inter packets are in,
  its prediction is formed while its coefficients are still arriving. Once its
  last packet is in, it is decoded straight into its rows of the frame
  (`riptide_decoder_decode_gpu_buffer_rows`).
- **Rate:** each stripe gets the share of the frame's bytes that the rate
  control would have given it across the whole frame. That share comes from
  the last frame's rate statistics: the same distortion-per-byte cut in every
  stripe, with at least a quarter of each stripe's share by area kept for
  content that changes. The frame's total stays exact.

Why horizontal stripes, and not cutting one transform up: a whole-frame
wavelet cannot be decoded until every packet is in, because its coarsest level
spans the frame. Every stage of the encoder also finishes the whole frame
before the next starts. A frame coded whole therefore overlaps nothing.

### Keeping both GPUs awake

Encoding and decoding a stripe at a time are short bursts, and both GPUs'
power management reads that as a light load and lowers the clock. On the M4
the same stripe decoded 4.4x slower while streaming than back to back. On the
host's Radeon the first stripe came out after 1.04 ms instead of 0.45. Neither
clock can be set from an ordinary process. So each end submits a tiny kernel
back to back on a low-priority queue of its own while frames flow, and stops a
second after the last one. The GPU is never idle in its power management's
eyes, the filler does almost nothing, and the codec's work goes first. Any gap
in the filler, even 0.5 ms, lets the clock fall again. This is gmstream's
`core/gpuwarm.c` and `apple/GPUWarm.swift`.

### Losses

Every frame of a stream is decoded, in order, with whatever of it arrived. A
missing coefficient block is zero. A tile whose inter packets are missing keeps
its previous vectors and predicts. A frame of which nothing came is decoded
from nothing. These rules are exact, so the host can repeat them. The client
reports what it missed, and keeps repeating the report until a frame says the
host took it in. The host keeps its last 8 frames, re-decodes them as the
client did, and predicts the next frame from that. A report older than that, or
a client with nothing to predict from, gets an intra frame.

## Source

- `riptide.h`, `riptide_c.cpp`: the C API (Vulkan). The stripes, the own
  decode and replay, and the rate split live here.
- `riptide_encoder.*`, `riptide_decoder.*`, `riptide_common.*`: the Vulkan
  encoder and decoder: PyroWave's, with the inter passes.
- `riptide_inter.*`: the inter packets (layout, parsing, the loss rule), shared
  with Metal.
- `shaders/`: GLSL. `inter_*` are the prediction passes, `copy_rows.comp`
  cuts stripes out and puts decodes back, and the rest is PyroWave's.
  `shaders/slangmosh.hpp` is the compiled Vulkan table and `metal/shaders/` the
  Metal translation (regenerate both with `./shaders.sh`).
- `metal/`: the Metal encoder and decoder. The decoder is the client's; the
  encoder is for measuring on a Mac.
- `bitstream/bitstream.md` (PyroWave's v1, unchanged) and `bitstream/inter.md`
  (the inter packets).
- `tools/`: `rtenc.c` encodes a clip through the Vulkan C API as the host does;
  `rtdec.mm` decodes its output with Metal and compares it with the encoder's
  own decode; `rtrd.mm` and `bench.py` measure rate against distortion on the
  test clips (`RT_CLIPS`), speed, and loss.
- `HISTORY.md`: how it was built, one change at a time, each one measured.

## Building

The host: gmstream's `cargo build` runs `../build/riptide.sh`, which checks out
Granite (`checkout_granite.sh`, the same commit as PyroWave's) and builds
`libriptide.a` and `libriptide-c.a` with CMake. The clients compile `metal/`
directly (`../mac/build.sh`, `../ipad/project.yml`). Riptide's C and C++
symbols are its own (`riptide_*`, namespace `Riptide`), so it links next to
stock PyroWave.

## Using and tuning it

- **Choose it:** Riptide is its own codec (Settings, Codec: Riptide;
  `--riptide`). It needs plain UDP, where the client's loss reports travel.
- **Stripes per frame:** Settings, Riptide stripes (`--stripes N`; Auto is 128
  rows then 384). More stripes go out sooner, but each costs bits and a fixed
  amount of GPU work on both ends. Under a GPU-bound game, fewer stripes can be
  better. At 2560x1600, 120 Hz: 384-row stripes measured best or close to it
  under light and heavy load at 100 and 300 Mb/s. 256 rows were up to 0.6 ms
  slower; one stripe was 1-1.6 ms slower.
- **Keep the GPU awake** (Settings, on by default): trades some power for
  decode speed. The host's filler is always on while Riptide encodes;
  `GM_RIPTIDE_GPU_WARM=0` turns it off. Host layout overrides for experiments:
  `GM_RIPTIDE_STRIPE_ROWS`, `GM_RIPTIDE_FIRST_ROWS`.

## Limits and what is next

- **A GPU-bound game slows the host's encode** (5 stripes take ~4.4 ms
  instead of ~1.6): every pass of every stripe waits for its share of a busy
  GPU. None of these changed it: one submission for all stripes, polling a
  completion mark instead of the fence, fewer barriers, batching the per-band
  dispatches, two compute queues. Untried: running the motion search over the
  whole frame at once, before the stripes' second halves.
- **The hotspot carries a frame's stripes at about 280 Mb/s,** against ~740 for
  one large burst. Small bursts aggregate worse on the air. The hotspot's own
  aggregation settings are untried.
- **Sharp static detail is the wavelet's weak spot** (Minecraft, StarCraft gain
  least).
