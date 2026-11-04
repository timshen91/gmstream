# Inter frames (extension to bitstream v1)

An inter frame is coded as the difference between the source and a
prediction from the previous decoded frame, the *reference*. Its packets are
the v1 packets (start of frame header and 32x32 coefficient blocks, unchanged)
plus *inter packets*, which carry the prediction map and the motion vectors.
Whether a frame is predicted, and from which frame, is signalled by the higher
level protocol (the reference is always the decoder's previous decoded frame of
the same sequence of frames; the protocol says which frames start over intra).
Code in `riptide_inter.hpp`; the prediction itself in `shaders/inter_predict.h`.

## Inter packet

`extended` = 1 and `code` = `BITSTREAM_EXTENDED_CODE_INTER` (1), in the same bits
as the start of frame header:

```c
struct BitstreamInterHeader
{
  uint32_t reserved0 : 16;
  uint32_t payload_words : 12; // packet size in u32 words, this header included
  uint32_t sequence : 3;
  uint32_t extended : 1;       // 1
  uint32_t first_tile : 12;
  uint32_t tile_count : 12;
  uint32_t code : 2;           // 1
  uint32_t reserved1 : 6;
};
```

followed by `tile_count` tile records, bit packed LSB first into u32 words,
the last word padded with zero bits.

## Grid

- One vector per 16x16 luma block (`vw = ceil(w / 16)`, `vh = ceil(h / 16)`),
  quarter pel, in luma units; 4:2:0 chroma uses half the displacement.
- One weight per 32x32 luma block (`mw = ceil(w / 32)`, `mh = ceil(h / 32)`):
  1 predict, 0 intra (the DC shift).
- A tile is 4x4 vectors, i.e. 2x2 weights, 64x64 luma pixels; `tw = ceil(vw / 4)`,
  `th = ceil(vh / 4)`, numbered row major.

## Tile record

1. One bit per weight block of the tile inside the frame (row major).
2. A 4-bit width `wx`, then a 4-bit width `wy`.
3. For each of the tile's vectors inside the frame (row major), `wx` bits of
   the x component's zigzag delta from that vector's previous value; then the
   same for y with `wy` bits. Zigzag: `d >= 0 ? 2d : -2d - 1`.

The previous value is the decoder's vector after the previous frame (zero after
an intra frame).

## Packets

Records go into packets greedily in tile order: a packet takes whole records
while its size (8 header bytes plus the records rounded up to a word) stays
within the encoder's inter packet boundary. Encoders charge exactly this size
to the frame's budget. Inter packets are sent before the frame's coefficient
packets, so a decoder can form the prediction while the rest arrives.

## Loss

A tile none of whose records arrived keeps its previous vectors and predicts
all its weight blocks; missing coefficient blocks are zero, as in v1. A decoder
decodes every frame of an inter sequence in order: a frame of which nothing
arrived is decoded with nothing, from the previous vectors. These rules are
exact, so an encoder told which packets a decoder missed can replay that
decoder and predict from what it actually has.
