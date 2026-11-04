#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

// Implementation of signed integer mod accurate to SPIR-V specification
template<typename Tx, typename Ty>
inline Tx spvSMod(Tx x, Ty y)
{
    Tx remainder = x - y * (x / y);
    return select(Tx(remainder + y), remainder, remainder == 0 || (x >= 0) == (y >= 0));
}

struct MotionCell
{
    float2 v00;
    float2 v10;
    float2 v01;
    float2 v11;
    float2 f;
    float vbs;
};

struct Registers
{
    int refresh_period;
    int refresh_phase;
    int ref_y;
};

static inline __attribute__((always_inline))
MotionCell motion_cell(thread const int2& p, thread const float& vbs, texture2d<int> uVectors, sampler uVectorsSmplr)
{
    int2 gsize = int2(uVectors.get_width(), uVectors.get_height());
    float2 g = ((float2(p) + float2(0.5)) / float2(vbs)) - float2(0.5);
    int2 i0 = int2(floor(g));
    MotionCell c;
    c.f = g - float2(i0);
    int2 a = clamp(i0, int2(0), gsize - int2(1));
    int2 b = clamp(i0 + int2(1), int2(0), gsize - int2(1));
    float s = vbs / 64.0;
    c.v00 = float2(uVectors.read(uint2(a), 0).xy) * s;
    c.v10 = float2(uVectors.read(uint2(int2(b.x, a.y)), 0).xy) * s;
    c.v01 = float2(uVectors.read(uint2(int2(a.x, b.y)), 0).xy) * s;
    c.v11 = float2(uVectors.read(uint2(b), 0).xy) * s;
    c.vbs = vbs;
    return c;
}

static inline __attribute__((always_inline))
float2 motion_in_cell(thread const MotionCell& c, thread const int2& o)
{
    float2 f = c.f + (float2(o) / float2(c.vbs));
    return mix(mix(c.v00, c.v10, float2(f.x)), mix(c.v01, c.v11, float2(f.x)), float2(f.y));
}

static inline __attribute__((always_inline))
float sample_reference(thread float2& q, thread const int& ref_y, texture2d<float> uReference, sampler uReferenceSmplr)
{
    q.y += float(ref_y);
    float2 fl = floor(q);
    float2 f = q - fl;
    float4 t = uReference.gather(uReferenceSmplr, ((fl + float2(1.0)) / float2(int2(uReference.get_width(), uReference.get_height()))), int2(0), component::x).wzxy;
    return mix(mix(t.x, t.y, f.x), mix(t.z, t.w, f.x), f.y);
}

static inline __attribute__((always_inline))
float2 reduce2(thread const float& a, thread const float& b, thread uint& gl_LocalInvocationIndex, threadgroup spvUnsafeArray<float, 64>& s_a, threadgroup spvUnsafeArray<float, 64>& s_b)
{
    uint t = gl_LocalInvocationIndex;
    s_a[t] = a;
    s_b[t] = b;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = 32u; s > 0u; s = s >> 1u)
    {
        if (t < s)
        {
            s_a[t] += s_a[t + s];
            s_b[t] += s_b[t + s];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float2 r = float2(s_a[0], s_b[0]);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    return r;
}

kernel void riptide_inter_decide(constant Registers& _402 [[buffer(0)]], texture2d<float> uSource [[texture(0)]], texture2d<float, access::write> uWeights [[texture(1)]], texture2d<float> uReference [[texture(2)]], texture2d<int> uVectors [[texture(3)]], sampler uSourceSmplr [[sampler(0)]], sampler uReferenceSmplr [[sampler(2)]], sampler uVectorsSmplr [[sampler(3)]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]])
{
    threadgroup spvUnsafeArray<float, 64> s_a;
    threadgroup spvUnsafeArray<float, 64> s_b;
    int2 size = int2(uSource.get_width(), uSource.get_height());
    uint t = gl_LocalInvocationIndex;
    int2 p0 = (int2(gl_WorkGroupID.xy) * int2(32)) + int2(int(t & 1u) * 16, int(t >> uint(1)));
    float sum = 0.0;
    float sad = 0.0;
    float n = 0.0;
    int2 param = p0;
    float param_1 = 16.0;
    MotionCell mc0 = motion_cell(param, param_1, uVectors, uVectorsSmplr);
    int2 param_2 = p0 + int2(8, 0);
    float param_3 = 16.0;
    MotionCell mc1 = motion_cell(param_2, param_3, uVectors, uVectorsSmplr);
    float _360;
    float2 _373;
    float _393;
    spvUnsafeArray<float, 16> vals;
    for (int i = 0; i < 16; i++)
    {
        int2 p = p0 + int2(i, 0);
        bool inside = all(p < size);
        if (inside)
        {
            _360 = uSource.read(uint2(p), 0).x;
        }
        else
        {
            _360 = 0.0;
        }
        float s = _360;
        if (i < 8)
        {
            MotionCell param_4 = mc0;
            int2 param_5 = int2(i, 0);
            _373 = motion_in_cell(param_4, param_5);
        }
        else
        {
            MotionCell param_6 = mc1;
            int2 param_7 = int2(i - 8, 0);
            _373 = motion_in_cell(param_6, param_7);
        }
        float2 v = _373;
        if (inside)
        {
            float2 param_8 = float2(p) + v;
            int param_9 = _402.ref_y;
            float _408 = sample_reference(param_8, param_9, uReference, uReferenceSmplr);
            _393 = _408;
        }
        else
        {
            _393 = 0.0;
        }
        float r = _393;
        vals[i] = s;
        sum += s;
        sad += abs(s - r);
        n += float(inside);
    }
    float param_10 = sum;
    float param_11 = n;
    float2 _438 = reduce2(param_10, param_11, gl_LocalInvocationIndex, s_a, s_b);
    float2 sn = _438;
    float mean = sn.x / fast::max(sn.y, 1.0);
    float dev = 0.0;
    for (int i_1 = 0; i_1 < 16; i_1++)
    {
        if (all((p0 + int2(i_1, 0)) < size))
        {
            dev += abs(vals[i_1] - mean);
        }
    }
    float param_12 = sad;
    float param_13 = dev;
    float2 _479 = reduce2(param_12, param_13, gl_LocalInvocationIndex, s_a, s_b);
    float2 cost = _479;
    if (t == 0u)
    {
        int2 b = int2(gl_WorkGroupID.xy);
        bool _491 = _402.refresh_period > 0;
        bool _508;
        if (_491)
        {
            _508 = spvSMod((b.x * 7) + (b.y * 3), _402.refresh_period) == _402.refresh_phase;
        }
        else
        {
            _508 = _491;
        }
        bool refresh = _508;
        uWeights.write(float4(float((cost.x <= cost.y) && (!refresh))), uint2(b));
    }
}

