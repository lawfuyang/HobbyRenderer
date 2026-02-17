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
    
    float depth = g_Depth.Load(uint3(uvInt, 0));
    
    // Reconstruction
    float4 clipPos;
    clipPos.x = uv.x * 2.0f - 1.0f;
    clipPos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    clipPos.z = depth;
    clipPos.w = 1.0f;

    float4 worldPosFour = MatrixMultiply(clipPos, g_Sky.m_View.m_MatClipToWorldNoOffset);
    float3 worldPos = worldPosFour.xyz / worldPosFour.w;
    float3 V = normalize(g_Sky.m_CameraPos.xyz - worldPos);

    float3 cameraPos = (g_Sky.m_CameraPos.xyz - g_Sky.m_EarthCenter) / 1000.0; // km
    float3 viewRay = -V;
    float3 sunDir = g_Sky.m_SunDirection;

    float3 transmittance;
    float3 skyRadiance = GetSkyRadiance(
        BRUNETON_TRANSMITTANCE_TEXTURE, BRUNETON_SCATTERING_TEXTURE,
        cameraPos, viewRay, 0.0, sunDir, transmittance);

    // Sun disk
    float nu = dot(viewRay, sunDir);
    float sunAngularRadius = ATMOSPHERE.sun_angular_radius;
    if (nu > cos(sunAngularRadius))
    {
        float3 sunRadiance = ATMOSPHERE.solar_irradiance / (PI * sunAngularRadius * sunAngularRadius);
        skyRadiance += sunRadiance * transmittance;
    }

    return float4(skyRadiance * g_Sky.m_SunIntensity, 1.0);
}
