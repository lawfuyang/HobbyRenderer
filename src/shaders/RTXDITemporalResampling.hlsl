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
#include "Rtxdi/Utils/Checkerboard.hlsli"
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
void CSMain(
    uint2 GlobalIndex : SV_DispatchThreadID,
    uint2 LocalIndex  : SV_GroupThreadID)
{
    RTXDI_RuntimeParameters        rtParams = GetRuntimeParams();
    RTXDI_ReservoirBufferParameters rbp      = GetReservoirBufferParams();

    uint2 reservoirPosition = GlobalIndex;
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(reservoirPosition, rtParams.activeCheckerboardField);
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(pixelPosition >= viewportSize))
        return;

    int2  iPixel        = int2(pixelPosition);

    RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 2u);

    // Load current surface
    RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

    RTXDI_DIReservoir outReservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface))
    {
        // Load the reservoir produced by the initial sampling pass (current frame's new candidates)
        RTXDI_DIReservoir curReservoir = RTXDI_LoadDIReservoir(rbp, reservoirPosition,
            g_RTXDIConst.m_InitialSamplingOutputBufferIndex);

        // Motion vector — stored as pixel-space velocity (dx, dy).
        float3 mv = g_GBufferMV.Load(int3(iPixel, 0)).xyz;
        float3 pixelMotion = ConvertMotionVectorToPixelSpace(g_RTXDIConst.m_View, g_RTXDIConst.m_PrevView, iPixel, mv);

        RTXDI_DITemporalResamplingParameters tparams;
        tparams.screenSpaceMotion        = pixelMotion;
        tparams.sourceBufferIndex        = g_RTXDIConst.m_TemporalResamplingInputBufferIndex;
        tparams.maxHistoryLength         = 20u;
        tparams.biasCorrectionMode       = RTXDI_BIAS_CORRECTION_BASIC;
        tparams.depthThreshold           = 0.1;
        tparams.normalThreshold          = 0.5;
        tparams.enableVisibilityShortcut = false;
        tparams.enablePermutationSampling = true;
        tparams.uniformRandomNumber      = RTXDI_JenkinsHash(g_RTXDIConst.m_FrameIndex);

        RAB_LightSample selectedSample = RAB_EmptyLightSample();
        int2 temporalPixelPos;

        outReservoir = RTXDI_DITemporalResampling(
            pixelPosition, surface, curReservoir, rng,
            rtParams, rbp, tparams,
            temporalPixelPos, selectedSample);
    }

    // Boiling filter (operates within the 8×8 group)
    RTXDI_BoilingFilter(LocalIndex, RAB_GetBoilingFilterStrength(), outReservoir);

    RTXDI_StoreDIReservoir(outReservoir, rbp, reservoirPosition,
        g_RTXDIConst.m_TemporalResamplingOutputBufferIndex);
}
