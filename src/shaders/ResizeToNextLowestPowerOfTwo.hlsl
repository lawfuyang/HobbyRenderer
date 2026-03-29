#include "srrhi/hlsl/ResizeToNextLowestPowerOfTwo.hlsli"

static const srrhi::ResizeToNextLowestPowerOfTwoConstants g_ResizeToNextLowestPowerOfTwoConstants = srrhi::DownsampleTextureToPow2Inputs::GetConstants();

static const Texture2D<RESIZETOPOW2_TYPE>  g_InTexture  = srrhi::DownsampleTextureToPow2Inputs::GetInputTexture();
static RWTexture2D<RESIZETOPOW2_TYPE>      g_OutTexture = srrhi::DownsampleTextureToPow2Inputs::GetOutputTexture();

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
