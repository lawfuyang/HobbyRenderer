// SharcHelpers.hlsli — Shared helpers for SHARC passes (Query, Update).
//
// Must be included after SharcCommon.h and srrhi/hlsl/SHARC.hlsli.

// Assemble SharcParameters from per-pass bindings and hardcoded hash-grid defaults.
SharcParameters BuildSharcParameters(
    srrhi::SHARCConstants constants,
    RWStructuredBuffer<uint64_t> hashEntries,
    RWStructuredBuffer<SharcPackedData> resolved,
    RWStructuredBuffer<SharcAccumulationData> accumulation)
{
    SharcParameters sharcParams;

    // Hash-grid tuning
    sharcParams.hashGridParameters.cameraPosition = constants.m_CameraPosition;
    sharcParams.hashGridParameters.logarithmBase  = srrhi::SHARCConsts::HASH_GRID_LOGARITHM_BASE;
    sharcParams.hashGridParameters.sceneScale     = srrhi::SHARCConsts::HASH_GRID_SCENE_SCALE;
    sharcParams.hashGridParameters.levelBias      = srrhi::SHARCConsts::HASH_GRID_LEVEL_BIAS;

    sharcParams.hashGridData.capacity          = srrhi::SHARCConsts::SHARC_CACHE_ENTRIES;
    sharcParams.hashGridData.hashEntriesBuffer = hashEntries;
    sharcParams.radianceScale                  = srrhi::SHARCConsts::RADIANCE_SCALE;
    sharcParams.accumulationBuffer             = accumulation;
    sharcParams.resolvedBuffer                 = resolved;

    return sharcParams;
}