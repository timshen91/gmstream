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

// Implementation of the unsigned GLSL findMSB() function
template<typename T>
inline T spvFindUMSB(T x)
{
    return select(clz(T(0)) - (clz(x) + T(1)), T(-1), x == T(0));
}

template <typename ImageT>
void spvImageFence(ImageT img) { img.fence(); }

struct Registers
{
    float scale;
};

struct Stats
{
    uint sse_256;
};

static inline __attribute__((always_inline))
uint zigzag(thread const int& d)
{
    uint _18;
    if (d >= 0)
    {
        _18 = uint(2 * d);
    }
    else
    {
        _18 = uint(((-2) * d) - 1);
    }
    return _18;
}

kernel void riptide_inter_vector_rd(constant Registers& _315 [[buffer(0)]], const device Stats& stats [[buffer(6)]], texture2d<int, access::read_write> uVectors [[texture(1)]], texture2d<int> uVectorsPrev [[texture(2)]], texture2d<float> uSource [[texture(3)]], texture2d<float> uReference [[texture(4)]], texture2d<float> uWeights [[texture(5)]], sampler uVectorsPrevSmplr [[sampler(2)]], sampler uSourceSmplr [[sampler(3)]], sampler uReferenceSmplr [[sampler(4)]], sampler uWeightsSmplr [[sampler(5)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<int2, 16> s_vp;
    threadgroup spvUnsafeArray<int2, 16> s_v;
    threadgroup spvUnsafeArray<float, 256> s_d;
    int2 vsize = int2(uVectors.get_width(), uVectors.get_height());
    int2 size = int2(uSource.get_width(), uSource.get_height());
    int2 tile = int2(gl_WorkGroupID.xy) * int2(4);
    uint t = gl_LocalInvocationIndex;
    if (t < 16u)
    {
        int2 vb = min((tile + int2(int(t & 3u), int(t >> uint(2)))), (vsize - int2(1)));
        s_vp[t] = uVectorsPrev.read(uint2(vb), 0).xy;
        int2 _117;
        if (uWeights.read(uint2((vb / int2(2))), 0).x == 0.0)
        {
            _117 = s_vp[t];
        }
        else
        {
            spvImageFence(uVectors);
            _117 = uVectors.read(uint2(vb)).xy;
        }
        s_v[t] = _117;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float dsse = 0.0;
    for (int k = 0; k < 4; k++)
    {
        uint s = (t * 4u) + uint(k);
        int2 g = int2(int(s & 31u), int(s >> uint(5)));
        int2 p = (tile * int2(16)) + (int2(2) * g);
        int2 vb_1 = tile + (g / int2(8));
        bool _179 = any(p >= size);
        bool _187;
        if (!_179)
        {
            _187 = any(vb_1 >= vsize);
        }
        else
        {
            _187 = _179;
        }
        if (_187)
        {
            continue;
        }
        int j = ((g.y / 8) * 4) + (g.x / 8);
        float2 inv = float2(1.0) / float2(int2(uReference.get_width(), uReference.get_height()));
        float src = uSource.read(uint2(p), 0).x * 255.0;
        float a = uReference.sample(uReferenceSmplr, (((float2(p) + (float2(s_v[j]) * 0.25)) + float2(0.5)) * inv), level(0.0)).x * 255.0;
        float b = uReference.sample(uReferenceSmplr, (((float2(p) + (float2(s_vp[j]) * 0.25)) + float2(0.5)) * inv), level(0.0)).x * 255.0;
        dsse += (((src - a) * (src - a)) - ((src - b) * (src - b)));
    }
    s_d[t] = dsse;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s_1 = 128u; s_1 > 0u; s_1 = s_1 >> 1u)
    {
        if (t < s_1)
        {
            s_d[t] += s_d[t + s_1];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    bool _310 = t == 0u;
    bool _320;
    if (_310)
    {
        _320 = _315.scale > 0.0;
    }
    else
    {
        _320 = _310;
    }
    if (_320)
    {
        uint2 mx = uint2(0u);
        int n = 0;
        for (int y = tile.y; y < min((tile.y + 4), vsize.y); y++)
        {
            for (int x = tile.x; x < min((tile.x + 4), vsize.x); x++, n++)
            {
                int j_1 = ((y - tile.y) * 4) + (x - tile.x);
                int2 d = s_v[j_1] - s_vp[j_1];
                int param = d.x;
                int param_1 = d.y;
                mx = max(mx, uint2(zigzag(param), zigzag(param_1)));
            }
        }
        float bits = float(n) * float(((int(spvFindUMSB(mx.x)) + 1) + int(spvFindUMSB(mx.y))) + 1);
        float mse = (float(stats.sse_256) * 256.0) / float(size.x * size.y);
        float price = (_315.scale * 1.38600003719329833984375) * fast::max(mse, 1.0);
        if (((4.0 * s_d[0]) + (price * bits)) >= 0.0)
        {
            for (int y_1 = tile.y; y_1 < min((tile.y + 4), vsize.y); y_1++)
            {
                for (int x_1 = tile.x; x_1 < min((tile.x + 4), vsize.x); x_1++)
                {
                    uVectors.write(int4(s_vp[((y_1 - tile.y) * 4) + (x_1 - tile.x)], 0, 0), uint2(int2(x_1, y_1)));
                }
            }
        }
    }
}

