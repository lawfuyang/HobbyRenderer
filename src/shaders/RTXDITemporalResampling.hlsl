/*
 * RTXDITemporalResampling.hlsl
 *
 * Pass 2 of the ReSTIR DI pipeline.
 * Combines the initial-sample reservoir with the previous frame's reservoir
 * via temporal resampling, improving temporal stability.
 *
 * The boiling filter is enabled to suppress firefly artefacts that can arise
 * from large mis-weighted reservoirs being propagated across many frames.
 */

#define RTXDI_ENABLE_BOILING_FILTER    1
#define RTXDI_BOILING_FILTER_GROUP_SIZE 8

#include "RTXDIApplicationBridge.hlsli"

// RTXDI SDK
#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/DI/BoilingFilter.hlsli"
#include "Rtxdi/DI/TemporalResampling.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"

// ============================================================================
RTXDI_RuntimeParameters GetRuntimeParams()
{
    RTXDI_RuntimeParameters p;
    p.neighborOffsetMask      = g_RTXDIConst.m_NeighborOffsetMask;
    p.activeCheckerboardField = g_RTXDIConst.m_ActiveCheckerboardField;
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
void RTXDITemporalResampling_CSMain(
    uint2 GlobalIndex : SV_DispatchThreadID,
    uint2 LocalIndex  : SV_GroupThreadID)
{
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(GlobalIndex >= viewportSize))
        return;

    RTXDI_RuntimeParameters        rtParams = GetRuntimeParams();
    RTXDI_ReservoirBufferParameters rbp      = GetReservoirBufferParams();

    uint2 pixelPosition = GlobalIndex;
    int2  iPixel        = int2(pixelPosition);

    RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 2u);

    // Load current surface
    RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

    // Load the reservoir produced by the initial sampling pass
    RTXDI_DIReservoir curReservoir = RTXDI_LoadDIReservoir(rbp, pixelPosition,
        g_RTXDIConst.m_TemporalResamplingInputBufferIndex);

    // Motion vector — stored as NDC velocity (dx, dy).  Convert to pixel-space.
    float2 mv      = g_GBufferMV.Load(int3(iPixel, 0)).xy;
    float2 vpSizeF = float2(viewportSize);
    // NDC → pixel offset: multiply by (w/2, -h/2)
    float2 pixelMotion = mv * vpSizeF * float2(0.5, -0.5);

    RTXDI_DITemporalResamplingParameters tparams;
    tparams.screenSpaceMotion       = float3(pixelMotion, 0.0);
    tparams.sourceBufferIndex       = g_RTXDIConst.m_TemporalResamplingInputBufferIndex;
    tparams.maxHistoryLength        = 20u;
    tparams.biasCorrectionMode      = RTXDI_BIAS_CORRECTION_BASIC;
    tparams.depthThreshold          = 0.1;
    tparams.normalThreshold         = 0.5;
    tparams.enableVisibilityShortcut = false;
    tparams.enablePermutationSampling = true;
    tparams.uniformRandomNumber     = g_RTXDIConst.m_FrameIndex * 2699u;

    RAB_LightSample selectedSample = RAB_EmptyLightSample();
    int2 temporalPixelPos;

    RTXDI_DIReservoir outReservoir = RTXDI_DITemporalResampling(
        pixelPosition, surface, curReservoir, rng,
        rtParams, rbp, tparams,
        temporalPixelPos, selectedSample);

    // Boiling filter (operates within the 8×8 group)
    RTXDI_BoilingFilter(LocalIndex, RAB_GetBoilingFilterStrength(), outReservoir);

    RTXDI_StoreDIReservoir(outReservoir, rbp, pixelPosition,
        g_RTXDIConst.m_TemporalResamplingOutputBufferIndex);
}
