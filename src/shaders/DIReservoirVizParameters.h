////////////////////////////////////////////////////////////////////////////////
// DIReservoirVizParameters.h
//
// Shared C++/HLSL header for the DI Reservoir Subfield Visualization pass.
// Defines the DIReservoirField enum and DIReservoirVizParameters constant
// buffer struct. Both the CPU pass (RTXDIVisualizationRenderer) and the GPU
// shader (DIReservoirViz.hlsl) include this file.
//
// NOTE: This file is shared between C++ and HLSL. Use #ifdef __cplusplus
// guards for language-specific constructs.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ShaderShared.h"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

// ============================================================================
// DIReservoirField — selects which field of the DI reservoir to visualize.
// Values match the RTXDI FullSample reference exactly.
// ============================================================================
enum
#ifdef __cplusplus
class
#endif
DIReservoirField
{
    DI_RESERVOIR_FIELD_LIGHT_DATA        = 0,
    DI_RESERVOIR_FIELD_UV_DATA           = 1,
    DI_RESERVOIR_FIELD_TARGET_PDF        = 2,
    DI_RESERVOIR_FIELD_M                 = 3,
    DI_RESERVOIR_FIELD_PACKED_VISIBILITY = 4,
    DI_RESERVOIR_FIELD_SPATIAL_DISTANCE  = 5,
    DI_RESERVOIR_FIELD_AGE               = 6,
    DI_RESERVOIR_FIELD_CANONICAL_WEIGHT  = 7
};

// ============================================================================
// DIReservoirVizParameters — constant buffer for the DI reservoir viz pass.
// Bound at b0 in DIReservoirViz.hlsl.
// ============================================================================
struct DIReservoirVizParameters
{
#ifdef __cplusplus
    using uint = uint32_t;
#endif

    PlanarViewConstants             view;
    RTXDI_RuntimeParameters         runtimeParams;

    RTXDI_ReservoirBufferParameters reservoirBufferParams;
    RTXDI_DIBufferIndices           bufferIndices;

    DIReservoirField                diReservoirField;
    uint                            maxLightsInBuffer;
    uint                            pad1;
    uint                            pad2;
};
