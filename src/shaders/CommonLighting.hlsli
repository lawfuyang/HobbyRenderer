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
float3 ComputeF0(float specular, float3 baseColor, float metallic)
{
    return lerp(float3(0.04, 0.04, 0.04) * specular * 2.0, baseColor, metallic);
}

float D_GGX(float NdotH, float m)
{
    float m2 = m * m;
    float f = (NdotH * m2 - NdotH) * NdotH + 1.0;
    return m2 / (3.14159265 * f * f);
}

float Vis_SmithJointApprox(float a2, float NdotV, float NdotL)
{
    float visV = NdotL * sqrt(NdotV * (NdotV - a2 * NdotV) + a2);
    float visL = NdotV * sqrt(NdotL * (NdotL - a2 * NdotL) + a2);
    return 0.5 / (visV + visL);
}

float3 F_Schlick(float3 f0, float VdotH)
{
    float f90 = saturate(dot(f0, 50.0 * 0.33));
    return f0 + (f90 - f0) * pow(1.0 - VdotH, 5.0);
}

float OrenNayar(float nl, float nv, float lv, float a2, float s)
{
    float A = 1.0 - 0.5 * a2 / (a2 + 0.33);
    float B = 0.45 * a2 / (a2 + 0.09);
    float C = sqrt((1.0 - nl * nl) * (1.0 - nv * nv)) / max(nl, nv);
    float cos_phi_diff = lv - nl * nv;
    return s * nl * (A + B * max(0.0, cos_phi_diff) * C);
}

#endif // COMMON_LIGHTING_HLSLI
