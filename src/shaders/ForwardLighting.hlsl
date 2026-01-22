#include "ShaderShared.h"

cbuffer PerFrameCB : register(b0, space1)
{
    ForwardLightingPerFrameData perFrame;
};

StructuredBuffer<PerInstanceData> g_Instances : register(t0, space1);
StructuredBuffer<MaterialConstants> g_Materials : register(t1, space1);
StructuredBuffer<Vertex> g_Vertices : register(t2, space1);

SamplerState g_SamplerAnisoClamp : register(s0, space1);
SamplerState g_SamplerAnisoWrap  : register(s1, space1);

float3x3 MakeAdjugateMatrix(float4x4 m)
{
    return float3x3
    (
		cross(m[1].xyz, m[2].xyz),
		cross(m[2].xyz, m[0].xyz),
		cross(m[0].xyz, m[1].xyz)
	);
}

struct VSOut
{
    float4 Position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    nointerpolation uint instanceID : TEXCOORD2;
};

VSOut VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_StartInstanceLocation)
{
    PerInstanceData inst = g_Instances[instanceID];
    Vertex v = g_Vertices[vertexID];
    VSOut o;
    float4 worldPos = mul(float4(v.m_Pos, 1.0f), inst.m_World);
    o.Position = mul(worldPos, perFrame.m_ViewProj);

    // Alien math to calculate the normal in world space, without inverse-transposing the world matrix
    float3x3 adjugateWorldMatrix = MakeAdjugateMatrix(inst.m_World);

    o.normal = normalize(mul(v.m_Normal, adjugateWorldMatrix));
    o.uv = v.m_Uv;
    o.worldPos = worldPos.xyz;
    o.instanceID = instanceID;
    return o;
}

// Unpacks a 2 channel normal to xyz
float3 TwoChannelNormalX2(float2 normal)
{
    float2 xy = 2.0f * normal - 1.0f;
    float z = sqrt(1 - dot(xy, xy));
    return float3(xy.x, xy.y, z);
}

// Christian Schuler, "Normal Mapping without Precomputed Tangents", ShaderX 5, Chapter 2.6, pp. 131-140
// See also follow-up blog post: http://www.thetenthplanet.de/archives/1180
float3x3 CalculateTBNWithoutTangent(float3 p, float3 n, float2 uv)
{
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float r = 1.0 / (duv1.x * duv2.y - duv2.x * duv1.y);
    float3 T = (dp1 * duv2.y - dp2 * duv1.y) * r;
    float3 B = (dp2 * duv1.x - dp1 * duv2.x) * r;
    return float3x3(normalize(T), normalize(B), n);
}

// 0.08 is a max F0 we define for dielectrics which matches with Crystalware and gems (0.05 - 0.08)
// This means we cannot represent Diamond-like surfaces as they have an F0 of 0.1 - 0.2
float DielectricSpecularToF0(float specular)
{
    return 0.08f * specular;
}

//Note from Filament: vec3 f0 = 0.16 * reflectance * reflectance * (1.0 - metallic) + baseColor * metallic;
// F0 is the base specular reflectance of a surface
// For dielectrics, this is monochromatic commonly between 0.02 (water) and 0.08 (gems) and derived from a separate specular value
// For conductors, this is based on the base color we provided
float3 ComputeF0(float specular, float3 baseColor, float metalness)
{
    return lerp(DielectricSpecularToF0(specular).xxx, baseColor, metalness);
}

float3 ComputeF0(float3 baseColor, float metalness)
{
    const float kMaterialSpecular = 0.5f;
    return lerp(DielectricSpecularToF0(kMaterialSpecular).xxx, baseColor, metalness);
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float3 F_Schlick(float3 f0, float VdotH)
{
    float Fc = pow(1.0f - VdotH, 5.0f);
    return Fc + (1.0f - Fc) * f0;
}

// GGX / Trowbridge-Reitz
// Note the division by PI here
// [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"]
float D_GGX(float a2, float NdotH)
{
    float d = (NdotH * a2 - NdotH) * NdotH + 1;
    return a2 / (PI * d * d);
}

// Appoximation of joint Smith term for GGX
// Returned value is G2 / (4 * NdotL * NdotV). So predivided by specular BRDF denominator
// [Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"]
float Vis_SmithJointApprox(float a2, float NdotV, float NdotL)
{
    float Vis_SmithV = NdotL * (NdotV * (1 - a2) + a2);
    float Vis_SmithL = NdotV * (NdotL * (1 - a2) + a2);
    return 0.5 * rcp(Vis_SmithV + Vis_SmithL);
}

// Oren-Nayar diffuse model returning the scalar BRDF multiplier
// https://mimosa-pudica.net/improved-oren-nayar.html
float OrenNayar(float NdotL, float NdotV, float LdotV, float a2, float albedo)
{
  float s = LdotV - NdotL * NdotV;
  float t = lerp(1.0, max(NdotL, NdotV), step(0.0, s));

  float A = 1.0 + a2 * (albedo / (a2 + 0.13) + 0.5 / (a2 + 0.33));
  float B = 0.45 * a2 / (a2 + 0.09);

  return albedo * max(0.0, NdotL) * (A + B * s / t) / PI;
}

// Helper function to sample from bindless textures using a sampler index.
float4 SampleBindlessTexture(uint textureIndex, uint samplerIndex, float2 uv)
{
    Texture2D tex = ResourceDescriptorHeap[textureIndex];
    
    // Sample once using branch-based selection to avoid sampler-typed phi nodes.
    if (samplerIndex == SAMPLER_CLAMP_INDEX)
        return tex.Sample(g_SamplerAnisoClamp, uv);
    else
        return tex.Sample(g_SamplerAnisoWrap, uv);
}

float4 PSMain(VSOut input) : SV_TARGET
{
    // Instance + material
    PerInstanceData inst = g_Instances[input.instanceID];
    MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

    // Texture sampling (only when present)
    bool hasAlbedo = (mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0;
    float4 albedoSample = hasAlbedo
        ? SampleBindlessTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, input.uv)
        : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);

    bool hasORM = (mat.m_TextureFlags & TEXFLAG_ROUGHNESS_METALLIC) != 0;
    float4 ormSample = hasORM
        ? SampleBindlessTexture(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex, input.uv)
        : float4(mat.m_RoughnessMetallic.x, mat.m_RoughnessMetallic.y, 0.0f, 0.0f);

    bool hasNormal = (mat.m_TextureFlags & TEXFLAG_NORMAL) != 0;
    float4 nmSample = hasNormal
        ? SampleBindlessTexture(mat.m_NormalTextureIndex, mat.m_NormalSamplerIndex, input.uv)
        : float4(0.5f, 0.5f, 1.0f, 0.0f);

    // Normal (from normal map when available)
    float3 N;
    if (hasNormal)
    {
        float3 normalMap = TwoChannelNormalX2(nmSample.xy);
        float3x3 TBN = CalculateTBNWithoutTangent(input.worldPos, input.normal, input.uv);
        N = normalize(mul(normalMap, TBN));
    }
    else
    {
        N = normalize(input.normal);
    }

    // View / light directions
    float3 V = normalize(perFrame.m_CameraPos.xyz - input.worldPos);
    float3 L = perFrame.m_LightDirection;
    float3 H = normalize(V + L);

    // Dot products
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float LdotV = saturate(dot(L, V));

    // Base color and alpha
    float3 baseColor;
    float alpha;
    if (hasAlbedo)
    {
        baseColor = albedoSample.xyz * mat.m_BaseColor.xyz;
        alpha = albedoSample.w * mat.m_BaseColor.w;
    }
    else
    {
        baseColor = mat.m_BaseColor.xyz;
        alpha = mat.m_BaseColor.w;
    }

    // Material properties (roughness, metallic)
    float roughness = mat.m_RoughnessMetallic.x;
    float metallic = mat.m_RoughnessMetallic.y;
    if (hasORM)
    {
        // ORM texture layout: R = occlusion, G = roughness, B = metallic
        roughness = ormSample.y;
        metallic = ormSample.z;
    }

    // Derived values
    float a = roughness * roughness;
    float a2 = clamp(a * a, 0.0001f, 1.0f);

    // Diffuse (Oren-Nayar)
    float oren = OrenNayar(NdotL, NdotV, LdotV, a2, 1.0f);
    float3 diffuse = oren * (1.0f - metallic) * baseColor;

    // Specular
    const float materialSpecular = 0.5f;
    float3 specularColor = ComputeF0(materialSpecular, baseColor, metallic);

    float D = D_GGX(a2, NdotH);
    float Vis = Vis_SmithJointApprox(a2, NdotV, NdotL);
    float3 F = F_Schlick(specularColor, VdotH);
    float3 spec = (D * Vis) * F;

    // Lighting and ambient
    float3 radiance = float3(perFrame.m_LightIntensity, perFrame.m_LightIntensity, perFrame.m_LightIntensity);
    float3 ambient = (1.0f - NdotL) * baseColor * 0.03f; // IBL fallback
    float3 color = ambient + (diffuse + spec) * radiance * NdotL;

    return float4(color, alpha);
}
