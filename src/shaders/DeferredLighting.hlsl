#include "ShaderShared.h"
#include "Bindless.hlsli"
#include "CommonLighting.hlsli"

cbuffer DeferredCB : register(b1, space1)
{
    DeferredLightingConstants g_Deferred;
};

// These are bound to space 1 because we use the global bindless descriptor table in space 0.
Texture2D<float4> g_GBufferAlbedo    : register(t0, space1);
Texture2D<float2> g_GBufferNormals   : register(t1, space1);
Texture2D<float4> g_GBufferORM       : register(t2, space1);
Texture2D<float4> g_GBufferEmissive  : register(t3, space1);
Texture2D<float2> g_GBufferMotion    : register(t7, space1);
Texture2D<float>  g_Depth            : register(t4, space1);
RaytracingAccelerationStructure g_SceneAS : register(t5, space1);
StructuredBuffer<PerInstanceData> g_Instances : register(t10, space1);
StructuredBuffer<MaterialConstants> g_Materials : register(t11, space1);
StructuredBuffer<VertexQuantized> g_Vertices : register(t12, space1);
StructuredBuffer<MeshData> g_MeshData : register(t13, space1);
StructuredBuffer<uint> g_Indices : register(t14, space1);
SamplerState g_SamplerAnisoClamp : register(s0, space1);
SamplerState g_SamplerAnisoWrap  : register(s1, space1);

float4 DeferredLighting_PSMain(FullScreenVertexOut input) : SV_Target
{
    uint2 uvInt = uint2(input.pos.xy);
    float2 uv = input.uv;
    
    float depth = g_Depth.Load(uint3(uvInt, 0));
    if (depth == 0.0f) // Reversed-Z, 0 is far plane
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
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

    float4 worldPosFour = mul(clipPos, g_Deferred.m_View.m_MatClipToWorld);
    float3 worldPos = worldPosFour.xyz / worldPosFour.w;

    // Lighting calculations
    float3 V = normalize(g_Deferred.m_CameraPos.xyz - worldPos);
    float3 L = g_Deferred.m_LightDirection;

    LightingInputs lightingInputs;
    lightingInputs.N = N;
    lightingInputs.V = V;
    lightingInputs.L = L;
    lightingInputs.baseColor = baseColor;
    lightingInputs.roughness = roughness;
    lightingInputs.metallic = metallic;
    lightingInputs.ior = 1.5f; // Default IOR for opaque
    lightingInputs.lightIntensity = g_Deferred.m_LightIntensity;
    lightingInputs.worldPos = worldPos;
    lightingInputs.radianceMipCount = g_Deferred.m_RadianceMipCount;
    lightingInputs.enableRTShadows = g_Deferred.m_EnableRTShadows != 0;
    lightingInputs.sceneAS = g_SceneAS;
    lightingInputs.instances = g_Instances;
    lightingInputs.meshData = g_MeshData;
    lightingInputs.materials = g_Materials;
    lightingInputs.indices = g_Indices;
    lightingInputs.vertices = g_Vertices;
    lightingInputs.clampSampler = g_SamplerAnisoClamp;
    lightingInputs.wrapSampler = g_SamplerAnisoWrap;

    PrepareLightingByproducts(lightingInputs);

    LightingComponents directLight = ComputeDirectionalLighting(lightingInputs);
    float3 color = directLight.diffuse + directLight.specular;
    
    IBLComponents iblComp = ComputeIBL(lightingInputs);
    float3 ibl = iblComp.ibl;
    ibl *= float(g_Deferred.m_EnableIBL) * g_Deferred.m_IBLIntensity;

    color += ibl + emissive;

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
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_IRRADIANCE)
            color = iblComp.irradiance;
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_RADIANCE)
            color = iblComp.radiance;
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_IBL)
            color = ibl;
        else if (g_Deferred.m_DebugMode == DEBUG_MODE_MOTION_VECTORS)
            color = float3(abs(g_GBufferMotion.Load(uint3(uvInt, 0)).xy), 0.0f);
        
        if (g_Deferred.m_DebugMode == DEBUG_MODE_INSTANCES ||
            g_Deferred.m_DebugMode == DEBUG_MODE_MESHLETS ||
            g_Deferred.m_DebugMode == DEBUG_MODE_LOD)
            {
                color = baseColor;
            }
    }

    return float4(color, alpha);
}
