#include "Common.hlsli"

#include "srrhi/hlsl/Cluster.hlsli"

/*
	Geometry streaming (docs/cluster-lod-implementation-plan.md phase 5).

	The traversal asks for the groups it could not draw; the host serves those requests from a CPU-side copy of
	the cooked cluster data into one device arena and hands back an address per group. These two kernels are the
	device halves of that exchange:

	StreamUpdate  one thread per patch: writes the address table entry of a group that was just loaded, or
	              clears it for a group that was unloaded. It runs before the frame's uploads, so an address is
	              never valid before the bytes behind it exist, and freed space is never reused while the
	              previous occupant's address is still in the table.
	AgeFilter     one thread per resident (non-persistent) group: ages it, and asks for it to be unloaded once it
	              has gone `m_AgeThreshold` frames without the traversal touching it.

	The address table is ClusterGroupState::m_Address, an element offset into the arena below
	kClusterInvalidAddressStart. Above it the group is missing and the low bits carry the frame index of the last
	load request, which is what keeps a group from being requested twice in one frame.
*/

static const srrhi::ClusterStreamUpdateConstants g_Update = srrhi::ClusterStreamUpdateInputs::GetStreamCB();
static const StructuredBuffer<srrhi::ClusterStreamPatch>  g_Patches     = srrhi::ClusterStreamUpdateInputs::GetPatches();
static RWStructuredBuffer<srrhi::ClusterGroupState>       g_GroupStates = srrhi::ClusterStreamUpdateInputs::GetGroupStates();

static const srrhi::ClusterStreamAgeConstants g_Age = srrhi::ClusterStreamAgeInputs::GetStreamCB();
static const StructuredBuffer<uint>                       g_ActiveGroups   = srrhi::ClusterStreamAgeInputs::GetActiveGroups();
static RWStructuredBuffer<srrhi::ClusterGroupState>       g_AgedStates     = srrhi::ClusterStreamAgeInputs::GetGroupStates();
static RWStructuredBuffer<uint>                           g_UnloadRequests = srrhi::ClusterStreamAgeInputs::GetUnloadRequests();
static RWStructuredBuffer<srrhi::ClusterStreamCounters>   g_StreamCounters = srrhi::ClusterStreamAgeInputs::GetStreamCounters();

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ClusterStreamUpdate_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint patchIndex = dispatchThreadId.x;
	if (patchIndex >= g_Update.m_PatchCount)
		return;

	const srrhi::ClusterStreamPatch patch = g_Patches[patchIndex];

	srrhi::ClusterGroupState state;
	if (patchIndex < g_Update.m_PatchUnloadCount)
	{
		// Tagged with this frame so the next request for the group is accepted immediately.
		state.m_Address = srrhi::ClusterStream::kInvalidAddressStart |
			(g_Update.m_FrameIndex & srrhi::ClusterStream::kInvalidAddressMask);
		state.m_Age = 0;
	}
	else
	{
		state.m_Address = patch.m_Address;
		state.m_Age = 0;
	}

	g_GroupStates[patch.m_GroupIndex] = state;
}

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ClusterStreamAgeFilter_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint activeIndex = dispatchThreadId.x;
	if (activeIndex >= g_Age.m_ActiveGroupCount)
		return;

	const uint groupIndex = g_ActiveGroups[activeIndex];
	srrhi::ClusterGroupState state = g_AgedStates[groupIndex];

	// The host removes an unloaded group from the active list in the same frame it patches the address, so an
	// active entry is always resident; a stale one is skipped rather than aged into a request.
	if (state.m_Address >= srrhi::ClusterStream::kInvalidAddressStart)
		return;

	state.m_Age += 1;
	g_AgedStates[groupIndex] = state;

	// Exactly one frame per idle period, so a group the host could not unload this frame (budget) is not
	// re-requested every frame after it; the host keeps it in its own pending list instead.
	if (state.m_Age != g_Age.m_AgeThreshold)
		return;

	uint slot;
	InterlockedAdd(g_StreamCounters[0].m_UnloadCount, 1, slot);
	if (slot < g_Age.m_MaxUnloads)
		g_UnloadRequests[slot] = groupIndex;
}
