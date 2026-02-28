/*
 * RTXDISpatialResampling.hlsl
 *
 * Pass 3 of the ReSTIR DI pipeline (optional, runs after temporal resampling).
 * Gathers light reservoirs from spatially-neighbouring pixels and combines them
 * with the centre pixel's current reservoir, improving per-frame sampling quality.
 *
 * Input  : g_RTXDIConst.m_SpatialResamplingInputBufferIndex  (temporal output or initial output)
 * Output : g_RTXDIConst.m_SpatialResamplingOutputBufferIndex
 */

#include "RTXDIApplicationBridge.hlsli"

// RTXDI SDK
#include "Rtxdi/Utils/Checkerboard.hlsli"
#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/DI/SpatialResampling.hlsli"
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
void CSMain(uint2 GlobalIndex : SV_DispatchThreadID)
{
    RTXDI_RuntimeParameters         rtParams = GetRuntimeParams();
    RTXDI_ReservoirBufferParameters rbp      = GetReservoirBufferParams();

    uint2 reservoirPosition = GlobalIndex;
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(reservoirPosition, rtParams.activeCheckerboardField);
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(pixelPosition >= viewportSize))
        return;

    int2  iPixel        = int2(pixelPosition);

    // Use pass index 3 (distinct from initial=1, temporal=2) for decorrelated RNG
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 3u);

    RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

    // Default to an empty reservoir in case the pixel is invalid
    RTXDI_DIReservoir outReservoir = RTXDI_EmptyDIReservoir();

    if (RAB_IsSurfaceValid(surface))
    {
        // Load the centre pixel's reservoir (temporal output, or initial output if temporal is off)
        RTXDI_DIReservoir centerReservoir = RTXDI_LoadDIReservoir(rbp, reservoirPosition,
            g_RTXDIConst.m_SpatialResamplingInputBufferIndex);

        RTXDI_DISpatialResamplingParameters sparams;
        sparams.sourceBufferIndex             = g_RTXDIConst.m_SpatialResamplingInputBufferIndex;
        sparams.numSamples                    = g_RTXDIConst.m_SpatialNumSamples;
        sparams.numDisocclusionBoostSamples   = 0;
        sparams.targetHistoryLength           = 20u;   // match temporal maxHistoryLength
        sparams.biasCorrectionMode            = RTXDI_BIAS_CORRECTION_BASIC;
        sparams.samplingRadius                = g_RTXDIConst.m_SpatialSamplingRadius;
        sparams.depthThreshold                = 0.1f;
        sparams.normalThreshold               = 0.5f;
        sparams.enableMaterialSimilarityTest  = true;
        sparams.discountNaiveSamples          = false;

        RAB_LightSample selectedSample = RAB_EmptyLightSample();
        outReservoir = RTXDI_DISpatialResampling(
            pixelPosition, surface, centerReservoir,
            rng, rtParams, rbp, sparams, selectedSample);
    }

    RTXDI_StoreDIReservoir(outReservoir, rbp, reservoirPosition,
        g_RTXDIConst.m_SpatialResamplingOutputBufferIndex);
}
