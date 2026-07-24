#include "Common.hlsli"
#include "CommonLighting.hlsli"
#include "CommonShadow.hlsli"
#include "srrhi/hlsl/Common.hlsli"
#include "srrhi/hlsl/ShadowMask.hlsli"

static const srrhi::ShadowMaskConstants g_CB             = srrhi::ShadowMaskInputs::GetCB();
static const Texture2D<float>           g_Depth          = srrhi::ShadowMaskInputs::GetDepth();
static const Texture2D<float2>          g_GBufferNormals = srrhi::ShadowMaskInputs::GetGBufferNormals();
static const Texture2DArray<float>      g_CSMShadowMap     = srrhi::ShadowMaskInputs::GetCSMShadowMap();
static const Texture2DArray<float>      g_CSMShadowMapMips = srrhi::ShadowMaskInputs::GetCSMShadowMapMips();
static const Texture2D<float2>          g_BlueNoiseTex     = srrhi::ShadowMaskInputs::GetBlueNoiseTex();
static const Texture2D<float4>          g_MotionVectors  = srrhi::ShadowMaskInputs::GetMotionVectors();
static       RWTexture2D<float>         g_RWShadowMask   = srrhi::ShadowMaskInputs::GetRWShadowMask();
static       RWTexture2D<float>         g_RWShadowHistory = srrhi::ShadowMaskInputs::GetRWShadowHistory();
static       RWTexture2D<float4>        g_RWDebugOutput  = srrhi::ShadowMaskInputs::GetRWDebugOutput();
static const SamplerComparisonState     g_ShadowSampler       = srrhi::ShadowMaskInputs::GetShadowSampler();
static const SamplerState               g_ShadowSamplerPoint  = srrhi::ShadowMaskInputs::GetShadowSamplerPoint();
static const SamplerState               g_SamplerMinReduction = srrhi::ShadowMaskInputs::GetSamplerMinReduction();
static const SamplerState               g_SamplerLinearClamp  = srrhi::ShadowMaskInputs::GetSamplerLinearClamp();

// ---------------------------------------------------------------------------
// Dispatch 1 — PCSS or fixed PCF shadow mask
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void ShadowMask_CSMain(uint3 dispatchID : SV_DispatchThreadID)
{
    uint2 uvInt = dispatchID.xy;

    if (any(uvInt >= uint2(g_CB.m_OutputSize)))
        return;

    float depth = g_Depth.Load(uint3(uvInt, 0));

    // Sky / background (reversed-Z: far = 0) → fully lit
    if (depth == srrhi::CommonConsts::DEPTH_FAR)
    {
        g_RWShadowMask[uvInt] = 1.0f;
        return;
    }

    // Reconstruct world position
    float2 uv        = (float2(uvInt) + 0.5f) / g_CB.m_OutputSize;
    float4 clipPos   = float4(UVToClipXY(uv), depth, 1.0f);
    float4 worldPos4 = mul(clipPos, g_CB.m_ClipToWorld);
    float3 worldPos  = worldPos4.xyz / worldPos4.w;

    // Decode world-space normal
    float2 encodedNormal = g_GBufferNormals.Load(uint3(uvInt, 0)).rg;
    float3 worldNormal   = DecodeNormal(encodedNormal);

    // View-space depth for cascade selection
    float4 viewPos4 = mul(float4(worldPos, 1.0f), g_CB.m_WorldToView);
    float  viewDepth = viewPos4.z;

    float texelSize = 1.0f / (float)srrhi::CommonConsts::kShadowMapResolution;

    float shadow;
    float dbgBlockerDepth   = 0.0f;
    float dbgPenumbraRadius = 0.0f;
    float dbgEarlyOut       = 0.0f;
#if PCSS
    shadow = ComputePCSSShadow(
        worldPos,
        worldNormal,
        viewDepth,
        g_CSMShadowMap,
        g_CSMShadowMapMips,
        g_ShadowSampler,
        g_ShadowSamplerPoint,
        g_SamplerMinReduction,
        g_BlueNoiseTex,
        g_CB.m_ShadowViewProj,
        g_CB.m_CascadeSplits,
        g_CB.m_SearchRadii,
        g_CB.m_LightAngularRadius,
        g_CB.m_NormalBias,
        texelSize,
        g_CB.m_FrameIndex,
        uvInt,
        g_CB.m_EnableCascadeBlend,
        dbgBlockerDepth,
        dbgPenumbraRadius,
        dbgEarlyOut
    );
#else
    shadow = ComputeCSMShadow(
        worldPos,
        worldNormal,
        viewDepth,
        g_CSMShadowMap,
        g_ShadowSampler,
        g_CB.m_ShadowViewProj,
        g_CB.m_CascadeSplits,
        g_CB.m_NormalBias,
        texelSize,
        g_CB.m_EnableCascadeBlend
    );
#endif

    g_RWShadowMask[uvInt] = shadow;

    // Write PCSS debug data when a PCSS debug mode is active
    if (g_CB.m_CSMDebugMode >= 9u)
        g_RWDebugOutput[uvInt] = float4(dbgBlockerDepth, dbgPenumbraRadius, dbgEarlyOut, shadow);
}

// ---------------------------------------------------------------------------
// Dispatch 2 — Temporal shadow history resolve
// Reads the raw stochastic mask from g_RWShadowMask (written by dispatch 1),
// reprojects using motion vectors, neighbourhood-clamps the history, blends,
// and writes the final mask + updated history.
// ---------------------------------------------------------------------------
static const float kDisocclusionDepthEps = 0.01f; // depth difference threshold for disocclusion

[numthreads(8, 8, 1)]
void ShadowMaskTemporal_CSMain(uint3 dispatchID : SV_DispatchThreadID)
{
    uint2 uvInt = dispatchID.xy;

    if (any(uvInt >= uint2(g_CB.m_OutputSize)))
        return;

    float current = g_RWShadowMask[uvInt];

    // --- Neighbourhood min/max clamp (3x3 of current raw mask) ---
    float neighbourMin = 1.0f;
    float neighbourMax = 0.0f;
    [unroll]
    for (int nx = -1; nx <= 1; ++nx)
    {
        [unroll]
        for (int ny = -1; ny <= 1; ++ny)
        {
            int2 coord = (int2)uvInt + int2(nx, ny);
            // clamp to screen bounds
            coord = clamp(coord, int2(0, 0), (int2)uint2(g_CB.m_OutputSize) - 1);
            float s = g_RWShadowMask[coord];
            neighbourMin = min(neighbourMin, s);
            neighbourMax = max(neighbourMax, s);
        }
    }

    // --- Reproject using motion vectors ---
    // Motion vectors are stored as (dx, dy) in UV space (RGBA16_FLOAT, using RG).
    float2 motionUV = g_MotionVectors.Load(uint3(uvInt, 0)).rg;
    float2 prevUV   = (float2(uvInt) + 0.5f) / g_CB.m_OutputSize + motionUV;

    // Disocclusion check: if reprojected UV is out of screen, skip history
    bool bValidHistory = all(prevUV >= 0.0f) && all(prevUV <= 1.0f);

    float blended;
    if (bValidHistory)
    {
        // Sample history with linear clamp
        float historyVal = g_RWShadowHistory[uvInt].r;

        // Neighbourhood clamp: prevent ghosting from stale history
        historyVal = clamp(historyVal, neighbourMin, neighbourMax);

        const float kTemporalAlpha = 0.9f;
        blended = lerp(current, historyVal, kTemporalAlpha);
    }
    else
    {
        // Disocclusion: use raw current sample, no history blend
        blended = current;
    }

    // Write final resolved mask and update history for next frame
    g_RWShadowMask[uvInt]    = blended;
    g_RWShadowHistory[uvInt] = blended;
}
