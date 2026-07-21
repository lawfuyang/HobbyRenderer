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
    float4x4               shadowViewProj[4],
    float4                 cascadeSplits,
    float                  normalBias,   // texel units (default: 3.0)
    float                  texelSize,    // 1.0 / shadowMapResolution
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

    float shadow = Compute3x3PCF(
        shadowMap, shadowSampler,
        float3(shadowUV.xy, (float)cascadeIndex),
        compareDepth, texelSize);

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
                float shadow1 = Compute3x3PCF(
                    shadowMap, shadowSampler,
                    float3(uv1.xy, (float)(cascadeIndex + 1u)),
                    uv1.z, texelSize);
                shadow = lerp(shadow, shadow1, blendFactor);
            }
        }
    }
#endif

    return shadow;
}

#endif // COMMON_SHADOW_HLSLI
