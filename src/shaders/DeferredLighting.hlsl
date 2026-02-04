#include "ShaderShared.h"
#include "CommonLighting.hlsli"

cbuffer DeferredCB : register(b1, space0)
{
    DeferredLightingConstants g_Deferred;
};

Texture2D<float4> g_GBufferAlbedo    : register(t0, space0);
Texture2D<float2> g_GBufferNormals   : register(t1, space0);
Texture2D<float4> g_GBufferORM       : register(t2, space0);
Texture2D<float4> g_GBufferEmissive  : register(t3, space0);
Texture2D<float>  g_Depth            : register(t4, space0);
RaytracingAccelerationStructure g_SceneAS : register(t5, space0);

RWTexture2D<float4> g_OutColor : register(u0, space0);

[numthreads(8, 8, 1)]
void DeferredLighting_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float2 dims;
    g_OutColor.GetDimensions(dims.x, dims.y);
    if (dispatchThreadID.x >= dims.x || dispatchThreadID.y >= dims.y)
        return;

    uint2 uvInt = dispatchThreadID.xy;
    float2 uv = (float2(uvInt) + 0.5f) / dims;
    
    float depth = g_Depth.Load(uint3(uvInt, 0));
    if (depth == 0.0f) // Reversed-Z, 0 is far plane
    {
        g_OutColor[uvInt] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    float4 albedoAlpha = g_GBufferAlbedo.Load(uint3(uvInt, 0));
    float3 baseColor = albedoAlpha.rgb;
    float alpha = albedoAlpha.a;

    float3 N = DecodeNormal(g_GBufferNormals.Load(uint3(uvInt, 0)));
    float4 orm = g_GBufferORM.Load(uint3(uvInt, 0));
    float occlusion = orm.r;
    float roughness = orm.g;
    float metallic = orm.b;

    float3 emissive = g_GBufferEmissive.Load(uint3(uvInt, 0)).rgb;

    // Reconstruct world position
    float4 clipPos;
    clipPos.x = uv.x * 2.0f - 1.0f;
    clipPos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    clipPos.z = depth;
    clipPos.w = 1.0f;

    float4 worldPosFour = mul(clipPos, g_Deferred.m_InvViewProj);
    float3 worldPos = worldPosFour.xyz / worldPosFour.w;

    // Lighting calculations
    float3 V = normalize(g_Deferred.m_CameraPos.xyz - worldPos);
    float3 L = g_Deferred.m_LightDirection;
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float LdotV = saturate(dot(L, V));

    float a = roughness * roughness;
    float a2 = clamp(a * a, 0.0001f, 1.0f);

    float oren = OrenNayar(NdotL, NdotV, LdotV, a2, 1.0f);
    float3 diffuse = oren * (1.0f - metallic) * baseColor;

    const float materialSpecular = 0.5f;
    float3 specularColor = ComputeF0(materialSpecular, baseColor, metallic);

    float D = D_GGX(a2, NdotH);
    float Vis = Vis_SmithJointApprox(a2, NdotV, NdotL);
    float3 F = F_Schlick(specularColor, VdotH);
    float3 spec = (D * Vis) * F;

    float3 radiance = float3(g_Deferred.m_LightIntensity, g_Deferred.m_LightIntensity, g_Deferred.m_LightIntensity);

    // Raytraced shadows
    float shadow = 1.0f;
    if (g_Deferred.m_EnableRTShadows != 0)
    {
        RayDesc ray;
        ray.Origin = worldPos + N * 0.1f;
        ray.Direction = L;
        ray.TMin = 0.0f;
        ray.TMax = 10000.0f;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            shadow = 0.0f;
        }
    }

    float3 ambient = (1.0f - (g_Deferred.m_EnableRTShadows ? shadow : 0.0f)) * baseColor * 0.03f;
    float3 color = ambient + (diffuse + spec) * radiance * NdotL * shadow;
    color += emissive;

    // Debug visualizations
    if (g_Deferred.m_DebugMode != DEBUG_MODE_NONE)
    {
        if (g_Deferred.m_DebugMode == DEBUG_MODE_WORLD_NORMALS)
            color = N * 0.5f + 0.5f;
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_ALBEDO)
            color = baseColor;
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_ROUGHNESS)
            color = roughness.xxx;
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_METALLIC)
            color = metallic.xxx;
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_EMISSIVE)
            color = emissive;
        
        if (g_Deferred.m_DebugMode == DEBUG_MODE_INSTANCES ||
            g_Deferred.m_DebugMode == DEBUG_MODE_MESHLETS ||
            g_Deferred.m_DebugMode == DEBUG_MODE_LOD)
            {
                color = baseColor;
            }
    }

    g_OutColor[uvInt] = float4(color, alpha);
}
