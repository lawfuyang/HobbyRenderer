#include "ShaderShared.h"

cbuffer HZBFromDepthCB : register(b0)
{
    HZBFromDepthConstants g_HZBFromDepthConstants;
};

Texture2D<float> g_DepthBuffer : register(t0);
SamplerState g_MaxSampler : register(s0);
RWTexture2D<float> g_HZB : register(u0);

[numthreads(8,8,1)]
void HZBFromDepth_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 coord = dispatchThreadId.xy;
    if (coord.x >= g_HZBFromDepthConstants.m_Width || coord.y >= g_HZBFromDepthConstants.m_Height) return;
    float2 uv = (coord + 0.5) / float2(g_HZBFromDepthConstants.m_Width, g_HZBFromDepthConstants.m_Height);
    float depth = g_DepthBuffer.SampleLevel(g_MaxSampler, uv, 0);
    g_HZB[coord] = depth;
}
