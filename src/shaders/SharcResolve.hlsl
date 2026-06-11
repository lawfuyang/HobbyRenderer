// SharcResolve.hlsl
//
// SHARC Resolve pass — compute shader that blends per-frame accumulation data
// with previously resolved cross-frame data, handles temporal accumulation,
// stale entry eviction, and resets the accumulation buffer for the next frame.
//
// Dispatched as (ceil(entriesNum / 256), 1, 1) thread groups.

#define SHARC_RADIANCE_SCALE 1e3f
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1

#include "SharcCommon.h"

#include "srrhi/hlsl/SHARC.hlsli"

static const srrhi::SharcConstants g_Sharc = srrhi::SharcResolveInputs::GetSharcCB();

static RWStructuredBuffer<uint64_t> u_HashEntries     = srrhi::SharcResolveInputs::GetHashEntries();
static RWStructuredBuffer<SharcAccumulationData>    u_AccumulationBuf = srrhi::SharcResolveInputs::GetAccumulationBuffer();
static RWStructuredBuffer<SharcPackedData> u_ResolvedBuf = srrhi::SharcResolveInputs::GetResolvedBuffer();

static const uint LINEAR_BLOCK_SIZE = 256;

[numthreads(256, 1, 1)]
void SharcResolve_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    SharcParameters sharcParams;
    sharcParams.hashGridParameters.cameraPosition = g_Sharc.m_CameraPosition;
    sharcParams.hashGridParameters.sceneScale     = g_Sharc.m_SceneScale;
    sharcParams.hashGridParameters.logarithmBase  = SHARC_GRID_LOGARITHM_BASE;
    sharcParams.hashGridParameters.levelBias      = SHARC_GRID_LEVEL_BIAS;

    sharcParams.hashGridData.capacity             = g_Sharc.m_EntriesNum;
    sharcParams.hashGridData.hashEntriesBuffer    = u_HashEntries;

    sharcParams.accumulationBuffer                = u_AccumulationBuf;
    sharcParams.resolvedBuffer                    = u_ResolvedBuf;
    sharcParams.radianceScale                     = SHARC_RADIANCE_SCALE;

    SharcResolveParameters resolveParams;
    resolveParams.cameraPositionPrev    = g_Sharc.m_CameraPositionPrev;
    resolveParams.accumulationFrameNum  = g_Sharc.m_AccumulationFrameNum;
    resolveParams.responsiveFrameNum    = 8; // fixed responsive window
    resolveParams.staleFrameNumMax      = g_Sharc.m_StaleFrameNumMax;
    resolveParams.frameIndex            = g_Sharc.m_FrameIndex;

    SharcResolveEntry(dispatchThreadID.x, sharcParams, resolveParams);
}
