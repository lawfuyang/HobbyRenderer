#ifndef SHARC_COMMON_HLSLI
#define SHARC_COMMON_HLSLI

#include "SharcCommon.h"

SharcParameters BuildSharcParameters(float3 cameraPosition, float sceneScale, uint capacity,
                                     RWStructuredBuffer<uint64_t> hashEntriesBuffer,
                                     RWStructuredBuffer<SharcAccumulationData> accumulationBuffer,
                                     RWStructuredBuffer<SharcPackedData> resolvedBuffer)
{
    SharcParameters sp;
    sp.hashGridParameters.cameraPosition = cameraPosition;
    sp.hashGridParameters.sceneScale     = sceneScale;
    sp.hashGridParameters.logarithmBase  = SHARC_GRID_LOGARITHM_BASE;
    sp.hashGridParameters.levelBias      = SHARC_GRID_LEVEL_BIAS;

    sp.hashGridData.capacity             = capacity;
    sp.hashGridData.hashEntriesBuffer    = hashEntriesBuffer;

    sp.accumulationBuffer                = accumulationBuffer;
    sp.resolvedBuffer                    = resolvedBuffer;
    sp.radianceScale                     = SHARC_RADIANCE_SCALE;

    return sp;
}

#endif // SHARC_COMMON_HLSLI
