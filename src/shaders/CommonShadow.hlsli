// CommonShadow.hlsli — Shared CSM shadow evaluation utilities
// Included by ShadowMask.hlsl, CSMDebug.hlsli, and any future shadow-consuming pass.
//
// Bias strategy: Normal-offset bias (GDC 2011, Daniel Holbert — "Saying Goodbye to Shadow Acne")
// Pushes the sample position outward along the surface normal before transforming to light space.
// This "thickens" the receiver uniformly in world space and is bounded at all angles.
//
// Standard depth convention: near = 0.0, far = 1.0
//   - Shadow map depth comparison: Less (hardware default)
//   - Sky/background: depth == 1.0 (far clear value)

#ifndef COMMON_SHADOW_HLSLI
#define COMMON_SHADOW_HLSLI

#include "srrhi/hlsl/Common.hlsli"

// ---------------------------------------------------------------------------
// Cascade selection
// ---------------------------------------------------------------------------

// Returns cascade index 0-3: count of split boundaries that viewDepth exceeds.
// cascadeSplits.xyzw = view-space far depths for cascades 0-3.
uint SelectCascade(float viewDepth, float4 cascadeSplits)
{
    return dot(uint3(viewDepth >= cascadeSplits.xyz), 1);
}

// ---------------------------------------------------------------------------
// 3x3 PCF — 9 taps, each a hardware-accelerated bilinear comparison
// ---------------------------------------------------------------------------

float Compute3x3PCF(
    Texture2DArray<float>  shadowMap,
    SamplerComparisonState shadowSampler,
    float3                 shadowUV,      // .xy = UV in [0,1], .z = array slice (float)
    float                  compareDepth,  // depth to compare (bias already applied)
    float                  texelSize)     // 1.0 / shadowMapResolution
{
    float shadow = 0.0f;
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            shadow += shadowMap.SampleCmpLevelZero(
                shadowSampler,
                float3(shadowUV.xy + float2(x, y) * texelSize, shadowUV.z),
                compareDepth);
        }
    }
    return shadow / 9.0f;
}

// ---------------------------------------------------------------------------
// PCSS helpers
// ---------------------------------------------------------------------------

// 16-sample Poisson disc — good coverage at low cost
static const float2 kPoissonDisk16[16] =
{
    float2(-0.94201624f, -0.39906216f), float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f), float2( 0.34495938f,  0.29387760f),
    float2(-0.91588581f,  0.45771432f), float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f,  0.27676845f), float2( 0.97484398f,  0.75648379f),
    float2( 0.44323325f, -0.97511554f), float2( 0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f), float2( 0.79197514f,  0.19090188f),
    float2(-0.24188840f,  0.99706507f), float2(-0.81409955f,  0.91437590f),
    float2( 0.19984126f,  0.78641367f), float2( 0.14383161f, -0.14100790f),
};

// Sun angular size as a fraction of shadow map width.
// Controls blocker search radius and penumbra width.
static const float LIGHT_SIZE_UV = 0.05f;

// Light-space near plane (world units). Used in penumbra estimation.
static const float SHADOW_NEAR_PLANE = 0.1f;

// BlockerSearch — samples shadow map with a point sampler (no comparison) in a
// Poisson disc of radius searchRadius texels. Returns the average depth of
// texels that are in front of the receiver (potential blockers), or -1 if none.
//
// Reversed-Z: a texel is a blocker when sampleDepth < receiverDepth
// (closer to light = smaller depth value in standard depth).
float BlockerSearch(
    Texture2DArray<float> shadowMap,
    SamplerState          pointSampler,  // non-comparison point sampler
    float3                shadowUV,      // .xy = UV, .z = array slice
    float                 receiverDepth, // receiver depth in light-space NDC
    float                 searchRadius,  // in texels
    float                 texelSize)     // 1.0 / shadowMapResolution
{
    float blockerSum   = 0.0f;
    uint  blockerCount = 0u;

    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        float2 offset      = kPoissonDisk16[i] * searchRadius * texelSize;
        float  sampleDepth = shadowMap.SampleLevel(
            pointSampler,
            float3(shadowUV.xy + offset, shadowUV.z),
            0).r;

        // Standard depth: blocker is closer to the light → smaller depth value
        if (sampleDepth < receiverDepth)
        {
            blockerSum += sampleDepth;
            ++blockerCount;
        }
    }

    return (blockerCount > 0u) ? (blockerSum / (float)blockerCount) : -1.0f;
}

// VariableKernelPCF — 16-sample Poisson disc PCF with a comparison sampler,
// kernel radius driven by the estimated penumbra width.
float VariableKernelPCF(
    Texture2DArray<float>  shadowMap,
    SamplerComparisonState shadowSampler,
    float3                 shadowUV,      // .xy = UV, .z = array slice
    float                  compareDepth,  // depth to compare (bias already applied)
    float                  penumbraWidth, // in texels
    float                  texelSize)     // 1.0 / shadowMapResolution
{
    float shadow = 0.0f;

    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        float2 offset = kPoissonDisk16[i] * penumbraWidth * texelSize;
        shadow += shadowMap.SampleCmpLevelZero(
            shadowSampler,
            float3(shadowUV.xy + offset, shadowUV.z),
            compareDepth);
    }

    return shadow / 16.0f;
}

// ComputePCSSShadow — full PCSS: blocker search → penumbra estimation → variable PCF.
// Requires both a point sampler (for blocker search) and a comparison sampler (for PCF).
float ComputePCSSShadow(
    Texture2DArray<float>  shadowMap,
    SamplerState           pointSampler,
    SamplerComparisonState shadowSampler,
    float3                 shadowUV,      // .xy = UV, .z = array slice
    float                  compareDepth,  // receiver depth in light-space NDC
    float                  texelSize)     // 1.0 / shadowMapResolution
{
    // 1. Blocker search — radius scales with light size and receiver distance
    float searchRadius    = LIGHT_SIZE_UV * (compareDepth - SHADOW_NEAR_PLANE) / max(compareDepth, 1e-5f);
    float avgBlockerDepth = BlockerSearch(shadowMap, pointSampler, shadowUV, compareDepth, searchRadius, texelSize);

    // No blockers found → fully lit
    if (avgBlockerDepth < 0.0f)
        return 1.0f;

    // 2. Penumbra estimation (in texels)
    float penumbraWidth = (compareDepth - avgBlockerDepth) * LIGHT_SIZE_UV / max(avgBlockerDepth, 1e-5f);

    // 3. Variable-width PCF
    return VariableKernelPCF(shadowMap, shadowSampler, shadowUV, compareDepth, penumbraWidth, texelSize);
}

// ---------------------------------------------------------------------------
// Full CSM shadow evaluation
// ---------------------------------------------------------------------------
// Cascade selection + normal-offset bias + 3x3 PCF (or PCSS when PCSS=1).
//
// normalBias: offset in shadow-map texels (default 3.0). Multiplied by the
//             per-cascade world-space texel size derived from the shadow VP matrix.
//
// When CASCADE_BLEND=1, pixels near a cascade boundary are blended between
// the current and next cascade to eliminate hard seams.
float ComputeCSMShadow(
    float3                 worldPos,
    float3                 worldNormal,
    float                  viewDepth,
    Texture2DArray<float>  shadowMap,
    SamplerComparisonState shadowSampler,
    SamplerState           pointSampler,
    float4x4               shadowViewProj[4],
    float4                 cascadeSplits,
    float                  normalBias,   // texel units (default: 3.0)
    float                  texelSize,     // 1.0 / shadowMapResolution
    uint                   bEnableCascadeBlend
)
{
    uint cascadeIndex = SelectCascade(viewDepth, cascadeSplits);

    // Normal-offset bias: push sample position outward along world-space normal
    // before transforming to light space.
    // Compute world-space texel size from the ortho shadow VP matrix:
    // VP row 0 magnitude = 2 / worldWidth, and 1 texel = 2 / resolution in clip space.
    // => worldTexelSize = worldWidth / resolution = 2 / (resolution * |VP_row0|)
    float3 vpRow0 = float3(shadowViewProj[cascadeIndex]._11, shadowViewProj[cascadeIndex]._21, shadowViewProj[cascadeIndex]._31);
    float   worldTexelSize = 2.0f / (srrhi::CommonConsts::kShadowMapResolution * max(length(vpRow0), 1e-10f));
    float3  offsetWorldPos = worldPos + worldNormal * normalBias * worldTexelSize;

    float4 lightSpacePos = mul(float4(offsetWorldPos, 1.0f), shadowViewProj[cascadeIndex]);
    float3 shadowUV      = lightSpacePos.xyz / lightSpacePos.w;

    // NDC → UV: x [-1,1]→[0,1], y [1,-1]→[0,1] (flip Y for D3D)
    shadowUV.xy = shadowUV.xy * float2(0.5f, -0.5f) + 0.5f;

    // Out-of-bounds → fully lit (border sampler handles this, but explicit is safer)
    if (any(shadowUV.xy < 0.0f) || any(shadowUV.xy > 1.0f))
        return 1.0f;

    float compareDepth = shadowUV.z;

#if PCSS
    float shadow = ComputePCSSShadow(
        shadowMap, pointSampler, shadowSampler,
        float3(shadowUV.xy, (float)cascadeIndex),
        compareDepth, texelSize);
#else
    float shadow = Compute3x3PCF(
        shadowMap, shadowSampler,
        float3(shadowUV.xy, (float)cascadeIndex),
        compareDepth, texelSize);
#endif

#if CASCADE_BLEND
    if (bEnableCascadeBlend && cascadeIndex < 3u)
    {
        // Blend band: last 10% of this cascade's depth range
        float splitNear  = (cascadeIndex == 0u) ? 0.0f : cascadeSplits[cascadeIndex - 1];
        float splitFar   = cascadeSplits[cascadeIndex];
        float bandSize   = (splitFar - splitNear) * 0.1f;
        float blendFactor = saturate((viewDepth - (splitFar - bandSize)) / max(bandSize, 1e-5f));

        if (blendFactor > 0.0f)
        {
            // Transform into next cascade's light space (reuse normal-offset world pos)
            float4 lsPos1 = mul(float4(offsetWorldPos, 1.0f), shadowViewProj[cascadeIndex + 1u]);
            float3 uv1    = lsPos1.xyz / lsPos1.w;
            uv1.xy        = uv1.xy * float2(0.5f, -0.5f) + 0.5f;

            if (all(uv1.xy >= 0.0f) && all(uv1.xy <= 1.0f))
            {
#if PCSS
                float shadow1 = ComputePCSSShadow(
                    shadowMap, pointSampler, shadowSampler,
                    float3(uv1.xy, (float)(cascadeIndex + 1u)),
                    uv1.z, texelSize);
#else
                float shadow1 = Compute3x3PCF(
                    shadowMap, shadowSampler,
                    float3(uv1.xy, (float)(cascadeIndex + 1u)),
                    uv1.z, texelSize);
#endif
                shadow = lerp(shadow, shadow1, blendFactor);
            }
        }
    }
#endif

    return shadow;
}

#endif // COMMON_SHADOW_HLSLI
