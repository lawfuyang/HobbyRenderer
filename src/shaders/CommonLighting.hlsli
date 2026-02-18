#ifndef COMMON_LIGHTING_HLSLI
#define COMMON_LIGHTING_HLSLI

#include "ShaderShared.h"
#include "RaytracingCommon.hlsli"

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

float CalculateRTShadow(LightingInputs inputs, float3 L, float maxDist)
{
    if (!inputs.enableRTShadows) return 1.0f;

    RayDesc ray;
    ray.Origin = inputs.worldPos + inputs.N * 0.1f;
    ray.Direction = L;
    ray.TMin = 0.1f;
    ray.TMax = maxDist;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(inputs.sceneAS, RAY_FLAG_NONE, 0xFF, ray);
    
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instanceIndex = q.CandidateInstanceIndex();
            uint primitiveIndex = q.CandidatePrimitiveIndex();
            float2 bary = q.CandidateTriangleBarycentrics();

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
                // ignore
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

    return 1.0f;
}

LightingComponents ComputeDirectionalLighting(LightingInputs inputs, GPULight light)
{
    LightingComponents result;
    result.diffuse = 0;
    result.specular = 0;

    float3 L = normalize(-light.m_Direction);
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
    
    float shadow = CalculateRTShadow(inputs, L, dist);

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
