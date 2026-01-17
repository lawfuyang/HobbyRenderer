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

[numthreads(64, 1, 1)]
void Culling_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint instanceIndex = dispatchThreadId.x;
	if (instanceIndex >= g_Culling.m_NumPrimitives)
		return;

	PerInstanceData inst = g_InstanceData[instanceIndex];

    if (g_Culling.m_EnableFrustumCulling && !FrustumAABBTest(inst.m_Min, inst.m_Max, g_Culling.m_FrustumPlanes, g_Culling.m_View))
        return;

	uint index;
	InterlockedAdd(g_VisibleCount[0], 1, index);

	DrawIndexedIndirectArguments args;
	args.m_IndexCount = inst.m_IndexCount;
	args.m_InstanceCount = 1;
	args.m_StartIndexLocation = inst.m_IndexOffset;
	args.m_BaseVertexLocation = 0;
	args.m_StartInstanceLocation = instanceIndex;

	g_VisibleArgs[index] = args;
}
