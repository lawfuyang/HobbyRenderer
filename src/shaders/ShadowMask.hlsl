#include "Common.hlsli"
#include "CommonLighting.hlsli"
#include "CommonShadow.hlsli"
#include "srrhi/hlsl/Common.hlsli"
#include "srrhi/hlsl/ShadowMask.hlsli"

static const srrhi::ShadowMaskConstants g_CB  = srrhi::ShadowMaskInputs::GetCB();
static const Texture2D<float>           g_Depth         = srrhi::ShadowMaskInputs::GetDepth();
static const Texture2D<float2>          g_GBufferNormals = srrhi::ShadowMaskInputs::GetGBufferNormals();
static const Texture2DArray<float>      g_CSMShadowMap  = srrhi::ShadowMaskInputs::GetCSMShadowMap();
static       RWTexture2D<float>         g_RWShadowMask  = srrhi::ShadowMaskInputs::GetRWShadowMask();
static const SamplerComparisonState     g_ShadowSampler = srrhi::ShadowMaskInputs::GetShadowSampler();
static const SamplerState               g_PointSampler  = srrhi::ShadowMaskInputs::GetPointSampler();

[numthreads(8, 8, 1)]
void ShadowMask_CSMain(uint3 dispatchID : SV_DispatchThreadID)
{
    uint2 uvInt = dispatchID.xy;

    // Bounds check
    if (any(uvInt >= uint2(g_CB.m_OutputSize)))
        return;

    float depth = g_Depth.Load(uint3(uvInt, 0));

    // Sky / background (reversed-Z: far = 0) → fully lit
    if (depth == srrhi::CommonConsts::DEPTH_FAR)
    {
        g_RWShadowMask[uvInt] = 1.0f;
        return;
    }

    // Reconstruct world position from depth
    float2 uv = (float2(uvInt) + 0.5f) / g_CB.m_OutputSize;
    float4 clipPos = float4(UVToClipXY(uv), depth, 1.0f);
    float4 worldPos4 = mul(clipPos, g_CB.m_ClipToWorld);
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // Decode world-space normal from GBuffer (octahedral RG16_FLOAT encoding)
    float2 encodedNormal = g_GBufferNormals.Load(uint3(uvInt, 0)).rg;
    float3 worldNormal = DecodeNormal(encodedNormal);

    // Reconstruct view-space depth for cascade selection
    // View space: camera looks down -Z, so negate the Z component
    float4 viewPos4 = mul(float4(worldPos, 1.0f), g_CB.m_WorldToView);
    float viewDepth = -viewPos4.z;

    float texelSize = 1.0f / (float)srrhi::CommonConsts::kShadowMapResolution;

    float shadow = ComputeCSMShadow(
        worldPos,
        worldNormal,
        viewDepth,
        g_CSMShadowMap,
        g_ShadowSampler,
        g_PointSampler,
        g_CB.m_ShadowViewProj,
        g_CB.m_CascadeSplits,
        g_CB.m_NormalBias,
        texelSize,
        g_CB.m_EnableCascadeBlend
    );

    g_RWShadowMask[uvInt] = shadow;
}
