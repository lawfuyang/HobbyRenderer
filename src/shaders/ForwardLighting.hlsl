// Basic Forward Lighting shaders (VS + PS)

// Define forward-lighting specific macro so shared header exposes relevant types
#define FORWARD_LIGHTING_DEFINE
// Include shared types (VertexInput, PerObjectData)
#include "ShaderShared.hlsl"

// Define the cbuffer here using the shared PerObjectData struct
cbuffer PerObjectCB : register(b0)
{
    PerObjectData perObject;
};

// Hardcoded directional light (in world space)
static const float3 kLightDirConst = float3(0.6f, -0.7f, -0.4f);

struct VSOut
{
    float4 Position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

VSOut VSMain(VertexInput input)
{
    VSOut o;
    float4 worldPos = mul(float4(input.pos, 1.0f), perObject.World);
    o.Position = mul(worldPos, perObject.ViewProj);
    o.normal = normalize(mul((float3x3)perObject.World, input.normal));
    o.uv = input.uv;
    o.worldPos = worldPos.xyz;
    return o;
}

// Helper BRDF functions
float3 Fresnel_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * denom * denom + 1e-6);
}

float G_SchlickGGX(float NdotV, float k)
{
    return NdotV / (NdotV * (1.0f - k) + k + 1e-6);
}

float G_Smith(float NdotV, float NdotL, float a)
{
    float k = (a + 1.0f);
    k = (k * k) / 8.0f;
    return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);
}

// Oren-Nayar diffuse model returning the scalar BRDF multiplier
float OrenNayar(float3 N, float3 V, float3 L, float roughness, float NdotV, float NdotL)
{
    // Map roughness [0,1] (PBR) to sigma angle in radians [0, PI/2]
    float sigma = roughness * (PI * 0.5f);
    float sigma2 = sigma * sigma;
    float A = 1.0f - 0.5f * sigma2 / (sigma2 + 0.33f);
    float B = 0.45f * sigma2 / (sigma2 + 0.09f);

    float3 Vt = V - N * NdotV;
    float3 Lt = L - N * NdotL;
    float VtLen = length(Vt);
    float LtLen = length(Lt);
    float cosPhi = 0.0f;
    if (VtLen > 1e-6 && LtLen > 1e-6)
    {
        cosPhi = dot(Vt / VtLen, Lt / LtLen);
        cosPhi = max(0.0f, cosPhi);
    }

    float theta_i = acos(NdotL);
    float theta_r = acos(NdotV);
    float alpha = max(theta_i, theta_r);
    float beta = min(theta_i, theta_r);
    float oren = A + B * cosPhi * sin(alpha) * tan(beta);
    return oren;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    // Reusable locals
    float3 N = normalize(input.normal);
    float3 V = normalize(perObject.CameraPos.xyz - input.worldPos);
    float3 L = normalize(kLightDirConst);
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 baseColor = perObject.BaseColor.xyz;
    float alpha = perObject.BaseColor.w;
    float roughness = perObject.RoughnessMetallic.x;
    float metallic = perObject.RoughnessMetallic.y;

    // Fresnel (Schlick)
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    float3 F = Fresnel_Schlick(VdotH, F0);

    // Specular terms (GGX)
    float a = roughness * roughness;
    float D = D_GGX(NdotH, a);
    float G = G_Smith(NdotV, NdotL, a);
    float3 spec = (D * G) * F / max(4.0f * NdotV * NdotL, 1e-6);

    // Diffuse via Oren-Nayar
    float oren = OrenNayar(N, V, L, roughness, NdotV, NdotL);
    float3 diffuse = (1.0f - metallic) * baseColor * (1.0f - F0) * oren / PI;

    float3 radiance = float3(1.0f, 1.0f, 1.0f);
    // Fake ambient (IBL fallback): small ambient multiplied by baseColor and reduced by metallic
    float3 ambient = baseColor * 0.03f * (1.0f - metallic);
    float3 color = ambient + (diffuse + spec) * radiance * NdotL;
    return float4(color, alpha);
}
