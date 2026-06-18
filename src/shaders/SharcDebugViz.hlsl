// SharcDebugViz.hlsl
//
// SHARC Debug Visualization pass — supports multiple debug overlay modes.
//
// Modes (switched via g_Sharc.m_DebugMode):
//   DEBUG_MODE_SHARC_BOUNCE_HEATMAP:   per-pixel bounce-count heatmap
//   DEBUG_MODE_SHARC_HASH_GRID:        world-space hash grid visualization
//   DEBUG_MODE_SHARC_CACHED_RADIANCE:  raw cached radiance from SharcGetCachedRadiance()
//
// Color mapping for bounce heatmap:
//   0 bounces → black  (no indirect, primary hit only)
//   1 bounce  → green  (one indirect bounce, cache hit)
//   2 bounces → yellow (two indirect bounces)
//   3+ bounces → red   (three or more bounces, no cache hit)

// SHARC defines — must be set before including SharcCommon.h
#define SHARC_RADIANCE_SCALE 1e3f
#include "SharcCommon.hlsli"

#include "CommonLighting.hlsli"

#include "srrhi/hlsl/SHARC.hlsli"

static const srrhi::SharcConstants g_Sharc = srrhi::SharcDebugVizInputs::GetSharcCB();

static RWTexture2D<float4> g_HeatmapOutput    = srrhi::SharcDebugVizInputs::GetHeatmapOutput();
static const Texture2D<uint>   g_BounceCountInput = srrhi::SharcDebugVizInputs::GetBounceCountInput();
static const Texture2D<float>  g_Depth            = srrhi::SharcDebugVizInputs::GetDepth();
static const Texture2D<float2> g_GBufferNormals   = srrhi::SharcDebugVizInputs::GetGBufferNormals();
static const Texture2D<float4> g_CachedRadianceInput = srrhi::SharcDebugVizInputs::GetCachedRadianceInput();

// Map a bounce count to a heatmap color
float3 BounceCountToColor(uint bounceCount)
{
    if (bounceCount == 0)
        return float3(0.0f, 0.0f, 0.0f);   // black: no indirect
    else if (bounceCount == 1)
        return float3(0.0f, 1.0f, 0.0f);   // green: 1 indirect bounce (cache hit)
    else if (bounceCount == 2)
        return float3(1.0f, 1.0f, 0.0f);   // yellow: 2 bounces
    else
        return float3(1.0f, 0.0f, 0.0f);   // red: 3+ bounces (no cache hit)
}

[numthreads(8, 8, 1)]
void SharcDebugViz_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if (g_Sharc.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_HASH_GRID)
    {
        // ── Hash Grid Visualization ────────────────────────────────────────

        float depth = g_Depth.Load(uint3(pixel, 0));

        // Reversed-Z: DEPTH_FAR (0.0) = sky / no geometry
        if (depth == srrhi::CommonConsts::DEPTH_FAR)
        {
            g_HeatmapOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
            return;
        }

        // Reconstruct world-space position from depth
        float2 uv      = (float2(pixel) + 0.5f) * g_Sharc.m_ViewportSizeInv;
        float2 clipXY  = UVToClipXY(uv);
        float4 clipPos = float4(clipXY, depth, 1.0f);
        float4 worldPos4 = MatrixMultiply(clipPos, g_Sharc.m_MatClipToWorldNoOffset);
        float3 worldPos   = worldPos4.xyz / worldPos4.w;

        // Decode GBuffer normals (oct-encoded, stored [0,1])
        float3 normal = DecodeNormal(g_GBufferNormals.Load(uint3(pixel, 0)));

        // Build hash grid parameters matching the SHARC cache
        HashGridParameters gridParams;
        gridParams.cameraPosition = g_Sharc.m_CameraPosition;
        gridParams.logarithmBase  = SHARC_GRID_LOGARITHM_BASE;
        gridParams.sceneScale     = g_Sharc.m_SceneScale;
        gridParams.levelBias      = SHARC_GRID_LEVEL_BIAS;

        float3 color = HashGridDebugColoredHash(worldPos, normal, gridParams);
        g_HeatmapOutput[pixel] = float4(color, 1.0f);
    }
    else if (g_Sharc.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_CACHED_RADIANCE)
    {
        // ── Cached Radiance View ───────────────────────────────────────────
        // Raw output of SharcGetCachedRadiance(); black where no cache hit.
        float4 cachedRadiance = g_CachedRadianceInput.Load(uint3(pixel, 0));
        g_HeatmapOutput[pixel] = cachedRadiance;
    }
    else
    {
        // ── Bounce Count Heatmap (default / backward compatible) ───────────

        uint bounceCount = g_BounceCountInput[pixel];
        float3 heatmapColor = BounceCountToColor(bounceCount);

        g_HeatmapOutput[pixel] = float4(heatmapColor, 1.0f);
    }
}
