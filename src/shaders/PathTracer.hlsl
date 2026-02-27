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

VK_IMAGE_FORMAT("rgba32f")
RWTexture2D<float4> g_Output : register(u0);

VK_IMAGE_FORMAT("rgba32f")
RWTexture2D<float4> g_Accumulation : register(u1);

// ─── Transmissive BSDF Helpers ───────────────────────────────────────────────
// Evaluates the exact dielectric Fresnel reflectance (unpolarized).
// Based on RTXPT Fresnel.hlsli / pbr-book §8.2.
// eta       = etaI / etaT  (e.g. 1.0 / IOR for air-to-glass)
// cosThetaI = cosine of angle between surface normal and incident direction (> 0)
// cosThetaT = [out] cosine of refracted angle (set to 0 for total internal reflection)
// returns   fractional energy that is reflected [0, 1]
float EvalFresnelDielectric(float eta, float cosThetaI, out float cosThetaT)
{
    if (cosThetaI < 0.0f)
    {
        eta       = 1.0f / eta;
        cosThetaI = -cosThetaI;
    }
    float sinThetaTSq = eta * eta * (1.0f - cosThetaI * cosThetaI);
    if (sinThetaTSq >= 1.0f)            // total internal reflection
    {
        cosThetaT = 0.0f;
        return 1.0f;
    }
    cosThetaT  = sqrt(max(0.0f, 1.0f - sinThetaTSq));
    float Rs   = (eta * cosThetaI - cosThetaT) / (eta * cosThetaI + cosThetaT);
    float Rp   = (eta * cosThetaT - cosThetaI) / (eta * cosThetaT + cosThetaI);
    return 0.5f * (Rs * Rs + Rp * Rp);
}

// Beer-Lambert transmittance over a ray segment of length `dist`.
// sigmaA = absorption coefficient,  sigmaS = scattering coefficient (both per-unit-length).
float3 EvalTransmittance(float3 sigmaA, float3 sigmaS, float dist)
{
    return exp(-(sigmaA + sigmaS) * dist);
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
bool TraceRay(RayDesc ray, RNG rng, out RayHitInfo hit)
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

            if (mat.m_AlphaMode == ALPHA_MODE_MASK)
            {
                if (AlphaTest(uvSample, mat))
                    q.CommitNonOpaqueTriangleHit();
            }
            else if (mat.m_AlphaMode == ALPHA_MODE_BLEND)
            {
                float alpha = mat.m_BaseColor.w;
                if ((mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0)
                {
                    alpha *= SampleBindlessTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uvSample).w;
                }

                // For transmissive materials, always keep the hit and let the BSDF branch
                // decide reflection/transmission. Otherwise use stochastic alpha coverage.
                if (mat.m_TransmissionFactor > 0.0f || NextFloat(rng) < saturate(alpha))
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
    RNG rng = InitRNG(dispatchThreadID.xy, g_PathTracer.m_AccumulationIndex);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 accumulatedRadiance = float3(0.0f, 0.0f, 0.0f);
    bool inDielectricVolume = false;
    float interiorIOR = 1.0f;
    float3 interiorSigmaA = float3(0.0f, 0.0f, 0.0f);
    float3 interiorSigmaS = float3(0.0f, 0.0f, 0.0f);

    int maxBounces = (int)g_PathTracer.m_MaxBounces;

    for (int bounce = 0; bounce < maxBounces; ++bounce)
    {
        RayHitInfo hit;
        bool didHit = TraceRay(ray, rng, hit);

        if (didHit)
        {
            PerInstanceData inst  = g_Instances[hit.m_InstanceIndex];
            MeshData mesh         = g_MeshData[inst.m_MeshDataIndex];
            MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

            // Apply Beer-Lambert attenuation only while the current segment is in a medium.
            if (inDielectricVolume)
            {
                throughput *= EvalTransmittance(interiorSigmaA, interiorSigmaS, hit.m_RayT);
            }

            FullHitAttributes attr = GetFullHitAttributes(hit, ray, inst, mesh, g_Indices, g_Vertices);
            PBRAttributes     pbr  = GetPBRAttributes(attr, mat, 0.0f);

            // ── Next Event Estimation (direct lighting) ────────────────────
            float3 p_atmo = GetAtmospherePos(attr.m_WorldPos);

            float3 Ng = normalize(attr.m_WorldNormal);
            float3 N = pbr.normal;
            float3 V = -ray.Direction;
            bool isFrontFace = dot(Ng, ray.Direction) < 0.0f;

            // Flip normal for back-face hits so lighting is consistent
            if (dot(N, V) < 0.0f)
                N = -N;

            LightingInputs inputs;
            inputs.N                = N;
            inputs.V                = V;
            inputs.L                = float3(0, 0, 0); // will be set per-light in the loop
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
            inputs.sunShadow        = 0.0f; // unused in PATH_TRACER_MODE — each sample casts its own shadow ray

            PrepareLightingByproducts(inputs);

            // ── BSDF-Driven Transmission ──────────────────────────────────
            // Handles both thin-walled surfaces (no refraction bend, eta_refract = 1)
            // and thick/volumetric surfaces (full IOR refraction).
            // Uses exact dielectric Fresnel for lobe selection and GGX microfacets
            // for rough transmission with proper refraction PDF Jacobian.
            if (mat.m_TransmissionFactor > 0.0f || mat.m_AlphaMode == ALPHA_MODE_BLEND)
            {
                float effectiveAlpha     = (mat.m_AlphaMode == ALPHA_MODE_BLEND) ? pbr.alpha : 1.0f;
                float transmissionFactor = max(mat.m_TransmissionFactor, 1.0f - effectiveAlpha);

                float materialIOR = max(mat.m_IOR, 1.0001f);
                float outsideIOR  = inDielectricVolume ? interiorIOR : 1.0f;
                float etaSurface  = isFrontFace ? (outsideIOR / materialIOR) : (materialIOR / outsideIOR);

                // etaFresnel uses the actual interface ratio; etaRefract is forced to 1 for thin surfaces.
                float etaFresnel = etaSurface;
                float etaRefract = (mat.m_IsThinSurface != 0) ? 1.0f : etaFresnel;

                // Dielectric Fresnel at the geometry normal for lobe-selection probability
                float cosThetaT_geo;
                float F = EvalFresnelDielectric(etaFresnel, max(dot(N, V), 0.0f), cosThetaT_geo);

                // Transmission probability: (1 - Fresnel) * transmissionFactor
                float probT = saturate((1.0f - F) * transmissionFactor);

                if (NextFloat(rng) < probT)
                {
                    // ── Transmission path ─────────────────────────────────
                    float3 refractedDir;
                    float3 bsdfWeight;

                    if (pbr.roughness <= 0.08f)
                    {
                        // ── Delta (specular) transmission ─────────────────
                        // Refract with etaRefract: thin→pass-through, thick→bend
                        refractedDir = refract(ray.Direction, N, etaRefract);
                        if (dot(refractedDir, refractedDir) < 1e-8f)
                            refractedDir = reflect(ray.Direction, N); // TIR fallback

                        // Delta lobe weight: baseColor * (1-F) / probT cancels to just baseColor
                        // since probT = (1-F) * transmissionFactor already accounts for selection probability
                        bsdfWeight = pbr.baseColor.rgb;
                    }
                    else
                    {
                        // ── Specular (GGX rough) transmission ─────────────
                        float3 H    = SampleGGX_VNDF(NextFloat2(rng), N, V, pbr.roughness);
                        float VdotH = saturate(dot(V, H));

                        // Microfacet Fresnel (Fresnel uses real IOR, refract uses etaRefract)
                        float cosThetaT_mf;
                        float F_mf = EvalFresnelDielectric(etaFresnel, VdotH, cosThetaT_mf);

                        // Refract through half-vector H.
                        // Formula (RTXPT convention, wi = V view, wo = transmitted):
                        //   wo = (etaRefract * VdotH - cosThetaT_mf) * H - etaRefract * V
                        // For etaRefract = 1 (thin) this reduces to -V = ray.Direction, preserving pass-through.
                        float cosThetaT_for_dir;
                        EvalFresnelDielectric(etaRefract, VdotH, cosThetaT_for_dir);
                        refractedDir = (etaRefract * VdotH - cosThetaT_for_dir) * H - etaRefract * V;
                        if (dot(refractedDir, refractedDir) < 1e-8f)
                            refractedDir = reflect(ray.Direction, H); // TIR fallback
                        refractedDir = normalize(refractedDir);

                        // GGX transmission weight = (1 - F_mf) * G1(L_t) * NdotL_t / probT
                        // Derivation: BSDF_T * NdotL_t / (probT * pdf_VNDF * J_refraction)
                        // where J_refraction is the refraction Jacobian dΩ_h / dΩ_t:
                        //   J = eta_refract^2 * |LdotH| / (|LdotH| + etaRefract * VdotH)^2
                        // The refraction PDF Jacobian correction (from RTXPT BxDF::evalPdf):
                        //   pdf_T = pdf_VNDF(H) * VdotH * 4 * |LdotH| / (|LdotH| + eta * VdotH)^2
                        // Combining BSDF/pdf gives the simplified form below.
                        float alpha  = pbr.roughness * pbr.roughness;
                        float alpha2 = alpha * alpha;
                        float NdotL_t = abs(dot(N, refractedDir));
                        float G1_t = (NdotL_t > 1e-5f)
                            ? 2.0f * NdotL_t / (NdotL_t + sqrt(alpha2 + (1.0f - alpha2) * NdotL_t * NdotL_t))
                            : 0.0f;

                        // GGX transmission weight accounts for microfacet normal distribution
                        // after stochastic lobe selection (probT already factored in)
                        bsdfWeight = pbr.baseColor.rgb * (1.0f - F_mf) * G1_t * NdotL_t;
                    }

                    throughput *= bsdfWeight;

                    if (mat.m_IsThinSurface == 0)
                    {
                        if (isFrontFace)
                        {
                            inDielectricVolume = true;
                            interiorIOR = materialIOR;
                            interiorSigmaA = mat.m_SigmaA;
                            interiorSigmaS = mat.m_SigmaS;
                        }
                        else
                        {
                            inDielectricVolume = false;
                            interiorIOR = 1.0f;
                            interiorSigmaA = float3(0.0f, 0.0f, 0.0f);
                            interiorSigmaS = float3(0.0f, 0.0f, 0.0f);
                        }
                    }

                    // Advance ray: push the origin past the surface (opposite to geometry normal)
                    ray.Origin    = attr.m_WorldPos - N * 0.001f;
                    ray.Direction = normalize(refractedDir);
                    ray.TMin      = 1e-4f;
                    ray.TMax      = 1e10f;
                    continue;
                }
                // Reflection fallback: fall through to emissive + direct lighting + BRDF sampling
            }

            // ── Emissive radiance ──────────────────────────────────────────
            accumulatedRadiance += throughput * pbr.emissive;

            LightingComponents direct = AccumulateDirectLighting(inputs, g_PathTracer.m_LightCount, g_PathTracer.m_CosSunAngularRadius, rng);
            accumulatedRadiance += throughput * (direct.diffuse + (bounce == 0 ? direct.specular : 0.0f));

            // ── Russian Roulette termination (start after bounce 2) ────────
            if (bounce >= 2)
            {
                float continuePr = saturate(max(throughput.r, max(throughput.g, throughput.b)));
                if (NextFloat(rng) > continuePr)
                    break;
                throughput /= continuePr;
            }

            // ── BRDF Importance Sampling ───────────────────────────────────

            // Specular selection probability: higher for metals / high Fresnel
            float specProb = clamp(lerp(inputs.F.r * 0.5f + 0.5f * pbr.metallic, 1.0f, pbr.metallic), 0.1f, 0.9f);

            float3 newDir;
            float3 brdfWeight;

            if (NextFloat(rng) < specProb)
            {
                // ---- Specular: GGX VNDF sample ----
                float3 H = SampleGGX_VNDF(NextFloat2(rng), N, V, pbr.roughness);
                newDir = reflect(-V, H);

                if (dot(N, newDir) <= 0.0f)
                    break;

                brdfWeight = EvalGGX_Weight(inputs.F0, N, V, newDir, H, pbr.roughness) / specProb;
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
            if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01f)
                break;

            // ── Advance ray ────────────────────────────────────────────────
            ray.Origin    = attr.m_WorldPos;
            ray.Direction = newDir;
            ray.TMin      = 1e-4f;
            ray.TMax      = 1e10f;
        }
        else
        {
            // ── Miss: accumulate sky and end path ──────────────────────────
            bool bAddSunDisk = (bounce == 0); // only add sun disk for primary rays to avoid brightening from multiple bounces
            float3 skyRadiance = GetAtmosphereSkyRadiance(
                ray.Origin,
                ray.Direction,
                g_PathTracer.m_SunDirection,
                g_Lights[0].m_Intensity,
                bAddSunDisk);

            accumulatedRadiance += throughput * skyRadiance;
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
