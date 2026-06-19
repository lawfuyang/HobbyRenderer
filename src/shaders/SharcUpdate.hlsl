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

#include "RaytracingCommon.hlsli"
#include "CommonLighting.hlsli"
#include "Atmosphere.hlsli"

#include "SharcCommon.hlsli"

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

// ─── Compute Shader Entry Point ───────────────────────────────────────────────

[numthreads(8, 8, 1)]
void SharcUpdate_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
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

    // Build primary ray using inverse projection from the constant buffer
    float2 uv      = (float2(pixel) + 0.5f) * g_Sharc.m_ViewportSizeInv;
    float2 clipPos = UVToClipXY(uv);

    float4 rayEndFar = MatrixMultiply(float4(clipPos, 0.9f, 1.0f), g_Sharc.m_MatClipToWorldNoOffset);
    rayEndFar.xyz   /= rayEndFar.w;

    RayDesc ray;
    ray.Origin    = g_Sharc.m_CameraPosition;
    ray.Direction = normalize(rayEndFar.xyz - ray.Origin);
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
        bool didHit = TraceRayStandard(ray, rng, hit, g_SceneAS, g_Instances, g_MeshData, g_Materials, g_Indices, g_Vertices);

        if (didHit)
        {
            srrhi::PerInstanceData inst  = g_Instances[hit.m_InstanceIndex];
            srrhi::MeshData mesh         = g_MeshData[inst.m_MeshDataIndex];
            srrhi::MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

            inst.m_LODIndex = 0;
            FullHitAttributes attr = GetFullHitAttributes(hit, ray, inst, mesh, g_Indices, g_Vertices);
            PBRAttributes     pbr  = GetPBRAttributes(attr, mat, 0.0f);
            pbr.roughness = max(g_Sharc.m_RoughnessThreshold, pbr.roughness);

            float3 N = pbr.normal;
            float3 V = -ray.Direction;
            if (dot(N, V) < 0.0f) N = -N;

            // Direct lighting at this hit point — shared helper
            LightingInputs inputs = FillSharcLightingInputs(
                N, V, attr.m_WorldPos, pbr, mat,
                g_SceneAS, g_Instances, g_MeshData, g_Materials,
                g_Indices, g_Vertices, g_Lights, g_Sharc.m_SunDirection);

            PrepareLightingByproducts(inputs);
            LightingComponents direct = AccumulateDirectLighting(inputs, g_Sharc.m_LightCount, g_Lights[0].m_CosSunAngularRadius, rng);
            float3 directLighting = pbr.emissive + direct.diffuse + direct.specular;

            // SHARC hit update
            SharcHitData sharcHit;
            sharcHit.positionWorld = attr.m_WorldPos;
            sharcHit.normalWorld   = normalize(attr.m_WorldNormal);

            bool continueTracing = SharcUpdateHit(sharcParams, sharcState, sharcHit, directLighting, NextFloat(rng));
            if (!continueTracing)
                break;

            // BRDF importance sampling — shared helper
            float3 newDir;
            float3 brdfWeight;
            bool isSpecular;
            if (!SampleSharcDirection(N, V, pbr, inputs.F, inputs.F0, rng, newDir, brdfWeight, isSpecular))
                break;
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
                g_Sharc.m_SunDirection,
                g_Lights[0].m_Intensity, false);

            SharcUpdateMiss(sharcParams, sharcState, skyRadiance);
            break;
        }
    }
}
