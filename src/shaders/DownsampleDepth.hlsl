#include "ShaderShared.h"

cbuffer DownsampleCB : register(b0)
{
    DownsampleConstants g_DownsampleConstants;
};

SamplerState g_MinReductionSampler : register(s0);
Texture2D<float> g_HZBMipIn : register(t0);
RWTexture2D<float> g_HZBMipOut : register(u0);

[numthreads(8,8,1)]
void Downsample_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 coord = dispatchThreadId.xy;

    if (coord.x >= g_DownsampleConstants.m_OutputWidth || coord.y >= g_DownsampleConstants.m_OutputHeight)
    {
        return;
    }

    float value = g_HZBMipIn.SampleLevel(g_MinReductionSampler, (coord + 0.5) / float2(g_DownsampleConstants.m_OutputWidth, g_DownsampleConstants.m_OutputHeight), 0);
    g_HZBMipOut[coord] = value;
}
