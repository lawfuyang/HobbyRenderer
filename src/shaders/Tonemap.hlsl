#include "ShaderShared.h"

PUSH_CONSTANT TonemapConstants TonemapCB;

Texture2D<float3> HDRColorInput : register(t0);
StructuredBuffer<float> ExposureInput : register(t1);
Texture2D<float3> BloomInput : register(t2);

// Input color is non-negative and resides in the Linear Rec. 709 color space.
// Output color is also Linear Rec. 709, but in the [0, 1] range.
float3 PBRNeutralToneMapping(float3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d = 1. - startCompression;
    float newPeak = 1. - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
    return lerp(color, newPeak * float3(1, 1, 1), g);
}

float3 sRGB_OETF(float3 x)
{
    return saturate(select(
        x <= 0.0031308,
        x * 12.92,
        1.055 * pow(x, 1.0 / 2.4) - 0.055
    ));
}

float4 Tonemap_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[SAMPLER_LINEAR_CLAMP_INDEX];
    uint2 pixelPos = uint2(input.uv * float2(TonemapCB.m_Width, TonemapCB.m_Height));
    float3 color = HDRColorInput[pixelPos];
    float exposure = ExposureInput[0];
    
    float3 bloom = 0;
    if (TonemapCB.m_EnableBloom)
    {
        bloom = BloomInput.SampleLevel(linearClampSampler, input.uv, 0).rgb * TonemapCB.m_BloomIntensity;
    }

    if (TonemapCB.m_DebugBloom)
    {
        return float4(bloom, 1.0f);
    }

    // Apply exposure to both HDR color and bloom
    color = (color + bloom) * exposure;
    
    float3 tonemapped = PBRNeutralToneMapping(color);

    // Gamma correction
    tonemapped = sRGB_OETF(tonemapped);
    
    return float4(tonemapped, 1.0f);
}
