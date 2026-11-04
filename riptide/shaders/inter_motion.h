// Inter frames: motion. One vector per 16x16 luma block, quarter pel, in
// uVectors (RG16 signed). The vector at a pixel is bilinear between block
// centres (8x8 blocks of 4:2:0 chroma, half the displacement), so the field is
// smooth and the prediction has no block edges.
#ifndef INTER_MOTION_H_
#define INTER_MOTION_H_

// Reference sample at pixel position q (texel index space), bilinear. A
// stripe of the frame (coded as its own image) predicts from the whole
// reference: ref_y is the stripe's first row in it.
float sample_reference(vec2 q, int ref_y)
{
    q.y += float(ref_y);
    vec2 fl = floor(q), f = q - fl;
    vec4 t = textureGather(uReference, (fl + 1.0) / vec2(textureSize(uReference, 0))).wzxy;
    return mix(mix(t.x, t.y, f.x), mix(t.z, t.w, f.x), f.y);
}

// Displacement in the plane's pixels at pixel p; vbs is the plane's vector
// block size (16 luma, 8 for 4:2:0 chroma).
vec2 motion_at(ivec2 p, float vbs)
{
    ivec2 gsize = textureSize(uVectors, 0);
    vec2 g = (vec2(p) + 0.5) / vbs - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 f = g - vec2(i0);
    ivec2 a = clamp(i0, ivec2(0), gsize - 1), b = clamp(i0 + 1, ivec2(0), gsize - 1);
    vec2 v00 = vec2(texelFetch(uVectors, a, 0).xy), v10 = vec2(texelFetch(uVectors, ivec2(b.x, a.y), 0).xy);
    vec2 v01 = vec2(texelFetch(uVectors, ivec2(a.x, b.y), 0).xy), v11 = vec2(texelFetch(uVectors, b, 0).xy);
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y) * (vbs / 64.0);
}

// The four vectors around pixel p and p's position between them, for callers
// that interpolate several neighbouring pixels from the same cell (an even
// aligned 2x2 never straddles two cells).
struct MotionCell
{
    vec2 v00, v10, v01, v11;
    vec2 f;
    float vbs;
};

MotionCell motion_cell(ivec2 p, float vbs)
{
    MotionCell c;
    ivec2 gsize = textureSize(uVectors, 0);
    vec2 g = (vec2(p) + 0.5) / vbs - 0.5;
    ivec2 i0 = ivec2(floor(g));
    c.f = g - vec2(i0);
    ivec2 a = clamp(i0, ivec2(0), gsize - 1), b = clamp(i0 + 1, ivec2(0), gsize - 1);
    float s = vbs / 64.0;
    c.v00 = vec2(texelFetch(uVectors, a, 0).xy) * s;
    c.v10 = vec2(texelFetch(uVectors, ivec2(b.x, a.y), 0).xy) * s;
    c.v01 = vec2(texelFetch(uVectors, ivec2(a.x, b.y), 0).xy) * s;
    c.v11 = vec2(texelFetch(uVectors, b, 0).xy) * s;
    c.vbs = vbs;
    return c;
}

// The displacement at p + o, o a small offset within the same cell.
vec2 motion_in_cell(MotionCell c, ivec2 o)
{
    vec2 f = c.f + vec2(o) / c.vbs;
    return mix(mix(c.v00, c.v10, f.x), mix(c.v01, c.v11, f.x), f.y);
}

#endif
