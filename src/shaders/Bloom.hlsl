#include "ShaderShared.h"

PUSH_CONSTANT BloomConstants BloomCB;

Texture2D<float3> InputTexture : register(t0);

// --- Prefilter ---

float3 SafeHDR(float3 c)
{
    return min(c, 65504.0); // Clamp to half-float max
}

float3 Prefilter(float3 color)
{
    float brightness = max(color.r, max(color.g, color.b));
    
    // Soft knee
    float soft = brightness + BloomCB.m_Knee;
    soft = clamp(soft, 0.0, 2.0 * BloomCB.m_Knee);
    soft = (soft * soft) / (4.0 * BloomCB.m_Knee + 1e-6);
    
    float contribution = max(soft, brightness);
    contribution /= max(brightness, 1e-4);
    
    return color * contribution;
}

float4 Prefilter_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[SAMPLER_LINEAR_CLAMP_INDEX];
    float3 color = InputTexture.SampleLevel(linearClampSampler, input.uv, 0).rgb;
    return float4(Prefilter(SafeHDR(color)) * BloomCB.m_Strength, 1.0);
}

// --- Downsample (Jimenez 13-tap) ---

float4 Downsample_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[SAMPLER_LINEAR_CLAMP_INDEX];
    float2 texelSize = 1.0 / float2(BloomCB.m_Width, BloomCB.m_Height);
    float2 uv = input.uv;

    // Jimenez 13-tap downsample pattern
    // Sampling at -1.0, 0.0, 1.0 in target texel space 
    // corresponds to -2.0, 0.0, 2.0 in source texel space.
    // Sampling at -0.5, 0.5 in target texel space
    // corresponds to -1.0, 1.0 in source texel space.

    float3 a = InputTexture.SampleLevel(linearClampSampler, uv + float2(-1, -1) * texelSize, 0).rgb;
    float3 b = InputTexture.SampleLevel(linearClampSampler, uv + float2( 0, -1) * texelSize, 0).rgb;
    float3 c = InputTexture.SampleLevel(linearClampSampler, uv + float2( 1, -1) * texelSize, 0).rgb;

    float3 d = InputTexture.SampleLevel(linearClampSampler, uv + float2(-0.5, -0.5) * texelSize, 0).rgb;
    float3 e = InputTexture.SampleLevel(linearClampSampler, uv + float2( 0.5, -0.5) * texelSize, 0).rgb;

    float3 f = InputTexture.SampleLevel(linearClampSampler, uv + float2(-1,  0) * texelSize, 0).rgb;
    float3 g = InputTexture.SampleLevel(linearClampSampler, uv + float2( 0,  0) * texelSize, 0).rgb;
    float3 h = InputTexture.SampleLevel(linearClampSampler, uv + float2( 1,  0) * texelSize, 0).rgb;

    float3 i = InputTexture.SampleLevel(linearClampSampler, uv + float2(-0.5,  0.5) * texelSize, 0).rgb;
    float3 j = InputTexture.SampleLevel(linearClampSampler, uv + float2( 0.5,  0.5) * texelSize, 0).rgb;

    float3 k = InputTexture.SampleLevel(linearClampSampler, uv + float2(-1,  1) * texelSize, 0).rgb;
    float3 l = InputTexture.SampleLevel(linearClampSampler, uv + float2( 0,  1) * texelSize, 0).rgb;
    float3 m = InputTexture.SampleLevel(linearClampSampler, uv + float2( 1,  1) * texelSize, 0).rgb;

    float3 result = g * 0.125;
    result += (a + c + k + m) * 0.03125;
    result += (b + f + h + l) * 0.0625;
    result += (d + e + i + j) * 0.125;

    return float4(result, 1.0);
}

// --- Upsample (9-tap tent filter) ---

Texture2D<float3> SourceTexture : register(t0); // Lower res mip (Up[i+1])
Texture2D<float3> BloomTexture  : register(t1); // Current res mip (Down[i])

float4 Upsample_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[SAMPLER_LINEAR_CLAMP_INDEX];
    float2 texelSize = 1.0 / float2(BloomCB.m_Width, BloomCB.m_Height);
    float d = BloomCB.m_UpsampleRadius;

    // 9-tap tent filter
    float3 a = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2(-d, -d) * texelSize, 0).rgb;
    float3 b = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2( 0, -d) * texelSize, 0).rgb;
    float3 c = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2( d, -d) * texelSize, 0).rgb;

    float3 d_ = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2(-d,  0) * texelSize, 0).rgb;
    float3 e = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2( 0,  0) * texelSize, 0).rgb;
    float3 f = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2( d,  0) * texelSize, 0).rgb;

    float3 g = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2(-d,  d) * texelSize, 0).rgb;
    float3 h = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2( 0,  d) * texelSize, 0).rgb;
    float3 i = SourceTexture.SampleLevel(linearClampSampler, input.uv + float2( d,  d) * texelSize, 0).rgb;

    float3 upsample = e * 0.25;
    upsample += (b + d_ + f + h) * 0.125;
    upsample += (a + c + g + i) * 0.0625;

    float3 bloom = BloomTexture.SampleLevel(linearClampSampler, input.uv, 0).rgb;

    return float4(bloom + upsample, 1.0);
}
