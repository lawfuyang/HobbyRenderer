#define DEFERRED_PASS
#include "ShaderShared.h"
#include "Bindless.hlsli"
#include "Atmosphere.hlsli"

cbuffer SkyCB : register(b0)
{
    SkyConstants g_Sky;
};

Texture2D<float> g_Depth : register(t4);

float4 Sky_PSMain(FullScreenVertexOut input) : SV_Target
{
    uint2 uvInt = uint2(input.pos.xy);
    float2 uv = input.uv;
    
    // Reconstruction
    float4 clipPos;
    clipPos.x = uv.x * 2.0f - 1.0f;
    clipPos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    clipPos.z = 0.5f;
    clipPos.w = 1.0f;

    float4 worldPosFour = MatrixMultiply(clipPos, g_Sky.m_View.m_MatClipToWorldNoOffset);
    float3 worldPos = worldPosFour.xyz / worldPosFour.w;
    float3 V = normalize(g_Sky.m_CameraPos.xyz - worldPos);

    float3 viewRay = -V;
    float3 skyRadiance = GetAtmosphereSkyRadiance(g_Sky.m_CameraPos.xyz, viewRay, g_Sky.m_SunDirection, g_Sky.m_SunIntensity);

    return float4(skyRadiance, 1.0);
}
