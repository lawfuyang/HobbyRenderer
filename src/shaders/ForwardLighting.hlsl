// Basic Forward Lighting shaders (VS + PS)

// Define forward-lighting specific macro so shared header exposes relevant types
#define FORWARD_LIGHTING_DEFINE
// Include shared types (VertexInput, PerObjectData)
#include "ShaderShared.hlsl"

// Define the cbuffer here using the shared PerFrameData struct
cbuffer PerFrameCB : register(b0, space1)
{
    PerFrameData perFrame;
};

// Structured buffer for per-instance data
StructuredBuffer<PerInstanceData> instances : register(t0, space1);
// Structured buffer for material constants
StructuredBuffer<MaterialConstants> materials : register(t1, space1);

SamplerState g_Sampler : register(s0, space1);

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

VSOut VSMain(VertexInput input, uint instanceID : SV_StartInstanceLocation)
{
    PerInstanceData inst = instances[instanceID];
    VSOut o;
    float4 worldPos = mul(float4(input.m_Pos, 1.0f), inst.m_World);
    o.Position = mul(worldPos, perFrame.m_ViewProj);

    // Alien math to calculate the normal in world space, without inverse-transposing the world matrix
    float3x3 adjugateWorldMatrix = MakeAdjugateMatrix(inst.m_World);

    o.normal = normalize(mul(input.m_Normal, adjugateWorldMatrix));
    o.uv = input.m_Uv;
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

    float3x3 M = float3x3(dp1, dp2, cross(dp1, dp2));
    float2x3 inverseM = float2x3(cross(M[1], M[2]), cross(M[2], M[0]));
    float3 t = normalize(mul(float2(duv1.x, duv2.x), inverseM));
    float3 b = normalize(mul(float2(duv1.y, duv2.y), inverseM));
    return float3x3(t, b, n);
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

// https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile
float3 EnvBRDFApprox(float3 specularColor, float roughness, float ndotv)
{
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndotv)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return specularColor * AB.x + AB.y;
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

// Helper function to sample from bindless textures
float4 SampleBindlessTexture(uint textureIndex, float2 uv)
{
    Texture2D tex = ResourceDescriptorHeap[textureIndex];
    return tex.Sample(g_Sampler, uv);
}

float4 PSMain(VSOut input) : SV_TARGET
{
    PerInstanceData inst = instances[input.instanceID];
    MaterialConstants mat = materials[inst.m_MaterialIndex];

    // Only sample textures when the material indicates presence.
    // When absent, assign the "sample" variables from MaterialConstants to avoid sampling.
    bool hasAlbedo = (mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0;
    float4 albedoSample = hasAlbedo
        ? SampleBindlessTexture(mat.m_AlbedoTextureIndex, input.uv)
        : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);

    bool hasORM = (mat.m_TextureFlags & TEXFLAG_ROUGHNESS_METALLIC) != 0;
    float4 ormSample = hasORM
        ? SampleBindlessTexture(mat.m_RoughnessMetallicTextureIndex, input.uv)
        : float4(mat.m_RoughnessMetallic.x, mat.m_RoughnessMetallic.y, 0.0f, 0.0f);

    bool hasNormal = (mat.m_TextureFlags & TEXFLAG_NORMAL) != 0;
    float4 nmSample = hasNormal
        ? SampleBindlessTexture(mat.m_NormalTextureIndex, input.uv)
        : float4(0.5f, 0.5f, 1.0f, 0.0f);

    float3 normalMap = TwoChannelNormalX2(nmSample.xy);

    // Compute TBN matrix
    float3x3 TBN = CalculateTBNWithoutTangent(input.worldPos, input.normal, input.uv);

    // Reusable locals
    float3 N = normalize(mul(normalMap, TBN));
    float3 V = normalize(perFrame.m_CameraPos.xyz - input.worldPos);
    float3 H = normalize(V + perFrame.m_LightDirection);

    float NdotL = saturate(dot(N, perFrame.m_LightDirection));
    float NdotV = saturate(abs(dot(N, V)) + 1e-5); // Bias to avoid artifacting
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float LdotV = saturate(dot(perFrame.m_LightDirection, V));

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
    // Start with material constants, then override from ORM texture when present
    float roughness = mat.m_RoughnessMetallic.x;
    float metallic = mat.m_RoughnessMetallic.y;
    if (hasORM)
    {
        // ORM texture layout: R = occlusion, G = roughness, B = metallic
        roughness = ormSample.y;
        metallic = ormSample.z;
    }

    float a = roughness * roughness;
	float a2 = clamp(a * a, 0.0001f, 1.0f);

    // Diffuse BRDF via Oren-Nayar
    float oren = OrenNayar(NdotL, NdotV, LdotV, a2, 1.0f);
    float3 diffuse = oren * (1.0f - metallic) * baseColor / PI;
    
    const float materialSpecular = 0.5f; // TODO
    float3 specularColor = ComputeF0(materialSpecular, baseColor, metallic);

	// Generalized microfacet Specular BRDF
	float D = D_GGX(a2, NdotH);
	float Vis = Vis_SmithJointApprox(a2, NdotV, NdotL);
    float3 F = F_Schlick(specularColor, VdotH);
    float3 spec = (D * Vis) * F;

    spec += EnvBRDFApprox(specularColor, roughness, NdotV);

    float3 radiance = float3(perFrame.m_LightIntensity, perFrame.m_LightIntensity, perFrame.m_LightIntensity);
    // Fake ambient (IBL fallback): small ambient multiplied by baseColor and reduced by metallic
    float3 ambient = (1.0f - NdotL) * baseColor * 0.03f;
    float3 color = ambient + (diffuse + spec) * radiance * NdotL;
    return float4(color, alpha);
}
