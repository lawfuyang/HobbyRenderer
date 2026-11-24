#pragma once

#include "ClusterLodGpu.h"
#include "Utilities.h"

// Geometry streaming (docs/cluster-lod-implementation-plan.md phase 5).
//
// Cluster groups are the streaming unit. Every group's triangles and vertex attributes live in one 16-byte aligned blob
// in a device arena, and a residency table (one ClusterGroupState per global group) says where each blob is. The
// traversal asks for the groups it could not draw; this class serves those requests the same way texture
// streaming does:
//
// The blob bytes are never materialised in system RAM: they stay in the cooked mesh file, which is mapped
// read-only, and a load copies the group's range straight into the arena from the mapping. The file's per-group
// table (ClusterLod::BlobFile) is filled by the scene load and consumed here.
//
//   device asks   the traversal (and the age filter) append group indices to request lists, which are copied to a
//                 cache-host-visible staging buffer at the end of the frame and signalled with an event query;
//   host decides  a request read back two or more frames later is turned into an arena allocation, an upload of
//                 the blob bytes and one entry in the patch list;
//   device serves ClusterStreamUpdate writes those addresses into the residency table, so the next traversal sees
//                 the group.
//
// The arena is bounded by Renderer::GetStreamingBudgetBytes() - the VRAM budget it shares with the texture tile
// heaps, which are only offered what the arena does not reserve (Renderer::GetTileStreamingBudgetBytes). The budget
// defaults to what the OS says this application may use. Nothing here is sized by a switch: the arena holds
// every group when the budget allows it, and when the budget is lowered it is re-packed and the groups that do not
// fit are dropped (coarsest level first), then asked for again as the camera needs them.
//
// Unload frees a blob's block, load takes another, and the only thing that refers to a block is the address in the
// table. Fragmentation shows up as a load that does not fit and is dropped - the traversal asks again next frame.
class GeometryStreaming
{
public:
    // Requests read back per slot. The lists themselves are fixed-size device buffers; anything past the budget
    // is dropped and asked for again on a later frame, which costs nothing because the frame tag has changed.
    // This is the device-side cap; the host applies the byte budget below on top of it.
    static constexpr uint32_t kMaxLoadsPerFrame = 512;
    static constexpr uint32_t kMaxUnloadsPerFrame = 1024;

    // Capacities of the device-side request lists (also the readback ranges).
    static constexpr uint32_t kLoadListCapacity = 2048;
    static constexpr uint32_t kUnloadListCapacity = 2048;

    static constexpr uint32_t kMaxPatches = kMaxLoadsPerFrame + kMaxUnloadsPerFrame;

    // A request is consumed on the first frame its event has completed. The slots pipeline several frames of
    // requests, so a high per-frame byte budget is not throttled by a single readback.
    static constexpr uint32_t kReadbackSlots = 8;

    struct Stats
    {
        uint32_t m_ResidentGroups = 0;
        uint32_t m_GroupCount = 0;
        uint32_t m_ActiveGroups = 0;
        uint64_t m_ResidentBytes = 0;
        uint64_t m_ArenaBytes = 0;              // the arena's current reservation
        uint64_t m_BudgetBytes = 0;             // what the streaming budget allows it to become
        uint64_t m_WastedBytes = 0;             // arena minus resident blobs: fragmentation plus alignment
        uint32_t m_LoadedThisFrame = 0;
        uint32_t m_UnloadedThisFrame = 0;
        uint32_t m_DroppedLoads = 0;
        uint32_t m_RequestsLastFrame = 0;
        uint32_t m_EvictedByBudget = 0;         // groups the budget has forced out since the last resize
        uint64_t m_LoadBytesThisFrame = 0;      // bytes copied out of the cooked file for this frame's loads
        uint64_t m_LoadBudgetBytes = 0;         // Config::m_ClusterLoadBytesPerFrame, 0 = uncapped
        uint64_t m_BlobFileBytes = 0;           // size of the mapped cooked file

        // Per LOD level, for the residency histogram: how many groups exist at each level and how many of them
        // are in the arena right now.
        uint32_t m_LevelCount = 0;
        uint32_t m_GroupsPerLevel[ClusterLod::kMaxLODLevels] = {};
        uint32_t m_ResidentPerLevel[ClusterLod::kMaxLODLevels] = {};
    };

    // Creates the arena, the residency table, the request lists and the readback slots, and makes the groups that
    // fit the streaming budget resident. The blob bytes come from the cooked file the scene load mapped
    // (Renderer::m_ClusterBlobFile); only the group metadata is taken from `data`.
    void Initialize(nvrhi::ICommandList* cmd, ClusterLod::GpuData& data);
    void Shutdown();

    // Start of frame, after texture streaming's own pre-render step: consume a completed request readback, apply
    // the loads and unloads it asked for, patch the residency table and age the resident groups.
    void PreRender(nvrhi::ICommandList* cmd);

    // End of frame, after the render passes have run: hand this frame's request lists to a readback slot.
    void PostRender(nvrhi::ICommandList* cmd);

    uint32_t GetFrameIndex() const { return m_FrameIndex; }
    uint32_t GetMaxLoads() const { return kMaxLoadsPerFrame; }
    const Stats& GetStats() const { return m_Stats; }

    // What the arena holds right now, in bytes. The texture tile heaps are only offered what the budget leaves
    // after this, because the arena's reservation is fixed at scene load while heaps grow on demand.
    uint64_t GetReservedBytes() const { return uint64_t(m_ArenaElements) * sizeof(uint32_t); }

    // Buffers the traversal and the cluster mesh shader bind.
    nvrhi::BufferHandle GetStreamDataBuffer() const { return m_StreamData; }
    nvrhi::BufferHandle GetGroupStateBuffer() const { return m_GroupStates; }
    nvrhi::BufferHandle GetLoadRequestBuffer() const { return m_LoadRequests; }
    nvrhi::BufferHandle GetStreamCounterBuffer() const { return m_StreamCounters; }
    // One uint per geometry: the first LOD level of the suffix that is fully resident. The discrete-LOD path may
    // seed a traversal at that level or coarser and nowhere finer, because a level that is missing groups would
    // draw holes where the continuous cut draws whatever is available.
    nvrhi::BufferHandle GetDiscreteLevelBuffer() const { return m_DiscreteLevels; }
    // The host mirror of that table, so the CPU reference classifies exactly what the device does.
    uint32_t GetDiscreteLevel(uint32_t geometryIndex) const
    {
        return geometryIndex < m_DiscreteLevelsHost.size() ? m_DiscreteLevelsHost[geometryIndex] : 0u;
    }

private:
    struct Residency
    {
        uint32_t m_Offset = 0;    // arena element offset
        uint32_t m_Elements = 0;  // blob size in elements
        bool m_bResident = false;
    };

    struct FreeBlock
    {
        uint32_t m_Offset;
        uint32_t m_Elements;
    };

    struct ReadbackSlot
    {
        nvrhi::BufferHandle m_Staging;
        nvrhi::EventQueryHandle m_Event;
        bool m_bInFlight = false;
    };

    // Elements the streaming budget allows the arena to hold, capped at what the scene actually needs.
    uint32_t CapacityElementsForBudget() const;

    // One geometry's slice of the global group array, and how many levels it has.
    struct GeometryRange
    {
        uint32_t m_GroupOffset = 0;
        uint32_t m_GroupCount = 0;
        uint32_t m_LODLevelCount = 0;
    };

    // Publishing the discrete level is a cheap derivation of the residency table, done after every change.
    void UpdateDiscreteLevels(nvrhi::ICommandList* cmd);

    void BuildInitialResidency(nvrhi::ICommandList* cmd);
    // Drops resident groups (coarsest level first) until they fit the arena.
    void TrimToCapacity();
    // Rebuilds the arena and the residency table from the groups that are marked resident, and uploads both.
    void Pack(nvrhi::ICommandList* cmd);
    // Rebuilds the arena at (at most) the current budget, dropping resident groups that no longer fit and
    // re-addressing the rest. Publishes the whole state table, so no patch pass is needed.
    void Resize(nvrhi::ICommandList* cmd);
    void ConsumeCompletedRequests();
    void ApplyUnloads(std::vector<srrhi::ClusterStreamPatch>& patches);
    void ApplyLoads(std::vector<srrhi::ClusterStreamPatch>& patches);
    void DispatchPatches(nvrhi::ICommandList* cmd, uint32_t unloadCount);
    void UploadLoadedBlobs(nvrhi::ICommandList* cmd);
    void DispatchAgeFilter(nvrhi::ICommandList* cmd);
    void UploadActiveGroups(nvrhi::ICommandList* cmd);
    // Marks a group resident with no arena address yet; the next Resize/Pack assigns one.
    void MarkResident(uint32_t groupIndex);
    void MarkResident(uint32_t groupIndex, uint32_t offset, uint32_t elements);
    void MarkUnresident(uint32_t groupIndex);

    uint32_t Allocate(uint32_t elements);
    void Free(uint32_t offset, uint32_t elements);

    nvrhi::BufferHandle CreateArenaBuffer(uint32_t elements) const;

    // The mapped bytes of one group's arena blob, or nullptr when the mapping is unusable.
    const uint8_t* GetGroupBlob(uint32_t groupIndex, uint32_t& outByteSize) const;

    uint32_t m_FrameIndex = 0;
    uint32_t m_GroupCount = 0;
    uint64_t m_TotalElements = 0;
    uint64_t m_CoarsestElements = 0;     // every primitive's coarsest level: the cheapest complete cover
    uint32_t m_ArenaElements = 0;
    uint64_t m_ResizedBudgetBytes = 0;   // the budget the arena was last sized for

    // The cooked data the blobs are uploaded from: the mapped mesh file and its per-group ranges.
    std::unique_ptr<MemoryMappedDataReader> m_BlobFile;
    std::vector<ClusterLod::GroupBlobRef> m_GroupFiles;   // parallel to m_GroupBlobs

    std::vector<ClusterLod::StreamGroup> m_GroupBlobs;

    // The +-1 LOD overlap table (global group order): the unload order and the speculative prefetch read it.
    std::vector<ClusterLod::GroupOverlap> m_Overlaps;
    std::vector<uint32_t> m_OverlapGroups;

    std::vector<Residency> m_Residency;
    std::vector<FreeBlock> m_FreeBlocks;           // sorted by offset, coalesced

    // Resident groups the age filter may reclaim, and the mirror of what the device currently holds.
    std::vector<uint32_t> m_ActiveGroups;
    std::vector<uint32_t> m_UploadedActiveGroups;
    bool m_bForceActiveListUpload = true;   // a Pack rewrote the table, so a matching list is not proof of upload

    std::vector<uint32_t> m_PendingLoads;
    std::vector<uint32_t> m_PendingUnloads;

    // This frame's work, kept as members so a frame does not allocate.
    std::vector<srrhi::ClusterStreamPatch> m_Patches;
    std::vector<uint32_t> m_LoadedThisFrame;

    ReadbackSlot m_Slots[kReadbackSlots];
    uint32_t m_NextSlot = 0;

    std::vector<GeometryRange> m_Geometries;
    std::vector<uint32_t> m_DiscreteLevelsHost;   // mirror of what the device holds, so an upload is only needed on change

    nvrhi::BufferHandle m_StreamData;       // the arena, one uint per element
    nvrhi::BufferHandle m_GroupStates;      // ClusterGroupState per global group
    nvrhi::BufferHandle m_LoadRequests;     // group indices the traversal could not draw
    nvrhi::BufferHandle m_UnloadRequests;   // group indices the age filter wants back
    nvrhi::BufferHandle m_StreamCounters;   // ClusterStreamCounters
    nvrhi::BufferHandle m_ActiveGroupList;  // group indices the age filter iterates
    nvrhi::BufferHandle m_PatchList;        // ClusterStreamPatch list for ClusterStreamUpdate
    nvrhi::BufferHandle m_DiscreteLevels;   // one uint per geometry: first fully resident LOD level

    Stats m_Stats{};   // refreshed by PreRender for the stats line
};
