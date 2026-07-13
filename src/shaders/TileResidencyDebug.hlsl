#include "Common.hlsli"
#include "srrhi/hlsl/TileResidencyDebug.hlsli"

static const Texture2D<float4> SrcTex = srrhi::TileResidencyDebugInputs::GetSrcTexture();

SamplerState g_PointClampSampler : register(s0);

float4 PSMain(FullScreenVertexOut input) : SV_Target
{
    return SrcTex.SampleLevel(g_PointClampSampler, input.uv, 0);
}
