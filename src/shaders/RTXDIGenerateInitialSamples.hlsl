/*
 * RTXDIGenerateInitialSamples.hlsl
 *
 * Pass 1 of the ReSTIR DI pipeline.
 * Samples candidate lights for each pixel using RTXDI_SampleLightsForSurface,
 * stores the resulting reservoir into the light reservoir buffer.
 */

#include "RTXDIApplicationBridge.hlsli"

// RTXDI SDK
#include "Rtxdi/Utils/Checkerboard.hlsli"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"

// ============================================================================
// Helper: build RTXDI_LightBufferParameters from g_RTXDIConst
// ============================================================================
RTXDI_LightBufferParameters GetLightBufferParams()
{
    RTXDI_LightBufferParameters p;
    p.localLightBufferRegion.firstLightIndex    = g_RTXDIConst.m_LocalLightFirstIndex;
    p.localLightBufferRegion.numLights          = g_RTXDIConst.m_LocalLightCount;
    p.infiniteLightBufferRegion.firstLightIndex = g_RTXDIConst.m_InfiniteLightFirstIndex;
    p.infiniteLightBufferRegion.numLights       = g_RTXDIConst.m_InfiniteLightCount;
    p.environmentLightParams.lightPresent       = g_RTXDIConst.m_EnvLightPresent;
    p.environmentLightParams.lightIndex         = g_RTXDIConst.m_EnvLightIndex;
    return p;
}

RTXDI_RuntimeParameters GetRuntimeParams()
{
    RTXDI_RuntimeParameters p;
    p.neighborOffsetMask       = g_RTXDIConst.m_NeighborOffsetMask;
    p.activeCheckerboardField  = g_RTXDIConst.m_ActiveCheckerboardField;
    return p;
}

RTXDI_ReservoirBufferParameters GetReservoirBufferParams()
{
    RTXDI_ReservoirBufferParameters p;
    p.reservoirBlockRowPitch = g_RTXDIConst.m_ReservoirBlockRowPitch;
    p.reservoirArrayPitch    = g_RTXDIConst.m_ReservoirArrayPitch;
    return p;
}

// ============================================================================
[numthreads(8, 8, 1)]
void CSMain(uint2 GlobalIndex : SV_DispatchThreadID)
{
    RTXDI_RuntimeParameters rtParams = GetRuntimeParams();
    RTXDI_ReservoirBufferParameters rbp = GetReservoirBufferParams();

    uint2 reservoirPosition = GlobalIndex;
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(reservoirPosition, rtParams.activeCheckerboardField);
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(pixelPosition >= viewportSize))
        return;

    int2  iPixel = int2(pixelPosition);

    // Initialise RNG
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 1u);
    RAB_RandomSamplerState coherentRng = RAB_InitRandomSampler(pixelPosition / 16u, 1u);

    // Fetch G-buffer surface for this pixel
    RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

    RTXDI_LightBufferParameters lbp = GetLightBufferParams();

    RTXDI_SampleParameters sampleParams = RTXDI_InitSampleParameters(
        /*numLocalLightSamples=*/  max(1u, lbp.localLightBufferRegion.numLights > 0 ? 1u : 0u),
        /*numInfiniteLightSamples=*/ (lbp.infiniteLightBufferRegion.numLights > 0 ? 1u : 0u),
        /*numEnvironmentMapSamples=*/ 0u,
        /*numBrdfSamples=*/          0u);

    RAB_LightSample selectedSample;
    RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(
        rng, coherentRng, surface, sampleParams, lbp,
        ReSTIRDI_LocalLightSamplingMode_UNIFORM,
        selectedSample);

    RTXDI_StoreDIReservoir(reservoir, rbp, reservoirPosition,
        g_RTXDIConst.m_InitialSamplingOutputBufferIndex);
}
