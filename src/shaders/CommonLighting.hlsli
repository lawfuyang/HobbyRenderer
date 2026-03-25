#ifndef COMMON_LIGHTING_HLSLI
#define COMMON_LIGHTING_HLSLI

#include "ShaderShared.h"
#include "RaytracingCommon.hlsli"
#include "Packing.hlsli"

#define LIGHT_SHADOW_SAMPLES 1

// RNG for stochastic sampling — only present in path-tracer compute kernels, not rasterized passes
#include "RNG.hlsli"

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float calcLuminance(float3 c) { return Luminance(c); }

// ---- Fresnel (scalar overload for FullSample compatibility) ----
float Schlick_Fresnel(float F0, float cosTheta)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

// ---- Fresnel (float3 overload for specularF0 vectors) ----
float3 Schlick_Fresnel(float3 F0, float cosTheta)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

// ---- Lambertian diffuse (FullSample: Lambert(N, L) = NdotL / PI) ----
float Lambert(float3 N, float3 L)
{
    return max(0.0f, dot(N, L)) / M_PI;
}

// ---- ONB construction (Pixar branchless) ----
// Alias: use BuildTangentFrame below for world-space ONB construction.

// Octahedral encoding for normals
// From: http://jcgt.org/published/0003/02/01/
float2 octWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * select(v.xy >= 0.0, 1.0, -1.0);
}

float2 EncodeNormal(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : octWrap(n.xy);
    n.xy = n.xy * 0.5 + 0.5;
    return n.xy;
}

float3 DecodeNormal(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, -t, t);
    return normalize(n);
}

// PBR Helper functions
float3 ComputeF0(float3 baseColor, float metallic, float ior)
{
    float dielectricF0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    return lerp(float3(dielectricF0, dielectricF0, dielectricF0), baseColor, metallic);
}

// FullSample-compatible metallic workflow split.
void GetReflectivityFromMetallic(float metalness, float3 baseColor, out float3 diffuseAlbedo, out float3 specularF0)
{
    const float dielectricSpecular = 0.04f;
    diffuseAlbedo = lerp(baseColor * (1.0f - dielectricSpecular), float3(0.0f, 0.0f, 0.0f), metalness);
    specularF0 = lerp(float3(dielectricSpecular, dielectricSpecular, dielectricSpecular), baseColor, metalness);
}

// GGX normal distribution function.
// Returns D(NdotH, roughness) = alpha^2 / (PI * ((NdotH^2*(alpha^2-1)+1))^2)
float D_GGX(float NdotH, float roughness)
{
    float alpha  = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom  = (NdotH * NdotH * (alpha2 - 1.0f) + 1.0f);
    return alpha2 / (PI * denom * denom);
}

// Height-correlated Smith G2 divided by (4 * NdotV), expressed as G2/NdotV.
// Returns 2*NdotL / (NdotV*sqrt(alpha2+(1-alpha2)*NdotL^2) + NdotL*sqrt(alpha2+(1-alpha2)*NdotV^2))
// Matches FullSample's donut::shaders::brdf.hlsli GGX_times_NdotL convention.
float G_SmithOverNdotV_Exact(float roughness, float NdotV, float NdotL)
{
    float alpha  = roughness * roughness;
    float alpha2 = alpha * alpha;
    float g1 = NdotV * sqrt(alpha2 + (1.0f - alpha2) * NdotL * NdotL);
    float g2 = NdotL * sqrt(alpha2 + (1.0f - alpha2) * NdotV * NdotV);
    return 2.0f * NdotL / max(g1 + g2, 1e-6f);
}

// Evaluate the full GGX specular BRDF * NdotL for a given light direction.
// Returns F * D * G * 0.25 (the 4*NdotV*NdotL denominator is folded into G_SmithOverNdotV_Exact).
float3 GGXTimesNdotL_Exact(float3 V, float3 L, float3 N, float roughness, float3 F0)
{
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f)
        return 0.0f;

    float VdotH = saturate(dot(V, H));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));

    float  G  = G_SmithOverNdotV_Exact(roughness, NdotV, NdotL);
    float  D  = D_GGX(NdotH, roughness);
    float  Fc = pow(max(1.0f - VdotH, 0.0f), 5.0f);
    float3 F  = F0 + (1.0f - F0) * Fc;

    return F * (D * G * 0.25f);
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float3 F_Schlick(float3 SpecularColor, float VDotH)
{
	float Fc = pow(1.0 - VDotH, 5);
	
	// Anything less than 2% is physically impossible and is instead considered to be shadowing
	return saturate( 50.0 * SpecularColor.g ) * Fc + (1 - Fc) * SpecularColor;
}

float OrenNayar(float NdotL, float NdotV, float LdotV, float roughness)
{
    if (NdotL <= 0.0 || NdotV <= 0.0) return 0.0;

    float sigma2 = roughness * roughness;

    float A = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float B = 0.45 * sigma2 / (sigma2 + 0.09);

    float cosPhi = LdotV - NdotL * NdotV;
    float sinAlphaTanBeta =
        sqrt((1.0 - NdotL*NdotL) * (1.0 - NdotV*NdotV)) /
        max(NdotL, NdotV);

    return NdotL * (A + B * max(0.0, cosPhi) * sinAlphaTanBeta) / PI;
}

float DisneyBurleyDiffuse(float NdotL, float NdotV, float LdotH, float perceptualRoughness)
{
    if (NdotL <= 0 || NdotV <= 0)
        return 0;

    // Burley diffuse uses perceptual roughness directly
    float rough = perceptualRoughness;
    float rough2 = rough * rough;

    // Schlick-style Fresnel for diffuse
    float FL = pow(1.0 - NdotL, 5.0);
    float FV = pow(1.0 - NdotV, 5.0);

    float Fd90 = 0.5 + 2.0 * rough2 * LdotH * LdotH;

    float Fd =
        lerp(1.0, Fd90, FL) *
        lerp(1.0, Fd90, FV);

    return Fd * NdotL / PI;
}

float3 SampleHemisphereCosine(float2 u, float3 normal)
{
    float phi = 2.0f * PI * u.x;
    float cosTheta = sqrt(u.y);
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    
    float3 localDir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
    
    float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    
    return tangent * localDir.x + normal * localDir.y + bitangent * localDir.z;
}

float3 SampleHemisphereUniform(float2 u, float3 normal)
{
    float phi = 2.0f * PI * u.x;
    float cosTheta = u.y;
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    
    float3 localDir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
    
    float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    
    return tangent * localDir.x + normal * localDir.y + bitangent * localDir.z;
}

float ConvertToLinearDepth(float depth, float4x4 matViewToClip)
{
    // Linearize using projection matrix constants — no world-pos reconstruction needed.
    // projA = M._33, projB = M._43 (row-major, view * proj convention).
    float projA = matViewToClip._33;
    float projB = matViewToClip._43;
    return projB / (depth - projA);
}

float ConvertToClipZ(float SceneDepth, float2 InvDeviceZToWorldZTransform)
{
    return (1.0f / SceneDepth + InvDeviceZToWorldZTransform[1]) / InvDeviceZToWorldZTransform[0];
}

float ComputeZSliceFromDepth(float SceneDepth, float3 GridZParams)
{
	return log2(SceneDepth * GridZParams.x + GridZParams.y) * GridZParams.z;
}

float ComputeDepthFromZSlice(float ZSlice, float3 GridZParams)
{
	return (exp2(ZSlice / GridZParams.z) - GridZParams.y) / GridZParams.x;
}

float3 ComputeCellViewSpacePosition(uint3 GridCoordinate, float3 GridZParams, uint3 ViewGridSize, float2 InvDeviceZToWorldZTransform, float4x4 ClipToView, out float viewSpaceZ)
{
	float2 VolumeUV = (GridCoordinate.xy + float2(0.5f, 0.5f)) / ViewGridSize.xy;
	float2 VolumeNDC = (VolumeUV * 2 - 1) * float2(1, -1);

	viewSpaceZ = ComputeDepthFromZSlice(GridCoordinate.z + 0.5f, GridZParams);

	float ClipZ = ConvertToClipZ(viewSpaceZ, InvDeviceZToWorldZTransform);
	float4 CenterPosition = mul(float4(VolumeNDC, ClipZ, 1), ClipToView);
	return CenterPosition.xyz / CenterPosition.w;
}

float3 ComputeCellViewSpacePosition(uint3 GridCoordinate, float3 GridZParams, uint3 ViewGridSize, float2 InvDeviceZToWorldZTransform, float4x4 ClipToView)
{
	float ViewSpaceZ;
    return ComputeCellViewSpacePosition(GridCoordinate, GridZParams, ViewGridSize, InvDeviceZToWorldZTransform, ClipToView, ViewSpaceZ);
}

float3 ComputeMotionVectors(float3 worldPos, float3 prevWorldPos, PlanarViewConstants view, PlanarViewConstants viewPrev)
{
    // FIXME: Switch back to m_MatWorldToClip (jittered) once TAA is implemented
    float4 clipPos = MatrixMultiply(float4(worldPos, 1.0), view.m_MatWorldToClipNoOffset);
    float4 prevClipPos = MatrixMultiply(float4(prevWorldPos, 1.0), viewPrev.m_MatWorldToClipNoOffset);

    // clipPos.w is the linear view-space depth
    float currentDepth = clipPos.w;
    float previousDepth = prevClipPos.w;

    clipPos.xyz /= clipPos.w;
    prevClipPos.xyz /= prevClipPos.w;

    float2 windowPos = clipPos.xy * view.m_ClipToWindowScale + view.m_ClipToWindowBias;
    float2 prevWindowPos = prevClipPos.xy * viewPrev.m_ClipToWindowScale + viewPrev.m_ClipToWindowBias;

    // FIXME: When TAA is implemented, if we use jittered matrices, we need to add back the jitter offset correction:
    // return prevWindowPos.xy - windowPos.xy + (g_PerFrame.m_View.m_PixelOffset - g_PerFrame.m_PrevView.m_PixelOffset);
    return float3(prevWindowPos.xy - windowPos.xy, previousDepth - currentDepth);
}

float3 ConvertMotionVectorToPixelSpace(PlanarViewConstants view, PlanarViewConstants viewPrev, int2 pixelPosition, float3 motionVector)
{
    float2 currentPixelCenter = float2(pixelPosition.xy) + 0.5;
    float2 previousPosition = currentPixelCenter + motionVector.xy;
    previousPosition *= viewPrev.m_ViewportSize * view.m_ViewportSizeInv;
    motionVector.xy = previousPosition - currentPixelCenter;
    return motionVector;
}

struct LightingInputs
{
    float3 N;
    float3 V;
    float3 L;
    float3 baseColor;
    float roughness;
    float metallic;
    float ior;
    float3 worldPos;
    uint radianceMipCount;
    bool enableRTShadows;
    RaytracingAccelerationStructure sceneAS;
    StructuredBuffer<PerInstanceData> instances;
    StructuredBuffer<MeshData> meshData;
    StructuredBuffer<MaterialConstants> materials;
    StructuredBuffer<uint> indices;
    StructuredBuffer<VertexQuantized> vertices;
    StructuredBuffer<GPULight> lights;

    float3 sunRadiance; // Atmosphere-aware direct radiance (already contains BRDF/n.l)
    float3 sunDirection;
    bool useSunRadiance;
    float sunShadow;

    // Derived values
    float3 F0;       // Base reflectivity at normal incidence
    float3 kD;       // Diffuse coefficient, (1 - metallic)
    float3 F;        // Fresnel term for the current view angle

    float NdotV; // Dot product of surface normal and view direction
    float NdotL; // Dot product of surface normal and light direction   
    float NdotH; // Dot product of surface normal and half vector
    float VdotH; // Dot product of view direction and half vector
    float LdotV; // Dot product of light direction and view direction
    float LdotH; // Dot product of light direction and half vector
};

struct IBLComponents
{
    float3 irradiance;
    float3 radiance;
    float3 ibl;
};

void PrepareLightingByproducts(inout LightingInputs inputs)
{
    inputs.NdotV = saturate(dot(inputs.N, inputs.V));
    inputs.NdotL = saturate(dot(inputs.N, inputs.L));

    // Guard against degenerate half-vector when V ≈ -L (viewer looking away from light).
    // normalize(0) produces NaN in HLSL which propagates to bright/white pixels.
    float3 VplusL = inputs.V + inputs.L;
    float  VplusLLen = dot(VplusL, VplusL);
    float3 H = (VplusLLen > 1e-8f) ? (VplusL * rsqrt(VplusLLen)) : inputs.N;
    inputs.NdotH = saturate(dot(inputs.N, H));
    inputs.VdotH = saturate(dot(inputs.V, H));
    inputs.LdotV = saturate(dot(inputs.L, inputs.V));
    inputs.LdotH = saturate(dot(inputs.L, H));

    inputs.F0 = ComputeF0(inputs.baseColor, inputs.metallic, inputs.ior);
    inputs.kD = (1.0f - inputs.metallic);
    inputs.F = F_Schlick(inputs.F0, inputs.VdotH);
}

struct LightingComponents
{
    float3 diffuse;
    float3 specular;
};

// Compute the specular BRDF term: (D * G2 * F) / (4 * NdotV * NdotL)
// Uses the height-correlated Smith G2 (Exact) for correctness.
// Does NOT include NdotL — caller (EvaluateDirectLight) multiplies by NdotL.
float3 ComputeSpecularBRDF(float3 F, float NdotH, float NdotV, float NdotL, float roughness)
{
    float alpha  = roughness * roughness;
    float alpha2 = alpha * alpha;

    float D = D_GGX(NdotH, roughness);

    // Height-correlated Smith G2 (full form, no NdotL baked in)
    float g1 = NdotV * sqrt(alpha2 + (1.0f - alpha2) * NdotL * NdotL);
    float g2 = NdotL * sqrt(alpha2 + (1.0f - alpha2) * NdotV * NdotV);
    float G2 = 0.5f / max(g1 + g2, 1e-6f); // = G2 / (4*NdotV*NdotL)

    return D * G2 * F;
}

LightingComponents EvaluateDirectLight(LightingInputs inputs, float3 radiance, float shadow)
{
    LightingComponents components;

    // DisneyBurleyDiffuse already returns Fd * NdotL / PI — do NOT multiply by NdotL again.
    float diffuseTerm = DisneyBurleyDiffuse(inputs.NdotL, inputs.NdotV, inputs.LdotH, inputs.roughness);
    float3 diffuse = diffuseTerm * inputs.kD * inputs.baseColor;

    // ComputeSpecularBRDF returns D*G*F / (4*NdotV*NdotL) — multiply by NdotL once here.
    float3 spec = ComputeSpecularBRDF(inputs.F, inputs.NdotH, inputs.NdotV, inputs.NdotL, inputs.roughness);

    components.diffuse = diffuse * radiance * shadow;
    components.specular = spec * inputs.NdotL * radiance * shadow;

    return components;
}

// with 'bWithTransmission', accumulates transmission through semi-transparent (ALPHA_MODE_BLEND) surfaces
// during shadow ray tracing. Returns transmission factor: 1.0 = unshadowed, 
// [0, 1) = partially transmitted light, 0.0 = fully blocked opaque geometry.
template <bool bWithTransmission = false>
float CalculateRTShadow(LightingInputs inputs, float3 L, float maxDist)
{
    if (!inputs.enableRTShadows) return 1.0f;

    // Use a small TMin-only offset (no normal bias on Origin) to avoid
    // self-intersection. A large normal bias (N * 0.1) causes:
    //   - Coplanar decals to appear unoccluded (origin pushed past them).
    //   - Contact shadows to vanish for nearby geometry.
    // This matches the RTXDI SetupShadowRay convention (offset = 0.01 for
    // final shading, 0.001 for conservative visibility).
    static const float kShadowBias = 0.01f;
    RayDesc ray;
    ray.Origin    = inputs.worldPos;
    ray.Direction = L;
    ray.TMin      = kShadowBias;
    ray.TMax      = max(kShadowBias, maxDist - kShadowBias * 2.0f);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | (bWithTransmission ? RAY_FLAG_NONE : RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)> q;
    q.TraceRayInline(inputs.sceneAS, RAY_FLAG_NONE, 0xFF, ray);

    float transmission = 1.0f; // Accumulate transmission through translucent surfaces
    bool inVolume = false;
    float inVolumeStartT = 0.0f;
    float3 sigmaT = 0.0f;
    
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instanceIndex = q.CandidateInstanceIndex();
            uint primitiveIndex = q.CandidatePrimitiveIndex();
            float2 bary = q.CandidateTriangleBarycentrics();
            float hitT = q.CandidateTriangleRayT();

            PerInstanceData inst = inputs.instances[instanceIndex];
            MeshData mesh = inputs.meshData[inst.m_MeshDataIndex];
            MaterialConstants mat = inputs.materials[inst.m_MaterialIndex];

            if (mat.m_AlphaMode == ALPHA_MODE_MASK)
            {
                TriangleVertices tv = GetTriangleVertices(primitiveIndex, inst.m_LODIndex, mesh, inputs.indices, inputs.vertices);
                RayGradients grad = GetShadowRayGradients(tv, bary, ray.Origin, inst.m_World);
                
                if (AlphaTestGrad(grad.uv, grad.ddx, grad.ddy, mat))
                {
                    q.CommitNonOpaqueTriangleHit();
                }
            }
            else if (mat.m_AlphaMode == ALPHA_MODE_BLEND)
            {
                if (bWithTransmission)
                {
                    // Surface coverage attenuation (alpha blend); transmissive materials should
                    // remain hittable and be handled as light filters instead of disappearing.
                    float2 uvSample = GetInterpolatedUV(primitiveIndex, inst.m_LODIndex, bary, mesh, inputs.indices, inputs.vertices);
                    float alpha = mat.m_BaseColor.w;
                    if ((mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0)
                    {
                        alpha *= SampleBindlessTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uvSample).w;
                    }
                    
                    float opacity = saturate(alpha * (1.0f - mat.m_TransmissionFactor));
                    transmission *= (1.0f - opacity);

                    // Volumetric attenuation for thick transmissive media.
                    if (mat.m_TransmissionFactor > 0.0f && mat.m_IsThinSurface == 0)
                    {
                        TriangleVertices tv = GetTriangleVertices(primitiveIndex, inst.m_LODIndex, mesh, inputs.indices, inputs.vertices);
                        float3 localNormal = tv.v0.m_Normal * (1.0f - bary.x - bary.y) + tv.v1.m_Normal * bary.x + tv.v2.m_Normal * bary.y;
                        float3 worldNormal = normalize(TransformNormal(localNormal, inst.m_World));
                        bool isFrontFace = dot(worldNormal, ray.Direction) < 0.0f;

                        if (isFrontFace)
                        {
                            inVolume = true;
                            inVolumeStartT = hitT;
                            sigmaT = mat.m_SigmaA + mat.m_SigmaS;
                        }
                        else if (inVolume)
                        {
                            float segmentDist = max(0.0f, hitT - inVolumeStartT);
                            float3 tr = exp(-sigmaT * segmentDist);
                            transmission *= dot(tr, float3(0.2126f, 0.7152f, 0.0722f));
                            inVolume = false;
                        }
                    }

                    if (transmission <= 1e-3f)
                        q.CommitNonOpaqueTriangleHit();
                }
                else
                {
                    // Do not commit hit; continue through translucent surface
                }
            }
            else if (mat.m_AlphaMode == ALPHA_MODE_OPAQUE)
            {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        return 0.0f;
    }

    if (inVolume)
    {
        float segmentDist = max(0.0f, ray.TMax - inVolumeStartT);
        float3 tr = exp(-sigmaT * segmentDist);
        transmission *= dot(tr, float3(0.2126f, 0.7152f, 0.0722f));
    }

    return saturate(transmission);
}

// ─── Rasterized lighting path (deterministic, single-ray hard shadows) ────────
// These implementations are used by all rasterized passes (forward, deferred, sky).
// No RNG — shadows are either pre-computed (sunShadow field) or single RT queries.
LightingComponents ComputeDirectionalLighting(LightingInputs inputs, GPULight light)
{
    LightingComponents result;
    result.diffuse = 0;
    result.specular = 0;

    float3 L = inputs.sunDirection;
    if (dot(inputs.N, L) <= 0.0) return result;

    inputs.L = L;
    PrepareLightingByproducts(inputs);

    float3 radiance = light.m_Color * light.m_Intensity;
    float shadow = 0;

    // Use atmosphere-aware sun radiance if available
    if (inputs.useSunRadiance)
    {
        radiance = inputs.sunRadiance;
        shadow = inputs.sunShadow;
    }
    else
    {
        shadow = CalculateRTShadow(inputs, L, 1e10f);
    }

    return EvaluateDirectLight(inputs, radiance, shadow);
}

LightingComponents ComputeSpotLighting(LightingInputs inputs, GPULight light)
{
    LightingComponents result;
    result.diffuse = 0;
    result.specular = 0;

    if (light.m_Intensity <= 0.0) return result;

    float3 L_unnorm = light.m_Position - inputs.worldPos;
    float distSq = dot(L_unnorm, L_unnorm);
    
    // Aggressive culling: Range test
    if (light.m_Range > 0.0 && distSq > light.m_Range * light.m_Range) return result;

    float dist = sqrt(distSq);
    float3 L = L_unnorm / dist;
    
    // Front-face culling
    float NdotL = dot(inputs.N, L);
    if (NdotL <= 0.0) return result;

    // Spot falloff culling
    float3 lightDir = normalize(light.m_Direction);
    float cosTheta = dot(-L, lightDir);
    float cosOuter = cos(light.m_SpotOuterConeAngle);
    if (cosTheta < cosOuter) return result;

    // Spot falloff math
    float cosInner = cos(light.m_SpotInnerConeAngle);
    float spotAttenuation = saturate((cosTheta - cosOuter) / (cosInner - cosOuter));
    
    // Distance attenuation
    float distAttenuation = 1.0 / (distSq + 1.0);
    if (light.m_Range > 0.0)
    {
        distAttenuation *= pow(saturate(1.0 - pow(dist / light.m_Range, 4.0)), 2.0);
    }
    
    float3 radiance = light.m_Color * light.m_Intensity * spotAttenuation * distAttenuation;
    
    inputs.L = L;
    PrepareLightingByproducts(inputs);
    
    float shadow = CalculateRTShadow<true>(inputs, L, dist);

    return EvaluateDirectLight(inputs, radiance, shadow);
}

LightingComponents AccumulateDirectLighting(LightingInputs inputs, uint lightCount)
{
    LightingComponents total;
    total.diffuse = 0;
    total.specular = 0;

    for (uint i = 0; i < lightCount; ++i)
    {
        GPULight light = inputs.lights[i];
        
        [branch]
        if (light.m_Type == 0) // Directional
        {
            LightingComponents comp = ComputeDirectionalLighting(inputs, light);
            total.diffuse += comp.diffuse;
            total.specular += comp.specular;
        }
        else if (light.m_Type == 2) // Spot
        {
            LightingComponents comp = ComputeSpotLighting(inputs, light);
            total.diffuse += comp.diffuse;
            total.specular += comp.specular;
        }
    }
    return total;
}

// ─── GGX VNDF Importance Sampling (shared by PathTracer & RTXDI bridge) ──────
// Heitz 2018: "Sampling the GGX Distribution of Visible Normals"
// All functions operate in world space; the caller supplies N, V, L as world-space vectors.

// Build an orthonormal tangent frame (T, B) around a world-space normal N.
void BuildTangentFrame(float3 N, out float3 T, out float3 B)
{
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// Sample a GGX microfacet half-vector using the Visible Normal Distribution (VNDF).
// Returns the half-vector in world space.
// u        = two uniform random numbers in [0,1)
// N, V     = world-space surface normal and view direction
// roughness = perceptual roughness (alpha = roughness^2 is used internally)
float3 SampleGGX_VNDF(float2 u, float3 N, float3 V, float roughness)
{
    float alpha = roughness * roughness;

    float3 T, B;
    BuildTangentFrame(N, T, B);

    // View in local tangent space (T=x, N=y, B=z)
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

// Evaluate the PDF of VNDF GGX sampling for a given (N, V, L) triple.
// Returns the solid-angle PDF of sampling direction L given view V on surface N.
float SampleGGX_VNDF_PDF(float roughness, float3 N, float3 V, float3 L)
{
    float3 H    = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float alpha  = roughness * roughness;
    float D      = D_GGX(NdotH, roughness);
    return (VdotH > 0.0f) ? D / (4.0f * VdotH) : 0.0f;
}

// Evaluate the BRDF weight (reflectance / PDF) for a GGX VNDF-sampled specular bounce.
// Weight = F * G2/G1  (the distribution D and PDF cancel, leaving only the masking ratio).
float3 EvalGGX_VNDF_Weight(float3 F0, float3 N, float3 V, float3 L, float3 H, float roughness)
{
    float alpha  = roughness * roughness;
    float alpha2 = alpha * alpha;

    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float VdotH = saturate(dot(V, H));

    if (NdotV <= 0.0f || NdotL <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float3 F = F_Schlick(F0, VdotH);

    // Smith G2/G1 for GGX (height-correlated ratio simplifies to G1(L))
    float G1L = 2.0f * NdotL / (NdotL + sqrt(alpha2 + (1.0f - alpha2) * NdotL * NdotL));
    return F * G1L;
}

// Samples a direction uniformly within a solid-angle cone cap around `dir`.
// cosHalfAngle = cos(half-angle of cone); u = two uniform randoms in [0,1).
// PDF = 1 / (2*PI*(1 - cosHalfAngle)); caller averages N samples so PDFs cancel.
float3 SampleConeSolidAngle(float3 dir, float cosHalfAngle, float2 u)
{
    // Map u.x linearly so cosTheta ranges from 1 (cone centre) to cosHalfAngle (edge)
    float cosTheta = 1.0f - u.x * (1.0f - cosHalfAngle);
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    float phi      = 2.0f * PI * u.y;

    float3 localDir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));

    // Build ONB with dir as the local +Y axis
    float3 up        = abs(dir.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent   = normalize(cross(up, dir));
    float3 bitangent = cross(dir, tangent);

    return tangent * localDir.x + dir * localDir.y + bitangent * localDir.z;
}

// Directional (sun) light with multi-sample soft shadow across the sun disc.
// cosSunAngularRadius = cos(half-angular-diameter of sun disc) from PathTracerConstants.
// Shadow rays are distributed uniformly over the disc solid angle; averaging N
// samples gives an unbiased penumbra estimate without explicit PDF weighting
// because EvaluateDirectLight already applies NdotL and the disc is treated as
// a uniform emitter (radiance is constant across the visible disc).
LightingComponents ComputeDirectionalLighting(LightingInputs inputs, GPULight light, float cosSunAngularRadius, inout RNG rng)
{
    LightingComponents result;
    result.diffuse  = 0;
    result.specular = 0;

    // Early-out: back-facing to sun centre
    if (dot(inputs.N, inputs.sunDirection) <= 0.0f) return result;

    float3 radiance = inputs.useSunRadiance ? inputs.sunRadiance : (light.m_Color * light.m_Intensity);

    [unroll]
    for (int s = 0; s < LIGHT_SHADOW_SAMPLES; ++s)
    {
        float3 L_s = SampleConeSolidAngle(inputs.sunDirection, cosSunAngularRadius, NextFloat2(rng));
        if (dot(inputs.N, L_s) <= 0.0f) continue;

        inputs.L = L_s;
        PrepareLightingByproducts(inputs);

        float shadow = CalculateRTShadow<true>(inputs, L_s, 1e10f);
        LightingComponents comp = EvaluateDirectLight(inputs, radiance, shadow);
        result.diffuse  += comp.diffuse;
        result.specular += comp.specular;
    }

    result.diffuse  /= float(LIGHT_SHADOW_SAMPLES);
    result.specular /= float(LIGHT_SHADOW_SAMPLES);
    return result;
}

// Point light with sphere-area soft shadow.
// When m_Radius == 0 the sphere jitter collapses to zero and this behaves like
// a hard-shadow point light (but still fires LIGHT_SHADOW_SAMPLES identical rays;
// reduce LIGHT_SHADOW_SAMPLES or set radius > 0 for efficiency).
// Attenuation is evaluated at the light centre, same as the rasterized path.
LightingComponents ComputePointLighting(LightingInputs inputs, GPULight light, inout RNG rng)
{
    LightingComponents result;
    result.diffuse  = 0;
    result.specular = 0;

    if (light.m_Intensity <= 0.0f) return result;

    float3 toLight = light.m_Position - inputs.worldPos;
    float  distSq  = dot(toLight, toLight);

    // Range cull
    if (light.m_Range > 0.0f && distSq > light.m_Range * light.m_Range) return result;

    float dist = sqrt(distSq);

    // Distance attenuation evaluated at centre  (same formula as rasterized spot)
    float distAttenuation = 1.0f / (distSq + 1.0f);
    if (light.m_Range > 0.0f)
        distAttenuation *= pow(saturate(1.0f - pow(dist / light.m_Range, 4.0f)), 2.0f);

    float3 radiance = light.m_Color * light.m_Intensity * distAttenuation;

    [unroll]
    for (int s = 0; s < LIGHT_SHADOW_SAMPLES; ++s)
    {
        // Uniform sample on the sphere surface; offset = 0 when m_Radius == 0
        float2 u         = NextFloat2(rng);
        float  cosT      = 1.0f - 2.0f * u.x;
        float  sinT      = sqrt(max(0.0f, 1.0f - cosT * cosT));
        float  phi_s     = 2.0f * PI * u.y;
        float3 sphereDir = float3(sinT * cos(phi_s), cosT, sinT * sin(phi_s));

        float3 samplePos  = light.m_Position + sphereDir * light.m_Radius;
        float3 toSample   = samplePos - inputs.worldPos;
        float  sampleDist = length(toSample);
        float3 L_s        = toSample / sampleDist;

        if (dot(inputs.N, L_s) <= 0.0f) continue;

        inputs.L = L_s;
        PrepareLightingByproducts(inputs);

        float shadow = CalculateRTShadow<true>(inputs, L_s, sampleDist);
        LightingComponents comp = EvaluateDirectLight(inputs, radiance, shadow);
        result.diffuse  += comp.diffuse;
        result.specular += comp.specular;
    }

    result.diffuse  /= float(LIGHT_SHADOW_SAMPLES);
    result.specular /= float(LIGHT_SHADOW_SAMPLES);
    return result;
}

// Spot light with radius-based sphere-area soft shadow.
// Cone attenuation is evaluated with the unperturbed centre direction to avoid
// incorrect penumbra / banding at cone edges when m_Radius > 0.
LightingComponents ComputeSpotLighting(LightingInputs inputs, GPULight light, inout RNG rng)
{
    LightingComponents result;
    result.diffuse  = 0;
    result.specular = 0;

    if (light.m_Intensity <= 0.0f) return result;

    float3 L_unnorm   = light.m_Position - inputs.worldPos;
    float  distSq     = dot(L_unnorm, L_unnorm);

    // Range cull
    if (light.m_Range > 0.0f && distSq > light.m_Range * light.m_Range) return result;

    float  dist      = sqrt(distSq);
    float3 L_center  = L_unnorm / dist;

    // Front-face cull with centre direction
    if (dot(inputs.N, L_center) <= 0.0f) return result;

    // Spot cone attenuation — use centre direction only (not jittered sample)
    float3 lightDir        = normalize(light.m_Direction);
    float  cosTheta_center = dot(-L_center, lightDir);
    float  cosOuter        = cos(light.m_SpotOuterConeAngle);
    if (cosTheta_center < cosOuter) return result;

    float cosInner        = cos(light.m_SpotInnerConeAngle);
    float spotAttenuation = saturate((cosTheta_center - cosOuter) / (cosInner - cosOuter));

    // Distance attenuation
    float distAttenuation = 1.0f / (distSq + 1.0f);
    if (light.m_Range > 0.0f)
        distAttenuation *= pow(saturate(1.0f - pow(dist / light.m_Range, 4.0f)), 2.0f);

    float3 radiance = light.m_Color * light.m_Intensity * spotAttenuation * distAttenuation;

    [unroll]
    for (int s = 0; s < LIGHT_SHADOW_SAMPLES; ++s)
    {
        // Uniform sample on the sphere surface; offset = 0 when m_Radius == 0
        float2 u         = NextFloat2(rng);
        float  cosT      = 1.0f - 2.0f * u.x;
        float  sinT      = sqrt(max(0.0f, 1.0f - cosT * cosT));
        float  phi_s     = 2.0f * PI * u.y;
        float3 sphereDir = float3(sinT * cos(phi_s), cosT, sinT * sin(phi_s));

        float3 samplePos  = light.m_Position + sphereDir * light.m_Radius;
        float3 toSample   = samplePos - inputs.worldPos;
        float  sampleDist = length(toSample);
        float3 L_s        = toSample / sampleDist;

        if (dot(inputs.N, L_s) <= 0.0f) continue;

        inputs.L = L_s;
        PrepareLightingByproducts(inputs);

        float shadow = CalculateRTShadow<true>(inputs, L_s, sampleDist);
        LightingComponents comp = EvaluateDirectLight(inputs, radiance, shadow);
        result.diffuse  += comp.diffuse;
        result.specular += comp.specular;
    }

    result.diffuse  /= float(LIGHT_SHADOW_SAMPLES);
    result.specular /= float(LIGHT_SHADOW_SAMPLES);
    return result;
}

// Signature extended with cosSunAngularRadius and inout RNG rng.
LightingComponents AccumulateDirectLighting(LightingInputs inputs, uint lightCount, float cosSunAngularRadius, inout RNG rng)
{
    LightingComponents total;
    total.diffuse  = 0;
    total.specular = 0;

    for (uint i = 0; i < lightCount; ++i)
    {
        GPULight light = inputs.lights[i];

        [branch]
        if (light.m_Type == 0) // Directional
        {
            LightingComponents comp = ComputeDirectionalLighting(inputs, light, cosSunAngularRadius, rng);
            total.diffuse  += comp.diffuse;
            total.specular += comp.specular;
        }
        else if (light.m_Type == 1) // Point
        {
            LightingComponents comp = ComputePointLighting(inputs, light, rng);
            total.diffuse  += comp.diffuse;
            total.specular += comp.specular;
        }
        else if (light.m_Type == 2) // Spot
        {
            LightingComponents comp = ComputeSpotLighting(inputs, light, rng);
            total.diffuse  += comp.diffuse;
            total.specular += comp.specular;
        }
    }
    return total;
}

IBLComponents ComputeIBL(LightingInputs inputs)
{
    IBLComponents components;

    SamplerState clampSampler = SamplerDescriptorHeap[SAMPLER_ANISOTROPIC_CLAMP_INDEX];

    // Diffuse IBL
    TextureCube irradianceMap = ResourceDescriptorHeap[DEFAULT_TEXTURE_IRRADIANCE];
    float3 irradiance = irradianceMap.Sample(clampSampler, inputs.N).rgb;
    float3 diffuseIBL = irradiance * inputs.baseColor * inputs.kD;

    // Specular IBL
    TextureCube prefilteredEnvMap = ResourceDescriptorHeap[DEFAULT_TEXTURE_RADIANCE];
    float3 R = reflect(-inputs.V, inputs.N);
    
    float mipLevel = inputs.roughness * (float(inputs.radianceMipCount) - 1.0f);
    float3 prefilteredColor = prefilteredEnvMap.SampleLevel(clampSampler, R, mipLevel).rgb;

    Texture2D brdfLut = ResourceDescriptorHeap[DEFAULT_TEXTURE_BRDF_LUT];
    float2 brdf = brdfLut.SampleLevel(clampSampler, float2(inputs.NdotV, inputs.roughness), 0).rg;

    float3 specularIBL = prefilteredColor * (inputs.F0 * brdf.x + brdf.y);

    components.irradiance = diffuseIBL;
    components.radiance = specularIBL;
    components.ibl = (diffuseIBL + specularIBL);

    return components;
}

// ─── Bent normal: flip shading normal to the same hemisphere as flat normal ──
float3 getBentNormal(float3 flatNormal, float3 shadingNormal, float3 rayDirection)
{
    float3 result = shadingNormal;
    if (dot(result, flatNormal) < 0.0f)
        result = reflect(result, flatNormal);
    return normalize(result);
}

// ─── Orthonormal basis construction ──────────────────────────────────────────
// Builds a right-handed ONB from a unit normal N.
// Frisvad / Duff et al. branchless variant.
void branchlessONB(float3 N, out float3 T, out float3 B)
{
    float s  = (N.z >= 0.0f) ? 1.0f : -1.0f;
    float a  = -1.0f / (s + N.z);
    float b  = N.x * N.y * a;
    T = float3(1.0f + s * N.x * N.x * a, s * b, -s * N.x);
    B = float3(b, s + N.y * N.y * a, -N.y);
}

// Alias used by RAB_Surface.hlsli
void ConstructONB(float3 N, out float3 T, out float3 B)
{
    branchlessONB(N, T, B);
}

// ─── One-sided Smith G1 masking for GGX ──────────────────────────────────────
float G1_Smith(float roughness, float NdotV)
{
    float alpha  = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom  = NdotV + sqrt(alpha2 + (1.0f - alpha2) * NdotV * NdotV);
    return (denom > 0.0f) ? (2.0f * NdotV / denom) : 0.0f;
}

// ─── Triangle / barycentric helpers ──────────────────────────────────────────

// Sample a random point on a triangle; returns barycentric coords (u,v,w).
float3 sampleTriangle(float2 rndSample)
{
    float sqrtx = sqrt(rndSample.x);
    return float3(1.0f - sqrtx, sqrtx * (1.0f - rndSample.y), sqrtx * rndSample.y);
}

// Maps ray hit UV (barycentrics from TraceRay) to (w, u, v) barycentric coords.
float3 hitUVToBarycentric(float2 hitUV)
{
    return float3(1.0f - hitUV.x - hitUV.y, hitUV.x, hitUV.y);
}

// Inverse of sampleTriangle.
float2 randomFromBarycentric(float3 barycentric)
{
    float sqrtx = 1.0f - barycentric.x;
    return float2(sqrtx * sqrtx, barycentric.z / sqrtx);
}

// ─── Disk / sphere sampling ───────────────────────────────────────────────────

// Uniform sample on a unit disk.
float2 sampleDisk(float2 rand)
{
    float angle = 2.0f * PI * rand.x;
    return float2(cos(angle), sin(angle)) * sqrt(rand.y);
}

// Cosine-weighted hemisphere sample in local tangent space (+Z = up).
// Returns direction; solidAnglePdf = cos(theta) / PI.
float3 SampleCosHemisphere(float2 rand, out float solidAnglePdf)
{
    float2 tangential = sampleDisk(rand);
    float elevation   = sqrt(saturate(1.0f - rand.y));
    solidAnglePdf     = elevation / PI;
    return float3(tangential.xy, elevation);
}

// Uniform sphere sample in local tangent space (+Z = up).
// solidAnglePdf = 1 / (4*PI).
float3 sampleSphere(float2 rand, out float solidAnglePdf)
{
    rand.y = rand.y * 2.0f - 1.0f;
    float2 tangential = sampleDisk(float2(rand.x, 1.0f - rand.y * rand.y));
    solidAnglePdf     = 0.25f / PI;
    return float3(tangential.xy, rand.y);
}

// ─── PDF conversion ───────────────────────────────────────────────────────────

// Convert area-measure PDF to solid-angle-measure PDF.
float pdfAtoW(float pdfA, float distance_, float cosTheta)
{
    return pdfA * (distance_ * distance_) / cosTheta;
}

// ─── Reflectivity helpers (RTXDI-compatible names) ───────────────────────────

// Metallic workflow split — RTXDI bridge uses this name.
// Equivalent to GetReflectivityFromMetallic above.
void getReflectivity(float metalness, float3 baseColor, out float3 o_albedo, out float3 o_baseReflectivity)
{
    GetReflectivityFromMetallic(metalness, baseColor, o_albedo, o_baseReflectivity);
}

// Approximate metalness from diffuse albedo and specular F0.
float getMetalness(float3 diffuseAlbedo, float3 specularF0)
{
    if (all(diffuseAlbedo == 0.0f)) return 1.0f;
    float F0 = calcLuminance(specularF0);
    return saturate(1.0417f * (F0 - 0.04f)); // 1/0.96 = 1.0417
}

// ─── GGX VNDF sampling in local tangent space (RTXDI bridge variant) ─────────
// Input Ve is the view direction already transformed into local tangent space
// (T=x, B=y, N=z). Returns the half-vector in the same local space.
// Use SampleGGX_VNDF (world-space) for world-space callers.
float3 sampleGGX_VNDF(float3 Ve, float roughness, float2 random)
{
    float alpha = roughness * roughness;

    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1   = lensq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) / sqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
    float3 T2   = cross(Vh, T1);

    float r   = sqrt(random.x);
    float phi = 2.0f * PI * random.y;
    float t1  = r * cos(phi);
    float t2  = r * sin(phi);
    float s   = 0.5f * (1.0f + Vh.z);
    t2        = (1.0f - s) * sqrt(max(0.0f, 1.0f - t1 * t1)) + s * t2;

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;
    return float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z));
}

// ─── Equirectangular UV ↔ direction ──────────────────────────────────────────

float2 directionToEquirectUV(float3 normalizedDirection)
{
    float elevation = asin(normalizedDirection.y);
    float azimuth   = 0.0f;
    if (abs(normalizedDirection.y) < 1.0f)
        azimuth = atan2(normalizedDirection.z, normalizedDirection.x);
    return float2(azimuth / (2.0f * PI) - 0.25f, 0.5f - elevation / PI);
}

float3 equirectUVToDirection(float2 uv, out float cosElevation)
{
    float azimuth   = (uv.x + 0.25f) * (2.0f * PI);
    float elevation = (0.5f - uv.y) * PI;
    cosElevation    = cos(elevation);
    return float3(cos(azimuth) * cosElevation, sin(elevation), sin(azimuth) * cosElevation);
}

// ─── Spherical direction from angles and ONB axes ────────────────────────────
float3 sphericalDirection(float sinTheta, float cosTheta, float sinPhi, float cosPhi, float3 x, float3 y, float3 z)
{
    return sinTheta * cosPhi * x + sinTheta * sinPhi * y + cosTheta * z;
}

#endif // COMMON_LIGHTING_HLSLI
