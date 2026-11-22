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
static const StructuredBuffer<srrhi::ClusterRecord>   g_ClusterRecords = srrhi::ClusterTraversalInputs::GetClusterRecords();
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

static RWStructuredBuffer<srrhi::ClusterNodeRef>            g_RejectNodes      = srrhi::ClusterTraversalInputs::GetRejectNodes();
static RWStructuredBuffer<uint>                             g_RejectNodeCount  = srrhi::ClusterTraversalInputs::GetRejectNodeCount();
static RWStructuredBuffer<srrhi::ClusterGroupRef>           g_RejectGroups     = srrhi::ClusterTraversalInputs::GetRejectGroups();
static RWStructuredBuffer<uint>                             g_RejectGroupCount = srrhi::ClusterTraversalInputs::GetRejectGroupCount();
static RWStructuredBuffer<srrhi::ClusterJob>                g_ClusterJobsSecond     = srrhi::ClusterTraversalInputs::GetClusterJobsSecond();
static RWStructuredBuffer<uint>                             g_ClusterJobCountSecond = srrhi::ClusterTraversalInputs::GetClusterJobCountSecond();
static RWStructuredBuffer<uint>                             g_RejectClusterMasks    = srrhi::ClusterTraversalInputs::GetRejectClusterMasks();
static RWStructuredBuffer<srrhi::ClusterGroupRef>           g_Pass0Groups           = srrhi::ClusterTraversalInputs::GetPass0Groups();
static RWStructuredBuffer<uint>                             g_Pass0GroupCount       = srrhi::ClusterTraversalInputs::GetPass0GroupCount();

// Geometry streaming: which groups are in the arena, and where a missing one is asked for.
static RWStructuredBuffer<srrhi::ClusterGroupState>         g_GroupStates      = srrhi::ClusterTraversalInputs::GetGroupStates();
static RWStructuredBuffer<uint>                             g_LoadRequests     = srrhi::ClusterTraversalInputs::GetLoadRequests();
static RWStructuredBuffer<srrhi::ClusterStreamCounters>     g_StreamCounters   = srrhi::ClusterTraversalInputs::GetStreamCounters();

// The grid slots the traversal owns in ClusterIndirectArgs: the first pass' mesh dispatch, the second pass'
// INIT grid, its mesh dispatch, and the grid of the pass that re-tests the per-cluster rejections.
static const uint kIndirectSlotFirstPassMesh = 0;
static const uint kIndirectSlotSecondPassInit = 1;
static const uint kIndirectSlotSecondPassMesh = 2;
static const uint kIndirectSlotRejectClusters = 3;

static const uint kNoGroup = 0xFFFFFFFFu;
static const uint kNoForcedLOD = 0xFFFFFFFFu;

// A cluster is culled when its projected half-extent is under this many pixels on both axes. One pixel is the
// reference renderer's CULL_MIN_PIXEL_SIZE: nothing of the cluster can land on a sample below that.
static const float2 kMinClusterPixelSize = 1.0f;

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

// A sphere that is already in world space. PerInstanceData::m_Center/m_Radius are, which is why the instance
// culling pass tests them against the view matrix alone - transforming them again by the instance's own world
// matrix rejects every instance that is not sitting at the origin.
//
// The two tests are reported separately: a shape the frustum rejects can only be rejected again next pass, so
// only an occlusion rejection is worth recording in the reject lists.
void SphereTestsWorld(float3 centerWorld, float radiusWorld, out bool bFrustumVisible, out bool bOcclusionVisible)
{
	if (g_Traversal.m_DisableCulling)
	{
		bFrustumVisible = true;
		bOcclusionVisible = true;
		return;
	}

	const float3 centerView = MatrixMultiply(float4(centerWorld, 1.0f), g_Traversal.m_View).xyz;

	bFrustumVisible = g_Traversal.m_EnableFrustumCulling ?
		FrustumSphereTest(centerView, radiusWorld, g_Traversal.m_FrustumPlanes) : true;

	bOcclusionVisible = true;
	if (bFrustumVisible && g_Traversal.m_EnableOcclusionCulling)
	{
		const SamplerState minSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_MIN_REDUCTION_INDEX];
		bOcclusionVisible = OcclusionSphereTest(centerView, radiusWorld, uint2(g_Traversal.m_HZBWidth, g_Traversal.m_HZBHeight),
			g_Traversal.m_P00, g_Traversal.m_P11, g_HZB, minSampler);
	}
}

// A sphere in the primitive's own space (node and group bounds), which the instance's world matrix carries into
// world space first.
void SphereTests(float3 centerLocal, float radiusLocal, float4x4 world, out bool bFrustumVisible, out bool bOcclusionVisible)
{
	if (g_Traversal.m_DisableCulling)
	{
		bFrustumVisible = true;
		bOcclusionVisible = true;
		return;
	}

	const float worldScale = GetMaxScale(world);
	const float3 centerWorld = MatrixMultiply(float4(centerLocal, 1.0f), world).xyz;
	SphereTestsWorld(centerWorld, radiusLocal * worldScale, bFrustumVisible, bOcclusionVisible);
}

// Only the first pass records, and only when the second pass is going to read the lists: with the feature off
// this is exactly the phase-3 traversal, including what it costs.
bool RecordsRejects()
{
	return g_Traversal.m_CullPass == 0 && g_Traversal.m_EnableRejectLists != 0;
}

// True when the group's geometry is in the streaming arena. With streaming off every group is resident and this
// is always true.
bool GroupIsResident(uint groupIndex)
{
	return g_Traversal.m_StreamingEnabled == 0 ||
		g_GroupStates[groupIndex].m_Address < srrhi::ClusterStream::kInvalidAddressStart;
}

// Asks the host to stream a group in. The address table doubles as the request ledger: an unresident group's
// address carries the frame index of the last request, so the second instance to reach the same group in the
// same frame sees its own tag and stays quiet.
void RequestGroup(uint groupIndex)
{
	const uint tagged = srrhi::ClusterStream::kInvalidAddressStart |
		(g_Traversal.m_FrameIndex & srrhi::ClusterStream::kInvalidAddressMask);

	uint previousAddress;
	InterlockedMax(g_GroupStates[groupIndex].m_Address, tagged, previousAddress);
	if (previousAddress == tagged)
		return;

	uint slot;
	InterlockedAdd(g_StreamCounters[0].m_LoadCount, 1, slot);
	if (slot < g_Traversal.m_MaxLoads)
		g_LoadRequests[slot] = groupIndex;
}

// A cluster's own bounding sphere, in view space with the instance's scale folded in. The group's sphere is the
// conservative test that decides whether the group is worth walking at all; this one is tight enough to matter,
// because a group bounds several clusters and a sphere over ~700 triangles is almost never occluded.
void UnpackClusterSphere(uint clusterIndex, const srrhi::PerInstanceData instance, out float3 centerView, out float radiusWorld)
{
	const srrhi::ClusterRecord record = g_ClusterRecords[clusterIndex];
	const float3 centerLocal = float3(
		f16tof32(record.m_CenterRadius[0] & 0xFFFF),
		f16tof32(record.m_CenterRadius[0] >> 16),
		f16tof32(record.m_CenterRadius[1] & 0xFFFF));
	const float radiusLocal = f16tof32(record.m_CenterRadius[1] >> 16);

	const float3 centerWorld = MatrixMultiply(float4(centerLocal, 1.0f), instance.m_World).xyz;
	radiusWorld = radiusLocal * GetMaxScale(instance.m_World);
	centerView = MatrixMultiply(float4(centerWorld, 1.0f), g_Traversal.m_View).xyz;
}

// A cluster whose projected half-extent is under a pixel on both axes cannot land on a sample, so drawing it is
// pure geometry cost. vk_lod_clusters calls this CULL_MIN_PIXEL_SIZE.
//
// Two cases are left alone: without an HZB the screen dimensions are zero and every cluster would test as
// sub-pixel, and a sphere crossing the near plane does not have the tight conservative rect this reads.
bool ClusterTooSmall(float3 centerView, float radiusWorld)
{
	if (g_Traversal.m_HZBWidth == 0 || g_Traversal.m_HZBHeight == 0)
		return false;

	if (centerView.z - radiusWorld <= 0.1f)
		return false;

	float4 aabb;
	ProjectSphereView(centerView, radiusWorld, 0.1f, g_Traversal.m_P00, g_Traversal.m_P11, aabb);

	const float2 halfExtent = (aabb.zw - aabb.xy) * 0.5f * float2(g_Traversal.m_HZBWidth, g_Traversal.m_HZBHeight);
	return !any(halfExtent > kMinClusterPixelSize);
}

bool ClusterOccluded(float3 centerView, float radiusWorld)
{
	const SamplerState minSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_MIN_REDUCTION_INDEX];
	return !OcclusionSphereTest(centerView, radiusWorld, uint2(g_Traversal.m_HZBWidth, g_Traversal.m_HZBHeight),
		g_Traversal.m_P00, g_Traversal.m_P11, g_HZB, minSampler);
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
	if (g_Traversal.m_CullPass != 0)
	{
		// Second pass: seed the node queue from what the first pass rejected and let the node passes below run
		// unchanged. Instance roots are in that list, so nothing here reads InstanceData. The rejected groups are
		// already sitting in the group queue this pass walks, so they are not copied.
		const uint itemIndex = dispatchThreadId.x;

		if (itemIndex < g_RejectNodeCount[0])
		{
			uint nodeSlot;
			InterlockedAdd(g_NodeQueueCountOut[0], 1, nodeSlot);
			g_NodeQueueOut[nodeSlot] = g_RejectNodes[itemIndex];
		}
		return;
	}

	if (dispatchThreadId.x >= g_Traversal.m_NumInstances)
		return;

	const uint instanceIndex = g_Traversal.m_InstanceBaseIndex + dispatchThreadId.x;
	const srrhi::PerInstanceData instance = g_InstanceData[instanceIndex];
	const srrhi::ClusterGeometry geometry = g_Geometries[instance.m_MeshDataIndex];
	if (geometry.m_LODLevelCount == 0)
		return;

	bool bFrustumVisible;
	bool bOcclusionVisible;
	SphereTestsWorld(instance.m_Center, instance.m_Radius, bFrustumVisible, bOcclusionVisible);

	if (!bFrustumVisible)
		return;

	if (!bOcclusionVisible)
	{
		// The instance sits behind last frame's depth. Its root is recorded instead of dropped, and the second
		// pass re-tests the root's children against the depth this frame wrote.
		uint rejectSlot;
		InterlockedAdd(g_RejectNodeCount[0], 1, rejectSlot);

		srrhi::ClusterNodeRef rejectedRoot;
		rejectedRoot.m_InstanceIndex = instanceIndex;
		rejectedRoot.m_NodeIndex = geometry.m_NodeOffset;
		g_RejectNodes[rejectSlot] = rejectedRoot;

		InterlockedAdd(g_Counters[0].m_RejectNodeCount, 1);
		return;
	}

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

			bool bFrustumVisible;
			bool bOcclusionVisible;
			SphereTests(centerLocal, radiusLocal, instance.m_World, bFrustumVisible, bOcclusionVisible);

			if (!bFrustumVisible)
				continue;

			if (!bOcclusionVisible)
			{
				// Rejected by occlusion alone, so the second pass can still find it behind this frame's depth:
				// record the child rather than its subtree, which is what the second pass descends from.
				if (RecordsRejects())
				{
					uint rejectSlot;
					InterlockedAdd(g_RejectNodeCount[0], 1, rejectSlot);

					srrhi::ClusterNodeRef rejectedChild;
					rejectedChild.m_InstanceIndex = nodeRef.m_InstanceIndex;
					rejectedChild.m_NodeIndex = childIndex;
					g_RejectNodes[rejectSlot] = rejectedChild;

					InterlockedAdd(g_Counters[0].m_RejectNodeCount, 1);
				}
				continue;
			}

			uint slot;
			InterlockedAdd(g_NodeQueueCountOut[0], 1, slot);

			srrhi::ClusterNodeRef childRef;
			childRef.m_InstanceIndex = nodeRef.m_InstanceIndex;
			childRef.m_NodeIndex = childIndex;
			g_NodeQueueOut[slot] = childRef;
		}

		return;
	}

	const uint groupIndex = NodeGetGroupIndex(packed);

	// A group whose blob is not in the arena cannot be drawn, so it is asked for instead of queued. The levels
	// around it still cover its region: the groups pass draws a finer cluster while the coarser group that
	// replaced it is missing.
	if (!GroupIsResident(groupIndex))
	{
		RequestGroup(groupIndex);
		return;
	}

	uint slot;
	InterlockedAdd(g_GroupQueueCount[0], 1, slot);

	srrhi::ClusterGroupRef groupRef;
	groupRef.m_InstanceIndex = nodeRef.m_InstanceIndex;
	groupRef.m_GroupIndex = groupIndex;
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
		// Above the forced level nothing is drawn; at or below it, a cluster is drawn unless a coarser group that
		// is still at or above the forced level replaced it. That is the same rule as the metric cut with "the
		// group is too coarse" standing in for a level comparison, and unlike an equality test it keeps regions
		// covered when a group was too stuck to produce a coarser level.
		if (group.m_LODLevel > forcedLevel)
			return;
	}
	else if (GroupTooCoarse(groupRef.m_GroupIndex, instance.m_World))
	{
		return;
	}

	// The cut picked this group's level for this region, so it is the level whose geometry is about to be drawn.
	// That - not merely being visited - is what the age filter counts as use: a level the cut skipped is worth
	// unloading, and the arena then converges on what is actually on screen when the budget is tight.
	g_GroupStates[groupRef.m_GroupIndex].m_Age = 0;

	float3 centerLocal;
	float radiusLocal;
	GetBoundsSphere(group.m_Bounds, centerLocal, radiusLocal);

	bool bFrustumVisible;
	bool bOcclusionVisible;
	SphereTests(centerLocal, radiusLocal, instance.m_World, bFrustumVisible, bOcclusionVisible);

	if (!bFrustumVisible)
		return;

	if (!bOcclusionVisible)
	{
		if (RecordsRejects())
		{
			uint rejectSlot;
			InterlockedAdd(g_RejectGroupCount[0], 1, rejectSlot);

			srrhi::ClusterGroupRef rejectedGroup;
			rejectedGroup.m_InstanceIndex = groupRef.m_InstanceIndex;
			rejectedGroup.m_GroupIndex = groupRef.m_GroupIndex;
			g_RejectGroups[rejectSlot] = rejectedGroup;

			InterlockedAdd(g_Counters[0].m_RejectGroupCount, 1);
		}
		return;
	}

	for (uint i = 0; i < group.m_ClusterCount; ++i)
	{
		const uint clusterIndex = group.m_ClusterOffset + i;

		// A coarser group that replaced this cluster covers its region already when that group is detailed
		// enough for this camera. This is per cluster, not per group: one group holds clusters that were
		// replaced by different coarser groups. A replacement that is not resident covers nothing, so the
		// finer cluster stays: that is the "finest available" rule while a level streams in.
		const uint generating = g_GeneratingGroups[clusterIndex];
		if (generating != kNoGroup && GroupIsResident(generating))
		{
			const bool bReplacedByCoarser = bForcedLevel ?
				(g_Groups[generating].m_LODLevel <= forcedLevel) :
				!GroupTooCoarse(generating, instance.m_World);
			if (bReplacedByCoarser)
				continue;
		}

		// The group's sphere bounds every cluster in it, so two tests only the cluster can answer run here: is it
		// too small to land on a sample at all, and is it behind this frame's depth.
		float3 clusterCenterView;
		float clusterRadiusWorld;
		UnpackClusterSphere(clusterIndex, instance, clusterCenterView, clusterRadiusWorld);

		if (g_Traversal.m_DisableCulling == 0)
		{
			// Sub-pixel geometry is dropped outright: no depth buffer will ever bring it back, so it is not a
			// candidate for the second pass' reject list.
			if (ClusterTooSmall(clusterCenterView, clusterRadiusWorld))
				continue;

			// Occlusion is the reject a later pass can undo, so it is the one that leaves a bit behind.
			if (g_Traversal.m_EnableOcclusionCulling && ClusterOccluded(clusterCenterView, clusterRadiusWorld))
			{
				if (RecordsRejects())
				{
					InterlockedOr(g_RejectClusterMasks[clusterIndex >> 5], 1u << (clusterIndex & 31));
					InterlockedAdd(g_Counters[0].m_RejectClusterCount, 1);
				}
				continue;
			}
		}

		// Every kept cluster takes its own slot. Reserving the whole group up front would leave the skipped
		// clusters as holes, and the mesh dispatch draws the list by index. Each pass appends to its own list so
		// the first pass' draw is still reading its own when the second pass fills its.
		uint jobIndex;
		if (g_Traversal.m_CullPass == 0)
		{
			InterlockedAdd(g_ClusterJobCount[0], 1, jobIndex);
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
			if (group.m_LODLevel != 0)
				InterlockedAdd(g_Counters[0].m_CoarseClusterCount, 1);
		}
		else
		{
			InterlockedAdd(g_ClusterJobCountSecond[0], 1, jobIndex);
			if (g_Traversal.m_MaxClusters != 0 && jobIndex >= g_Traversal.m_MaxClusters)
			{
				InterlockedAdd(g_Counters[0].m_ClusterCapped, 1);
				continue;
			}

			srrhi::ClusterJob job;
			job.m_InstanceIndex = groupRef.m_InstanceIndex;
			job.m_ClusterIndex = clusterIndex;
			g_ClusterJobsSecond[jobIndex] = job;

			InterlockedAdd(g_Counters[0].m_SecondPassClusterCount, 1);
		}
	}
}

[numthreads(1, 1, 1)]
void ClusterBuildArgs_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	// One thread group per visible cluster: the mesh shader's group size is one cluster.
	uint clusterCount = g_Traversal.m_CullPass == 0 ? g_ClusterJobCount[0] : g_ClusterJobCountSecond[0];
	if (g_Traversal.m_MaxClusters != 0)
		clusterCount = min(clusterCount, g_Traversal.m_MaxClusters);

	const uint slot = g_Traversal.m_CullPass == 0 ? kIndirectSlotFirstPassMesh : kIndirectSlotSecondPassMesh;
	g_ClusterIndirectArgs[slot].m_ThreadGroupCountX = clusterCount;
	g_ClusterIndirectArgs[slot].m_ThreadGroupCountY = 1;
	g_ClusterIndirectArgs[slot].m_ThreadGroupCountZ = 1;

	if (g_Traversal.m_CullPass != 0)
		return;

	// The second pass' seed grid: one thread per entry of the larger reject list. Nothing rejected leaves the
	// grid empty, which is how a static camera skips the second pass entirely.
	const uint rejectCount = max(g_RejectNodeCount[0], g_RejectGroupCount[0]);
	const uint rejectGrid = (rejectCount + srrhi::CommonConsts::kThreadsPerGroup - 1) / srrhi::CommonConsts::kThreadsPerGroup;

	g_ClusterIndirectArgs[kIndirectSlotSecondPassInit].m_ThreadGroupCountX = rejectGrid;
	g_ClusterIndirectArgs[kIndirectSlotSecondPassInit].m_ThreadGroupCountY = 1;
	g_ClusterIndirectArgs[kIndirectSlotSecondPassInit].m_ThreadGroupCountZ = 1;

	// The per-cluster re-test walks the first pass' group queue, which pass 1 leaves untouched.
	const uint rejectClusterGrid =
		(g_GroupQueueCount[0] + srrhi::CommonConsts::kThreadsPerGroup - 1) / srrhi::CommonConsts::kThreadsPerGroup;

	g_ClusterIndirectArgs[kIndirectSlotRejectClusters].m_ThreadGroupCountX = rejectClusterGrid;
	g_ClusterIndirectArgs[kIndirectSlotRejectClusters].m_ThreadGroupCountY = 1;
	g_ClusterIndirectArgs[kIndirectSlotRejectClusters].m_ThreadGroupCountZ = 1;
}

// Second pass companion to the groups pass: the first pass left one bit per cluster it deferred, so this kernel
// re-tests only those against the depth buffer the first pass just filled and appends the survivors. The lod
// decision is not redone - the bit is the decision, and only the visibility answer changed.
[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ClusterRejectClusters_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint itemIndex = dispatchThreadId.x;
	if (itemIndex >= g_Pass0GroupCount[0])
		return;

	const srrhi::ClusterGroupRef groupRef = g_Pass0Groups[itemIndex];
	const srrhi::ClusterGroup group = g_Groups[groupRef.m_GroupIndex];
	const srrhi::PerInstanceData instance = g_InstanceData[groupRef.m_InstanceIndex];

	// Most groups are deferred entirely or not at all, so the word the group's range falls in answers the common
	// case with one load. A neighbouring group's bits can set it too, which only costs the loop below.
	const uint firstWord = group.m_ClusterOffset >> 5;
	const uint lastWord = (group.m_ClusterOffset + group.m_ClusterCount - 1) >> 5;
	uint anyReject = 0;
	for (uint word = firstWord; word <= lastWord; ++word)
		anyReject |= g_RejectClusterMasks[word];

	if (anyReject == 0)
		return;

	for (uint i = 0; i < group.m_ClusterCount; ++i)
	{
		const uint clusterIndex = group.m_ClusterOffset + i;
		if ((g_RejectClusterMasks[clusterIndex >> 5] & (1u << (clusterIndex & 31))) == 0)
			continue;

		float3 clusterCenterView;
		float clusterRadiusWorld;
		UnpackClusterSphere(clusterIndex, instance, clusterCenterView, clusterRadiusWorld);

		if (ClusterOccluded(clusterCenterView, clusterRadiusWorld))
			continue;

		uint jobIndex;
		InterlockedAdd(g_ClusterJobCountSecond[0], 1, jobIndex);
		if (g_Traversal.m_MaxClusters != 0 && jobIndex >= g_Traversal.m_MaxClusters)
		{
			InterlockedAdd(g_Counters[0].m_ClusterCapped, 1);
			continue;
		}

		srrhi::ClusterJob job;
		job.m_InstanceIndex = groupRef.m_InstanceIndex;
		job.m_ClusterIndex = clusterIndex;
		g_ClusterJobsSecond[jobIndex] = job;

		InterlockedAdd(g_Counters[0].m_SecondPassClusterCount, 1);
	}
}
