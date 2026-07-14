#include "Common.hlsli"
#include "srrhi/hlsl/TileResidencyDebug.hlsli"
#include "StreamingMipLUT.hlsli"

static const Texture2D<float4> SrcTex = srrhi::TileResidencyDebugInputs::GetSrcTexture();
static const Texture2D<uint>   MinMipTex = srrhi::TileResidencyDebugInputs::GetMinMipTexture();

SamplerState g_PointClampSampler : register(s0);

float4 PSMain(FullScreenVertexOut input) : SV_Target
{
    return SrcTex.SampleLevel(g_PointClampSampler, input.uv, 0);
}

float4 MinMipPSMain(FullScreenVertexOut input) : SV_Target
{
    uint minMip = MinMipTex.SampleLevel(g_PointClampSampler, input.uv, 0);
    return float4(GetStreamingMipLUTColor((int)minMip), 1.0f);
}
