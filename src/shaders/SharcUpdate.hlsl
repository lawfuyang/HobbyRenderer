// SharcUpdate.hlsl
//
// SHARC Update pass — sparse path tracing that populates the radiance cache.
// Compiled with SHARC_UPDATE=1.
//
// Dispatched over a downscaled grid (1 pixel per NxN block) to keep cost low.
// Each path segment is treated independently per the SHARC spec:
//   - SharcInit() at path start
//   - SharcUpdateHit() on each hit (may terminate early)
//   - SharcUpdateMiss() on sky miss
//   - SharcSetThroughput() after selecting next ray direction

// SHARC defines — must be set before including SharcCommon.h
#define SHARC_UPDATE 1
#define SHARC_ENABLE_CACHE_RESAMPLING 1
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1
#define SHARC_RADIANCE_SCALE 1e3f

#include "SharcCommon.hlsli"

#include "RaytracingCommon.hlsli"
#include "CommonLighting.hlsli"
#include "Atmosphere.hlsli"

#include "srrhi/hlsl/SHARC.hlsli"

static const srrhi::SharcConstants g_Sharc = srrhi::SharcUpdateInputs::GetSharcCB();

static const RaytracingAccelerationStructure            g_SceneAS   = srrhi::SharcUpdateInputs::GetSceneAS();
static const StructuredBuffer<srrhi::GPULight>          g_Lights    = srrhi::SharcUpdateInputs::GetLights();
static const StructuredBuffer<srrhi::PerInstanceData>   g_Instances = srrhi::SharcUpdateInputs::GetInstances();
static const StructuredBuffer<srrhi::MeshData>          g_MeshData  = srrhi::SharcUpdateInputs::GetMeshData();
static const StructuredBuffer<srrhi::MaterialConstants> g_Materials = srrhi::SharcUpdateInputs::GetMaterials();
static const StructuredBuffer<uint>                     g_Indices   = srrhi::SharcUpdateInputs::GetIndices();
static const StructuredBuffer<srrhi::VertexQuantized>   g_Vertices  = srrhi::SharcUpdateInputs::GetVertices();

static RWStructuredBuffer<uint64_t>                     u_HashEntries      = srrhi::SharcUpdateInputs::GetHashEntries();
static RWStructuredBuffer<SharcAccumulationData>        u_AccumulationBuf  = srrhi::SharcUpdateInputs::GetAccumulationBuffer();
static RWStructuredBuffer<SharcPackedData>              u_ResolvedBuf      = srrhi::SharcUpdateInputs::GetResolvedBuffer();

// ─── Helpers ─────────────────────────────────────────────────────────────────

bool TraceRayUpdate(RayDesc ray, RNG rng, out RayHitInfo hit)
{
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instanceIndex  = q.CandidateInstanceIndex();
            uint primitiveIndex = q.CandidatePrimitiveIndex();
            float2 bary         = q.CandidateTriangleBarycentrics();

            srrhi::PerInstanceData inst = g_Instances[instanceIndex];
            srrhi::MeshData mesh        = g_MeshData[inst.m_MeshDataIndex];
            srrhi::MaterialConstants mat= g_Materials[inst.m_MaterialIndex];

            float2 uvSample = GetInterpolatedUV(primitiveIndex, 0, bary, mesh, g_Indices, g_Vertices);

            if (mat.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_MASK)
            {
                if (AlphaTest(uvSample, mat))
                    q.CommitNonOpaqueTriangleHit();
            }
            else if (mat.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_BLEND)
            {
                if (NextFloat(rng) < saturate(mat.m_BaseColor.w))
                    q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        hit.m_InstanceIndex  = q.CommittedInstanceIndex();
        hit.m_PrimitiveIndex = q.CommittedPrimitiveIndex();
        hit.m_Barycentrics   = q.CommittedTriangleBarycentrics();
        hit.m_RayT           = q.CommittedRayT();
        return true;
    }

    hit = (RayHitInfo)0;
    return false;
}

// ─── Compute Shader Entry Point ───────────────────────────────────────────────

[numthreads(8, 8, 1)]
void SharcUpdate_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // Viewport size is stored in the constant buffer
    uint viewW = (uint)g_Sharc.m_EntriesNum; // placeholder — actual viewport from dispatch size
    // We use the dispatch thread ID directly as the pixel coordinate for the
    // sparse update grid. The dispatch is sized to (width/downscale, height/downscale).

    // Stochastic pixel offset within the downscale block for temporal coverage
    RNG rng = InitRNG(dispatchThreadID.xy, g_Sharc.m_FrameIndex);

    // Build primary ray from the sparse pixel position
    // Use a simple jitter within the 5x5 block for temporal coverage
    const uint kDownscale = 5u;
    uint2 pixelBase = dispatchThreadID.xy * kDownscale;
    uint2 jitterOffset = uint2(
        g_Sharc.m_FrameIndex % kDownscale,
        (g_Sharc.m_FrameIndex / kDownscale) % kDownscale
    );
    uint2 pixel = pixelBase + jitterOffset;

    // We don't have viewport size in this CB — use a fixed large value and
    // rely on the dispatch size to bound execution.
    // The actual viewport dimensions are passed implicitly via dispatch size.

    float2 uv = (float2(pixel) + 0.5f) / float2(1920.0f, 1080.0f); // approximate; actual size from dispatch
    float2 clipPos = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // Build ray from clip position using inverse projection stored in camera position
    // For a proper implementation we need the view/proj matrices — stored in the CB
    // For now use a simple forward ray from camera position
    // NOTE: Full implementation requires view matrix in the CB (added in SharcRenderer)
    RayDesc ray;
    ray.Origin    = g_Sharc.m_CameraPosition;
    ray.Direction = normalize(float3(clipPos.x, clipPos.y, -1.0f)); // simplified
    ray.TMin      = 0.001f;
    ray.TMax      = 1e10f;

    SharcParameters sharcParams = BuildSharcParameters(
        g_Sharc.m_CameraPosition,
        g_Sharc.m_SceneScale,
        g_Sharc.m_EntriesNum,
        u_HashEntries,
        u_AccumulationBuf,
        u_ResolvedBuf
    );

    SharcState sharcState;
    SharcInit(sharcState);

    float3 throughput = float3(1.0f, 1.0f, 1.0f);

    const int kMaxBounces = 8;
    for (int bounce = 0; bounce < kMaxBounces; ++bounce)
    {
        RayHitInfo hit;
        bool didHit = TraceRayUpdate(ray, rng, hit);

        if (didHit)
        {
            srrhi::PerInstanceData inst  = g_Instances[hit.m_InstanceIndex];
            srrhi::MeshData mesh         = g_MeshData[inst.m_MeshDataIndex];
            srrhi::MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

            inst.m_LODIndex = 0;
            FullHitAttributes attr = GetFullHitAttributes(hit, ray, inst, mesh, g_Indices, g_Vertices);
            PBRAttributes     pbr  = GetPBRAttributes(attr, mat, 0.0f);

            float3 N = pbr.normal;
            float3 V = -ray.Direction;
            if (dot(N, V) < 0.0f) N = -N;

            // Direct lighting at this hit point
            LightingInputs inputs;
            inputs.N                = N;
            inputs.V                = V;
            inputs.L                = float3(0, 0, 0);
            inputs.worldPos         = attr.m_WorldPos;
            inputs.baseColor        = pbr.baseColor;
            inputs.roughness        = pbr.roughness;
            inputs.metallic         = pbr.metallic;
            inputs.ior              = mat.m_IOR;
            inputs.radianceMipCount = 0;
            inputs.enableRTShadows  = true;
            inputs.sceneAS          = g_SceneAS;
            inputs.instances        = g_Instances;
            inputs.meshData         = g_MeshData;
            inputs.materials        = g_Materials;
            inputs.indices          = g_Indices;
            inputs.vertices         = g_Vertices;
            inputs.lights           = g_Lights;
            inputs.sunRadiance      = GetAtmosphereSunRadiance(GetAtmospherePos(attr.m_WorldPos), g_Sharc.m_CameraPosition - attr.m_WorldPos, g_Lights[0].m_Intensity);
            inputs.sunDirection     = normalize(g_Sharc.m_CameraPosition - attr.m_WorldPos); // placeholder
            inputs.useSunRadiance   = true;
            inputs.sunShadow        = 0.0f;

            PrepareLightingByproducts(inputs);
            LightingComponents direct = AccumulateDirectLighting(inputs, 1u, 0.9999f, rng);
            float3 directLighting = pbr.emissive + direct.diffuse + direct.specular;

            // SHARC hit update
            SharcHitData sharcHit;
            sharcHit.positionWorld = attr.m_WorldPos;
            sharcHit.normalWorld   = normalize(attr.m_WorldNormal);

            bool continueTracing = SharcUpdateHit(sharcParams, sharcState, sharcHit, directLighting, NextFloat(rng));
            if (!continueTracing)
                break;

            // Sample next direction (diffuse cosine-weighted)
            float3 newDir = SampleHemisphereCosine(NextFloat2(rng), N);
            if (dot(N, newDir) <= 0.0f) break;

            float3 brdfWeight = pbr.baseColor * (1.0f - pbr.metallic);
            throughput *= brdfWeight;

            SharcSetThroughput(sharcState, throughput);
            throughput = float3(1.0f, 1.0f, 1.0f); // reset per-segment

            ray.Origin    = attr.m_WorldPos;
            ray.Direction = newDir;
            ray.TMin      = 1e-4f;
            ray.TMax      = 1e10f;
        }
        else
        {
            // Sky miss
            float3 skyRadiance = GetAtmosphereSkyRadiance(
                ray.Origin, ray.Direction,
                normalize(g_Sharc.m_CameraPosition), // placeholder sun dir
                g_Lights[0].m_Intensity, false);

            SharcUpdateMiss(sharcParams, sharcState, skyRadiance);
            break;
        }
    }
}
