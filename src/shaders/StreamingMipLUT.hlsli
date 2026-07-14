#ifndef STREAMING_MIP_LUT_HLSLI
#define STREAMING_MIP_LUT_HLSLI

#include "srrhi/hlsl/Common.hlsli"

static const float3 kStreamingMipLUT[srrhi::CommonConsts::MAX_MIP_COUNT] = {
    float3(1.0f, 1.0f, 1.0f),          // 0: white
    float3(1.0f, 0.25f, 0.25f),         // 1: light red
    float3(0.25f, 1.0f, 0.25f),         // 2: light green
    float3(0.25f, 0.25f, 1.0f),         // 3: light blue
    float3(1.0f, 0.25f, 1.0f),          // 4: light magenta
    float3(1.0f, 1.0f, 0.25f),          // 5: light yellow
    float3(0.25f, 1.0f, 1.0f),          // 6: light cyan
    float3(0.9f, 0.5f, 0.2f),           // 7: orange
    float3(0.59f, 0.48f, 0.8f),         // 8: dark magenta
    float3(0.53f, 0.25f, 0.11f),        // 9
    float3(0.8f, 0.48f, 0.53f),         // 10
    float3(0.64f, 0.8f, 0.48f),         // 11
    float3(0.48f, 0.75f, 0.8f),         // 12
    float3(0.5f, 0.25f, 0.75f),         // 13
    float3(0.99f, 0.68f, 0.42f),        // 14
    float3(0.4f, 0.5f, 0.6f),           // 15
};

// Returns the debug color for a given mip level (0–15).
// Mip >= 16 returns olive-ish green.
float3 GetStreamingMipLUTColor(int mipLevel)
{
    if (mipLevel > 15)
        return float3(0.3f, 0.4f, 0.2f);
    else
        return kStreamingMipLUT[max(mipLevel, 0)];
}

#endif // STREAMING_MIP_LUT_HLSLI
