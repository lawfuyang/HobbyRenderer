#include "srrhi/hlsl/HDR.hlsli"

static const srrhi::HistogramConstants CB = srrhi::LuminanceHistogramInputs::GetHistogramConstants();

static const Texture2D<float3> HDRColor = srrhi::LuminanceHistogramInputs::GetHDRColor();
static const RWStructuredBuffer<uint> Histogram = srrhi::LuminanceHistogramInputs::GetHistogram();

groupshared uint LocalHistogram[256];

float GetLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint ColorToBin(float3 color)
{
    float lum = GetLuminance(color);
    if (lum < 0.0001f) return 0;
    
    float range = CB.m_MaxLogLuminance - CB.m_MinLogLuminance;
    float logLum = clamp((log2(lum) - CB.m_MinLogLuminance) / range, 0.0f, 1.0f);
    return (uint)(logLum * 254.0f + 1.0f);
}

[numthreads(16, 16, 1)]
void LuminanceHistogram_CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    uint threadIdx = gtid.y * 16 + gtid.x;
    LocalHistogram[threadIdx] = 0;
    
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < CB.m_Width && dtid.y < CB.m_Height)
    {
        float3 color = HDRColor[dtid.xy];
        uint bin = ColorToBin(color);
        InterlockedAdd(LocalHistogram[bin], 1);
    }

    GroupMemoryBarrierWithGroupSync();

    // Sum up local histogram to global buffer
    InterlockedAdd(Histogram[threadIdx], LocalHistogram[threadIdx]);
}
