#include "Common.hlsli"
#include "Culling.hlsli"

#include "srrhi/hlsl/Cluster.hlsli"

/*
	Cluster LOD traversal (docs/cluster-lod-implementation-plan.md §3.3), phase 1: build the visible-cluster
	list and count it, without drawing from it yet.

	Init       one thread per instance of the bucket: instance sphere test, then seed the node queue with the
	           primitive's root node.
	Run        one dispatch per tree depth: interior nodes test their children's bounds and queue the survivors,
	           leaf nodes queue their group.
	Groups     one thread per queued group: the cut rule, then every cluster of a kept group is appended to the
	           visible-cluster list.
	BuildArgs  the mesh dispatch grid, taken from the visible-cluster count.

	The cut rule is ClusterLod::EvaluateCut, verbatim: a group's clusters are drawn when the group itself is
	detailed enough for this camera and the coarser group that replaced it is not. The node tree is only used
	to reject whole subtrees spatially, so the two produce the same set of clusters.
*/

static const srrhi::ClusterTraversalConstants g_Traversal = srrhi::ClusterTraversalInputs::GetTraversalCB();

static const StructuredBuffer<srrhi::PerInstanceData> g_InstanceData = srrhi::ClusterTraversalInputs::GetInstanceData();
static const StructuredBuffer<srrhi::ClusterGeometry> g_Geometries   = srrhi::ClusterTraversalInputs::GetGeometry();
static const StructuredBuffer<srrhi::ClusterNode>     g_Nodes        = srrhi::ClusterTraversalInputs::GetNodes();
static const StructuredBuffer<srrhi::ClusterBounds>   g_NodeBounds   = srrhi::ClusterTraversalInputs::GetNodeBounds();
static const StructuredBuffer<srrhi::ClusterGroup>    g_Groups       = srrhi::ClusterTraversalInputs::GetGroups();
static const StructuredBuffer<uint>                   g_GeneratingGroups = srrhi::ClusterTraversalInputs::GetGeneratingGroups();
static const Texture2D<float>                         g_HZB          = srrhi::ClusterTraversalInputs::GetHZB();

static RWStructuredBuffer<srrhi::ClusterNodeRef>            g_NodeQueueIn       = srrhi::ClusterTraversalInputs::GetNodeQueueIn();
static RWStructuredBuffer<srrhi::ClusterNodeRef>            g_NodeQueueOut      = srrhi::ClusterTraversalInputs::GetNodeQueueOut();
static RWStructuredBuffer<uint>                             g_NodeQueueCountIn  = srrhi::ClusterTraversalInputs::GetNodeQueueCountIn();
static RWStructuredBuffer<uint>                             g_NodeQueueCountOut = srrhi::ClusterTraversalInputs::GetNodeQueueCountOut();
static RWStructuredBuffer<srrhi::ClusterGroupRef>           g_GroupQueue        = srrhi::ClusterTraversalInputs::GetGroupQueue();
static RWStructuredBuffer<uint>                             g_GroupQueueCount   = srrhi::ClusterTraversalInputs::GetGroupQueueCount();
static RWStructuredBuffer<srrhi::ClusterJob>                g_ClusterJobs       = srrhi::ClusterTraversalInputs::GetClusterJobs();
static RWStructuredBuffer<uint>                             g_ClusterJobCount   = srrhi::ClusterTraversalInputs::GetClusterJobCount();
static RWStructuredBuffer<srrhi::DispatchIndirectArguments> g_ClusterIndirectArgs = srrhi::ClusterTraversalInputs::GetClusterIndirectArgs();
static RWStructuredBuffer<srrhi::ClusterCounters>           g_Counters          = srrhi::ClusterTraversalInputs::GetCounters();

static const uint kNoGroup = 0xFFFFFFFFu;
static const uint kNoForcedLOD = 0xFFFFFFFFu;

// ClusterLod::Node, whose two halves are packed into one uint because neither HLSL nor srrhi has bitfields.
bool NodeIsLeaf(uint packed)
{
	return (packed & 1u) != 0;
}

uint NodeGetChildOffset(uint packed)
{
	return packed >> 6;
}

uint NodeGetChildCount(uint packed)
{
	return ((packed >> 1) & 0x1Fu) + 1u;
}

uint NodeGetGroupIndex(uint packed)
{
	return (packed >> 1) & 0x7FFFFFu;
}

// A box's bounding sphere: conservative, and it lets the node and group tests share the sphere tests that the
// instance culling pass already uses.
void GetBoundsSphere(srrhi::ClusterBounds bounds, out float3 centerLocal, out float radiusLocal)
{
	centerLocal = (bounds.m_Min + bounds.m_Max) * 0.5f;
	radiusLocal = length(bounds.m_Max - bounds.m_Min) * 0.5f;
}

bool SphereVisible(float3 centerLocal, float radiusLocal, float4x4 world)
{
	if (g_Traversal.m_DisableCulling)
		return true;

	const float worldScale = GetMaxScale(world);
	const float3 centerView = MatrixMultiply(MatrixMultiply(float4(centerLocal, 1.0f), world), g_Traversal.m_View).xyz;
	const float radiusView = radiusLocal * worldScale;

	bool bVisible = true;
	if (g_Traversal.m_EnableFrustumCulling)
	{
		bVisible = FrustumSphereTest(centerView, radiusView, g_Traversal.m_FrustumPlanes);
	}

	if (bVisible && g_Traversal.m_EnableOcclusionCulling)
	{
		const SamplerState minSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_MIN_REDUCTION_INDEX];
		bVisible = OcclusionSphereTest(centerView, radiusView, uint2(g_Traversal.m_HZBWidth, g_Traversal.m_HZBHeight),
			g_Traversal.m_P00, g_Traversal.m_P11, g_HZB, minSampler);
	}

	return bVisible;
}

// ClusterLod::GetGroupPixelError: the quadric error of the group, projected to pixels at this camera.
bool GroupTooCoarse(uint groupIndex, float4x4 world)
{
	const srrhi::ClusterGroup group = g_Groups[groupIndex];

	const float worldScale = GetMaxScale(world);
	const float3 centerWorld = MatrixMultiply(float4(group.m_Metric.m_Sphere.xyz, 1.0f), world).xyz;
	const float radiusWorld = group.m_Metric.m_Sphere.w * worldScale;

	const float distance = length(centerWorld - g_Traversal.m_CameraPosition.xyz) - radiusWorld;
	const float pixelError = group.m_Metric.m_MaxQuadricError * worldScale * g_Traversal.m_FocalPixels / max(distance, 1e-4f);

	return pixelError >= g_Traversal.m_PixelErrorThreshold;
}

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ClusterTraversalInit_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= g_Traversal.m_NumInstances)
		return;

	const uint instanceIndex = g_Traversal.m_InstanceBaseIndex + dispatchThreadId.x;
	const srrhi::PerInstanceData instance = g_InstanceData[instanceIndex];
	const srrhi::ClusterGeometry geometry = g_Geometries[instance.m_MeshDataIndex];
	if (geometry.m_LODLevelCount == 0)
		return;

	if (!SphereVisible(instance.m_Center, instance.m_Radius, instance.m_World))
		return;

	InterlockedAdd(g_Counters[0].m_InstanceCount, 1);

	uint slot;
	InterlockedAdd(g_NodeQueueCountOut[0], 1, slot);

	srrhi::ClusterNodeRef nodeRef;
	nodeRef.m_InstanceIndex = instanceIndex;
	nodeRef.m_NodeIndex = geometry.m_NodeOffset;   // node 0 of the primitive is its root
	g_NodeQueueOut[slot] = nodeRef;
}

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ClusterTraversalRun_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint itemIndex = dispatchThreadId.x;
	if (itemIndex >= g_NodeQueueCountIn[0])
		return;

	const srrhi::ClusterNodeRef nodeRef = g_NodeQueueIn[itemIndex];
	const srrhi::PerInstanceData instance = g_InstanceData[nodeRef.m_InstanceIndex];
	const uint packed = g_Nodes[nodeRef.m_NodeIndex].m_Packed;

	InterlockedAdd(g_Counters[0].m_NodeCount, 1);

	if (!NodeIsLeaf(packed))
	{
		const uint childOffset = NodeGetChildOffset(packed);
		const uint childCount = NodeGetChildCount(packed);

		for (uint i = 0; i < childCount; ++i)
		{
			const uint childIndex = childOffset + i;

			float3 centerLocal;
			float radiusLocal;
			GetBoundsSphere(g_NodeBounds[childIndex], centerLocal, radiusLocal);

			if (!SphereVisible(centerLocal, radiusLocal, instance.m_World))
				continue;

			uint slot;
			InterlockedAdd(g_NodeQueueCountOut[0], 1, slot);

			srrhi::ClusterNodeRef childRef;
			childRef.m_InstanceIndex = nodeRef.m_InstanceIndex;
			childRef.m_NodeIndex = childIndex;
			g_NodeQueueOut[slot] = childRef;
		}

		return;
	}

	uint slot;
	InterlockedAdd(g_GroupQueueCount[0], 1, slot);

	srrhi::ClusterGroupRef groupRef;
	groupRef.m_InstanceIndex = nodeRef.m_InstanceIndex;
	groupRef.m_GroupIndex = NodeGetGroupIndex(packed);
	g_GroupQueue[slot] = groupRef;

	InterlockedAdd(g_Counters[0].m_GroupCount, 1);
}

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ClusterTraversalGroups_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint itemIndex = dispatchThreadId.x;
	if (itemIndex >= g_GroupQueueCount[0])
		return;

	const srrhi::ClusterGroupRef groupRef = g_GroupQueue[itemIndex];
	const srrhi::ClusterGroup group = g_Groups[groupRef.m_GroupIndex];
	const srrhi::PerInstanceData instance = g_InstanceData[groupRef.m_InstanceIndex];

	// Forced levels clamp to the primitive's own coarsest level, which is what ClusterLod::EvaluateCut does: a
	// validation sweep must not silently draw nothing for a primitive that has fewer levels than the forced one.
	const srrhi::ClusterGeometry geometry = g_Geometries[instance.m_MeshDataIndex];
	const bool bForcedLevel = g_Traversal.m_ForcedLOD != kNoForcedLOD;
	const uint forcedLevel = min(g_Traversal.m_ForcedLOD, geometry.m_LODLevelCount - 1);

	if (bForcedLevel)
	{
		if (group.m_LODLevel != forcedLevel)
			return;
	}
	else if (GroupTooCoarse(groupRef.m_GroupIndex, instance.m_World))
	{
		return;
	}

	float3 centerLocal;
	float radiusLocal;
	GetBoundsSphere(group.m_Bounds, centerLocal, radiusLocal);
	if (!SphereVisible(centerLocal, radiusLocal, instance.m_World))
		return;

	uint firstJob;
	InterlockedAdd(g_ClusterJobCount[0], group.m_ClusterCount, firstJob);

	for (uint i = 0; i < group.m_ClusterCount; ++i)
	{
		const uint clusterIndex = group.m_ClusterOffset + i;

		// A coarser group that replaced this cluster covers its region already when that group is detailed
		// enough for this camera. This is per cluster, not per group: one group holds clusters that were
		// replaced by different coarser groups.
		if (!bForcedLevel)
		{
			const uint generating = g_GeneratingGroups[clusterIndex];
			if (generating != kNoGroup && !GroupTooCoarse(generating, instance.m_World))
				continue;
		}

		const uint jobIndex = firstJob + i;
		if (g_Traversal.m_MaxClusters != 0 && jobIndex >= g_Traversal.m_MaxClusters)
		{
			InterlockedAdd(g_Counters[0].m_ClusterCapped, 1);
			continue;
		}

		srrhi::ClusterJob job;
		job.m_InstanceIndex = groupRef.m_InstanceIndex;
		job.m_ClusterIndex = clusterIndex;
		g_ClusterJobs[jobIndex] = job;

		InterlockedAdd(g_Counters[0].m_ClusterCount, 1);
	}
}

[numthreads(1, 1, 1)]
void ClusterBuildArgs_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	// One thread group per visible cluster: the mesh shader's group size is one cluster.
	uint clusterCount = g_ClusterJobCount[0];
	if (g_Traversal.m_MaxClusters != 0)
		clusterCount = min(clusterCount, g_Traversal.m_MaxClusters);

	g_ClusterIndirectArgs[0].m_ThreadGroupCountX = clusterCount;
	g_ClusterIndirectArgs[0].m_ThreadGroupCountY = 1;
	g_ClusterIndirectArgs[0].m_ThreadGroupCountZ = 1;
}
