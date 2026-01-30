#define GPU_CULLING_DEFINE
#include "ShaderShared.h"
#include "Culling.h"

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
StructuredBuffer<MeshData> g_MeshData : register(t2);
RWStructuredBuffer<DrawIndexedIndirectArguments> g_VisibleArgs : register(u0);
RWStructuredBuffer<uint> g_VisibleCount : register(u1);
RWStructuredBuffer<uint> g_OccludedIndices : register(u2);
RWStructuredBuffer<uint> g_OccludedCount : register(u3);
RWStructuredBuffer<DispatchIndirectArguments> g_DispatchIndirectArgs : register(u4);
RWStructuredBuffer<MeshletJob> g_MeshletJobs : register(u5);
RWStructuredBuffer<uint> g_MeshletJobCount : register(u6);
RWStructuredBuffer<DispatchIndirectArguments> g_MeshletIndirectArgs : register(u7);
SamplerState g_MinReductionSampler : register(s0);

[numthreads(kThreadsPerGroup, 1, 1)]
void Culling_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint instanceIndex = dispatchThreadId.x;
	if (instanceIndex >= g_Culling.m_NumPrimitives)
		return;

	uint actualInstanceIndex = instanceIndex + g_Culling.m_InstanceBaseIndex;
	if (g_Culling.m_Phase == 1)
	{
		// Phase 2: Only process instances that were occluded in Phase 1
		if (instanceIndex >= g_OccludedCount[0])
			return;
		actualInstanceIndex = g_OccludedIndices[instanceIndex];
	}

	PerInstanceData inst = g_InstanceData[actualInstanceIndex];
    MeshData mesh = g_MeshData[inst.m_MeshDataIndex];

    float3 sphereViewCenter = mul(float4(inst.m_Center, 1.0), g_Culling.m_View).xyz;

    bool isVisible = true;

    // Frustum culling
    if (g_Culling.m_EnableFrustumCulling)
    {
        isVisible &= FrustumSphereTest(sphereViewCenter, inst.m_Radius, g_Culling.m_FrustumPlanes);
    }

    // Occlusion culling
    if (g_Culling.m_EnableOcclusionCulling)
    {
        isVisible &= OcclusionSphereTest(sphereViewCenter, inst.m_Radius, uint2(g_Culling.m_HZBWidth, g_Culling.m_HZBHeight), g_Culling.m_P00, g_Culling.m_P11, g_HZB, g_MinReductionSampler);
    }

    // Phase 1: Store visible instances for rendering occluded indices for Phase 2
    // Phase 2: Store visible instances for rendering newly tested visible instances against Phase 1 HZB
    if (isVisible)
    {
        uint lodIndex = 0;
        if (g_Culling.m_ForcedLOD != -1)
        {
            lodIndex = (uint)clamp(g_Culling.m_ForcedLOD, 0, (int)mesh.m_LODCount - 1);
        }
        else
        {
            float d = max(length(sphereViewCenter), 1e-5);
            float targetPixelError = 2.0f;
            float worldScale = GetMaxScale(inst.m_World);

            for (uint i = 0; i < mesh.m_LODCount; ++i)
            {
                float error = mesh.m_LODErrors[i] * worldScale;
                float projectedError = error * g_Culling.m_P11 * (float)g_Culling.m_HZBHeight / d;
                if (projectedError <= targetPixelError)
                {
                    lodIndex = i;
                }
            }
        }

        if (g_Culling.m_UseMeshletRendering)
        {
            uint visibleIndex;
            InterlockedAdd(g_MeshletJobCount[0], 1, visibleIndex);

            DispatchIndirectArguments args;
            args.m_ThreadGroupCountX = DivideAndRoundUp(mesh.m_MeshletCounts[lodIndex], kThreadsPerGroup);
            args.m_ThreadGroupCountY = 1;
            args.m_ThreadGroupCountZ = 1;
            g_MeshletIndirectArgs[visibleIndex] = args;

            MeshletJob job;
            job.m_InstanceIndex = actualInstanceIndex;
            job.m_LODIndex = lodIndex;
            g_MeshletJobs[visibleIndex] = job;
        }
        else
        {
            uint visibleIndex;
            InterlockedAdd(g_VisibleCount[0], 1, visibleIndex);

            DrawIndexedIndirectArguments args;
            args.m_IndexCount = mesh.m_IndexCounts[lodIndex];
            args.m_InstanceCount = 1;
            args.m_StartIndexLocation = mesh.m_IndexOffsets[lodIndex];
            args.m_BaseVertexLocation = 0;
            args.m_StartInstanceLocation = actualInstanceIndex;

            g_VisibleArgs[visibleIndex] = args;
        }
    }

    if (g_Culling.m_Phase == 0)
    {
        // Phase 1: Store visible instances for rendering occluded indices for Phase 2
        if (!isVisible)
        {
            uint occludedIndex;
            InterlockedAdd(g_OccludedCount[0], 1, occludedIndex);
            g_OccludedIndices[occludedIndex] = actualInstanceIndex;
        }
    }
}

[numthreads(1, 1, 1)]
void BuildIndirect_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    g_DispatchIndirectArgs[0].m_ThreadGroupCountX = DivideAndRoundUp(g_OccludedCount[0], kThreadsPerGroup);
    g_DispatchIndirectArgs[0].m_ThreadGroupCountY = 1;
    g_DispatchIndirectArgs[0].m_ThreadGroupCountZ = 1;
}
