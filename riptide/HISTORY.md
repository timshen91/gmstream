# Inter-frame PyroWave (branch `inter`)

Stock PyroWave plus prediction from the previous decoded frame, one change at a
time, each measured before the next. The rule for every change: a fixed amount
of parallel work per pixel or block on the GPU, nothing serial or adaptive.

## Benchmark

- Quality: `tools/bench.py quality TAG --bin build/pwrd.TAG -- ARGS`. Five AOM
  CTC 1080p gaming clips (Dota 2, Euro Truck 2, Minecraft, StarCraft II, Witcher
  3), the first 60 consecutive frames, frames 0-9 left out (the intra start),
  at 20 / 40 / 75 / 150 / 300 KB per frame (9.6-144 Mb/s at 60 fps; PyroWave's
  exact rate control). PSNR-Y, YUV-PSNR 6:1:1, PSNR-HVS-M at 2 screen heights
  (the metric PyroWave was tuned with).
- `tools/bench.py bd stock TAG`: BD-rate against stock PyroWave on the same
  frames (negative: fewer bits for the same quality).
- Speed: `tools/bench.py speed build/pwrd.A:ARGS build/pwrd.B:ARGS`: encode and
  decode GPU time per 1080p frame at 75 KB on the M4, back to back, the builds
  alternated over rounds (the GPU clock drifts), medians.
- Side information the bitstream does not carry yet is counted in the frame's
  bytes and taken off the coefficients' budget.
- Loss: `tools/bench.py loss BIN:ARGS ...`. A second decoder, the client,
  misses 10% of frame 20's packets (1400-byte packets, a contiguous burst at
  5%, 35% or 65% of the frame) and carries on from its own decoded frames.
  Reported: the client's PSNR-Y loss against the same run without loss, on
  that frame, summed over the 30 frames from it, and on frames +1/+10/+29.
  `PW_REPORT=d`: the client's loss report reaches the encoder d frames later.
- Each measured build is kept as `build/pwrd.TAG`.

## Steps

| step | change | PSNR-Y | YUV | HVS-M | encode / decode GPU ms |
|---|---|---|---|---|---|
| 0 | stock PyroWave, intra only (`stock`) | 0 | 0 | 0 | 1.44 / 0.253 |
| 1 | every frame predicted from the last decoded one, no motion (`s1diff`) | +4.0% | +6.9% | -2.7% | 1.26 / 0.313 |
| 2 | per 32x32 block: predict or intra, weights bilinear between blocks (`s2mode`) | **-29.2%** | -27.4% | -29.7% | 1.62 / 0.353 |
| 3a | prediction as its own pass writing 8-bit planes (refactor) | ~0 | | | 0.97 / 0.389 (step 2 alongside: 1.00 / 0.350) |
| 3b | motion: 16x16 vectors by 3-D recursive search, bilinear vector field, bilinear sampling (`s3b_l0.02`) | **-35.3%** | -35.2% | -36.1% | 3.06 / 0.571 (step 2 alongside: 0.97 / 0.355; stock 0.81 / 0.250) |
| 3c | vectors fetched once per 2x2 (prediction) / per 8 pixels (decision) | 0 (identical output) | | | 2.95 / 0.509 (3b alongside: 3.07 / 0.572) |
| 3d | cheaper search (rejected): 16 samples / 4 passes / both, against 3b | +24.3% / +5.7% / +28.3% | | | 1.64 / 2.43 / 1.38 encode (3c: 2.95) |
| 3e | zero-motion bias 0.1 in the search (`s3e_z0.1`) | **-36.9%** | -36.6% | -37.7% | unchanged |
| 3f | bicubic (Catmull-Rom) reference sampling (rejected, 2 clips) | worse | | | +3 gathers per pixel |
| 4 | vectors and map charged to the frame's budget on the GPU (`s4side`) | -36.5% | -36.5% | -37.4% | 2.657 / 0.504 (3e alongside: 2.657 / 0.504) |
| 5a | leaky state: prediction 0.5 + a w (ref - 0.5), a = 0.99 / 0.97 (rejected), against 4 | +4.3% / +8.1% | | | unchanged |
| 5b | rotating intra refresh, every block once per 60 / 30 frames (rejected), against 4 | +5.6% / +10.5% | | | unchanged |
| 5c | loss report: the encoder replays the client's decode (simulated in pwrd; no bits) | 0 | | | one decode per loss report |
| 6 | search: one thread per (block, candidate), hardware bilinear (`s6search`) | -36.5% (0.0% against 4) | -36.5% | -37.4% | 1.759 / 0.506 (5 alongside: 2.663 / 0.507) |
| 7 | prediction in exact integer arithmetic, shared by encoder and decoder (`s7int`) | -36.4% (+0.1% against 6) | -36.4% | -37.4% | 2.066 / 0.526 (6 alongside: 1.996 / 0.507) |
| 8 | search reads each block's source once into shared memory | 0 (identical output) | | | 1.730 / 0.531 (7 alongside: 1.872 / 0.529) |
| 9 | prediction pass, one thread per 4x4 / 4x2 / 2x1 (rejected) | 0 (identical output) | | | decode 0.568 / 0.575 / 0.599 (8: 0.529) |
| 10 | search pulled toward last frame's vector (rejected; 0.01-0.2 SAD units per quarter pel) | -2.4 to +0.9 dB per clip and rate | | | |
| 11 | vectors under intra blocks set to last frame's (free to send) (`s11free`) | **-37.0%** (-0.6% against 7) | -36.9% | -37.9% | one more barrier |
| 12 | per 4x4 tile: keep the vectors only if worth their bits at the rate's price (off; PW_VRD=0.5 / 1 / 2) | -0.1% / 0.0% / +0.3% against 11 | | | two small passes |
| 13 | measured: drift when the client's float arithmetic differs (no change) | | | | |
| 15 | Vulkan: encoder (front / back), decoder, C API with own decode and replay | AMD encode -> Metal decode: 29.46 dB mean on Euro Truck at 75 KB, within 0.01% of the encoder's own decode | | | |
| 14 | the side information in the bitstream (`bitstream/inter.md`) | 26.536 / 29.585 dB at 20 / 75 KB on Euro Truck (11: 26.551 / 29.585) | | | |

## Notes per step

Speed: encode GPU (the "encode compute" pass) / decode GPU (best of 200),
ms, 1080p at 75 KB on the M4. GMStream was running and sharing the GPU during
these runs, so compare within one run (the same row of runs), not across
steps measured at different times: steps 0-2 were one run, 3a another.

**Step 1** (frame differencing): per clip PSNR-Y Dota -65%, StarCraft -56%
(mostly static), Euro Truck +6%, Witcher +56%, Minecraft +79% (moving: the
difference of two shifted images carries more than the image). Decode +0.06 ms
(one more texel read per output pixel).

**Step 2** (per block mode): the encoder's decide pass compares, per 32x32
luma block, the absolute residual sum with the block's absolute deviation from
its mean, and predicts where the residual is no larger. 1 bit per block (255
bytes at 1080p), counted. Per clip PSNR-Y: Dota -69.5%, StarCraft -60.9%,
Euro Truck -8.4%, Witcher -5.0%, Minecraft -2.2%: never worse than intra now.

**Step 3a**: the prediction is computed once per pixel in its own pass
(inter_predict.comp, one thread per 2x2 so the weight corners are fetched once)
and stored as 8 bits; the transforms subtract and add it with one gather, the
way intra subtracts 0.5. Results within +-0.02 dB of step 2 (the 8-bit
rounding). One pixel per thread cost 0.446 ms of decode; 2x2 brought it to
0.389. Motion compensation goes into this pass.

**Step 3b** (motion): `inter_search.comp`, six passes of 3-D recursive
search on 16x16 luma blocks (one threadgroup per block, every block updated at
once from the last pass's field): candidates are the current vector, zero,
last frame's vector, the four neighbours' and two nudges halving from 8 px to
1/4 px; cost is SAD over a 2:1 subsampled block plus lambda times the L1
distance to the neighbours' mean. The prediction pass interpolates the vectors
bilinearly between block centres and samples the reference bilinearly; the
mode decision now uses the motion compensated reference. Vectors are counted
as 4-bit widths per 4x4 vectors and component plus zigzag deltas from last
frame's vector (not a bitstream yet).
- lambda 0.02 / 0.05 / 0.2: mean PSNR-Y -35.3 / -33.4 / -30.2%; lambda 0.01
  made the field noisy (11 KB of vectors a frame on Witcher at 20 KB). 0.02 is
  the default.
- Per clip at 0.02 (step 2 in brackets): Euro Truck -43.0% (-8.4), Dota -69.0
  (-69.5), StarCraft -58.9 (-60.9), Witcher -2.6 (-5.0), Minecraft -3.2 (-2.2).
  Driving gains a lot; the static clips and Witcher lose a little to the
  vectors' cost.
- A synthetic pan (one Witcher frame moved 4, 2 px per frame): +5.1 dB at 75 KB
  over step 3a, so the search finds real translation; Witcher's own motion
  (parallax, foliage) is what it does not capture.
- Cost: encode GPU 0.97 -> 3.06 ms, decode 0.355 -> 0.571 ms. Needs work
  before more is added.

**Step 3c**: an even aligned 2x2 never straddles a vector cell, so the
prediction pass fetches the four corner vectors once per 2x2 and only
interpolates per pixel; the decision pass does the same per 8 pixels. Output
identical. Profile at 3b: each search pass ~0.29 ms (6 passes 3.02 ms, 3
passes 2.04, 1 pass 1.57 encode compute); decode's prediction pass ~0.33 ms of
the 0.57.

**Step 3d** (rejected): 16 samples per block (4:1 subsampled, four blocks per
threadgroup, shuffle reductions) or 4 passes (nudges 8, 2, 1/2, 1/4 px) instead
of 6. Both lose more than they save; the static clips suffer most with 16
samples (Dota +55%, StarCraft +43%): with sparse samples, wrong vectors tie
with zero, and any fractional vector blurs the prediction through bilinear
sampling. Kept as knobs (PW_SEARCH16, PW_SEARCH_STEPS); the default is
unchanged.

**Step 3e** (zero bias): the zero vector's cost is reduced by a fixed amount in
SAD units, so static blocks keep exactly zero motion (no blur from fractional
positions, no vector bytes). Against 3b: 0.1 -2.5%, 0.3 -1.7% (StarCraft -9.6%
but Euro Truck +2.6%), 1.0 +5.6%. Default 0.1. Against stock: Dota -70.4%,
StarCraft -63.2%, Euro Truck -43.2%, Witcher -4.1%, Minecraft -3.4%.

**Step 3f** (rejected): Catmull-Rom bicubic sampling in the prediction and the
decision (4 gathers per pixel): Euro Truck -0.06 to -0.19 dB, Witcher -0.06 to
-0.15 dB at 20/75/300 KB. The reference is a decoded frame; sharpening
interpolation amplifies its coding noise, where bilinear's low-pass acts like
a mild loop filter. Kept as a knob (PW_CUBIC), off.

**Step 4** (every frame the same size): until now the vectors were sent on
top of the coefficients' budget, so inter frames came out 75.7-85.7 KB at a
75 KB target (vectors 1.4-10.7 KB, varying frame to frame). A one-threadgroup
pass (`inter_side_cost.comp`) sizes the vectors exactly as they are counted
(per 4x4 vectors and component a 4-bit width, then zigzag deltas from last
frame's) plus the 1-bit map, and adds it to the payload the rate control
analysis accumulates, so resolve takes it out of the coefficients. Every frame
is now at or under the target, intra or inter (74.85-75.00 KB at 75 KB).
Quality against 3e: -0.4% PSNR-Y (Dota -1.0, Witcher -1.2, Euro Truck +0.6),
i.e. unchanged. `PW_PERFRAME=1` makes pwrd print each frame's bytes, vector
bytes, map bytes and PSNR-Y.

**Step 5** (what the state does after a loss). Without loss handling, an error
in the client's state stays: at 75 KB the summed loss over 30 frames is 19.2
dB*frames on StarCraft (0.59 dB still lost 29 frames later), 10.2 on Witcher,
3.5 on Minecraft, against 1.1-1.9 for stock (whose loss ends with the frame).
Bilinear motion compensation blurs some of it away; static areas keep it.

| 75 KB, dB*frames (on the lost frame) | Dota | Euro Truck | Minecraft | StarCraft | Witcher |
|---|---|---|---|---|---|
| stock | 1.84 (1.84) | 1.91 | 1.09 | 1.64 | 1.20 |
| step 4, no report | 0.26 (0.02) | 2.59 | 3.48 | 19.16 (1.19) | 10.24 (4.73) |
| leak 0.97 (+8.1% bits) | 0.76 | 1.79 | 2.26 | 11.15 | 6.84 |
| refresh 30 (+10.5% bits) | 11.89 | 5.51 | 3.44 | 16.24 | 8.55 |
| report after 1 frame | 0.16 | 0.43 | 0.69 | 1.45 (1.19) | 4.43 (4.73) |
| report after 2 frames | 0.07 | 0.46 | 1.09 | 2.43 | 4.63 |
| report after 4 frames | 1.19 | 0.80 | 1.71 | 3.75 | 6.08 |

- 5a, a leaky state (the client's error fades by a per frame): forgetting has
  to be re-sent, and static content pays most (Dota +22%, StarCraft +14% at
  0.97). The error still lasts: 4.8% of the drift is left after 29 frames on
  StarCraft at 0.97, against 16.6% without. Rejected; knob PW_LEAK.
- 5b, refresh (each 32x32 block coded intra once per N frames, a scattered
  pattern): costs more than the leak and helps less; a lost intra block hurts
  more than a lost residual. Rejected; knob PW_REFRESH.
- 5c, a loss report: the client says which packets it missed; the encoder,
  which has every packet, decodes the frame again without them and predicts
  the next frame from that, the client's actual state. The next frame's
  ordinary residual then repairs the damage at the usual size (0.0-0.2 dB
  left on frame +1 at 20 KB with the report in by then), and nothing is
  spent while nothing is lost. A later report means replaying each frame
  since the loss, which the encoder can, because it keeps the packets and
  knows which were missed. Over the hotspot at 60 Hz the report should be in
  before the next frame is encoded (frame on the wire in ~5 ms, ~2 ms back,
  16.7 ms frame time). The library needs nothing new for this: the host runs a
  decoder on the packets minus the lost ones and passes its output to
  pyrowave_encoder_set_reference.
- The lost frame itself is hit harder in inter frames when the burst lands
  early (Witcher 75 KB: 13.5 dB at 5% into the frame against 1.8 for stock,
  better than stock at 35% and 65%). Blocks are sent coarse band first, and
  an inter frame's fine bands are mostly empty and not sent, so the coarse
  bands (LL4 and level 4) are a larger share of the frame's packets, and an
  early burst takes more of them. bitstream.md already suggests protecting
  LL4 (FEC or sending it twice); with prediction that matters more.

**Step 6** (search speed). Profiled with `PW_SPLIT=1` (one timed pass per
stage; the stages then cannot overlap, so their sum is a little above the
whole): six search passes took 1.90 ms of the 2.70 ms encode, 0.32 ms each,
against 0.02 ms for an empty kernel on the same grid. Taking parts out showed
where: the nine per-candidate reductions over 64 threads (subgroup sums,
shared memory, a barrier) ~0.07 ms a pass, the one-thread tail that picks the
best ~0.06, reference cache misses ~0.17; the bilinear arithmetic nothing
(hardware filtering alone left it at 1.91 ms). Now one thread per (block,
candidate) sums its 64 samples alone and the nine meet once in shared memory,
eight blocks per threadgroup (4, 16 and 32 were no better), reading the
reference through the sampler's bilinear filter (the search need not match the
decoder, and quarter-pel weights are exact in it). Same candidates, costs and
tie order, so the output differs only by float summation order: 0.0% mean
(-0.5 to +0.5% per clip). Search 1.90 -> 0.71 ms. Profile now: search 0.71,
quant 0.43 (stock's), dwt 0.17, predict 0.17, side + decide 0.14, rate
control + pack 0.12. The 16-sample variant (3d, rejected) and its PW_SEARCH16
knob are removed.

**Step 7** (exact prediction). The prediction (`inter_predict.h`) is now
integer arithmetic: the vector field bilinear in 1/(2 block) steps, the
reference bilinear at 1/64 pel from its 8-bit texels, the weight on a 0-256
scale, every rounding a shift. It gives the same bytes on any GPU and from any
compiler, which a float version only does for one compiled kernel on one
device: a prerequisite for an AMD host and an Apple client agreeing, once the
transform is exact too. Checked with `PW_CHECKPRED=1` (a decoder given the
reference, map and vectors but no coefficients outputs its own prediction,
compared byte for byte with the plane the encoder subtracted): 0 of 180M
samples differ. Quality +0.1%.
- First tried computing it in place in the decoder's last inverse transform
  level instead of a separate pass: decode 0.51 -> 0.56 ms, because the
  separate pass runs alongside dequant (whose dispatches leave the GPU idle)
  while the in-place one lengthens the transform. Kept separate.
- Integer division (by 255 for an 8-bit weight) cost ~0.07 ms of decode; the
  0-256 weight scale makes everything shifts. Each 2x2 fetches its vector and
  weight cells once.
- The rejected knobs PW_LEAK (5a) and PW_CUBIC (3f) are removed.

**Step 8**: the nine candidate threads of a block each read the same 64
source pixels; they now load them once into shared memory, cutting the
search's texture reads nearly in half. Output identical; encode 1.87 -> 1.73 ms
(search ~0.79 -> 0.63 in split timings).

**Step 9** (rejected): an aligned 4x4 never straddles a vector or weight
cell, so one thread per 4x4 fetches each cell once for 16 pixels, halving the
prediction pass's texture reads. Slower: decode 0.529 -> 0.568 ms, as were 4x2
and 2x1 (four times fewer threads, each with 16 dependent gathers, so latency
rather than fetches bounds it). The same kernel written generically for 2x2 was
also slower than step 8's hand-written one (0.567), so step 8's stays.

**Step 10** (rejected): adding lambda_t * |v - v_last| to the search cost
(vectors are coded against last frame's). PSNR-Y change at 0.01 / 0.05 / 0.2
against 0, 30 frames: Minecraft 20 KB +0.02 / +0.28 / +0.93 dB but 75 KB -0.09
/ -0.43 / -0.25; StarCraft 75 KB -0.51 / -1.74 / -1.88 (where motion starts or
changes, the field lags it); Witcher 20 KB +0.11 / +0.20 / +0.31, 75 KB -0.13
/ -0.26 / -0.21. Clip dependent, not just rate dependent.

**Step 11** (free intra vectors). Counting the vectors (`PW_MVSTATS=1`) at
75 KB: 63% of Witcher's and 94% of Minecraft's change from last frame each
frame, and a third of Witcher's changed ones (26% of Minecraft's) lie under
32x32 blocks decided intra, where their own value only reaches the blend
toward predicted neighbours. The side-cost pass now runs after the decision
and first sets those to last frame's vector (free to send); the prediction is
computed after it, so encoder and decoder still agree (checked). Vector bytes
per frame at 75 KB: Witcher 8.3 -> 4.8 KB, Minecraft 12.5 -> 10.5. BD-rate
against 7: Witcher -3.2%, Minecraft -0.8, Dota -0.5, Euro Truck +0.4, StarCraft
+1.0; mean -0.6%. Knob PW_KEEP_INTRA_VECTORS=1 turns it off.
- Other parallel vector codes, counted on the same vectors (`PW_MVSTATS`,
  bytes per frame at 75 KB, current code A): the tile's mean delta first
  then deltas from it (B), the tile's mean vector then vectors from it (C),
  the cheapest per tile with 2 bits for which (D). Witcher A 8308 / B 8576 /
  C 8037 / D 7580; Minecraft 12516 / 12322 / 11623 / 11193; Dota 4819 / 5136 /
  3389 / 3270. At most 5-30%: the fields themselves are noisy (Dota, whose
  clip alternates moving and still frames, changes 33% of its vectors a frame
  with 18% nonzero).

**Step 12** (off by default): price the vectors. A bit is worth about 2 ln 2
times the mean squared error per pixel (the slope of D(R) = c 2^(-2R/N)); the
encoder measures last frame's luma error itself (`inter_frame_mse.comp`, its
reference against a kept copy of last frame's source) and, per 4x4 vector
tile, keeps the searched vectors only if the squared error they save over last
frame's (free) vectors is worth their bits at that price times a calibration
PW_VRD (`inter_vector_rd.comp`, after the decision, block-constant vectors,
2:1 subsampled). 30 frames, PSNR-Y at 20 KB, scale 0 / 0.5 / 1 / 2: Minecraft
25.15 / 25.33 / 25.42 / 25.50 dB (vectors 10.9 -> 8.6 KB), StarCraft 27.11 /
27.37 / 27.40 / 27.43, Witcher 31.24 / 31.27 / 31.28 / 31.31; at 75 KB flat
except StarCraft 31.44 / 31.44 / 31.35 / 31.24. BD-rate over 20-300 KB against
11: -0.1% / 0.0% / +0.3% (Minecraft -0.8 / -1.6 / -2.2, Euro Truck -0.6 / -1.1
/ -1.4, StarCraft +1.3 / +3.0 / +4.5): the low-rate gain is real, but going
back to last frame's vectors makes the field lag where motion starts
(StarCraft's scrolling), the failure of step 10 again. Kept as a knob.

**Step 13** (a measurement): how far a client drifts when its transform's
float arithmetic differs from the encoder's, the AMD host and Apple client case
(the transform is fp16, not exact). `PW_CLIENT_PRECISION=p` decodes the client
on a device of another precision; with `PW_LOSS=0:0:0` the loss lines give its
mean squared difference from the encoder's own decode (8-bit units). Encoder at
the default precision 1 (fp16 math, fp16 storage):

| client | intra only (stock) | with prediction, frame 19 / 59 / 99 / 119 |
|---|---|---|
| precision 0 (fp16 math, fp16 storage): a large mismatch | Dota 0.34, StarCraft 0.44, flat | StarCraft 0.55 / 0.70 / 0.95 / 0.90 (0.3 dB at 150 KB); Dota 0.52 / 0.45 / 0.46 / 0.54 |
| precision 2 (fp32): rounding-level | Dota 0.03, StarCraft 0.04 | StarCraft 0.06 / 0.08 at frame 59; ~0.02 dB |

The prediction carries the mismatch forward, 1.3-1.6x stock's and slowly
growing in static content where nothing replaces it; rounding-level
differences stay negligible. An exact transform would remove it; it is not
urgent at rounding level, and a client and host with different precision
settings should not be paired.

**Step 14** (the bitstream). The map and the vectors travel in inter packets
(`bitstream/inter.md`, `pyrowave_inter.cpp`): per 64x64 tile its map bits and
its vectors as before, records packed greedily into packets of at most the
inter packet boundary, sent before the coefficients. The GPU side-cost pass
lays the records out by the same rule, so the frame's bytes are still exactly
within the target (Euro Truck at 75 KB: 74.93-74.99 KB, of which inter packets
7.2-8.1 KB). The decoder resolves the map and vectors from the packets it got
(a lost tile keeps its previous vectors and predicts); the side-channel setters
are gone. PW_CHECKPRED (a decoder given only the inter packets) still matches
the encoder byte for byte. `pwrd` counts only real packets now.
- Loss with the real packets (`bench.py loss`, inter packets lost too): a
  burst 5% into a frame takes inter packets, i.e. vectors, and without a
  report the damage lasts (75 KB, dB*frames: StarCraft 88.3, Minecraft 58.9,
  Euro Truck 36.9). With the report in by the next frame the encoder replays
  the client's decode (planes and vector state, `pyrowave_decoder_get/
  set_motion_state`, `pyrowave_encoder_set_motion_state`) and the next frame
  repairs it: 0.2-8.4 dB*frames, almost all the lost frame's own hit; two
  frames later: 0.4-11.4.

**Step 15** (Vulkan, for the host). The inter passes run in PyroWave's Vulkan
encoder and decoder (Granite; shaders through slangmosh). The encoder is two
parts that may be separate submissions: the front (motion search, decisions,
side cost, the map and vectors read back) and the back (prediction, transform,
rate control, packing). The C API (`pyrowave.h`): `pyrowave_encoder_enable_inter`
makes the encoder predict each frame from its own decode of the previous one
(it decodes every frame it packetizes) and keep the last frames;
`pyrowave_encoder_packetize_inter` hands out a frame's inter packets as soon as
the front is done, before the rest is coded, so they can be sent first;
`pyrowave_encoder_report_loss` replays a decoder's lossy decode from the kept
frames (or makes the next frame intra if the frame is gone).
`tools/pwenc.c` (Vulkan, built on the build box, run on the host's AMD GPU)
writes the packets and the encoder's own decode; `tools/pwdec.mm` decodes them
with the Metal decoder. Euro Truck, 30 frames at 75 KB: mean PSNR-Y 29.46 dB
(intra 28.06), the same as the Metal-only pipeline (frame 1: 28.747 against
28.748); the Mac's decode differs from the AMD encoder's own in 0.01% of luma
samples (MSE 0.0001), not growing. Losing frame 10's first 5 packets (its
inter packets) on the Mac with the loss reported: frame 10 is hit (28.2 dB),
frame 11 is back within 0.02% of the encoder; without the report the frames
drift to 23.8 dB by frame 29.
- Vulkan needs every declared binding bound: the transforms' binding 2 (the
  prediction) gets a placeholder when unused.

**Step 16** (in gmstream, live). The inter frames ship in gmstream as a patch
over upstream (its DESIGN.md §13, PROTOCOL.md "PyroWave inter frames"). Host
(the host, AMD) encode start -> all packets, intra / inter: 1080p 0.77 / 2.09 ms,
2560x1600 1.08 / 3.23, 4K 1.70 / 6.11 (the inter packets ready 0.3-1 ms before
the rest); present -> decoded on the Mac over Wi-Fi rises by 2-4 ms at the same
bitrate. The motion search is the cost on that GPU, so the next step is a
cheaper one (coarse passes at half resolution, or fewer passes).

**Step 17** (stripes: encode, network and decode overlapped). A frame is coded
as horizontal stripes, each an image of its own (its own transform, rate
control, packets and inter packets) that predicts from anywhere in the whole
previous frame: the inter shaders take the stripe's row in the reference
(`ref_y`), and `copy_rows.comp` cuts each stripe out of the frame. The C API
(`pyrowave_inter_info::stripe_rows`, `first_stripe_rows`) submits the stripes
one after another; `pyrowave_encoder_packetize_stripe` waits only for its own,
`pyrowave_encoder_decode_stripe` queues the encoder's own decode of it. Each
stripe's budget is the share the rate control would have given it across the
whole frame, from the last frame's statistics (`Encoder::set_rate_stats_readback`).
The Metal decoder takes a stripe's reference row
(`pyrowave_decoder_set_reference_stripe`) and writes a stripe straight into its
frame's rows (`pyrowave_decoder_decode_gpu_buffer_rows`; the transform's output
offset). Bit-exact between encoder and decoder as before; AMD encode -> Metal
decode within 0.01% of luma samples, loss replay across stripes exact.
- Bits (Euro Truck 1080p, 30 frames, PSNR-Y against one stripe): 384 rows
  -0.01 dB, 256 rows -0.05, 128 rows -0.15 at 75 KB.
- Live in gmstream (its DESIGN.md §13.3-13.5): at 2560x1600 @120, 100 Mb/s,
  present -> decoded 11.6 -> 4.9-5.9 ms p50 with a light GPU load, 7.5-8.1 under
  a GPU-bound one, with both GPUs kept awake (a tiny filler kernel on a low
  priority queue: each end's power management otherwise ran the stripes' short
  bursts at a fraction of the clock).
- Tried, no change under a GPU-bound load, reverted: one submission for every
  stripe; a completion mark polled instead of the fence; the packing and rate
  analysis bands in one dispatch each (`Batched`); the stripes over two compute
  queues; on Metal, the dequant bands in one dispatch (slower).
