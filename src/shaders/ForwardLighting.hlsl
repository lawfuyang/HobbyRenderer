// Basic Forward Lighting shaders (VS + PS)

// Define forward-lighting specific macro so shared header exposes relevant types
#define FORWARD_LIGHTING_DEFINE
// Include shared types (VertexInput, PerObjectData)
#include "ShaderShared.hlsl"

// Define the cbuffer here using the shared PerFrameData struct
cbuffer PerFrameCB : register(b0)
{
    PerFrameData perFrame;
};

// Structured buffer for per-instance data
StructuredBuffer<PerInstanceData> instances : register(t0);

SamplerState g_Sampler : register(s0);

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
    float4 worldPos = mul(float4(input.pos, 1.0f), inst.World);
    o.Position = mul(worldPos, perFrame.ViewProj);

    // Alien math to calculate the normal and tangent in world space, without inverse-transposing the world matrix
    // https://github.com/graphitemaster/normals_revisited
    // https://x.com/iquilezles/status/1866219178409316362
    // https://www.shadertoy.com/view/3s33zj
    float3x3 adjugateWorldMatrix = MakeAdjugateMatrix(inst.World);

    o.normal = normalize(mul(input.normal, adjugateWorldMatrix));
    o.uv = input.uv;
    o.worldPos = worldPos.xyz;
    o.instanceID = instanceID;
    return o;
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
float D_GGX(float NdotH, float a2)
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

    // Test bindless texture sampling - sample white texture
    //float4 whiteSample = SampleBindlessTexture(DEFAULT_TEXTURE_WHITE, input.uv);

    // Reusable locals
    float3 N = normalize(input.normal);
    float3 V = normalize(perFrame.CameraPos.xyz - input.worldPos);
    float3 L = perFrame.LightDirection;
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(abs(dot(N, V)) + 1e-5); // Bias to avoid artifacting
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float LdotV = saturate(dot(L, V));

    float3 baseColor = inst.BaseColor.xyz;// * whiteSample.xyz; // Modulate with white texture for testing
    float alpha = inst.BaseColor.w;
    float roughness = inst.RoughnessMetallic.x;
    float metallic = inst.RoughnessMetallic.y;

    float a = roughness * roughness;
	float a2 = clamp(a * a, 0.0001f, 1.0f);

    // Diffuse BRDF via Oren-Nayar
    float oren = OrenNayar(NdotL, NdotV, LdotV, a2, 1.0f);
    float3 diffuse = oren * (1.0f - metallic) * inst.BaseColor.xyz / PI;
    
    const float materialSpecular = 0.5f; // TODO
    float3 specularColor = ComputeF0(materialSpecular, baseColor, metallic);

	// Generalized microfacet Specular BRDF
	float D = D_GGX(a2, NdotH);
	float Vis = Vis_SmithJointApprox(a2, NdotV, NdotL);
    float3 F = F_Schlick(specularColor, VdotH);
    float3 spec = (D * Vis) * F;

    spec += EnvBRDFApprox(specularColor, roughness, NdotV);

    float3 radiance = float3(perFrame.LightIntensity, perFrame.LightIntensity, perFrame.LightIntensity);
    // Fake ambient (IBL fallback): small ambient multiplied by baseColor and reduced by metallic
    float3 ambient = baseColor * 0.03f * (1.0f - metallic);
    float3 color = ambient + (diffuse + spec) * radiance * NdotL;
    return float4(color, alpha);
}
