#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>
#include <metal_atomic>

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

struct Stats
{
    uint sse_256;
};

kernel void riptide_inter_frame_mse(device Stats& stats [[buffer(0)]], texture2d<float> uReference [[texture(1)]], texture2d<float> uLastSource [[texture(2)]], sampler uReferenceSmplr [[sampler(1)]], sampler uLastSourceSmplr [[sampler(2)]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]])
{
    threadgroup spvUnsafeArray<uint, 64> s_sum;
    int2 size = int2(uReference.get_width(), uReference.get_height());
    uint t = gl_LocalInvocationIndex;
    int2 p0 = (int2(gl_WorkGroupID.xy) * int2(32)) + int2(int(t & 1u) * 16, int(t >> uint(1)));
    uint sum = 0u;
    for (int i = 0; i < 16; i++)
    {
        int2 p = p0 + int2(i, 0);
        if (all(p < size))
        {
            int d = int((uReference.read(uint2(p), 0).x * 255.0) + 0.5) - int((uLastSource.read(uint2(p), 0).x * 255.0) + 0.5);
            sum += uint(d * d);
        }
    }
    s_sum[t] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = 32u; s > 0u; s = s >> 1u)
    {
        if (t < s)
        {
            s_sum[t] += s_sum[t + s];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (t == 0u)
    {
        uint _153 = atomic_fetch_add_explicit((device atomic_uint*)&stats.sse_256, (s_sum[0] + 128u) >> uint(8), memory_order_relaxed);
    }
}

