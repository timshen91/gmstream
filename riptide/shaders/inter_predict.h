// Inter frames: the prediction of one pixel, in integers so the encoder's
// prediction pass and the decoder's inverse transform (which computes it in
// place) agree exactly, on any GPU:
//
//   pred = 127.5 + w * (ref(p + d) - 127.5)    (8-bit units, rounded)
//
// w the block weights (32x32 luma blocks, 16x16 4:2:0 chroma; 0 or 1, bilinear
// between block centres, 1/64 of a block), d the motion field (one quarter-pel
// luma vector per 16x16, bilinear between block centres, half the
// displacement in 4:2:0 chroma), ref the reference sampled bilinearly at 1/64
// pel. Weight 0 is the intra DC shift.
//
// The includer declares uReference (R8, mirror-repeat sampler), uWeights
// (R8) and uVectors (RG16 signed). A reference texel's position is clamped by
// the sampler's mirror repeat (at the reference's edges, a stripe's too), so
// p itself may lie past the plane's edge.
#ifndef INTER_PREDICT_H_
#define INTER_PREDICT_H_

// An 8-bit texel's integer value (unorm reads are exact k / 255).
int texel8(float v)
{
    return int(v * 255.0 + 0.5);
}

// A block weight texel (0 .. 255) on a 0 .. 256 scale, so that 1 is a power
// of two: 0 and 255 map exactly to 0 and 1.
int weight256(float v)
{
    int t = texel8(v);
    return t + (t >> 7);
}

// Bilinear position of pixel p between block centres, blocks of 1 << shift:
// g = (2p + 1 - block) / (2 block). The cell's lower corner, and the fraction
// in 1 / (2 block) units. An even aligned pair of pixels never straddles two
// cells (the corners fall between odd and even pixels).
void cell(ivec2 p, int shift, out ivec2 i0, out ivec2 f)
{
    ivec2 n = 2 * p + 1 - (1 << shift);
    i0 = n >> (shift + 1); // a floor for either sign
    f = n - (i0 << (shift + 1));
}

// The predictions of the 2x2 pixels from p (even), as x = p, y = p + (1, 0),
// z = p + (0, 1), w = p + (1, 1). p is in the plane being predicted, which is
// a stripe of the frame starting at row ref_y of the reference (0 for a
// whole frame); the weights and vectors are the stripe's own.
ivec4 predict8_quad(ivec2 p, int ref_y)
{
    ivec2 size = textureSize(uReference, 0);
    ivec2 wsize = textureSize(uWeights, 0), vsize = textureSize(uVectors, 0);
    // Weight blocks are 32 luma pixels, so 16 in a half-width chroma plane;
    // vector blocks are half that.
    int wshift = size.x * 2 <= wsize.x * 32 ? 4 : 5;
    int vshift = wshift - 1;

    ivec2 vi, vf, wi, wf;
    cell(p, vshift, vi, vf);
    cell(p, wshift, wi, wf);
    ivec2 a = clamp(vi, ivec2(0), vsize - 1), b = clamp(vi + 1, ivec2(0), vsize - 1);
    ivec2 v00 = texelFetch(uVectors, a, 0).xy, v10 = texelFetch(uVectors, ivec2(b.x, a.y), 0).xy;
    ivec2 v01 = texelFetch(uVectors, ivec2(a.x, b.y), 0).xy, v11 = texelFetch(uVectors, b, 0).xy;
    a = clamp(wi, ivec2(0), wsize - 1);
    b = clamp(wi + 1, ivec2(0), wsize - 1);
    int w00 = weight256(texelFetch(uWeights, a, 0).x), w10 = weight256(texelFetch(uWeights, ivec2(b.x, a.y), 0).x);
    int w01 = weight256(texelFetch(uWeights, ivec2(a.x, b.y), 0).x), w11 = weight256(texelFetch(uWeights, b, 0).x);
    int D = 2 << vshift, E = 2 << wshift;

    ivec4 result;
    for (int k = 0; k < 4; k++)
    {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 q = p + o;
        // Each pixel step moves the fraction by 2 units.
        ivec2 f = vf + 2 * o;
        ivec2 v = (v00 * (D - f.x) + v10 * f.x) * (D - f.y) + (v01 * (D - f.x) + v11 * f.x) * f.y;
        // v is quarter-pel luma times D^2; in this plane's pixels that is
        // v / D^2 / 4 * block / 16, so at 1/64 pel v * block / D^2 = v / (4 block),
        // rounded.
        ivec2 d = ((q << 6) + ((v + (2 << vshift)) >> (vshift + 2)));
        ivec2 ip = (d >> 6) + ivec2(0, ref_y), fq = d & 63;
        vec4 t = textureGather(uReference, (vec2(ip) + 1.0) / vec2(size)).wzxy; // texels ip .. ip + 1, mirrored
        int r = (texel8(t.x) * (64 - fq.x) + texel8(t.y) * fq.x) * (64 - fq.y)
              + (texel8(t.z) * (64 - fq.x) + texel8(t.w) * fq.x) * fq.y; // x 4096
        int r16 = (r + 128) >> 8; // 1/16 units, 0 .. 4080

        f = wf + 2 * o;
        int w = (w00 * (E - f.x) + w10 * f.x) * (E - f.y) + (w01 * (E - f.x) + w11 * f.x) * f.y;
        int w8 = (w + (E * E >> 1)) >> (2 * wshift + 2); // 0 .. 256, 256 = 1

        // pred16 = 2040 + w * (r16 - 2040), rounded; >> is a floor for either sign.
        int pred16 = 2040 + ((w8 * (r16 - 2040) + 128) >> 8);
        result[k] = clamp((pred16 + 8) >> 4, 0, 255);
    }
    return result;
}

#endif
