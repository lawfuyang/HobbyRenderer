// SharcIndirectQuery.hlsl — Combined-mode SHARC indirect query pass.
//
// Runs ONLY when INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC is active (combined mode).
// Replaces the full ReSTIR GI pipeline (BrdfRayTracing → ShadeSecondarySurfaces
// → GI resampling → FinalShading) with a single lightweight SHARC cache lookup.
//
// Algorithm:
//   1. Reconstruct primary surface from G-buffer (via RAB_GetGBufferSurface).
//   2. Trace a cosine-weighted diffuse random walk until the accumulated path
//      length spans the local voxel size (identical to SHARCQuery.hlsl).
//   3. Query SharcGetCachedRadiance() at the final hit point.
//   4. Write DEMODULATED diffuse radiance to u_DiffuseLighting via
//      StoreShadingOutput(isFirstPass=false, isLastPass=true), additively
//      blending with the DI contribution and triggering NRD packing.
//
// Key difference from standalone SHARCQuery.hlsl:
//   SHARCQuery writes:  indirectRadiance * albedo * (1-metallic)  → g_RG_SHARCIndirect
//   This shader writes: indirectRadiance (DEMODULATED, no BSDF factor) → u_DiffuseLighting
//   CompositingPass then applies albedo once — no double-albedo multiplication.
//
// Standalone SHARC mode (INDIRECT_LIGHTING_MODE_SHARC) is NOT affected:
//   SHARCQuery.hlsl → g_RG_SHARCIndirect → DeferredRenderer (unchanged).

#pragma pack_matrix(row_major)

// ---- SHARC defines (must precede SharcCommon.h) ----------------------------
// NOTE: Do NOT #include "SharcCommon.h" here — it is already included
// transitively via RtxdiApplicationBridge.hlsli → RAB_Buffers.hlsli.
// These defines must be set BEFORE that include so SharcCommon.h sees them.
#define SHARC_QUERY                     1
#define SHARC_ENABLE_64_BIT_ATOMICS     1
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1

// ---- RTXDI application bridge (ResamplingPassInputs bindings) ---------------
// Provides: g_Const, t_GBufferDepth/Normals/Albedo/ORM, SceneBVH,
//           t_InstanceData/GeometryData/MaterialConstants/SceneIndices/SceneVertices,
//           u_DiffuseLighting, u_SpecularLighting,
//           u_SHARCHashEntries, u_SHARCResolved, u_SHARCAccumulation,
//           RAB_GetGBufferSurface, RTXDI_ReservoirPosToPixelPos, etc.
#include "RtxdiApplicationBridge/RtxdiApplicationBridge.hlsli"

// ---- Raytracing helpers (TraceRayStandard, GetFullHitAttributes, InitRNG) ---
#include "../../RaytracingCommon.hlsli"

#ifdef WITH_NRD
#define NRD_HEADER_ONLY
#include <NRD.hlsli>
#endif

#include "ShadingHelpers.hlsli"

// ============================================================================
// BuildSharcParameters — inline version for ResamplingPassInputs context.
//
// SharcHelpers.hlsli::BuildSharcParameters() takes srrhi::SHARCConstants, but
// this shader uses ResamplingConstants (g_Const from ResamplingPassInputs).
// All required values are available via g_Const and srrhi::SHARCConsts.
// ============================================================================
SharcParameters BuildSharcParams()
{
    SharcParameters sharcParams;

    // Camera position from current-frame view matrix (same field used in ShadeSecondarySurfaces.hlsl)
    sharcParams.hashGridParameters.cameraPosition = g_Const.view.m_CameraDirectionOrPosition.xyz;
    sharcParams.hashGridParameters.logarithmBase  = srrhi::SHARCConsts::HASH_GRID_LOGARITHM_BASE;
    sharcParams.hashGridParameters.sceneScale     = srrhi::SHARCConsts::HASH_GRID_SCENE_SCALE;
    sharcParams.hashGridParameters.levelBias      = srrhi::SHARCConsts::HASH_GRID_LEVEL_BIAS;

    sharcParams.hashGridData.capacity          = srrhi::SHARCConsts::SHARC_CACHE_ENTRIES;
    sharcParams.hashGridData.hashEntriesBuffer = u_SHARCHashEntries;
    sharcParams.radianceScale                  = srrhi::SHARCConsts::RADIANCE_SCALE;
    sharcParams.accumulationBuffer             = u_SHARCAccumulation;
    sharcParams.resolvedBuffer                 = u_SHARCResolved;

    return sharcParams;
}

// ============================================================================
// TraceIndirectRay — cosine-weighted diffuse random walk from the primary surface.
//
// Identical logic to SHARCQuery.hlsl::TraceIndirectRay, adapted to use
// ResamplingPassInputs resource bindings (SceneBVH, t_InstanceData, etc.)
// and g_Const.runtimeParams.frameIndex for RNG seeding.
//
// Continues bouncing until the accumulated path length >= local voxel size,
// or until MAX_BOUNCES is reached / a sky miss occurs.
//
// status values:
//   0 = valid hit, accumulated path >= voxel size → hitData is valid
//   1 = cosine direction clipped below primary surface
//   2 = sky miss at some bounce / max bounces reached
// ============================================================================
static const uint SHARC_QUERY_MAX_BOUNCES = 4;

struct IndirectRayResult
{
    SharcHitData hitData;
    uint         status;
};

IndirectRayResult TraceIndirectRay(uint2 pixel, float3 worldPos, float3 N, SharcParameters sharcParams)
{
    IndirectRayResult result = (IndirectRayResult)0;

    RNG rng = InitRNG(pixel, g_Const.runtimeParams.frameIndex);

    float3 origin    = worldPos;
    float3 normal    = N;
    float  totalDist = 0.0f;

    for (uint bounce = 0; bounce < SHARC_QUERY_MAX_BOUNCES; ++bounce)
    {
        float3 rayDir = SampleHemisphereCosine(NextFloat2(rng), normal);
        if (dot(normal, rayDir) <= 0.0f)
        {
            result.status = 1;
            return result;
        }

        RayDesc ray;
        ray.Origin    = origin + normal * 1e-3f;
        ray.Direction = rayDir;
        ray.TMin      = 1e-3f;
        ray.TMax      = 1e10f;

        RayHitInfo hitInfo;
        bool didHit = TraceRayStandard(ray, rng, hitInfo,
            SceneBVH,
            t_InstanceData, t_GeometryData, t_MaterialConstants,
            t_SceneIndices, t_SceneVertices);
        if (!didHit)
        {
            result.status = 2;
            return result;
        }

        srrhi::PerInstanceData inst = t_InstanceData[hitInfo.m_InstanceIndex];
        srrhi::MeshData        mesh = t_GeometryData[inst.m_MeshDataIndex];
        FullHitAttributes attr      = GetFullHitAttributes(hitInfo, ray, inst, mesh, t_SceneIndices, t_SceneVertices);

        float3 hitPos = attr.m_WorldPos;
        totalDist += length(hitPos - origin);

        uint  hitLevel  = HashGridGetLevel(hitPos, sharcParams.hashGridParameters);
        float voxelSize = HashGridGetVoxelSize(hitLevel, sharcParams.hashGridParameters);

        if (totalDist >= voxelSize)
        {
            result.hitData.positionWorld = hitPos;
            result.hitData.normalWorld   = normalize(attr.m_WorldNormal);
            result.status = 0;
            return result;
        }

        // Segment still too short — continue tracing from this surface
        origin = hitPos;
        normal = normalize(attr.m_WorldNormal);
    }

    // Reached max bounces without spanning the voxel
    result.status = 2;
    return result;
}

// ============================================================================
// Compute shader entry point
// ============================================================================

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void SharcIndirectQuery_CSMain(uint2 GlobalIndex : SV_DispatchThreadID)
{
    // Map reservoir-space index → full-screen pixel position (handles checkerboard)
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(GlobalIndex, g_Const.runtimeParams.activeCheckerboardField);

    if (any(pixelPosition >= uint2(g_Const.view.m_ViewportSize)))
        return;

    // Read primary surface from G-buffer.
    // RAB_GetGBufferSurface provides viewDepth, roughness, worldPos, normal, etc.
    const RAB_Surface primarySurface = RAB_GetGBufferSurface(pixelPosition, false);

    if (!RAB_IsSurfaceValid(primarySurface))
    {
        // Sky pixel — write zero so NRD/CompositingPass adds nothing.
        // isFirstPass=false: additive blend with DI (which already wrote 0 for sky)
        // isLastPass=true:   triggers NRD packing
        StoreShadingOutput(GlobalIndex, pixelPosition,
            /*viewDepth=*/0.0f, /*roughness=*/0.0f,
            /*diffuse=*/float3(0, 0, 0), /*specular=*/float3(0, 0, 0),
            /*lightDistance=*/0.0f,
            /*isFirstPass=*/false, /*isLastPass=*/true);
        return;
    }

    SharcParameters sharcParams = BuildSharcParams();

    // Trace a cosine-weighted diffuse walk from the primary surface until the
    // accumulated path length spans the local voxel size, then query the cache.
    IndirectRayResult rr = TraceIndirectRay(pixelPosition, primarySurface.worldPos, primarySurface.normal, sharcParams);

    float3 demodulatedDiffuse = float3(0, 0, 0);

    if (rr.status == 0)
    {
        float3 indirectRadiance = float3(0, 0, 0);
        bool found = SharcGetCachedRadiance(sharcParams, rr.hitData, indirectRadiance, false);

        // Write DEMODULATED radiance — no albedo × (1-metallic) factor here.
        // CompositingPass applies diffuseAlbedo once (remodulation happens there).
        // Contrast with standalone SHARCQuery.hlsl which writes the modulated
        // value directly to g_RG_SHARCIndirect for DeferredRenderer addition.
        if (found)
            demodulatedDiffuse = indirectRadiance;
    }

    // Additively blend SHARC diffuse with the DI contribution already in
    // u_DiffuseLighting (isFirstPass=false), then NRD-pack the combined signal
    // (isLastPass=true). hitT=0 matches the existing GI FinalShading.hlsl behavior.
    // SHARC has no specular indirect — specular channel is left as DI-only.
    StoreShadingOutput(GlobalIndex, pixelPosition,
        primarySurface.viewDepth, primarySurface.material.roughness,
        demodulatedDiffuse, /*specular=*/float3(0, 0, 0),
        /*lightDistance=*/0.0f,
        /*isFirstPass=*/false, /*isLastPass=*/true);
}
