#include "ShaderShared.h"
#include "CommonLighting.hlsli"

cbuffer VolumetricSkyVisibilityCB : register(b0)
{
    VolumetricSkyVisibilityConstants g_Consts;
};

// UAVs
VK_IMAGE_FORMAT("r8")
RWTexture3D<float> g_OutputVisibility : register(u0);

// SRVs
Texture3D g_HistoryVisibility : register(t0);
Texture2D g_DepthBuffer       : register(t1);
RaytracingAccelerationStructure g_SceneAS : register(t2);

SamplerState g_LinearClamp : register(s0);
SamplerState g_PointClamp  : register(s1);

float InterleavedGradientNoise(float2 uv)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

float3 SampleStratifiedSkyVisibility(float2 u)
{
    if (u.x < 0.7f)
    {
        float2 u2 = float2(u.x / 0.7f, u.y);
        return SampleHemisphereCosine(u2, float3(0, 1, 0));
    }
    else
    {
        float2 u2 = float2((u.x - 0.7f) / 0.3f, u.y);
        return SampleHemisphereUniform(u2, float3(0, 1, 0));
    }
}

[numthreads(4, 4, 4)]
void VisibilityCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Consts.m_ResolutionX || 
        id.y >= g_Consts.m_ResolutionY || 
        id.z >= g_Consts.m_ResolutionZ)
        return;

    float viewSpaceZ;
    uint3 gridSize = uint3(g_Consts.m_ResolutionX, g_Consts.m_ResolutionY, g_Consts.m_ResolutionZ);
    float3 viewPos = ComputeCellViewSpacePosition(id, g_Consts.m_SkyVisibilityGridZParams, gridSize, g_Consts.m_InvDeviceZToWorldZTransform, g_Consts.m_View.m_MatClipToViewNoOffset, viewSpaceZ);
    float3 worldPos = MatrixMultiply(float4(viewPos, 1.0f), g_Consts.m_View.m_MatViewToWorld).xyz;
    
    float visibility = 0.0f;

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_Consts.m_ResolutionX, g_Consts.m_ResolutionY);
    float seed = 0;//InterleavedGradientNoise(uv * 1337.0f + g_Consts.m_FrameIndex * 1.618f);

    for (uint i = 0; i < g_Consts.m_RaysPerFroxel; ++i)
    {
        float2 u = float2(
            frac(seed + float(i) / float(g_Consts.m_RaysPerFroxel)), 
            frac(seed * 1.61803398875f + (float(i) + 0.5f) / float(g_Consts.m_RaysPerFroxel))
        );
        
        float3 rayDirWorld = SampleStratifiedSkyVisibility(u);

        bool hit = false;
        
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
        RayDesc ray;
        ray.Origin = worldPos;
        ray.Direction = rayDirWorld;
        ray.TMin = 0.0f;
        ray.TMax = 1e10f;
        q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            hit = true;
        }
        
        if (!hit)
        {
            visibility += 1.0f;
        }
    }

    g_OutputVisibility[id] = visibility / float(g_Consts.m_RaysPerFroxel);
}

[numthreads(4, 4, 4)]
void TemporalCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Consts.m_ResolutionX || 
        id.y >= g_Consts.m_ResolutionY || 
        id.z >= g_Consts.m_ResolutionZ)
        return;

    float currentVisibility = g_OutputVisibility[id].r;
    
    float2 uv = (float2(id.xy) + 0.5f) / float2(g_Consts.m_ResolutionX, g_Consts.m_ResolutionY);
    float viewDepth = ComputeDepthFromZSlice(float(id.z) + 0.5f, g_Consts.m_SkyVisibilityGridZParams);
    
    float4 clipPos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, 0.0f, 1.0f);
    float4 viewPosDir = MatrixMultiply(clipPos, g_Consts.m_View.m_MatClipToViewNoOffset);
    float3 viewDir = normalize(viewPosDir.xyz / viewPosDir.w);
    float3 viewPos = viewDir * (viewDepth / viewDir.z);
    float3 worldPos = MatrixMultiply(float4(viewPos, 1.0f), g_Consts.m_View.m_MatViewToWorld).xyz;

    float4 prevClipPos = MatrixMultiply(float4(worldPos, 1.0f), g_Consts.m_PrevView.m_MatWorldToClip);
    float3 prevNDC = prevClipPos.xyz / prevClipPos.w;
    float2 prevUV = prevNDC.xy * 0.5f + 0.5f;
    prevUV.y = 1.0f - prevUV.y;
    
    float4 prevViewPos = MatrixMultiply(float4(worldPos, 1.0f), g_Consts.m_PrevView.m_MatWorldToView);
    float prevViewDepth = prevViewPos.z;
    float prevFroxelZ = ComputeZSliceFromDepth(prevViewDepth, g_Consts.m_SkyVisibilityGridZParams);
    
    float visibility = currentVisibility;
    float lerpFactor = 0.5f;

    if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f) && prevFroxelZ >= 0.0f && prevFroxelZ < float(g_Consts.m_ResolutionZ))
    {
        float3 historyUVZ = float3(prevUV, (prevFroxelZ + 0.5f) / float(g_Consts.m_ResolutionZ));
        float historyVisibility = g_HistoryVisibility.SampleLevel(g_LinearClamp, historyUVZ, 0.0f).r;
        visibility = lerp(historyVisibility, currentVisibility, lerpFactor);
    }
    
    g_OutputVisibility[id] = visibility;
}
