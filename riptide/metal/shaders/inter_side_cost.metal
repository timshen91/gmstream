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

// Implementation of signed integer mod accurate to SPIR-V specification
template<typename Tx, typename Ty>
inline Tx spvSMod(Tx x, Ty y)
{
    Tx remainder = x - y * (x / y);
    return select(Tx(remainder + y), remainder, remainder == 0 || (x >= 0) == (y >= 0));
}

// Implementation of the unsigned GLSL findMSB() function
template<typename T>
inline T spvFindUMSB(T x)
{
    return select(clz(T(0)) - (clz(x) + T(1)), T(-1), x == T(0));
}

template <typename ImageT>
void spvImageFence(ImageT img) { img.fence(); }

constant bool FreeIntraVectors_tmp [[function_constant(0)]];
constant bool FreeIntraVectors = is_function_constant_defined(FreeIntraVectors_tmp) ? FreeIntraVectors_tmp : true;

struct Registers
{
    uint packet_boundary;
    uint side_cap_bits;
};

struct Buckets
{
    uint count;
    uint consumed_payload;
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

kernel void riptide_inter_side_cost(constant Registers& _316 [[buffer(0)]], device Buckets& buckets [[buffer(4)]], texture2d<int, access::read_write> uVectors [[texture(1)]], texture2d<int> uVectorsPrev [[texture(2)]], texture2d<float> uWeights [[texture(3)]], sampler uVectorsPrevSmplr [[sampler(2)]], sampler uWeightsSmplr [[sampler(3)]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, 4096> s_tile_bits;
    threadgroup spvUnsafeArray<uint, 512> s_saved;
    threadgroup spvUnsafeArray<uint, 256> s_bits;
    threadgroup uint s_threshold;
    int2 size = int2(uVectors.get_width(), uVectors.get_height());
    int2 tiles = (size + int2(3)) / int2(4);
    int2 msize = int2(uWeights.get_width(), uWeights.get_height());
    uint bits = 0u;
    int _66 = int(gl_LocalInvocationIndex);
    for (int t = _66; t < (tiles.x * tiles.y); t += 256)
    {
        int2 base = int2(spvSMod(t, tiles.x), t / tiles.x) * int2(4);
        uint2 mx = uint2(0u);
        int n = 0;
        for (int y = base.y; y < min((base.y + 4), size.y); y++)
        {
            for (int x = base.x; x < min((base.x + 4), size.x); x++, n++)
            {
                int2 prev = uVectorsPrev.read(uint2(int2(x, y)), 0).xy;
                spvImageFence(uVectors);
                int2 v = uVectors.read(uint2(int2(x, y))).xy;
                bool _155 = FreeIntraVectors && any(v != prev);
                bool _170;
                if (_155)
                {
                    _170 = uWeights.read(uint2((int2(x, y) / int2(2))), 0).x == 0.0;
                }
                else
                {
                    _170 = _155;
                }
                if (_170)
                {
                    v = prev;
                    uVectors.write(int4(v, 0, 0), uint2(int2(x, y)));
                }
                int2 d = v - prev;
                int param = d.x;
                int param_1 = d.y;
                mx = max(mx, uint2(zigzag(param), zigzag(param_1)));
            }
        }
        uint2 width = uint2(int2(spvFindUMSB(mx)) + int2(1));
        int2 tm = min(int2(2), (msize - (base / int2(2))));
        uint tb = (uint(tm.x * tm.y) + 8u) + (uint(n) * (width.x + width.y));
        if (t < 4096)
        {
            s_tile_bits[t] = tb;
        }
        bits += tb;
    }
    for (uint b = gl_LocalInvocationIndex; b < 512u; b += 256u)
    {
        s_saved[b] = 0u;
    }
    s_bits[gl_LocalInvocationIndex] = bits;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = 128u; s > 0u; s = s >> 1u)
    {
        if (gl_LocalInvocationIndex < s)
        {
            s_bits[gl_LocalInvocationIndex] += s_bits[gl_LocalInvocationIndex + s];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    int count = tiles.x * tiles.y;
    if ((s_bits[0] > _316.side_cap_bits) && (count <= 4096))
    {
        int _328 = int(gl_LocalInvocationIndex);
        for (int t_1 = _328; t_1 < count; t_1 += 256)
        {
            int2 base_1 = int2(spvSMod(t_1, tiles.x), t_1 / tiles.x) * int2(4);
            int2 tm_1 = min(int2(2), (msize - (base_1 / int2(2))));
            uint frozen = uint(tm_1.x * tm_1.y) + 8u;
            uint _375 = atomic_fetch_add_explicit((threadgroup atomic_uint*)&s_saved[min(s_tile_bits[t_1], 511u)], s_tile_bits[t_1] - frozen, memory_order_relaxed);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (gl_LocalInvocationIndex == 0u)
        {
            uint total = s_bits[0];
            uint b_1 = 512u;
            for (;;)
            {
                bool _392 = b_1 > 0u;
                bool _399;
                if (_392)
                {
                    _399 = total > _316.side_cap_bits;
                }
                else
                {
                    _399 = _392;
                }
                if (_399)
                {
                    b_1--;
                    total -= s_saved[b_1];
                    continue;
                }
                else
                {
                    break;
                }
            }
            s_threshold = b_1;
            s_bits[0] = total;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        uint threshold = s_threshold;
        int _415 = int(gl_LocalInvocationIndex);
        for (int t_2 = _415; t_2 < count; t_2 += 256)
        {
            if (min(s_tile_bits[t_2], 511u) < threshold)
            {
                continue;
            }
            int2 base_2 = int2(spvSMod(t_2, tiles.x), t_2 / tiles.x) * int2(4);
            for (int y_1 = base_2.y; y_1 < min((base_2.y + 4), size.y); y_1++)
            {
                for (int x_1 = base_2.x; x_1 < min((base_2.x + 4), size.x); x_1++)
                {
                    uVectors.write(int4(uVectorsPrev.read(uint2(int2(x_1, y_1)), 0).xy, 0, 0), uint2(int2(x_1, y_1)));
                }
            }
            int2 tm_2 = min(int2(2), (msize - (base_2 / int2(2))));
            s_tile_bits[t_2] = uint(tm_2.x * tm_2.y) + 8u;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (gl_LocalInvocationIndex == 0u)
    {
        uint total_1 = 0u;
        if (count <= 4096)
        {
            uint open_bits = 0u;
            bool open = false;
            for (int t_3 = 0; t_3 < count; t_3++)
            {
                uint b_2 = s_tile_bits[t_3];
                bool _555;
                if (open)
                {
                    _555 = (8u + (4u * (((open_bits + b_2) + 31u) / 32u))) > _316.packet_boundary;
                }
                else
                {
                    _555 = open;
                }
                if (_555)
                {
                    total_1 += (8u + (4u * ((open_bits + 31u) / 32u)));
                    open = false;
                }
                if (!open)
                {
                    open_bits = 0u;
                    open = true;
                }
                open_bits += b_2;
            }
            if (open)
            {
                total_1 += (8u + (4u * ((open_bits + 31u) / 32u)));
            }
        }
        else
        {
            uint bytes = (s_bits[0] + 7u) / 8u;
            total_1 = bytes + (((bytes / max((_316.packet_boundary - 80u), 1u)) + 1u) * 80u);
        }
        uint _612 = atomic_fetch_add_explicit((device atomic_uint*)&buckets.consumed_payload, (total_1 + 3u) / 4u, memory_order_relaxed);
    }
}

