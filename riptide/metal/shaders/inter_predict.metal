#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct Registers
{
    int ref_y;
};

static inline __attribute__((always_inline))
void cell(thread const int2& p, thread const int& shift, thread int2& i0, thread int2& f)
{
    int2 n = ((int2(2) * p) + int2(1)) - int2(1 << shift);
    i0 = n >> int2(shift + 1);
    f = n - (i0 << int2(shift + 1));
}

static inline __attribute__((always_inline))
int texel8(thread const float& v)
{
    return int((v * 255.0) + 0.5);
}

static inline __attribute__((always_inline))
int weight256(thread const float& v)
{
    float param = v;
    int t = texel8(param);
    return t + (t >> 7);
}

static inline __attribute__((always_inline))
int4 predict8_quad(thread const int2& p, thread const int& ref_y, texture2d<float> uReference, sampler uReferenceSmplr, texture2d<float> uWeights, sampler uWeightsSmplr, texture2d<int> uVectors, sampler uVectorsSmplr)
{
    int2 size = int2(uReference.get_width(), uReference.get_height());
    int2 wsize = int2(uWeights.get_width(), uWeights.get_height());
    int2 vsize = int2(uVectors.get_width(), uVectors.get_height());
    int wshift = ((size.x * 2) <= (wsize.x * 32)) ? 4 : 5;
    int vshift = wshift - 1;
    int2 param = p;
    int param_1 = vshift;
    int2 param_2;
    int2 param_3;
    cell(param, param_1, param_2, param_3);
    int2 vi = param_2;
    int2 vf = param_3;
    int2 param_4 = p;
    int param_5 = wshift;
    int2 param_6;
    int2 param_7;
    cell(param_4, param_5, param_6, param_7);
    int2 wi = param_6;
    int2 wf = param_7;
    int2 a = clamp(vi, int2(0), vsize - int2(1));
    int2 b = clamp(vi + int2(1), int2(0), vsize - int2(1));
    int2 v00 = uVectors.read(uint2(a), 0).xy;
    int2 v10 = uVectors.read(uint2(int2(b.x, a.y)), 0).xy;
    int2 v01 = uVectors.read(uint2(int2(a.x, b.y)), 0).xy;
    int2 v11 = uVectors.read(uint2(b), 0).xy;
    a = clamp(wi, int2(0), wsize - int2(1));
    b = clamp(wi + int2(1), int2(0), wsize - int2(1));
    float param_8 = uWeights.read(uint2(a), 0).x;
    int w00 = weight256(param_8);
    float param_9 = uWeights.read(uint2(int2(b.x, a.y)), 0).x;
    int w10 = weight256(param_9);
    float param_10 = uWeights.read(uint2(int2(a.x, b.y)), 0).x;
    int w01 = weight256(param_10);
    float param_11 = uWeights.read(uint2(b), 0).x;
    int w11 = weight256(param_11);
    int D = 2 << vshift;
    int E = 2 << wshift;
    int4 result;
    for (int k = 0; k < 4; k++)
    {
        int2 o = int2(k & 1, k >> 1);
        int2 q = p + o;
        int2 f = vf + (int2(2) * o);
        int2 v = (((v00 * int2(D - f.x)) + (v10 * int2(f.x))) * int2(D - f.y)) + (((v01 * int2(D - f.x)) + (v11 * int2(f.x))) * int2(f.y));
        int2 d = (q << int2(6)) + ((v + int2(2 << vshift)) >> int2(vshift + 2));
        int2 ip = (d >> int2(6)) + int2(0, ref_y);
        int2 fq = d & int2(63);
        float4 t = uReference.gather(uReferenceSmplr, ((float2(ip) + float2(1.0)) / float2(size)), int2(0), component::x).wzxy;
        float param_12 = t.x;
        float param_13 = t.y;
        float param_14 = t.z;
        float param_15 = t.w;
        int r = (((texel8(param_12) * (64 - fq.x)) + (texel8(param_13) * fq.x)) * (64 - fq.y)) + (((texel8(param_14) * (64 - fq.x)) + (texel8(param_15) * fq.x)) * fq.y);
        int r16 = (r + 128) >> 8;
        f = wf + (int2(2) * o);
        int w = (((w00 * (E - f.x)) + (w10 * f.x)) * (E - f.y)) + (((w01 * (E - f.x)) + (w11 * f.x)) * f.y);
        int w8 = (w + ((E * E) >> 1)) >> ((2 * wshift) + 2);
        int pred16 = 2040 + (((w8 * (r16 - 2040)) + 128) >> 8);
        result[k] = clamp((pred16 + 8) >> 4, 0, 255);
    }
    return result;
}

kernel void riptide_inter_predict(constant Registers& _495 [[buffer(0)]], texture2d<float> uReference [[texture(0)]], texture2d<float, access::write> uPrediction [[texture(1)]], texture2d<float> uWeights [[texture(2)]], texture2d<int> uVectors [[texture(3)]], sampler uReferenceSmplr [[sampler(0)]], sampler uWeightsSmplr [[sampler(2)]], sampler uVectorsSmplr [[sampler(3)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    int2 size = int2(uPrediction.get_width(), uPrediction.get_height());
    int2 p = int2(2) * int2(gl_GlobalInvocationID.xy);
    if (any(p >= size))
    {
        return;
    }
    int2 param = p;
    int param_1 = _495.ref_y;
    int4 pred = predict8_quad(param, param_1, uReference, uReferenceSmplr, uWeights, uWeightsSmplr, uVectors, uVectorsSmplr);
    for (int k = 0; k < 4; k++)
    {
        int2 o = int2(k & 1, k >> 1);
        if (all((p + o) < size))
        {
            uPrediction.write(float4(float(pred[k]) / 255.0), uint2((p + o)));
        }
    }
}

