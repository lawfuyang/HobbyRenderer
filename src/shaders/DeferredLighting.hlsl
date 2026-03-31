#define DEFERRED_PASS
#include "Bindless.hlsli"
#include "CommonLighting.hlsli"
#include "Atmosphere.hlsli"

#include "srrhi/hlsl/GPULight.hlsli"
#include "srrhi/hlsl/DeferredLighting.hlsli"

static const srrhi::DeferredLightingConstants g_Deferred = srrhi::DeferredLightingInputs::GetDeferredCB();

static const Texture2D<float4>                          g_GBufferAlbedo      = srrhi::DeferredLightingInputs::GetGBufferAlbedo();
static const Texture2D<float2>                          g_GBufferNormals     = srrhi::DeferredLightingInputs::GetGBufferNormals();
static const Texture2D<float2>                          g_GBufferORM         = srrhi::DeferredLightingInputs::GetGBufferORM();
static const Texture2D<float4>                          g_GBufferEmissive    = srrhi::DeferredLightingInputs::GetGBufferEmissive();
static const Texture2D<float2>                          g_GBufferMotion      = srrhi::DeferredLightingInputs::GetGBufferMotion();
static const Texture2D<float>                           g_Depth              = srrhi::DeferredLightingInputs::GetDepth();
static const RaytracingAccelerationStructure            g_SceneAS            = srrhi::DeferredLightingInputs::GetSceneAS();
static const StructuredBuffer<srrhi::PerInstanceData>   g_Instances          = srrhi::DeferredLightingInputs::GetInstances();
static const StructuredBuffer<srrhi::MaterialConstants> g_Materials          = srrhi::DeferredLightingInputs::GetMaterials();
static const StructuredBuffer<srrhi::VertexQuantized>   g_Vertices           = srrhi::DeferredLightingInputs::GetVertices();
static const StructuredBuffer<srrhi::MeshData>          g_MeshData           = srrhi::DeferredLightingInputs::GetMeshData();
static const StructuredBuffer<uint>                     g_Indices            = srrhi::DeferredLightingInputs::GetIndices();
static const StructuredBuffer<srrhi::GPULight>          g_Lights             = srrhi::DeferredLightingInputs::GetLights();
static const Texture2D<float4>                          g_RTXDIDIComposited  = srrhi::DeferredLightingInputs::GetRTXDIDIComposited();

float4 DeferredLighting_PSMain(FullScreenVertexOut input) : SV_Target
{
    uint2 uvInt = uint2(input.pos.xy);
    float2 uv = input.uv;
    
    float depth = g_Depth.Load(uint3(uvInt, 0));
    
    // Position/Ray reconstruction
    float4 clipPos;
    clipPos.x = uv.x * 2.0f - 1.0f;
    clipPos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    clipPos.z = depth;
    clipPos.w = 1.0f;

    float4 worldPosFour = MatrixMultiply(clipPos, g_Deferred.m_View.m_MatClipToWorld);
    float3 worldPos = worldPosFour.xyz / worldPosFour.w;
    float3 V = normalize(g_Deferred.m_CameraPos.xyz - worldPos);

    float4 albedoAlpha = g_GBufferAlbedo.Load(uint3(uvInt, 0));
    float3 baseColor = albedoAlpha.rgb;
    float alpha = albedoAlpha.a;

    float3 N = DecodeNormal(g_GBufferNormals.Load(uint3(uvInt, 0)));
    float2 orm = g_GBufferORM.Load(uint3(uvInt, 0));
    float roughness = orm.r;
    float metallic = orm.g;

    float3 emissive = g_GBufferEmissive.Load(uint3(uvInt, 0)).rgb;

    LightingInputs lightingInputs;
    lightingInputs.N = N;
    lightingInputs.V = V;
    lightingInputs.L = float3(0, 1, 0); // Placeholder, updated in loop
    lightingInputs.baseColor = baseColor;
    lightingInputs.roughness = roughness;
    lightingInputs.metallic = metallic;
    lightingInputs.ior = 1.5f; // Default IOR for opaque
    lightingInputs.worldPos = worldPos;
    lightingInputs.radianceMipCount = g_Deferred.m_RadianceMipCount;
    lightingInputs.enableRTShadows = g_Deferred.m_EnableRTShadows != 0;
    lightingInputs.sceneAS = g_SceneAS;
    lightingInputs.instances = g_Instances;
    lightingInputs.meshData = g_MeshData;
    lightingInputs.materials = g_Materials;
    lightingInputs.indices = g_Indices;
    lightingInputs.vertices = g_Vertices;
    lightingInputs.lights = g_Lights;
    lightingInputs.sunRadiance = 0;
    lightingInputs.sunDirection = g_Deferred.m_SunDirection;
    lightingInputs.useSunRadiance = false;
    lightingInputs.sunShadow = 1.0f;

    float3 color = 0;
    if (g_Deferred.m_RenderingMode == srrhi::CommonConsts::RENDERING_MODE_IBL)
    {
        lightingInputs.L = g_Deferred.m_SunDirection;
        PrepareLightingByproducts(lightingInputs);
        IBLComponents iblRes = ComputeIBL(lightingInputs);
        color = iblRes.ibl + emissive;
    }
    else
    {
        if (g_Deferred.m_UseReSTIRDI != 0)
        {
            // CompositingPass already remodulated DI by albedo and added emissive
            color = g_RTXDIDIComposited.Load(uint3(uvInt, 0)).rgb;
        }
        else
        {
            float3 p_atmo = GetAtmospherePos(worldPos);

            if (g_Deferred.m_EnableSky)
            {
                // Use solar_irradiance * transmittance as the direct sun radiance at surface
                lightingInputs.sunRadiance = GetAtmosphereSunRadiance(p_atmo, g_Deferred.m_SunDirection, g_Lights[0].m_Intensity);
                lightingInputs.sunShadow = CalculateRTShadow(lightingInputs, lightingInputs.sunDirection, 1e10f);
                lightingInputs.useSunRadiance = true;
            }

            LightingComponents directLighting = AccumulateDirectLighting(lightingInputs, g_Deferred.m_LightCount);
            color = directLighting.diffuse + directLighting.specular;
            color += emissive;
        }
    }

    // Debug visualizations
    if (g_Deferred.m_DebugMode != srrhi::CommonConsts::DEBUG_MODE_NONE)
    {
        if (g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_WORLD_NORMALS)
            color = N * 0.5f + 0.5f;
        else if (g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_ALBEDO)
            color = baseColor;
        else if (g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_ROUGHNESS)
            color = roughness.xxx;
        else if (g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_METALLIC)
            color = metallic.xxx;
        else if (g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_EMISSIVE)
            color = emissive;
        else if (g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_MOTION_VECTORS)
            color = float3(abs(g_GBufferMotion.Load(uint3(uvInt, 0)).xy), 0.0f);

        if (g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_INSTANCES ||
            g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_MESHLETS ||
            g_Deferred.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_LOD)
            {
                color = baseColor;
            }
    }

    return float4(color, alpha);
}
