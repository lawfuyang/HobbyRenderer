#include "ShaderShared.h"

PUSH_CONSTANT
ResizeToNextLowestPowerOfTwoConstants g_ResizeToNextLowestPowerOfTwoConstants;

#if NUM_CHANNELS == 1
    typedef float ChannelType;
#elif NUM_CHANNELS == 3
    typedef float3 ChannelType;
#endif

Texture2D<ChannelType> g_InTexture : register(t0);

VK_IMAGE_FORMAT_UNKNOWN
RWTexture2D<ChannelType> g_OutTexture : register(u0);

[numthreads(8,8,1)]
void CS_ResizeToNextLowestPowerOfTwo(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 coord = dispatchThreadId.xy;
    if (coord.x >= g_ResizeToNextLowestPowerOfTwoConstants.m_Width || coord.y >= g_ResizeToNextLowestPowerOfTwoConstants.m_Height)
        return;

    float2 uv = (coord + 0.5) / float2(g_ResizeToNextLowestPowerOfTwoConstants.m_Width, g_ResizeToNextLowestPowerOfTwoConstants.m_Height);
    SamplerState inSampler = SamplerDescriptorHeap[g_ResizeToNextLowestPowerOfTwoConstants.m_SamplerIdx];
    g_OutTexture[coord] = g_InTexture.SampleLevel(inSampler, uv, 0);
}
