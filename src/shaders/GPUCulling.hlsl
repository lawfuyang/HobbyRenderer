#define GPU_CULLING_DEFINE
#include "ShaderShared.h"

/*
	-- 2 Phase Occlusion Culling --

	Works under the assumption that it's likely that objects visible in the previous frame, will be visible this frame.

	In Phase 1, we render all objects that were visible last frame by testing against the previous HZB.
	Occluded objects are stored in a list, to be processed later.
	The HZB is constructed from the current result.
	Phase 2 tests all previously occluded objects against the new HZB and renders unoccluded.
	The HZB is constructed again from this result to be used in the next frame.

	https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
*/

cbuffer CullingCB : register(b0)
{
    CullingConstants g_Culling;
};

StructuredBuffer<PerInstanceData> g_InstanceData : register(t0);
Texture2D<float> g_HZB : register(t1);
RWStructuredBuffer<DrawIndexedIndirectArguments> g_VisibleArgs : register(u0);
RWStructuredBuffer<uint> g_VisibleCount : register(u1);
RWStructuredBuffer<uint> g_OccludedIndices : register(u2);
RWStructuredBuffer<uint> g_OccludedCount : register(u3);
SamplerState g_MinReductionSampler : register(s0);

bool FrustumAABBTest(float3 min, float3 max, float4 planes[5], float4x4 view)
{
    // Compute all 8 corners of the AABB in world space
    float3 corners[8];
    corners[0] = float3(min.x, min.y, min.z);
    corners[1] = float3(max.x, min.y, min.z);
    corners[2] = float3(min.x, max.y, min.z);
    corners[3] = float3(max.x, max.y, min.z);
    corners[4] = float3(min.x, min.y, max.z);
    corners[5] = float3(max.x, min.y, max.z);
    corners[6] = float3(min.x, max.y, max.z);
    corners[7] = float3(max.x, max.y, max.z);

    // Transform to view space and find AABB in view space
    float3 viewMin = float3(1e30, 1e30, 1e30);
    float3 viewMax = float3(-1e30, -1e30, -1e30);
    for (int i = 0; i < 8; i++)
    {
        float4 worldPos = float4(corners[i], 1.0);
        float3 viewPos = mul(worldPos, view).xyz;
        viewMin.x = viewPos.x < viewMin.x ? viewPos.x : viewMin.x;
        viewMin.y = viewPos.y < viewMin.y ? viewPos.y : viewMin.y;
        viewMin.z = viewPos.z < viewMin.z ? viewPos.z : viewMin.z;
        viewMax.x = viewPos.x > viewMax.x ? viewPos.x : viewMax.x;
        viewMax.y = viewPos.y > viewMax.y ? viewPos.y : viewMax.y;
        viewMax.z = viewPos.z > viewMax.z ? viewPos.z : viewMax.z;
    }

    // Check against view-space frustum planes
    for (int i = 0; i < 5; i++)
    {
        float3 n = planes[i].xyz;
        float d = planes[i].w;
        // Find the p-vertex (farthest in the negative normal direction)
        float3 p = float3(
            n.x > 0 ? viewMax.x : viewMin.x,
            n.y > 0 ? viewMax.y : viewMin.y,
            n.z > 0 ? viewMax.z : viewMin.z
        );
        float dist = dot(n, p) + d;
        if (dist < 0)
            return false; // AABB is outside this plane
    }
    return true;
}

float Min8(float a, float b, float c, float d, float e, float f, float g, float h)
{
    return min(min(min(a, b), min(c, d)), min(min(e, f), min(g, h)));
}

float Max8(float a, float b, float c, float d, float e, float f, float g, float h)
{
    return max(max(max(a, b), max(c, d)), max(max(e, f), max(g, h)));
}

float2 Min8(float2 a, float2 b, float2 c, float2 d, float2 e, float2 f, float2 g, float2 h)
{
    return min(min(min(a, b), min(c, d)), min(min(e, f), min(g, h)));
}

float2 Max8(float2 a, float2 b, float2 c, float2 d, float2 e, float2 f, float2 g, float2 h)
{
    return max(max(max(a, b), max(c, d)), max(max(e, f), max(g, h)));
}

 // https://zeux.io/2023/01/12/approximate-projected-bounds/
void ProjectBox(float3 bmin, float3 bmax, float4x4 viewProj, out float4 aabb, out float nearZ)
{
    nearZ = 0.0f;
    
    float4 SX = mul(float4(bmax.x - bmin.x, 0.0, 0.0, 0.0), viewProj);
    float4 SY = mul(float4(0.0, bmax.y - bmin.y, 0.0, 0.0), viewProj);
    float4 SZ = mul(float4(0.0, 0.0, bmax.z - bmin.z, 0.0), viewProj);

    float4 P0 = mul(float4(bmin.x, bmin.y, bmin.z, 1.0), viewProj);
    float4 P1 = P0 + SZ;
    float4 P2 = P0 + SY;
    float4 P3 = P2 + SZ;
    float4 P4 = P0 + SX;
    float4 P5 = P4 + SZ;
    float4 P6 = P4 + SY;
    float4 P7 = P6 + SZ;

    aabb.xy = Min8(
        P0.xy / P0.w, P1.xy / P1.w, P2.xy / P2.w, P3.xy / P3.w,
        P4.xy / P4.w, P5.xy / P5.w, P6.xy / P6.w, P7.xy / P7.w);
    aabb.zw = Max8(
        P0.xy / P0.w, P1.xy / P1.w, P2.xy / P2.w, P3.xy / P3.w,
        P4.xy / P4.w, P5.xy / P5.w, P6.xy / P6.w, P7.xy / P7.w);

    // clip space -> uv space
    aabb = aabb.xwzy * float4(0.5f, -0.5f, 0.5f, -0.5f) + float4(0.5f, 0.5f, 0.5f, 0.5f);

    nearZ = Max8(P0.z / P0.w, P1.z / P1.w, P2.z / P2.w, P3.z / P3.w, P4.z / P4.w, P5.z / P5.w, P6.z / P6.w, P7.z / P7.w);
}

bool OcclusionAABBTest(float3 aabbMin, float3 aabbMax, float4x4 viewProj, uint2 HZBDims)
{
    // Project AABB to UV space
    float4 screenAABB;
    float nearZ;
    ProjectBox(aabbMin, aabbMax, viewProj, screenAABB, nearZ);

    screenAABB = screenAABB * float4(HZBDims.x, HZBDims.y, HZBDims.x, HZBDims.y);

    float minX = screenAABB.x;
    float minY = screenAABB.y;
    float maxX = screenAABB.z;
    float maxY = screenAABB.w;

    // Compute appropriate mip level based on screen-space AABB size
    float aabbWidth = maxX - minX;
    float aabbHeight = maxY - minY;
    float maxDim = max(aabbWidth, aabbHeight);
    float mipLevel = floor(log2(max(maxDim, 1.0f)));

    // Sample HZB at center using max reduction sampler
    float centerX = (minX + maxX) * 0.5f;
    float centerY = 1.0f - (minY + maxY) * 0.5f;
    float hzbDepth = g_HZB.SampleLevel(g_MinReductionSampler, float2(centerX / HZBDims.x, centerY / HZBDims.y), mipLevel);

    // If the closest point of our AABB is behind the farthest point in HZB, it's occluded (note reversed depth logic)
    return nearZ >= hzbDepth;
}

[numthreads(64, 1, 1)]
void Culling_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint instanceIndex = dispatchThreadId.x;
	if (instanceIndex >= g_Culling.m_NumPrimitives)
		return;

	uint actualInstanceIndex = instanceIndex;
	if (g_Culling.m_Phase == 1)
	{
		// Phase 2: Only process instances that were occluded in Phase 1
		if (instanceIndex >= g_OccludedCount[0])
			return;
		actualInstanceIndex = g_OccludedIndices[instanceIndex];
	}

	PerInstanceData inst = g_InstanceData[actualInstanceIndex];

    // Frustum culling
    if (g_Culling.m_EnableFrustumCulling && !FrustumAABBTest(inst.m_Min, inst.m_Max, g_Culling.m_FrustumPlanes, g_Culling.m_View))
        return;

    // Occlusion culling
    bool isVisible = true;
    if (g_Culling.m_EnableOcclusionCulling)
    {
        isVisible = OcclusionAABBTest(inst.m_Min, inst.m_Max, g_Culling.m_ViewProj, uint2(g_Culling.m_HZBWidth, g_Culling.m_HZBHeight));
    }

    if (g_Culling.m_Phase == 0)
    {
        // Phase 1: Store visible instances for rendering, occluded indices for Phase 2
        if (isVisible)
        {
            uint visibleIndex;
            InterlockedAdd(g_VisibleCount[0], 1, visibleIndex);

            DrawIndexedIndirectArguments args;
            args.m_IndexCount = inst.m_IndexCount;
            args.m_InstanceCount = 1;
            args.m_StartIndexLocation = inst.m_IndexOffset;
            args.m_BaseVertexLocation = 0;
            args.m_StartInstanceLocation = actualInstanceIndex;

            g_VisibleArgs[visibleIndex] = args;
        }
        else
        {
            uint occludedIndex;
            InterlockedAdd(g_OccludedCount[0], 1, occludedIndex);
            g_OccludedIndices[occludedIndex] = actualInstanceIndex;
        }
    }
    else
    {
        // Phase 2: Only process instances that were occluded in Phase 1
        if (isVisible)
        {
            uint visibleIndex;
            InterlockedAdd(g_VisibleCount[0], 1, visibleIndex);

            DrawIndexedIndirectArguments args;
            args.m_IndexCount = inst.m_IndexCount;
            args.m_InstanceCount = 1;
            args.m_StartIndexLocation = inst.m_IndexOffset;
            args.m_BaseVertexLocation = 0;
            args.m_StartInstanceLocation = actualInstanceIndex;

            g_VisibleArgs[visibleIndex] = args;
        }
    }
}
