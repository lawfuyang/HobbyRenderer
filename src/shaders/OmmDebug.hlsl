#include "Common.hlsli"
#include "srrhi/hlsl/OmmDebug.hlsli"

static const Texture2D<float4> SrcClass = srrhi::OmmDebugInputs::GetSrcClass();

SamplerState g_PointClampSampler : register(s0);

// Draws the texel classification of one baked alpha texture: 0 = opaque (green), 1 = transparent (red),
// 2 = mixed (yellow). Zoom and pan are the pass's viewport, and the "mixed only" filter is applied when the
// classification texture is uploaded, so the shader is a straight copy.
float4 PSMain(FullScreenVertexOut input) : SV_Target
{
    return float4(SrcClass.SampleLevel(g_PointClampSampler, input.uv, 0).rgb, 1.0f);
}
