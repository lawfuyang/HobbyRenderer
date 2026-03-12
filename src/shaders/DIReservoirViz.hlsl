////////////////////////////////////////////////////////////////////////////////
// DIReservoirViz.hlsl
//
// Compute shader for DI Reservoir Subfield Visualization.
// Reads from the DI light reservoir buffer and writes a color visualization
// to an output texture based on the selected DIReservoirField.
//
// Bindings:
//   b0 — DIReservoirVizParameters (volatile CB)
//   b1 — RTXDIConstants (for maxHistoryLength in M-field visualization)
//   u0 — RWStructuredBuffer<RTXDI_PackedDIReservoir> (DI reservoir buffer)
//   u1 — RWTexture2D<float4> (visualization output)
////////////////////////////////////////////////////////////////////////////////

#include "ShaderShared.h"
#include "DIReservoirVizParameters.h"
#include "RNG.hlsli"

// Pull in RTXDI parameter structs (defines RTXDI_PackedDIReservoir etc.)
#include "Rtxdi/RtxdiParameters.h"

// Constant buffers
ConstantBuffer<DIReservoirVizParameters> g_Const : register(b0);

cbuffer RTXDICBuf : register(b1)
{
    RTXDIConstants g_RTXDIConst;
};

// DI reservoir buffer at u0 — must be declared before ReservoirStorage.hlsli
RWStructuredBuffer<RTXDI_PackedDIReservoir> u_DIReservoirs : register(u0);
#define RTXDI_LIGHT_RESERVOIR_BUFFER u_DIReservoirs

// Visualization output texture at u1
RWTexture2D<float4> t_Output : register(u1);

// We only read reservoirs — disable the store function to avoid compile errors
// if the buffer is bound read-only in some configurations.
#define RTXDI_ENABLE_STORE_RESERVOIR 0

// Include RTXDI reservoir storage (RTXDI_LoadDIReservoir, RTXDI_PixelPosToReservoirPos, etc.)
// This also includes Rtxdi/DI/Reservoir.hlsli and Rtxdi/Utils/ReservoirAddressing.hlsli
#include "Rtxdi/DI/ReservoirStorage.hlsli"

[numthreads(16, 16, 1)]
void main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    // Convert reservoir position to pixel position (handles checkerboard)
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(GlobalIndex, g_Const.runtimeParams.activeCheckerboardField);

    if (any(pixelPosition >= uint2(g_Const.view.m_ViewportSize)))
        return;

    // Load the DI reservoir at this pixel's reservoir position
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(pixelPosition, g_Const.runtimeParams.activeCheckerboardField);
    RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(
        g_Const.reservoirBufferParams,
        reservoirPosition,
        g_Const.bufferIndices.spatialResamplingInputBufferIndex);

    switch (g_Const.diReservoirField)
    {
    case DI_RESERVOIR_FIELD_LIGHT_DATA:
    {
        uint lightValid = reservoir.lightData & RTXDI_DIReservoir_LightValidBit;
        uint lightIndex = reservoir.lightData & RTXDI_DIReservoir_LightIndexMask;
        if (!lightValid)
        {
            t_Output[pixelPosition] = float4(0, 0, 0, 1);
        }
        else
        {
            if (lightIndex > g_Const.maxLightsInBuffer)
                lightIndex -= g_Const.maxLightsInBuffer;
            t_Output[pixelPosition] = IndexToColor(lightIndex);
        }
        break;
    }
    case DI_RESERVOIR_FIELD_UV_DATA:
        t_Output[pixelPosition] = float4(RTXDI_GetDIReservoirSampleUV(reservoir), 0.0, 1.0);
        break;
    case DI_RESERVOIR_FIELD_TARGET_PDF:
        t_Output[pixelPosition] = reservoir.targetPdf;
        break;
    case DI_RESERVOIR_FIELD_M:
        t_Output[pixelPosition] = rgLerp(reservoir.M / (float)g_RTXDIConst.m_TemporalMaxHistoryLength);
        break;
    case DI_RESERVOIR_FIELD_PACKED_VISIBILITY:
        t_Output[pixelPosition] = reservoir.packedVisibility;
        break;
    case DI_RESERVOIR_FIELD_SPATIAL_DISTANCE:
        t_Output[pixelPosition] = float4(reservoir.spatialDistance, 1.0, 1.0);
        break;
    case DI_RESERVOIR_FIELD_AGE:
        t_Output[pixelPosition] = reservoir.age;
        break;
    case DI_RESERVOIR_FIELD_CANONICAL_WEIGHT:
        t_Output[pixelPosition] = reservoir.canonicalWeight;
        break;
    }
}
