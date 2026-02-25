#include "ShaderShared.h"
#include "RaytracingCommon.hlsli"
#include "CommonLighting.hlsli"
#include "Atmosphere.hlsli"

cbuffer PathTracerCB : register(b0)
{
    PathTracerConstants g_PathTracer;
};

RaytracingAccelerationStructure g_SceneAS : register(t0);
StructuredBuffer<GPULight> g_Lights : register(t1);
StructuredBuffer<PerInstanceData> g_Instances : register(t2);
StructuredBuffer<MeshData> g_MeshData : register(t3);
StructuredBuffer<MaterialConstants> g_Materials : register(t4);
StructuredBuffer<uint> g_Indices : register(t5);
StructuredBuffer<VertexQuantized> g_Vertices : register(t6);

VK_IMAGE_FORMAT_UNKNOWN
RWTexture2D<float4> g_Output : register(u0);

VK_IMAGE_FORMAT("rgba32f")
RWTexture2D<float4> g_Accumulation : register(u1);

// ─── PCG Random Number Generator ─────────────────────────────────────────────
// From: https://www.pcg-random.org/
struct RNG
{
    uint state;
};

uint PCGHash(uint seed)
{
    uint state = seed * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

RNG InitRNG(uint2 pixel, uint accumIndex)
{
    RNG rng;
    uint seed    = pixel.x + pixel.y * 65536u + accumIndex * 6700417u;
    rng.state    = PCGHash(seed);
    return rng;
}

float NextFloat(inout RNG rng)
{
    rng.state = PCGHash(rng.state);
    return float(rng.state) * (1.0f / 4294967296.0f);
}

float2 NextFloat2(inout RNG rng)
{
    return float2(NextFloat(rng), NextFloat(rng));
}

// ─── GGX VNDF Importance Sampling ────────────────────────────────────────────
// Heitz 2018: "Sampling the GGX Distribution of Visible Normals"
// Returns the sampled microfacet half-vector in world space.
float3 SampleGGX_VNDF(float2 u, float3 N, float3 V, float roughness)
{
    float alpha = roughness * roughness;

    // Build tangent frame
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);

    // View in local tangent space
    float3 Vl = float3(dot(V, T), dot(V, N), dot(V, B));

    // Stretch by alpha
    float3 Vh = normalize(float3(alpha * Vl.x, Vl.y, alpha * Vl.z));

    // Orthonormal basis around stretched view
    float lensq = Vh.x * Vh.x + Vh.z * Vh.z;
    float3 T1   = lensq > 0.0f ? float3(-Vh.z, 0.0f, Vh.x) / sqrt(lensq)
                                : float3(1.0f, 0.0f, 0.0f);
    float3 T2   = cross(Vh, T1);

    // Disk sample
    float r   = sqrt(u.x);
    float phi = 2.0f * PI * u.y;
    float t1  = r * cos(phi);
    float t2  = r * sin(phi);
    float s   = 0.5f * (1.0f + Vh.y);
    t2        = lerp(sqrt(max(0.0f, 1.0f - t1 * t1)), t2, s);

    // Half-vector in GGX-aligned space
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

    // Unstretch and transform back to world space
    float3 Ne = normalize(float3(alpha * Nh.x, max(0.0f, Nh.y), alpha * Nh.z));
    return T * Ne.x + N * Ne.y + B * Ne.z;
}

// Evaluates the BRDF weight (reflectance / PDF) for GGX VNDF sampling.
// Weight = F * G2/G1  (PDF cancels with the distribution term).
float3 EvalGGX_Weight(float3 F0, float3 N, float3 V, float3 L, float3 H, float roughness)
{
    float alpha  = roughness * roughness;
    float alpha2 = alpha * alpha;

    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float VdotH = saturate(dot(V, H));

    if (NdotV <= 0.0f || NdotL <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float3 F = F_Schlick(F0, VdotH);

    // Smith G2/G1 for GGX (height-correlated ratio simplifies nicely)
    float G1L = 2.0f * NdotL / (NdotL + sqrt(alpha2 + (1.0f - alpha2) * NdotL * NdotL));
    return F * G1L;
}

// ─── Utility: trace a ray and fill a RayHitInfo ────────────────────────────
bool TraceRay(RayDesc ray, out RayHitInfo hit)
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

            PerInstanceData inst = g_Instances[instanceIndex];
            MeshData mesh        = g_MeshData[inst.m_MeshDataIndex];
            MaterialConstants mat= g_Materials[inst.m_MaterialIndex];

            float2 uvSample = GetInterpolatedUV(primitiveIndex, bary, mesh, g_Indices, g_Vertices);
            if (AlphaTest(uvSample, mat))
                q.CommitNonOpaqueTriangleHit();
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
void PathTracer_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= (uint)g_PathTracer.m_View.m_ViewportSize.x ||
        dispatchThreadID.y >= (uint)g_PathTracer.m_View.m_ViewportSize.y)
        return;

    // ── Primary ray setup ───────────────────────────────────────────────────
    float2 jitter  = g_PathTracer.m_Jitter;
    float2 uv      = (float2(dispatchThreadID.xy) + 0.5f + jitter) * g_PathTracer.m_View.m_ViewportSizeInv;
    float2 clipPos = UVToClipXY(uv);

    float4 rayEndFar = MatrixMultiply(float4(clipPos, 0.9f, 1.0f), g_PathTracer.m_View.m_MatClipToWorldNoOffset);
    rayEndFar.xyz   /= rayEndFar.w;

    RayDesc ray;
    ray.Origin    = g_PathTracer.m_CameraPos.xyz;
    ray.Direction = normalize(rayEndFar.xyz - ray.Origin);
    ray.TMin      = 0.0f;
    ray.TMax      = 1e10f;

    // ── Path state ──────────────────────────────────────────────────────────
    RNG   rng                = InitRNG(dispatchThreadID.xy, g_PathTracer.m_AccumulationIndex);
    float3 throughput        = float3(1.0f, 1.0f, 1.0f);
    float3 accumulatedRadiance = float3(0.0f, 0.0f, 0.0f);

    int maxBounces = (int)g_PathTracer.m_MaxBounces;

    for (int bounce = 0; bounce < maxBounces; ++bounce)
    {
        RayHitInfo hit;
        bool didHit = TraceRay(ray, hit);

        if (didHit)
        {
            PerInstanceData inst  = g_Instances[hit.m_InstanceIndex];
            MeshData mesh         = g_MeshData[inst.m_MeshDataIndex];
            MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

            FullHitAttributes attr = GetFullHitAttributes(hit, ray, inst, mesh, g_Indices, g_Vertices);
            PBRAttributes     pbr  = GetPBRAttributes(attr.m_Uv, mat, 0.0f);

            float3 N = attr.m_WorldNormal;
            float3 V = -ray.Direction;

            // Flip normal for back-face hits so lighting is consistent
            if (dot(N, V) < 0.0f)
                N = -N;

            // ── Emissive radiance ──────────────────────────────────────────
            accumulatedRadiance += throughput * pbr.emissive;

            // ── Next Event Estimation (direct lighting) ────────────────────
            float3 p_atmo = GetAtmospherePos(attr.m_WorldPos);

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
            inputs.sunRadiance      = GetAtmosphereSunRadiance(p_atmo, g_PathTracer.m_SunDirection, g_Lights[0].m_Intensity);
            inputs.sunDirection     = g_PathTracer.m_SunDirection;
            inputs.useSunRadiance   = true;
            inputs.sunShadow        = CalculateRTShadow(inputs, g_PathTracer.m_SunDirection, 1e10f);

            PrepareLightingByproducts(inputs);

            LightingComponents direct = AccumulateDirectLighting(inputs, g_PathTracer.m_LightCount);
            accumulatedRadiance += throughput * (direct.diffuse + direct.specular);

            // ── Russian Roulette termination (start after bounce 2) ────────
            if (bounce >= 2)
            {
                float continuePr = saturate(max(throughput.r, max(throughput.g, throughput.b)));
                if (NextFloat(rng) > continuePr)
                    break;
                throughput /= continuePr;
            }

            // ── BRDF Importance Sampling ───────────────────────────────────
            float3 F0     = ComputeF0(pbr.baseColor, pbr.metallic, mat.m_IOR);
            float  NdotV  = saturate(dot(N, V));
            float3 Fapprox = F_Schlick(F0, NdotV);

            // Specular selection probability: higher for metals / high Fresnel
            float specProb = clamp(lerp(Fapprox.r * 0.5f + 0.5f * pbr.metallic, 1.0f, pbr.metallic), 0.1f, 0.9f);

            float3 newDir;
            float3 brdfWeight;

            if (NextFloat(rng) < specProb)
            {
                // ---- Specular: GGX VNDF sample ----
                float  alpha = max(pbr.roughness, 0.04f);
                float3 H     = SampleGGX_VNDF(NextFloat2(rng), N, V, alpha);
                newDir       = reflect(-V, H);

                if (dot(N, newDir) <= 0.0f)
                    break;

                brdfWeight = EvalGGX_Weight(F0, N, V, newDir, H, alpha) / specProb;
            }
            else
            {
                // ---- Diffuse: cosine-weighted hemisphere sample ----
                newDir = SampleHemisphereCosine(NextFloat2(rng), N);

                if (dot(N, newDir) <= 0.0f)
                    break;

                // weight = albedo * (1-metallic)  (PDF = NdotL/PI, BRDF = albedo/PI, NdotL cancels)
                brdfWeight = pbr.baseColor * (1.0f - pbr.metallic) / (1.0f - specProb);
            }

            throughput *= brdfWeight;

            // Bail out if path contribution is negligible
            if (max(throughput.r, max(throughput.g, throughput.b)) < 1e-5f)
                break;

            // ── Advance ray ────────────────────────────────────────────────
            ray.Origin    = attr.m_WorldPos + N * 1e-3f;
            ray.Direction = newDir;
            ray.TMin      = 1e-4f;
            ray.TMax      = 1e10f;
        }
        else
        {
            // ── Miss: accumulate sky and end path ──────────────────────────
            accumulatedRadiance += throughput * GetAtmosphereSkyRadiance(
                ray.Origin,
                ray.Direction,
                g_PathTracer.m_SunDirection,
                g_Lights[0].m_Intensity);
            break;
        }
    }

    // ── Accumulation ────────────────────────────────────────────────────────
    float4 accum = float4(accumulatedRadiance, 1.0f);
    if (g_PathTracer.m_AccumulationIndex > 0)
    {
        accum += g_Accumulation[dispatchThreadID.xy];
    }
    g_Accumulation[dispatchThreadID.xy] = accum;

    g_Output[dispatchThreadID.xy] = float4(accum.rgb / accum.a, 1.0f);
}
