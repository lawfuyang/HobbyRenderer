// SharcDebugViz.hlsl
//
// SHARC Debug Visualization pass — converts per-pixel bounce counts from the
// Query pass into a color heatmap overlay.
//
// Color mapping (matching SHARC documentation):
//   0 bounces → black  (no indirect, primary hit only)
//   1 bounce  → green  (one indirect bounce, cache hit)
//   2 bounces → yellow (two indirect bounces)
//   3+ bounces → red   (three or more bounces, no cache hit)
//
// The heatmap replaces the main HDR color output when enabled.

// SHARC defines — must be set before including SharcCommon.h
#define SHARC_RADIANCE_SCALE 1e3f
#include "SharcCommon.hlsli"

#include "srrhi/hlsl/SHARC.hlsli"

static const srrhi::SharcConstants g_Sharc = srrhi::SharcDebugVizInputs::GetSharcCB();

static RWTexture2D<uint>   g_BounceCountInput = srrhi::SharcDebugVizInputs::GetBounceCountInput();
static RWTexture2D<float4> g_HeatmapOutput    = srrhi::SharcDebugVizInputs::GetHeatmapOutput();

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

    uint bounceCount = g_BounceCountInput[pixel];
    float3 heatmapColor = BounceCountToColor(bounceCount);

    g_HeatmapOutput[pixel] = float4(heatmapColor, 1.0f);
}
