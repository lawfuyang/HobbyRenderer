#include "ShaderShared.h"

PUSH_CONSTANT
HZBFromDepthConstants g_HZBFromDepthConstants;

Texture2D<float> g_DepthBuffer : register(t0);
RWTexture2D<float> g_HZB : register(u0);

[numthreads(8,8,1)]
void HZBFromDepth_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 coord = dispatchThreadId.xy;
    if (coord.x >= g_HZBFromDepthConstants.m_Width || coord.y >= g_HZBFromDepthConstants.m_Height) return;
    float2 uv = (coord + 0.5) / float2(g_HZBFromDepthConstants.m_Width, g_HZBFromDepthConstants.m_Height);
    SamplerState minSampler = SamplerDescriptorHeap[SAMPLER_MIN_REDUCTION_INDEX];
    float depth = g_DepthBuffer.SampleLevel(minSampler, uv, 0);
    g_HZB[coord] = depth;
}
