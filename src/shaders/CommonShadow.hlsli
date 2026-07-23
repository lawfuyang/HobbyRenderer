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

// ---------------------------------------------------------------------------
// PCSS — Percentage-Closer Soft Shadows
// Only compiled when PCSS=1 (shader permutation).
// ---------------------------------------------------------------------------
#if PCSS

// Number of blocker search taps and PCF taps.
#define BLOCKER_SAMPLES 12
#define PCF_SAMPLES     16

// Penumbra clamp constants (in texels).
static const float kMinPenumbra   = 1.0f;
static const float kPenumbraScale = 1.0f;

// Nearest-biased weighting falloff.
static const float kBlockerWeightK = 50.0f;

// Temporal blend factor for shadow history.
static const float kTemporalAlpha = 0.9f;

static const bool kEnableBlockerEstimators = false;
static const bool kEnableAdaptivePCF = true;

// ---------------------------------------------------------------------------
// SampleBlueNoise — fetch a 2D offset in [-1,1]^2 from the 64x64 blue-noise
// texture. sampleIndex selects the texel within the tile; frameIndex shifts
// the tile each frame for temporal decorrelation.
// ---------------------------------------------------------------------------
float2 SampleBlueNoise(
    Texture2D<float2> blueNoiseTex,
    uint2             pixelPos,
    uint              sampleIndex,
    uint              frameIndex)
{
    // Per-frame tile shift (prime strides avoid aliasing)
    uint2 tileOffset = uint2(frameIndex * 7u, frameIndex * 13u) & 63u;
    // Per-sample offset within the tile (prime strides avoid row/column aliasing)
    uint2 texel = (pixelPos + tileOffset + uint2(sampleIndex * 5u, sampleIndex * 3u)) & 63u;
    // R8G8_UNORM [0,1] → [-1,1]
    return blueNoiseTex.Load(uint3(texel, 0)).rg * 2.0f - 1.0f;
}

// ---------------------------------------------------------------------------
// ComputePCSSShadow — full PCSS pipeline for one pixel.
//
// Returns visibility in [0,1]: 1 = fully lit, 0 = fully shadowed.
// Cascade 3 falls back to Compute3x3PCF (statistically ineffective at km scale).
// ---------------------------------------------------------------------------
float ComputePCSSShadow(
    float3                 worldPos,
    float3                 worldNormal,
    float                  viewDepth,
    Texture2DArray<float>  shadowMap,
    Texture2DArray<float>  shadowMapMips,     // R32_FLOAT min-reduction mip chain (separate UAV-capable copy)
    SamplerComparisonState shadowSampler,
    SamplerState           shadowSamplerPoint,
    SamplerState           samplerMinReduction,
    Texture2D<float2>      blueNoiseTex,
    float4x4               shadowViewProj[4],
    float4                 cascadeSplits,
    float4                 searchRadii,       // precomputed per-cascade UV search radius
    float                  lightAngularRadius, // sun half-angle in radians
    float                  normalBias,
    float                  texelSize,
    uint                   frameIndex,
    uint2                  pixelPos,
    uint                   bEnableCascadeBlend
)
{
    uint cascadeIndex = SelectCascade(viewDepth, cascadeSplits);

    // --- Normal-offset bias (same as ComputeCSMShadow) ---
    float3 vpRow0 = float3(shadowViewProj[cascadeIndex]._11, shadowViewProj[cascadeIndex]._21, shadowViewProj[cascadeIndex]._31);
    float  worldTexelSize  = 2.0f / (srrhi::CommonConsts::kShadowMapResolution * max(length(vpRow0), 1e-10f));
    float3 offsetWorldPos  = worldPos + worldNormal * normalBias * worldTexelSize;

    float4 lightSpacePos = mul(float4(offsetWorldPos, 1.0f), shadowViewProj[cascadeIndex]);
    float3 shadowUV      = lightSpacePos.xyz / lightSpacePos.w;
    shadowUV.xy = shadowUV.xy * float2(0.5f, -0.5f) + 0.5f;

    if (any(shadowUV.xy < 0.0f) || any(shadowUV.xy > 1.0f))
        return 1.0f;

    float receiverDepth = shadowUV.z;
    float bias          = 1e-4f; // small fixed depth-space epsilon for blocker comparisons
    float slice         = (float)cascadeIndex;

    // --- Cascade 3 fallback: fixed 3x3 PCF ---
    if (cascadeIndex == 3u)
    {
        return Compute3x3PCF(shadowMap, shadowSampler,
                             float3(shadowUV.xy, slice),
                             receiverDepth, texelSize);
    }

    // --- Stage 1: Shadow map min-reduction early-out ---
    // Query the shadow map's own min-reduction mip chain. If the shallowest
    // depth in the entire search region is still behind the receiver, there
    // are no blockers — skip the full search.
    float searchRadiusUV = searchRadii[cascadeIndex];
    {
        float shadowMip = log2(max(searchRadiusUV * (float)srrhi::CommonConsts::kShadowMapResolution, 1.0f));
        shadowMip = clamp(shadowMip, 0.0f, 10.0f); // shadow map has up to 11 mip levels (2048→1)
        float minBlockerDepth = shadowMapMips.SampleLevel(samplerMinReduction, float3(shadowUV.xy, slice), shadowMip).r;
        if (minBlockerDepth >= receiverDepth - bias)
            return 1.0f; // fully lit
    }

    // --- Stage 2: Blocker search (12 taps) ---
    float blockerDepths[BLOCKER_SAMPLES];
    float blockerSum     = 0.0f;
    float blockerCount   = 0.0f;
    float nearestBlocker = 1e10f;

    [unroll]
    for (int i = 0; i < BLOCKER_SAMPLES; ++i)
    {
        float2 xi     = SampleBlueNoise(blueNoiseTex, pixelPos, (uint)i, frameIndex);
        float2 offset = xi * searchRadiusUV;
        float  z      = shadowMap.SampleLevel(shadowSamplerPoint, float3(shadowUV.xy + offset, slice), 0).r;
        blockerDepths[i] = z;
        if (z < receiverDepth - bias)
        {
            blockerSum    += z;
            blockerCount  += 1.0f;
            nearestBlocker = min(nearestBlocker, z);
        }
    }

    if (blockerCount == 0.0f)
        return 1.0f; // fully lit — no blockers found in the entire wave

    float robustBlockerDepth;
    if (kEnableBlockerEstimators)
    {
        // --- Stage 3A: Nearest-biased weighted average (estimator A) ---
        float weightedSum   = 0.0f;
        float weightedTotal = 0.0f;
        [unroll]
        for (int j = 0; j < BLOCKER_SAMPLES; ++j)
        {
            if (blockerDepths[j] < receiverDepth - bias)
            {
                float w = exp(-abs(blockerDepths[j] - nearestBlocker) * kBlockerWeightK);
                weightedSum   += blockerDepths[j] * w;
                weightedTotal += w;
            }
        }
        float avgBlockerDepth = (weightedTotal > 0.0f) ? weightedSum / weightedTotal : nearestBlocker;

        // --- Stage 3B: Welford outlier rejection (estimator B) ---
        float wMean = 0.0f, wM2 = 0.0f, wCount = 0.0f;
        [unroll]
        for (int k = 0; k < BLOCKER_SAMPLES; ++k)
        {
            if (blockerDepths[k] < receiverDepth - bias)
            {
                wCount += 1.0f;
                float delta  = blockerDepths[k] - wMean;
                wMean  += delta / wCount;
                float delta2 = blockerDepths[k] - wMean;
                wM2    += delta * delta2;
            }
        }
        float variance = (wCount > 1.0f) ? wM2 / (wCount - 1.0f) : 0.0f;
        float stddev   = sqrt(variance);

        float filteredSum = 0.0f, filteredWeight = 0.0f;
        [unroll]
        for (int m = 0; m < BLOCKER_SAMPLES; ++m)
        {
            if (blockerDepths[m] < receiverDepth - bias
                && abs(blockerDepths[m] - wMean) <= 2.0f * stddev + 1e-5f)
            {
                float w = exp(-abs(blockerDepths[m] - nearestBlocker) * kBlockerWeightK);
                filteredSum    += blockerDepths[m] * w;
                filteredWeight += w;
            }
        }
        robustBlockerDepth = (filteredWeight > 0.0f)
            ? filteredSum / filteredWeight
            : avgBlockerDepth; // fallback to estimator A if all samples rejected
    }
    else
    {
        // Plain average — no weighting or outlier rejection
        robustBlockerDepth = blockerSum / blockerCount;
    }

    // --- Stage 4: Penumbra estimation ---
    float penumbraUV = (receiverDepth - robustBlockerDepth) / max(robustBlockerDepth, 1e-6f)
                     * lightAngularRadius
                     * kPenumbraScale;
    // Max penumbra is capped at the blocker search radius — can't be wider than what was searched.
    penumbraUV = clamp(penumbraUV, kMinPenumbra * texelSize, searchRadiusUV);

    // --- Stage 5: Adaptive PCF with blue-noise disc (16 taps) ---
    float shadow;
    if (kEnableAdaptivePCF)
    {
        float visibility = 0.0f;
        [unroll]
        for (int n = 0; n < PCF_SAMPLES; ++n)
        {
            // Offset PCF sample index past the blocker samples to avoid correlation
            float2 xi     = SampleBlueNoise(blueNoiseTex, pixelPos, (uint)(n + BLOCKER_SAMPLES), frameIndex);
            float2 offset = xi * penumbraUV;
            visibility += shadowMap.SampleCmpLevelZero(shadowSampler, float3(shadowUV.xy + offset, slice), receiverDepth);
        }
        shadow = visibility / (float)PCF_SAMPLES;
    }
    else
    {
        // Single-tap comparison at the exact shadow UV — no filtering
        shadow = shadowMap.SampleCmpLevelZero(shadowSampler, float3(shadowUV.xy, slice), receiverDepth);
    }

#if CASCADE_BLEND
    if (bEnableCascadeBlend && cascadeIndex < 3u)
    {
        float splitNear   = (cascadeIndex == 0u) ? 0.0f : cascadeSplits[cascadeIndex - 1];
        float splitFar    = cascadeSplits[cascadeIndex];
        float bandSize    = (splitFar - splitNear) * 0.1f;
        float blendFactor = saturate((viewDepth - (splitFar - bandSize)) / max(bandSize, 1e-5f));

        if (blendFactor > 0.0f)
        {
            float4 lsPos1 = mul(float4(offsetWorldPos, 1.0f), shadowViewProj[cascadeIndex + 1u]);
            float3 uv1    = lsPos1.xyz / lsPos1.w;
            uv1.xy        = uv1.xy * float2(0.5f, -0.5f) + 0.5f;

            if (all(uv1.xy >= 0.0f) && all(uv1.xy <= 1.0f))
            {
                // Next cascade uses fixed 3x3 PCF for the blend (avoids double PCSS cost)
                float shadow1 = Compute3x3PCF(shadowMap, shadowSampler,
                                              float3(uv1.xy, (float)(cascadeIndex + 1u)),
                                              uv1.z, texelSize);
                shadow = lerp(shadow, shadow1, blendFactor);
            }
        }
    }
#endif

    return shadow;
}

#endif // PCSS

#endif // COMMON_SHADOW_HLSLI
