#ifndef COMMON_LIGHTING_HLSLI
#define COMMON_LIGHTING_HLSLI

#include "ShaderShared.h"
#include "RaytracingCommon.hlsli"

#define LIGHT_SHADOW_SAMPLES 1

// RNG for stochastic sampling — only present in path-tracer compute kernels, not rasterized passes
#include "PathTracerRNG.hlsli"

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

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH2 = NdotH*NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
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

    float3 H = normalize(inputs.V + inputs.L);
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

LightingComponents EvaluateDirectLight(LightingInputs inputs, float3 radiance, float shadow)
{
    LightingComponents components;

    float diffuseTerm = DisneyBurleyDiffuse(inputs.NdotL, inputs.NdotV, inputs.LdotH, inputs.roughness);
    float3 diffuse = diffuseTerm * inputs.kD * inputs.baseColor;

    float NDF = DistributionGGX(inputs.NdotH, inputs.roughness);
    float G = GeometrySmith(inputs.NdotV, inputs.NdotL, inputs.roughness);

    float3 numerator = NDF * G * inputs.F; 
    float denominator = 4.0 * inputs.NdotV * inputs.NdotL + 0.0001;
    float3 spec = numerator / denominator;

    components.diffuse = diffuse * inputs.NdotL * radiance * shadow;
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

    RayDesc ray;
    ray.Origin = inputs.worldPos + inputs.N * 0.1f;
    ray.Direction = L;
    ray.TMin = 0.1f;
    ray.TMax = maxDist;

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
                TriangleVertices tv = GetTriangleVertices(primitiveIndex, mesh, inputs.indices, inputs.vertices);
                RayGradients grad = GetShadowRayGradients(tv, bary, ray.Origin, inst.m_World);
                
                if (AlphaTestGrad(grad.uv, grad.ddx, grad.ddy, mat))
                {
                    q.CommitNonOpaqueTriangleHit();
                }
            }
            else if (mat.m_AlphaMode == ALPHA_MODE_BLEND)
            {
                // Surface coverage attenuation (alpha blend); transmissive materials should
                // remain hittable and be handled as light filters instead of disappearing.
                float2 uvSample = GetInterpolatedUV(primitiveIndex, bary, mesh, inputs.indices, inputs.vertices);
                float alpha = mat.m_BaseColor.w;
                if ((mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0)
                {
                    alpha *= SampleBindlessTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uvSample).w;
                }
                    
                if (bWithTransmission)
                {
                    float opacity = saturate(alpha * (1.0f - mat.m_TransmissionFactor));
                    transmission *= (1.0f - opacity);

                    // Volumetric attenuation for thick transmissive media.
                    if (mat.m_TransmissionFactor > 0.0f && mat.m_IsThinSurface == 0)
                    {
                        TriangleVertices tv = GetTriangleVertices(primitiveIndex, mesh, inputs.indices, inputs.vertices);
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
    float cosTheta = dot(L, lightDir);
    float cosOuter = cos(light.m_SpotOuterConeAngle);
    if (cosTheta > cosOuter) return result;

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
    float  cosTheta_center = dot(L_center, lightDir);
    float  cosOuter        = cos(light.m_SpotOuterConeAngle);
    if (cosTheta_center > cosOuter) return result;

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

#endif // COMMON_LIGHTING_HLSLI
