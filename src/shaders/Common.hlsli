#ifndef COMMON_HLSLI
#define COMMON_HLSLI

#include "srrhi/hlsl/Common.hlsli"

struct FullScreenVertexOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

uint32_t DivideAndRoundUp(uint32_t dividend, uint32_t divisor)
{
    return (dividend + divisor - 1) / divisor;
}

// A stable colour per id, for the views that colour geometry by an index rather than by its material.
float3 HashColor(uint id)
{
    uint h = id * 0x27D4EB2Du;
    h = h ^ (h >> 15);
    h = h * 0x85EBCA6Bu;
    h = h ^ (h >> 13);
    h = h * 0xC2B2AE35u;
    h = h ^ (h >> 16);
    return float3((h & 0xFF) / 255.0f, ((h >> 8) & 0xFF) / 255.0f, ((h >> 16) & 0xFF) / 255.0f);
}

// Standard MatrixMultiply helper to enforce consistent multiplication order: mul(vector, matrix)
float4 MatrixMultiply(float4 v, float4x4 m)
{
    return mul(v, m);
}

float3 MatrixMultiply(float3 v, float3x3 m)
{
    return mul(v, m);
}

float GetMaxScale(float4x4 m)
{
    return max(length(m[0].xyz), max(length(m[1].xyz), length(m[2].xyz)));
}

float3x3 MakeAdjugateMatrix(float4x4 m)
{
    return float3x3
    (
        cross(m[1].xyz, m[2].xyz),
        cross(m[2].xyz, m[0].xyz),
        cross(m[0].xyz, m[1].xyz)
    );
}

float3 TransformNormal(float3 normal, float4x4 worldMatrix)
{
    float3x3 adjugateWorldMatrix = MakeAdjugateMatrix(worldMatrix);
    return normalize(MatrixMultiply(normal, adjugateWorldMatrix));
}

// Convert a UV to clip space coordinates (XY: [-1, 1])
float2 UVToClipXY(float2 uv)
{
    return uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
}

// Convert a clip position after projection and perspective divide to a UV
float2 ClipXYToUV(float2 xy)
{
    return xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
}

// Project a view-space position (+Z forward) to UV and output the linear depth in clipW.
float2 ProjectViewToUV(float3 viewPos, float4x4 matViewToClip, out float clipW)
{
    float4 clip = MatrixMultiply(float4(viewPos, 1.0f), matViewToClip);
    clipW = clip.w;
    return ClipXYToUV(clip.xy / clip.w);
}

// Tangent-frame transforms: convert between world space and local (T, B, N) space.
float3 TangentToLocal(float3 T, float3 B, float3 N, float3 V)
{
    return float3(dot(V, T), dot(V, B), dot(V, N));
}

float3 TangentToWorld(float3 T, float3 B, float3 N, float3 V)
{
    return V.x * T + V.y * B + V.z * N;
}

// Sample blue noise: kBlueNoiseSize x kBlueNoiseSize RG texture.
// Samples twice with per-frame offsets to get 4 decorrelated channels in [0,1], then
// animates the values with the golden-ratio sequence
// [Heitz & Belcour 2019, "Distributing Monte Carlo Errors as a Blue Noise in Screen Space"].
//
// The position shift alone is NOT enough: the per-frame strides are taken modulo the
// texture size (9491 % 64 == 19, 7459 % 64 == 35, ...), all coprime with 64, so the
// 4-tuple sequence repeats exactly every kBlueNoiseSize frames. A temporal accumulator fed
// by it converges to the mean of a fixed 64-sample set — i.e. it freezes on a spatially
// noisy estimate instead of converging to a smooth one.
// The four irrational increments extend the period to kBlueNoiseFrameCycle while preserving
// the blue-noise spatial spectrum (they are pure per-pixel value offsets).
float4 SampleBlueNoise(Texture2D<float4> blueNoise, uint2 pixelPos, uint frameIndex)
{
    const uint mask = srrhi::CommonConsts::kBlueNoiseSize - 1u;
    uint2 p0 = (pixelPos + frameIndex * uint2(9491u, 7459u)) & mask;
    uint2 p1 = (pixelPos + frameIndex * uint2(5851u, 3917u) + uint2(31u, 17u)) & mask;
    float2 a = blueNoise.Load(int3(p0, 0)).rg;
    float2 b = blueNoise.Load(int3(p1, 0)).rg;

    // Wrapped so the float multiply below keeps enough mantissa to stay well distributed.
    const uint  kBlueNoiseFrameCycle = 4096u;
    const float4 kGoldenRatioSequence = float4(0.618033988749895f, 0.324717957244746f,
                                               0.220744084605760f, 0.167303978261419f);
    float cycleIndex = float(frameIndex & (kBlueNoiseFrameCycle - 1u));

    return frac(float4(a, b) + kGoldenRatioSequence * cycleIndex);
}

// Catmull-Rom bicubic texture sampling: 9 bilinear taps over a 4x4 texel footprint.
// tex: texture to sample, samp: linear sampler, uv: [0,1] UV, resolution: texture dimensions.
float4 SampleTextureCatmullRom(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 resolution)
{
    float2 samplePos = uv * resolution;
    float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;

    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);

    float2 texPos0 = texPos1 - 1.0f;
    float2 texPos3 = texPos1 + 2.0f;
    float2 texPos12 = texPos1 + offset12;

    float2 texPos1Uv = texPos1 / resolution;
    float2 texPos2Uv = (texPos1 + 1.0f) / resolution;

    texPos0 /= resolution;
    texPos3 /= resolution;
    texPos12 /= resolution;

    float4 result = 0.0f;
    result += tex.SampleLevel(samp, float2(texPos0.x, texPos0.y), 0.0f) * w0.x * w0.y;
    result += tex.SampleLevel(samp, float2(texPos12.x, texPos0.y), 0.0f) * w12.x * w0.y;
    result += tex.SampleLevel(samp, float2(texPos3.x, texPos0.y), 0.0f) * w3.x * w0.y;
    result += tex.SampleLevel(samp, float2(texPos0.x, texPos12.y), 0.0f) * w0.x * w12.y;
    result += tex.SampleLevel(samp, float2(texPos12.x, texPos12.y), 0.0f) * w12.x * w12.y;
    result += tex.SampleLevel(samp, float2(texPos3.x, texPos12.y), 0.0f) * w3.x * w12.y;
    result += tex.SampleLevel(samp, float2(texPos0.x, texPos3.y), 0.0f) * w0.x * w3.y;
    result += tex.SampleLevel(samp, float2(texPos12.x, texPos3.y), 0.0f) * w12.x * w3.y;
    result += tex.SampleLevel(samp, float2(texPos3.x, texPos3.y), 0.0f) * w3.x * w3.y;

    // Anti-ringing. The Catmull-Rom kernel has negative lobes (w0/w3 are negative for most
    // fractional offsets), so it overshoots at high-contrast edges. Harmless for a one-off
    // resample, but when it is used to resample a temporal history that is then fed back into
    // itself at a jittered sub-pixel offset every frame, the overshoot compounds geometrically
    // and blows up into bright fireflies around bright features (emissive geometry especially).
    // Clamping to the 2x2 bilinear footprint bounds the result to values that actually exist in
    // the neighbourhood, which removes the overshoot without softening the filter.
    float4 c00 = tex.SampleLevel(samp, float2(texPos1Uv.x, texPos1Uv.y), 0.0f);
    float4 c10 = tex.SampleLevel(samp, float2(texPos2Uv.x, texPos1Uv.y), 0.0f);
    float4 c01 = tex.SampleLevel(samp, float2(texPos1Uv.x, texPos2Uv.y), 0.0f);
    float4 c11 = tex.SampleLevel(samp, float2(texPos2Uv.x, texPos2Uv.y), 0.0f);

    float4 neighborhoodMin = min(min(c00, c10), min(c01, c11));
    float4 neighborhoodMax = max(max(c00, c10), max(c01, c11));

    return clamp(max(result, 0.0f), neighborhoodMin, neighborhoodMax);
}

// Reconstruct world-space position from a UV coordinate, depth value, and clip-to-world matrix.
float3 ReconstructWorldPos(float2 uv, float depth, float4x4 matClipToWorld)
{
    float2 clipXY  = UVToClipXY(uv);
    float4 worldH = MatrixMultiply(float4(clipXY, depth, 1.0f), matClipToWorld);
    return worldH.xyz / worldH.w;
}

float3 DecodeOct(float2 e)
{
    float3 v = float3(e, 1.0f - abs(e.x) - abs(e.y));
    float t = max(-v.z, 0.0f);
    v.x += v.x >= 0.0f ? -t : t;
    v.y += v.y >= 0.0f ? -t : t;
    return normalize(v);
}

// Unpacks a 2 channel normal to xyz
float3 TwoChannelNormalX2(float2 normal)
{
    float2 xy = 2.0f * normal - 1.0f;
    float z = sqrt(saturate(1.0f - dot(xy, xy)));
    return float3(xy.x, xy.y, z);
}

float3 TransformNormalWithTBN(float2 nmSample, float3 normal, float3 tangent, float tangentSign)
{
    float3 normalMap = TwoChannelNormalX2(nmSample);
    float3 n_w = normalize(normal);
    float3 t_w = normalize(tangent);
    t_w = normalize(t_w - n_w * dot(t_w, n_w));
    float3 b_w = normalize(cross(n_w, t_w) * tangentSign);
    float3x3 TBN = float3x3(t_w, b_w, n_w);
    return normalize(MatrixMultiply(normalMap, TBN));
}

static const float3 kEarthCenter = float3(0.0f, -6360000.0f, 0.0f);

#endif // COMMON_HLSLI
