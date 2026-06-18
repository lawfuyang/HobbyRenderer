// SharcQuery.hlsl
//
// SHARC Query/Render pass — full path tracing with early termination via
// SHARC cache lookup on non-primary hits.
// Compiled with SHARC_QUERY=1.
//
// On each eligible hit (bounce >= 1), SharcGetCachedRadiance() is called.
// If the cache has a valid entry, the path terminates early using cached
// radiance instead of continuing to trace.

#define SHARC_QUERY 1
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1
#define SHARC_RADIANCE_SCALE 1e3f

#include "SharcCommon.hlsli"

#include "RaytracingCommon.hlsli"
#include "CommonLighting.hlsli"
#include "Atmosphere.hlsli"

#include "srrhi/hlsl/SHARC.hlsli"

static const srrhi::SharcConstants g_Sharc = srrhi::SharcQueryInputs::GetSharcCB();

static const RaytracingAccelerationStructure            g_SceneAS   = srrhi::SharcQueryInputs::GetSceneAS();
static const StructuredBuffer<srrhi::GPULight>          g_Lights    = srrhi::SharcQueryInputs::GetLights();
static const StructuredBuffer<srrhi::PerInstanceData>   g_Instances = srrhi::SharcQueryInputs::GetInstances();
static const StructuredBuffer<srrhi::MeshData>          g_MeshData  = srrhi::SharcQueryInputs::GetMeshData();
static const StructuredBuffer<srrhi::MaterialConstants> g_Materials = srrhi::SharcQueryInputs::GetMaterials();
static const StructuredBuffer<uint>                     g_Indices   = srrhi::SharcQueryInputs::GetIndices();
static const StructuredBuffer<srrhi::VertexQuantized>   g_Vertices  = srrhi::SharcQueryInputs::GetVertices();

static RWStructuredBuffer<uint64_t>                     u_HashEntries  = srrhi::SharcQueryInputs::GetHashEntries();
static RWStructuredBuffer<SharcAccumulationData>        u_AccumulationBuf = srrhi::SharcQueryInputs::GetDummyAccumulationBuffer();
static RWStructuredBuffer<SharcPackedData>              u_ResolvedBuf  = srrhi::SharcQueryInputs::GetResolvedBuffer();
static RWTexture2D<float4>                              g_Output       = srrhi::SharcQueryInputs::GetOutput();
static RWTexture2D<uint>                                g_BounceCount  = srrhi::SharcQueryInputs::GetBounceCountOutput();
static RWTexture2D<float4>                              g_CachedRadianceDbg = srrhi::SharcQueryInputs::GetCachedRadianceOutput();

// ─── Compute Shader Entry Point ───────────────────────────────────────────────

[numthreads(8, 8, 1)]
void SharcQuery_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // Bounds check — viewport size from dispatch
    uint2 pixel = dispatchThreadID.xy;

    RNG rng = InitRNG(pixel, g_Sharc.m_FrameIndex);

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

    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 accumulatedRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 cachedRadianceDbg   = float3(0.0f, 0.0f, 0.0f);
    uint   bounceCount = 0;

    const int kMaxBounces = 4;
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

            float3 N = pbr.normal;
            float3 V = -ray.Direction;
            if (dot(N, V) < 0.0f) N = -N;

            // On non-primary hits, try the SHARC cache first
            if (bounce > 0)
            {
                SharcHitData sharcHit;
                sharcHit.positionWorld = attr.m_WorldPos;
                sharcHit.normalWorld   = normalize(attr.m_WorldNormal);

                bounceCount = (uint)bounce;

                float3 cachedRadiance;
                if (SharcGetCachedRadiance(sharcParams, sharcHit, cachedRadiance, false))
                {
                    accumulatedRadiance += throughput * cachedRadiance;
                    cachedRadianceDbg = cachedRadiance;
                    break;
                }
            }

            // Direct lighting
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
            inputs.sunRadiance      = GetAtmosphereSunRadiance(GetAtmospherePos(attr.m_WorldPos), g_Sharc.m_SunDirection, g_Lights[0].m_Intensity);
            inputs.sunDirection     = g_Sharc.m_SunDirection;
            inputs.useSunRadiance   = true;
            inputs.sunShadow        = 0.0f; // unused by RNG variant — casts its own shadow rays

            PrepareLightingByproducts(inputs);
            LightingComponents direct = AccumulateDirectLighting(inputs, g_Sharc.m_LightCount, g_Lights[0].m_CosSunAngularRadius, rng);

            accumulatedRadiance += throughput * (pbr.emissive + direct.diffuse + (bounce == 0 ? direct.specular : 0.0f));

            // Russian roulette
            if (bounce >= 2)
            {
                float continuePr = saturate(max(throughput.r, max(throughput.g, throughput.b)));
                if (NextFloat(rng) > continuePr) break;
                throughput /= continuePr;
            }

            // BRDF sample (diffuse)
            float3 newDir = SampleHemisphereCosine(NextFloat2(rng), N);
            if (dot(N, newDir) <= 0.0f) break;

            throughput *= pbr.baseColor * (1.0f - pbr.metallic);

            ray.Origin    = attr.m_WorldPos;
            ray.Direction = newDir;
            ray.TMin      = 1e-4f;
            ray.TMax      = 1e10f;
        }
        else
        {
            float3 skyRadiance = GetAtmosphereSkyRadiance(
                ray.Origin, ray.Direction,
                g_Sharc.m_SunDirection,
                g_Lights[0].m_Intensity, bounce == 0);
            accumulatedRadiance += throughput * skyRadiance;
            break;
        }
    }

    g_Output[pixel]              = float4(accumulatedRadiance, 1.0f);
    if (g_Sharc.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_BOUNCE_HEATMAP)
        g_BounceCount[pixel]         = bounceCount;
    if (g_Sharc.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_CACHED_RADIANCE)
        g_CachedRadianceDbg[pixel] = float4(cachedRadianceDbg, 1.0f);
}
