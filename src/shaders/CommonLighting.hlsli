#ifndef COMMON_LIGHTING_HLSLI
#define COMMON_LIGHTING_HLSLI

#include "ShaderShared.h"

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
    float lightIntensity;
    float3 worldPos;
    uint radianceMipCount;
    bool enableRTShadows;
    RaytracingAccelerationStructure sceneAS;
    StructuredBuffer<PerInstanceData> instances;
    StructuredBuffer<MeshData> meshData;
    StructuredBuffer<MaterialConstants> materials;
    StructuredBuffer<uint> indices;
    StructuredBuffer<VertexQuantized> vertices;

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

LightingComponents ComputeDirectionalLighting(LightingInputs inputs)
{
    LightingComponents components;

    //float diffuseTerm = OrenNayar(NdotL, inputs.NdotV, LdotV, inputs.roughness);
    float diffuseTerm = DisneyBurleyDiffuse(inputs.NdotL, inputs.NdotV, inputs.LdotH, inputs.roughness);
    float3 diffuse = diffuseTerm * inputs.kD * inputs.baseColor;

    float NDF = DistributionGGX(inputs.NdotH, inputs.roughness);
    float G = GeometrySmith(inputs.NdotV, inputs.NdotL, inputs.roughness);
    float3 F = inputs.F;

    float3 numerator = NDF * G * F; 
    float denominator = 4.0 * inputs.NdotV * inputs.NdotL + 0.0001; // + 0.0001 to prevent divide by zero
    float3 spec = numerator / denominator;

    float3 radiance = float3(inputs.lightIntensity, inputs.lightIntensity, inputs.lightIntensity);

    // Raytraced shadows
    float shadow = 1.0f;
    if (inputs.enableRTShadows)
    {
        RayDesc ray;
        ray.Origin = inputs.worldPos + inputs.N * 0.1f;
        ray.Direction = inputs.L;
        ray.TMin = 0.1f;
        ray.TMax = 1e10f;

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
                    uint baseIndex = mesh.m_IndexOffsets[0];
                    uint i0 = inputs.indices[baseIndex + 3 * primitiveIndex + 0];
                    uint i1 = inputs.indices[baseIndex + 3 * primitiveIndex + 1];
                    uint i2 = inputs.indices[baseIndex + 3 * primitiveIndex + 2];

                    Vertex v0 = UnpackVertex(inputs.vertices[i0]);
                    Vertex v1 = UnpackVertex(inputs.vertices[i1]);
                    Vertex v2 = UnpackVertex(inputs.vertices[i2]);

                    float2 uv0 = v0.m_Uv;
                    float2 uv1 = v1.m_Uv;
                    float2 uv2 = v2.m_Uv;

                    float2 uv = uv0 * (1.0f - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
                    
                    // Compute approximate UV gradients for proper texture filtering in ray tracing
                    // Use triangle geometry and distance to estimate ddx/ddy
                    float3 p0 = mul(float4(v0.m_Pos, 1.0f), inst.m_World).xyz;
                    float3 p1 = mul(float4(v1.m_Pos, 1.0f), inst.m_World).xyz;
                    float3 p2 = mul(float4(v2.m_Pos, 1.0f), inst.m_World).xyz;
                    
                    // Interpolate hit position using barycentrics
                    float3 hitPos = p0 * (1.0f - bary.x - bary.y) + p1 * bary.x + p2 * bary.y;
                    float dist = length(hitPos - ray.Origin);
                    
                    // Compute triangle area in world space
                    float3 edge1 = p1 - p0;
                    float3 edge2 = p2 - p0;
                    float triangleArea = length(cross(edge1, edge2)) * 0.5f;
                    
                    // Compute UV range across the triangle
                    float2 uvMin = min(uv0, min(uv1, uv2));
                    float2 uvMax = max(uv0, max(uv1, uv2));
                    float2 uvRange = uvMax - uvMin;
                    
                    // Approximate gradient scale based on triangle size and distance
                    // This provides a heuristic for proper mip selection
                    float gradientScale = triangleArea / max(dist, 0.1f);
                    float2 ddx_uv = uvRange * gradientScale;
                    float2 ddy_uv = uvRange * gradientScale;
                    
                    bool hasAlbedo = (mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0;
                    float4 albedoSample = hasAlbedo 
                        ? SampleBindlessTextureGrad(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uv, ddx_uv, ddy_uv)
                        : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);
                    
                    float alpha = hasAlbedo ? (albedoSample.w * mat.m_BaseColor.w) : mat.m_BaseColor.w;
                    
                    if (alpha >= mat.m_AlphaCutoff)
                    {
                        q.CommitNonOpaqueTriangleHit();
                    }
                }
                else if (mat.m_AlphaMode == ALPHA_MODE_BLEND)
                {
                    // ignore. dont let transparent geometry cast shadows
                }
                else if (mat.m_AlphaMode == ALPHA_MODE_OPAQUE)
                {
                    // This should not happen if TLAS/BLAS flags are correct, but handle it just in case
                    q.CommitNonOpaqueTriangleHit();
                }
            }
        }

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            shadow = 0.0f;
        }
    }

    components.diffuse = diffuse * inputs.NdotL * radiance * shadow;
    components.specular = spec * inputs.NdotL * radiance * shadow;

    return components;
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
