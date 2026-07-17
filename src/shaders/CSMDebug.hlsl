#include "Common.hlsli"
#include "CommonShadow.hlsli"
#include "srrhi/hlsl/CSMDebug.hlsli"

// ---------------------------------------------------------------------------
// Cascade color palette — shared across all debug modes
// ---------------------------------------------------------------------------
static const float3 kCascadeColors[4] =
{
    float3(1.0f, 0.2f, 0.2f),  // Red    — Cascade 0 (near)
    float3(0.2f, 1.0f, 0.2f),  // Green  — Cascade 1
    float3(0.2f, 0.2f, 1.0f),  // Blue   — Cascade 2
    float3(1.0f, 1.0f, 0.2f),  // Yellow — Cascade 3 (far)
};

// Mode 1: Cascade Splits — tint scene by which cascade covers each pixel
float3 DebugCascadeSplits(uint cascadeIndex)
{
    return kCascadeColors[min(cascadeIndex, 3u)];
}

// Mode 2: Shadow Map Array Visualization
// Renders 4 horizontal strips at the bottom quarter of the screen,
// each showing one cascade's depth buffer linearized to [0,1] grayscale.
float3 DebugShadowMapViz(
    Texture2DArray<float> shadowMap,
    SamplerState          pointSampler,
    float2                screenUV,
    float2                outputSize,
    uint                  shadowMapRes)
{
    const float kStripHeight = 0.25f;
    if (screenUV.y < (1.0f - kStripHeight))
        return float3(-1, -1, -1);  // sentinel: caller should pass-through albedo

    float stripV = (screenUV.y - (1.0f - kStripHeight)) / kStripHeight;
    float stripU = screenUV.x;

    uint cascadeIndex = (uint)(stripU * 4.0f);
    cascadeIndex = min(cascadeIndex, 3u);
    float localU = frac(stripU * 4.0f);

    float depth = shadowMap.SampleLevel(pointSampler, float3(localU, stripV, (float)cascadeIndex), 0).r;
    return float3(depth, depth, depth);
}

// Mode 3: Raw Shadow Mask — grayscale display of the R8 shadow mask
float3 DebugShadowMask(float shadowFactor)
{
    return float3(shadowFactor, shadowFactor, shadowFactor);
}

// Mode 5: Alpha-Masked Overlay — orange = masked geometry, normal = opaque
float3 DebugAlphaMasked(float4 albedoAlpha, float3 baseColor)
{
    bool isMasked = (albedoAlpha.a < 0.99f);
    return isMasked ? float3(1.0f, 0.5f, 0.0f) : baseColor;
}

// Mode 6: Depth Compare — red = shadowed, green = lit
float3 DebugDepthCompare(float shadowFactor)
{
    float3 shadowedColor = float3(0.8f, 0.1f, 0.1f);
    float3 litColor      = float3(0.1f, 0.8f, 0.1f);
    return lerp(shadowedColor, litColor, shadowFactor);
}

// Mode 8: Blend Zone — white = in blend band, cascade color = outside
float3 DebugBlendZone(float viewDepth, float4 cascadeSplits)
{
    uint cascadeIndex = SelectCascade(viewDepth, cascadeSplits);

    float splitNear   = (cascadeIndex == 0) ? 0.0f : cascadeSplits[cascadeIndex - 1];
    float splitFar    = cascadeSplits[min(cascadeIndex, 3u)];
    float bandSize    = (splitFar - splitNear) * 0.1f;
    float blendFactor = saturate((viewDepth - (splitFar - bandSize)) / max(bandSize, 1e-5f));

    float3 cascadeColor = kCascadeColors[min(cascadeIndex, 3u)];
    return lerp(cascadeColor, float3(1.0f, 1.0f, 1.0f), blendFactor);
}

// ---------------------------------------------------------------------------

static const srrhi::CSMDebugConstants g_CB          = srrhi::CSMDebugInputs::GetCSMDebugCB();
static const Texture2D<float>         g_Depth       = srrhi::CSMDebugInputs::GetDepth();
static const Texture2D<float4>        g_Albedo      = srrhi::CSMDebugInputs::GetGBufferAlbedo();
static const Texture2DArray<float>    g_ShadowMap   = srrhi::CSMDebugInputs::GetCSMShadowMap();
static const Texture2D<float>         g_ShadowMask  = srrhi::CSMDebugInputs::GetShadowMask();
static const SamplerState             g_PointSampler = srrhi::CSMDebugInputs::GetPointSampler();

float4 CSMDebug_PSMain(FullScreenVertexOut input) : SV_Target
{
    uint2  uvInt = uint2(input.pos.xy);
    float2 uv    = input.uv;

    float depth = g_Depth.Load(uint3(uvInt, 0));

    // Reconstruct view-space depth for cascade selection
    float4 clipPos   = float4(UVToClipXY(uv), depth, 1.0f);
    float4 viewPos4  = MatrixMultiply(clipPos, g_CB.m_ClipToView);
    float  viewDepth = -viewPos4.z / viewPos4.w;  // negate: view-space Z is negative in front

    uint cascadeIndex = SelectCascade(viewDepth, g_CB.m_CascadeSplits);

    float3 output = float3(0.0f, 0.0f, 0.0f);

    switch (g_CB.m_DebugMode)
    {
        // ── Mode 1: Cascade Splits ──────────────────────────────────────────
        case srrhi::CSMDebugMode::CSM_DEBUG_CASCADE_SPLITS:
        {
            float3 cascadeColor = DebugCascadeSplits(cascadeIndex);
            float3 albedo       = g_Albedo.Load(uint3(uvInt, 0)).rgb;
            // 50/50 blend so scene geometry remains visible
            output = cascadeColor * 0.5f + albedo * 0.5f;
            break;
        }

        // ── Mode 2: Shadow Map Array Visualization ──────────────────────────
        case srrhi::CSMDebugMode::CSM_DEBUG_SHADOW_MAP_VIZ:
        {
            float3 vizColor = DebugShadowMapViz(
                g_ShadowMap,
                g_PointSampler,
                uv,
                g_CB.m_OutputSize,
                srrhi::CommonConsts::kShadowMapResolution);

            if (vizColor.x < 0.0f)
            {
                // Outside the strip — pass through albedo
                output = g_Albedo.Load(uint3(uvInt, 0)).rgb;
            }
            else
            {
                output = vizColor;
            }
            break;
        }

        // ── Mode 3: Raw Shadow Mask ─────────────────────────────────────────
        case srrhi::CSMDebugMode::CSM_DEBUG_SHADOW_MASK:
        {
            float s = g_ShadowMask.Load(uint3(uvInt, 0));
            output  = DebugShadowMask(s);
            break;
        }

        // ── Mode 4: PCF Footprint ───────────────────────────────────────────
        case srrhi::CSMDebugMode::CSM_DEBUG_PCF_FOOTPRINT:
        {
            // Visualize the 3×3 PCF kernel footprint as a colored grid overlay.
            // Each pixel shows which PCF tap it belongs to (center = white, edges = grey).
            float texelSize = 1.0f / (float)srrhi::CommonConsts::kShadowMapResolution;
            float2 shadowUV = float2(0.5f, 0.5f);  // placeholder center
            // Show a checkerboard pattern scaled to shadow-map texel size in screen space
            float2 screenTexelUV = uv * g_CB.m_OutputSize * texelSize;
            float checker = fmod(floor(screenTexelUV.x) + floor(screenTexelUV.y), 2.0f);
            float3 albedo = g_Albedo.Load(uint3(uvInt, 0)).rgb;
            output = lerp(albedo, kCascadeColors[min(cascadeIndex, 3u)], checker * 0.3f);
            break;
        }

        // ── Mode 5: Alpha-Masked Overlay ────────────────────────────────────
        case srrhi::CSMDebugMode::CSM_DEBUG_ALPHA_MASKED:
        {
            float4 albedoAlpha = g_Albedo.Load(uint3(uvInt, 0));
            output = DebugAlphaMasked(albedoAlpha, albedoAlpha.rgb);
            break;
        }

        // ── Mode 6: Depth Compare (red=shadowed, green=lit) ─────────────────
        case srrhi::CSMDebugMode::CSM_DEBUG_DEPTH_COMPARE:
        {
            float s = g_ShadowMask.Load(uint3(uvInt, 0));
            output  = DebugDepthCompare(s);
            break;
        }

        // ── Mode 7: Frustum Wireframe ───────────────────────────────────────
        // Wireframe frustum corners are rendered as ImGui 3D lines by CSMDebugRenderer.
        // This shader pass-through shows the scene albedo as background.
        case srrhi::CSMDebugMode::CSM_DEBUG_FRUSTUM_WIRE:
        {
            output = g_Albedo.Load(uint3(uvInt, 0)).rgb;
            break;
        }

        // ── Mode 8: Blend Zone ──────────────────────────────────────────────
        case srrhi::CSMDebugMode::CSM_DEBUG_BLEND_ZONE:
        {
            output = DebugBlendZone(viewDepth, g_CB.m_CascadeSplits);
            // Blend 60/40 with albedo so scene is still readable
            float3 albedo = g_Albedo.Load(uint3(uvInt, 0)).rgb;
            output = output * 0.6f + albedo * 0.4f;
            break;
        }

        default:
            discard;
            break;
    }

    return float4(output, 1.0f);
}
