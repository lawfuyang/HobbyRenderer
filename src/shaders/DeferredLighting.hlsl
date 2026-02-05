#include "ShaderShared.h"
#include "CommonLighting.hlsli"
#include "Bindless.hlsli"

cbuffer DeferredCB : register(b1, space1)
{
    DeferredLightingConstants g_Deferred;
};

// These are bound to space 1 because we use the global bindless descriptor table in space 0.
Texture2D<float4> g_GBufferAlbedo    : register(t0, space1);
Texture2D<float2> g_GBufferNormals   : register(t1, space1);
Texture2D<float4> g_GBufferORM       : register(t2, space1);
Texture2D<float4> g_GBufferEmissive  : register(t3, space1);
Texture2D<float>  g_Depth            : register(t4, space1);
RaytracingAccelerationStructure g_SceneAS : register(t5, space1);
StructuredBuffer<PerInstanceData> g_Instances : register(t10, space1);
StructuredBuffer<MaterialConstants> g_Materials : register(t11, space1);
StructuredBuffer<VertexQuantized> g_Vertices : register(t12, space1);
StructuredBuffer<MeshData> g_MeshData : register(t13, space1);
StructuredBuffer<uint> g_Indices : register(t14, space1);

struct FullScreenVertexOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

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
        ray.TMin = 0.1f;
        ray.TMax = 1e10f;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
        
        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                uint instanceIndex = q.CandidateInstanceIndex();
                uint primitiveIndex = q.CandidatePrimitiveIndex();
                float2 bary = q.CandidateTriangleBarycentrics();

                PerInstanceData inst = g_Instances[instanceIndex];
                MeshData mesh = g_MeshData[inst.m_MeshDataIndex];
                MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

                if (mat.m_AlphaMode == ALPHA_MODE_MASK)
                {
                    uint baseIndex = mesh.m_IndexOffsets[0];
                    uint i0 = g_Indices[baseIndex + 3 * primitiveIndex + 0];
                    uint i1 = g_Indices[baseIndex + 3 * primitiveIndex + 1];
                    uint i2 = g_Indices[baseIndex + 3 * primitiveIndex + 2];

                    float2 uv0 = UnpackVertex(g_Vertices[i0]).m_Uv;
                    float2 uv1 = UnpackVertex(g_Vertices[i1]).m_Uv;
                    float2 uv2 = UnpackVertex(g_Vertices[i2]).m_Uv;

                    float2 uv = uv0 * (1.0f - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
                    
                    bool hasAlbedo = (mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0;
                    float4 albedoSample = hasAlbedo 
                        ? SampleBindlessTextureLevel(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uv, 0)
                        : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);
                    
                    float alpha = hasAlbedo ? (albedoSample.w * mat.m_BaseColor.w) : mat.m_BaseColor.w;
                    
                    if (alpha >= mat.m_AlphaCutoff)
                    {
                        q.CommitNonOpaqueTriangleHit();
                    }
                }
                else if (mat.m_AlphaMode == ALPHA_MODE_OPAQUE)
                {
                    // This should not happen if TLAS/BLAS flags are correct, but handle it just in case
                    q.CommitNonOpaqueTriangleHit();
                }
                // ALPHA_MODE_BLEND: ignore as requested
            }
        }

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            shadow = 0.0f;
        }
    }

    float3 color = (diffuse + spec) * radiance * NdotL * shadow;
    color += emissive;

    // hack ambient until we have restir gi
    color += baseColor * 0.03f;

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

    return float4(color, alpha);
}
