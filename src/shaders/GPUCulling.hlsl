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
RWStructuredBuffer<DrawIndexedIndirectArguments> g_VisibleArgs : register(u0);
RWStructuredBuffer<uint> g_VisibleCount : register(u1);
RWStructuredBuffer<uint> g_OccludedIndices : register(u2);
RWStructuredBuffer<uint> g_OccludedCount : register(u3);

// HZB textures for occlusion culling
Texture2D<float> g_HZB : register(t1);
SamplerState g_HZBMaxSampler : register(s0);

bool FrustumAABBTest(Vector3 min, Vector3 max, Vector4 planes[5], Matrix view)
{
    // Compute all 8 corners of the AABB in world space
    Vector3 corners[8];
    corners[0] = Vector3(min.x, min.y, min.z);
    corners[1] = Vector3(max.x, min.y, min.z);
    corners[2] = Vector3(min.x, max.y, min.z);
    corners[3] = Vector3(max.x, max.y, min.z);
    corners[4] = Vector3(min.x, min.y, max.z);
    corners[5] = Vector3(max.x, min.y, max.z);
    corners[6] = Vector3(min.x, max.y, max.z);
    corners[7] = Vector3(max.x, max.y, max.z);

    // Transform to view space and find AABB in view space
    Vector3 viewMin = Vector3(1e30, 1e30, 1e30);
    Vector3 viewMax = Vector3(-1e30, -1e30, -1e30);
    for (int i = 0; i < 8; i++)
    {
        Vector4 worldPos = Vector4(corners[i], 1.0);
        Vector3 viewPos = mul(worldPos, view).xyz;
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
        Vector3 n = planes[i].xyz;
        float d = planes[i].w;
        // Find the p-vertex (farthest in the negative normal direction)
        Vector3 p = Vector3(
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

bool OcclusionAABBTest(Vector3 aabbMin, Vector3 aabbMax, Matrix viewProj)
{
    // Project AABB to screen space and test against HZB
    Vector3 corners[8];
    corners[0] = Vector3(aabbMin.x, aabbMin.y, aabbMin.z);
    corners[1] = Vector3(aabbMax.x, aabbMin.y, aabbMin.z);
    corners[2] = Vector3(aabbMin.x, aabbMax.y, aabbMin.z);
    corners[3] = Vector3(aabbMax.x, aabbMax.y, aabbMin.z);
    corners[4] = Vector3(aabbMin.x, aabbMin.y, aabbMax.z);
    corners[5] = Vector3(aabbMax.x, aabbMin.y, aabbMax.z);
    corners[6] = Vector3(aabbMin.x, aabbMax.y, aabbMax.z);
    corners[7] = Vector3(aabbMax.x, aabbMax.y, aabbMax.z);

    // Find screen-space AABB
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    float minDepth = 1e30f, maxDepth = -1e30f;

    for (int i = 0; i < 8; i++)
    {
        Vector4 clipPos = mul(Vector4(corners[i], 1.0f), viewProj);
        if (clipPos.w <= 0.0f) continue; // Behind camera

        Vector3 ndcPos = clipPos.xyz / clipPos.w;

        // Convert to screen space (0,0) to (width,height)
        float screenX = (ndcPos.x * 0.5f + 0.5f) * g_Culling.m_HZBWidth;
        float screenY = (1.0f - (ndcPos.y * 0.5f + 0.5f)) * g_Culling.m_HZBHeight; // Flip Y

        minX = min(minX, screenX);
        minY = min(minY, screenY);
        maxX = max(maxX, screenX);
        maxY = max(maxY, screenY);
        minDepth = min(minDepth, ndcPos.z);
        maxDepth = max(maxDepth, ndcPos.z);
    }

    if (maxX < 0 || minX >= g_Culling.m_HZBWidth || maxY < 0 || minY >= g_Culling.m_HZBHeight)
        return false; // Outside screen

    // Clamp to valid range
    minX = max(0, minX);
    minY = max(0, minY);
    maxX = min(g_Culling.m_HZBWidth - 1.0f, maxX);
    maxY = min(g_Culling.m_HZBHeight - 1.0f, maxY);

    // Compute appropriate mip level based on screen-space AABB size
    float aabbWidth = maxX - minX;
    float aabbHeight = maxY - minY;
    float maxDim = max(aabbWidth, aabbHeight);
    float mipLevel = max(0.0f, floor(log2(maxDim)));

    // Sample HZB at center using max reduction sampler
    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;
    float hzbDepth = g_HZB.SampleLevel(g_HZBMaxSampler, float2(centerX / g_Culling.m_HZBWidth, centerY / g_Culling.m_HZBHeight), mipLevel);

    // If the closest point of our AABB is behind the farthest point in HZB, it's occluded
    return minDepth <= hzbDepth;
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

    // Frustum culling (only in Phase 1)
    if (g_Culling.m_EnableFrustumCulling && !FrustumAABBTest(inst.m_Min, inst.m_Max, g_Culling.m_FrustumPlanes, g_Culling.m_View))
        return;

    // Occlusion culling
    bool isVisible = true;
    if (g_Culling.m_EnableOcclusionCulling)
    {
        isVisible = OcclusionAABBTest(inst.m_Min, inst.m_Max, g_Culling.m_ViewProj);
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
