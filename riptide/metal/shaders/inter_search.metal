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

struct Registers
{
    int _step;
    int iteration;
    float lambda;
    float zero_bias;
    int ref_y;
};

static inline __attribute__((always_inline))
float search_sample(thread const float2& q, texture2d<float> uReference, sampler uReferenceSmplr, constant Registers& _23)
{
    float _46 = uReference.sample(uReferenceSmplr, ((q + float2(0.5, 0.5 + float(_23.ref_y))) / float2(int2(uReference.get_width(), uReference.get_height()))), level(0.0)).x;
    return _46;
}

kernel void riptide_inter_search(constant Registers& _23 [[buffer(0)]], texture2d<float> uSource [[texture(0)]], texture2d<int, access::write> uOut [[texture(1)]], texture2d<float> uReference [[texture(2)]], texture2d<int> uCur [[texture(3)]], texture2d<int> uPrev [[texture(4)]], sampler uSourceSmplr [[sampler(0)]], sampler uReferenceSmplr [[sampler(2)]], sampler uCurSmplr [[sampler(3)]], sampler uPrevSmplr [[sampler(4)]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]])
{
    threadgroup spvUnsafeArray<spvUnsafeArray<float, 64>, 8> s_src;
    threadgroup spvUnsafeArray<spvUnsafeArray<float, 9>, 8> s_cost;
    threadgroup spvUnsafeArray<spvUnsafeArray<int2, 9>, 8> s_cand;
    int2 gsize = int2(uCur.get_width(), uCur.get_height());
    uint t = gl_LocalInvocationIndex;
    uint b = t / 9u;
    uint k = t % 9u;
    int2 blk = int2(int((gl_WorkGroupID.x * 8u) + b), int(gl_WorkGroupID.y));
    bool live = blk.x < gsize.x;
    blk.x = min(blk.x, (gsize.x - 1));
    int2 cur = uCur.read(uint2(blk), 0).xy;
    int2 prev = uPrev.read(uint2(blk), 0).xy;
    int2 n0 = uCur.read(uint2(clamp(blk + int2(-1, 0), int2(0), gsize - int2(1))), 0).xy;
    int2 n1 = uCur.read(uint2(clamp(blk + int2(1, 0), int2(0), gsize - int2(1))), 0).xy;
    int2 n2 = uCur.read(uint2(clamp(blk + int2(0, -1), int2(0), gsize - int2(1))), 0).xy;
    int2 n3 = uCur.read(uint2(clamp(blk + int2(0, 1), int2(0), gsize - int2(1))), 0).xy;
    int sgn = ((((blk.x + blk.y) + _23.iteration) & 1) != 0) ? (-1) : 1;
    int2 _183;
    if (k == 0u)
    {
        _183 = cur;
    }
    else
    {
        int2 _190;
        if (k == 1u)
        {
            _190 = int2(0);
        }
        else
        {
            int2 _197;
            if (k == 2u)
            {
                _197 = prev;
            }
            else
            {
                int2 _205;
                if (k == 3u)
                {
                    _205 = n0;
                }
                else
                {
                    int2 _213;
                    if (k == 4u)
                    {
                        _213 = n1;
                    }
                    else
                    {
                        int2 _221;
                        if (k == 5u)
                        {
                            _221 = n2;
                        }
                        else
                        {
                            int2 _229;
                            if (k == 6u)
                            {
                                _229 = n3;
                            }
                            else
                            {
                                int2 _237;
                                if (k == 7u)
                                {
                                    _237 = cur + int2(sgn * _23._step, 0);
                                }
                                else
                                {
                                    _237 = cur + int2(0, sgn * _23._step);
                                }
                                _229 = _237;
                            }
                            _221 = _229;
                        }
                        _213 = _221;
                    }
                    _205 = _213;
                }
                _197 = _205;
            }
            _190 = _197;
        }
        _183 = _190;
    }
    int2 c = _183;
    c = clamp(c, int2(-256), int2(256));
    int2 size = int2(uSource.get_width(), uSource.get_height());
    for (uint j = t; j < 512u; j += 72u)
    {
        uint jb = j / 64u;
        uint js = j % 64u;
        int2 sb = int2(int(min(((gl_WorkGroupID.x * 8u) + jb), uint(gsize.x - 1))), int(gl_WorkGroupID.y));
        s_src[jb][js] = uSource.read(uint2(min(((sb * int2(16)) + (int2(2) * int2(int(js & 7u), int(js >> uint(3))))), (size - int2(1)))), 0).x;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float2 off = float2(c) * 0.25;
    float sad = 0.0;
    for (int y = 0; y < 8; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            int2 p = min(((blk * int2(16)) + (int2(2) * int2(x, y))), (size - int2(1)));
            float2 param = float2(p) + off;
            sad += abs(s_src[b][(y * 8) + x] - search_sample(param, uReference, uReferenceSmplr, _23));
        }
    }
    float2 dv = abs(float2(c) - (float2(((n0 + n1) + n2) + n3) * 0.25));
    sad += (_23.lambda * (dv.x + dv.y));
    if (all(c == int2(0)))
    {
        sad -= _23.zero_bias;
    }
    s_cost[b][k] = sad;
    s_cand[b][k] = c;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if ((k == 0u) && live)
    {
        float best = s_cost[b][0];
        int2 bv = s_cand[b][0];
        for (int j_1 = 1; j_1 < 9; j_1++)
        {
            if (s_cost[b][j_1] < best)
            {
                best = s_cost[b][j_1];
                bv = s_cand[b][j_1];
            }
        }
        uOut.write(int4(bv, 0, 0), uint2(blk));
    }
}

