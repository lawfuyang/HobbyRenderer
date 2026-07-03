#include "Common.hlsli"

#include "srrhi/hlsl/Common.hlsli"
#include "srrhi/hlsl/HDR.hlsli"

static const srrhi::TonemapConstants TonemapCB = srrhi::TonemappingInputs::GetTonemapConstants();

static const Texture2D<float3> HDRColorInput = srrhi::TonemappingInputs::GetHDRColorInput();
static const Buffer<float> ExposureInput = srrhi::TonemappingInputs::GetExposureInput();

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
    uint2 pixelPos = uint2(input.uv * float2(TonemapCB.m_Width, TonemapCB.m_Height));
    float3 color = HDRColorInput[pixelPos];
    float exposure = ExposureInput[0];

    // Apply exposure
    color = color * exposure;
    
    float3 tonemapped = PBRNeutralToneMapping(color);

    // Gamma correction
    tonemapped = sRGB_OETF(tonemapped);
    
    return float4(tonemapped, 1.0f);
}

// ═══════════════════════════════════════════════════════════════
// HDR scRGB Tonemapping
// ═══════════════════════════════════════════════════════════════
// Input:  linear Rec.709 color * exposure (scRGB units)
// Output: linear scRGB values in [0, maxSCRGB]
// 1.0 scRGB = 80 nits (SDR white). Values > 1.0 are HDR.
// SDR content (≤ 1.0) passes through untouched.
// HDR content is compressed via Reinhard rolloff toward display peak.

float3 HDRDisplayTonemap(float3 x, float MaxDisplayNits)
{
    float maxSCRGB = MaxDisplayNits / 80.0;

    float lum = max(x.r, max(x.g, x.b));

    // SDR passthrough: no compression at or below 80 nits
    if (lum <= 1.0)
        return x;

    // Compress only the headroom above SDR white
    float hdrHeadroom = maxSCRGB - 1.0;
    float excess = lum - 1.0;
    float compressedExcess = excess * hdrHeadroom / (excess + hdrHeadroom);

    float newLum = 1.0 + compressedExcess;
    float3 result = x * (newLum / lum);

    // Safety clamp: never exceed display max
    return min(result, maxSCRGB);
}

float4 TonemapHDR_PSMain(FullScreenVertexOut input) : SV_Target
{
    uint2 pixelPos = uint2(input.uv * float2(TonemapCB.m_Width, TonemapCB.m_Height));
    float3 color = HDRColorInput[pixelPos];
    float exposure = ExposureInput[0];

    // Apply exposure
    color = color * exposure;

    float3 hdrOutput = HDRDisplayTonemap(color, TonemapCB.m_MaxDisplayNits);

    // No gamma correction — scRGB is linear
    return float4(hdrOutput, 1.0f);
}
