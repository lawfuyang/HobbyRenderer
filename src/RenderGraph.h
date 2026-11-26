#pragma once

#include "GraphicRHI.h"

// ============================================================================
// Resource handles — opaque indices into the graph's resource tables
// ============================================================================

struct RGResourceHandleBase
{
    uint32_t m_Index = UINT32_MAX;

    bool IsValid() const { return m_Index != UINT32_MAX; }
    void Invalidate() { m_Index = UINT32_MAX; }
    bool operator==(const RGResourceHandleBase& other) const { return m_Index == other.m_Index; }
    bool operator!=(const RGResourceHandleBase& other) const { return m_Index != other.m_Index; }
};

struct RGTextureHandle : public RGResourceHandleBase {};
struct RGBufferHandle : public RGResourceHandleBase {};

// ============================================================================
// Resource Descriptors
// ============================================================================

struct RGResourceDescBase
{
    virtual size_t ComputeHash() const = 0;
    virtual nvrhi::MemoryRequirements GetMemoryRequirements() const = 0;
};

struct RGTextureDesc : public RGResourceDescBase
{
    nvrhi::TextureDesc m_NvrhiDesc;

    size_t ComputeHash() const override;
    nvrhi::MemoryRequirements GetMemoryRequirements() const override;
};

struct RGBufferDesc : public RGResourceDescBase
{
    nvrhi::BufferDesc m_NvrhiDesc;

    size_t ComputeHash() const override;
    nvrhi::MemoryRequirements GetMemoryRequirements() const override;
};

// ============================================================================
// Resource Usage Flags
// ============================================================================

enum class RGResourceAccessMode : uint8_t
{
    Read,
    Write
};

// ============================================================================
// Internal Resource Tracking
// ============================================================================

namespace RenderGraphInternal
{

struct ResourceLifetime
{
    uint16_t m_FirstPass = UINT16_MAX;
    uint16_t m_LastPass = 0;

    bool IsValid() const { return m_FirstPass != UINT16_MAX; }
};

struct TransientResourceBase
{
    size_t m_Hash = 0;
    ResourceLifetime m_Lifetime;
    uint32_t m_AliasedFromIndex = UINT32_MAX;
    uint16_t m_PhysicalLastPass = 0;
    uint16_t m_DeclarationPass = 0;
    uint64_t m_LastFrameUsed = 0;
    uint64_t m_Offset = 0;
    uint64_t m_BlockOffset = 0;
    uint32_t m_HeapIndex = UINT32_MAX;
    // Which per-frame heap set this resource is allocated in (kPersistentFrameSet for persistent
    // resources). Resources only ever alias inside their own set, so two frames in flight never share
    // memory.
    uint32_t m_FrameSet = 0;
    bool m_IsAllocated = false;
    bool m_IsDeclaredThisFrame = false;
    bool m_IsPersistent = false;
    bool m_IsPhysicalOwner = false;
    nvrhi::HeapHandle m_Heap;

    virtual nvrhi::MemoryRequirements GetMemoryRequirements() const = 0;

    // The descriptor is the only input to the requirements, so one query per descriptor is enough. The cache is
    // invalidated wherever m_Desc is reassigned (see InvalidateMemoryRequirements in DeclareTexture/DeclareBuffer).
    const nvrhi::MemoryRequirements& GetCachedMemoryRequirements() const;
    void InvalidateMemoryRequirements() { m_HasCachedMemoryRequirements = false; }

    mutable nvrhi::MemoryRequirements m_CachedMemoryRequirements{};
    mutable bool m_HasCachedMemoryRequirements = false;

    virtual ~TransientResourceBase() = default;
};

struct TransientTexture : public TransientResourceBase
{
    RGTextureDesc m_Desc;
    nvrhi::TextureHandle m_PhysicalTexture;

    nvrhi::MemoryRequirements GetMemoryRequirements() const override
    {
        return m_Desc.GetMemoryRequirements();
    }
};

struct TransientBuffer : public TransientResourceBase
{
    RGBufferDesc m_Desc;
    nvrhi::BufferHandle m_PhysicalBuffer;

    nvrhi::MemoryRequirements GetMemoryRequirements() const override
    {
        return m_Desc.GetMemoryRequirements();
    }
};

} // namespace RenderGraphInternal

// ============================================================================
// Main Render Graph
// ============================================================================

// ----------------------------------------------------------------------------
// RenderGraph
//
// Records the resources and passes of a frame, then compiles them into a
// concrete execution plan (pass order, heap sub-allocation, aliasing, barriers)
// before the passes are recorded and submitted.
//
// Per-frame protocol:
//   Reset() -> BeginSetup() -> [Declare* / Read* / Write* / ScheduleRenderer]
//           -> EndSetup() -> Compile() -> [passes run] -> PostRender()
// ----------------------------------------------------------------------------
class RenderGraph
{
public:
    // Frame pipelining: transient heaps alternate between this many sets, one per frame in flight, so
    // the CPU can record frame N+1 while the GPU still executes frame N. Persistent resources live in
    // their own set and never cycle. Renderer::kFramesInFlight must match this.
    static constexpr uint32_t kFrameSetCount = 2;
    static constexpr uint32_t kPersistentFrameSet = UINT32_MAX;

    // ─── Nested types ──────────────────────────────────────────────────────

    struct Stats
    {
        uint32_t m_NumTextures = 0;
        uint32_t m_NumBuffers = 0;
        uint32_t m_NumAllocatedTextures = 0;
        uint32_t m_NumAllocatedBuffers = 0;
        uint32_t m_NumAliasedTextures = 0;
        uint32_t m_NumAliasedBuffers = 0;
        size_t m_TotalTextureMemory = 0;
        size_t m_TotalBufferMemory = 0;
        // Memory accounting, filled by Compile() from the cached requirements: what the live heaps hold, the
        // largest single free block, the sum every declared resource asked for, the physical owners' share of
        // it, and the difference (what aliasing saved).
        size_t m_HeapBytes = 0;
        size_t m_LargestFreeBlockBytes = 0;
        size_t m_UnAliasedBytes = 0;
        size_t m_AliasedBytes = 0;
        size_t m_AliasingSavingsBytes = 0;
        uint32_t m_NumAliasBarriers = 0;
        // Virtual-resource creations spent querying memory requirements this frame. Steady state is 0 once the
        // per-resource cache is warm; anything else means a descriptor keeps changing.
        uint32_t m_NumMemoryRequirementQueries = 0;
        // Transitions the declarations imply for this frame: one per change of state between consecutive passes
        // that touch a resource. With `--rendergraph-own-barriers` these are the barriers the graph places;
        // without it nvrhi places its own and the number is the size of the opportunity.
        uint32_t m_NumDeclaredStateChanges = 0;
    };

    // ─── Lifecycle ─────────────────────────────────────────────────────────

    void Shutdown();

    // Reset graph for new frame (doesn't free physical resources)
    void Reset();

    // ─── Resource declaration ──────────────────────────────────────────────

    // General purpose transient resource declaration (called during Setup phase).
    // Underlying memory is not guaranteed to be retained across the whole frame due to aliasing.
    bool DeclareTexture(const RGTextureDesc& desc, RGTextureHandle& outputHandle);
    bool DeclareBuffer(const RGBufferDesc& desc, RGBufferHandle& outputHandle);

    // "Persistent" resources are guaranteed to NOT be aliased with any other resource.
    // They are ideal for long-term caching of resources across frames; they are only
    // freed once the "owner" has not declared them for several frames.
    bool DeclarePersistentTexture(const RGTextureDesc& desc, RGTextureHandle& outputHandle);
    bool DeclarePersistentBuffer(const RGBufferDesc& desc, RGBufferHandle& outputHandle);

    // ─── Resource access registration ──────────────────────────────────────

    // Declare how a pass uses a resource (called during Setup phase). The state is an `nvrhi::ResourceStates`
    // because that is the vocabulary the barrier plan and the state timeline speak; the untyped overloads mean
    // what the resource itself implies: a read is `ShaderResource`, a texture write is the state its descriptor
    // makes possible (`DepthWrite` for a depth format, else `RenderTarget` or `UnorderedAccess`), and a buffer
    // write is `UnorderedAccess` (what `DeclareBuffer` already implies).
    void ReadTexture(RGTextureHandle handle);
    void ReadTexture(RGTextureHandle handle, nvrhi::ResourceStates state);
    void WriteTexture(RGTextureHandle handle);
    void WriteTexture(RGTextureHandle handle, nvrhi::ResourceStates state);

    void ReadBuffer(RGBufferHandle handle);
    void ReadBuffer(RGBufferHandle handle, nvrhi::ResourceStates state);
    void WriteBuffer(RGBufferHandle handle);
    void WriteBuffer(RGBufferHandle handle, nvrhi::ResourceStates state);

    // ─── Pass management (render-loop internals) ───────────────────────────

    // BeginSetup() is called exactly once per frame (after Reset()), before any
    // ScheduleRenderer() calls.  EndSetup() is called once after all renderers
    // have been scheduled, before Compile().
    void BeginSetup();
    void EndSetup();
    void BeginPass(const char* name);

    void ScheduleRenderer(class IRenderer* pRenderer);

    void Compile();
    void PostRender();

    // ─── Resource retrieval (valid only between Compile and the next Reset) ─

    nvrhi::TextureHandle GetTexture(RGTextureHandle handle, RGResourceAccessMode access) const;
    nvrhi::BufferHandle GetBuffer(RGBufferHandle handle, RGResourceAccessMode access) const;

    // Raw retrieval without access validation — internal render-loop use only.
    nvrhi::TextureHandle GetTextureRaw(RGTextureHandle handle) const;
    nvrhi::BufferHandle GetBufferRaw(RGBufferHandle handle) const;

    // ─── Synchronisation ───────────────────────────────────────────────────

    // Set the current active pass for validation (used during Render phase)
    void SetActivePass(uint16_t passIndex);

    // Insert global sync barriers for a given pass into the command list.
    // Must be called at the start of each pass's command list recording, before any GPU work.
    void InsertGlobalSyncBarriers(uint16_t passIndex, nvrhi::ICommandList* commandList) const;

    // ─── Graph-owned barriers (`--rendergraph-own-barriers`) ────────────────

    // With this on, the graph owns the state of every transient it allocates: it tells each pass's command list
    // where the resources that pass uses start, which is the one thing `keepInitialState` used to do for every list
    // (and by transitioning back to `initialState` at every close, cost a transition pair per pass). Automatic
    // barriers stay enabled, so the transition into each binding's state and every UAV hazard barrier are still
    // nvrhi's, placed from a starting point that is now true instead of assumed.
    void SetOwnBarriers(bool bOwn);
    bool OwnsBarriers() const { return m_bOwnBarriers; }

    // Called from the pass's own task, before and after its recorded commands.
    void EmitPassBarriers(uint16_t passIndex, nvrhi::ICommandList* commandList);
    void EndPassBarriers(uint16_t passIndex, nvrhi::ICommandList* commandList);

    // Returns the pass index for the current pass (valid after BeginPass, before Compile)
    uint16_t GetCurrentPassIndex() const { return m_CurrentPassIndex; }

    // ─── Debug & diagnostics ───────────────────────────────────────────────

    // Asks the next Compile() to rebuild the transient placement from scratch (`--relayout`).
    void RequestLayoutRebuild() { m_bRebuildLayoutPending = true; }
    // Pass names are indexed 1-based everywhere a pass index is stored, while m_PassNames is 0-based. Every
    // diagnostic goes through this so an out-of-range index reads "<invalid>" instead of throwing.
    const char* PassName(uint16_t passIndex1Based) const;

    // A graph resource a pass actually bound while recording, and the state that binding requires of it.
    struct UsedResource
    {
        const nvrhi::IResource* m_Resource = nullptr;
        nvrhi::ResourceStates m_State = nvrhi::ResourceStates::ShaderResource;
    };

    // Compares what a pass really bound against what its Setup declared. A binding the graph never learned about is
    // invisible to lifetimes, aliasing and any barrier plan built on them, so with `--rendergraph-strict-access`
    // each one logs and asserts. A handle that is not a graph transient (bindless static textures, scene-owned
    // buffers) is skipped: the lookup miss is expected and says nothing about the pass. Called by the renderer's
    // pass helpers, which are the ones that see the binding sets.
    void ValidateActualAccess(std::span<const UsedResource> usedResources);

    void RenderDebugUI();
    std::string ExportToString() const;

    static RGBufferDesc GetSPDAtomicCounterDesc(const char* debugName, uint32_t numElements = 1);

    // Exposed for diagnostics only — do not use in production code.
    uint32_t GetForceInvalidateFramesRemaining() const { return m_ForceInvalidateFramesRemaining; }

    // Enable/disable verbose informational logging (pool-reuse, aliasing, eviction …).
    void SetVerboseLogging(bool enabled) { m_bVerboseLogging = enabled; }
    bool IsVerboseLogging() const { return m_bVerboseLogging; }

// ============================================================================
// Private implementation
// ============================================================================
private:
    // ─── Nested types ──────────────────────────────────────────────────────

    struct HeapBlock
    {
        size_t m_Offset = 0;
        size_t m_Size = 0;
        bool m_IsFree = true;
    };

    struct HeapEntry
    {
        nvrhi::HeapHandle m_Heap;
        size_t m_Size = 0;
        uint64_t m_LastFrameUsed = 0;
        uint32_t m_HeapIdx = UINT32_MAX;
        // The per-frame set this heap belongs to (kPersistentFrameSet for persistent resources).
        uint32_t m_FrameSet = 0;
        std::vector<HeapBlock> m_Blocks;
    };

    struct PassAccess
    {
        std::unordered_set<uint32_t> m_ReadTextures;
        std::unordered_set<uint32_t> m_WriteTextures;
        std::unordered_set<uint32_t> m_ReadBuffers;
        std::unordered_set<uint32_t> m_WriteBuffers;
    };

    // One decided use of one resource, in the order the pass declared it. The sets above answer "did this pass
    // declare it?" in O(1); these records carry the state, which is what a timeline needs.
    struct AccessRecord
    {
        uint32_t m_Resource = 0;
        nvrhi::ResourceStates m_State = nvrhi::ResourceStates::ShaderResource;
        bool m_bIsBuffer = false;
    };

    // ─── Resource allocation & heap sub-allocation ─────────────────────────

    // Frees every non-persistent placement and hands each transient heap back as a single free block, so the next
    // allocation pass rebuilds the layout from scratch (`--relayout`).
    void RebuildLayout();

    // Generic resource allocation helper (avoids code duplication)
    void AllocateResourcesInternal(bool bIsBuffer, std::function<void(uint32_t, nvrhi::HeapHandle, uint64_t)> createAndBindResource);

    nvrhi::HeapHandle CreateHeap(size_t size, uint32_t frameSet);
    void SubAllocateResource(RenderGraphInternal::TransientResourceBase* resource, uint64_t alignment);
    void FreeBlock(uint32_t heapIdx, uint64_t blockOffset);

    // ─── Lifetime tracking & barriers ──────────────────────────────────────

    // Helper for updating resource lifetimes
    void UpdateResourceLifetime(RenderGraphInternal::ResourceLifetime& lifetime, uint16_t currentPass);

    // Turns the per-pass access records into the frame's state timeline, fills m_PassBarriers when the graph owns
    // the barriers, and counts where the timeline changes.
    void BuildBarrierPlan();

    // ─── Deferred release ──────────────────────────────────────────────────

    // Deferred release: when aliasing replaces a physical resource handle,
    // we must not immediately destroy the old D3D12 object because the GPU
    // may still have work in-flight from the previous frame referencing it.
    // Old handles are moved here and released at the start of the next
    // Compile(), which runs after the previous frame's ExecutePendingCommandLists
    // has synced the GPU (via waitForIdle).
    void FlushDeferredReleases();

    // ─── Pass bookkeeping ──────────────────────────────────────────────────

    uint16_t GetActivePassIndex() const;

    // ─── State ─────────────────────────────────────────────────────────────

    // Heaps and per-pass resource access sets
    std::vector<HeapEntry> m_Heaps;
    std::vector<PassAccess> m_PassAccesses;

    // Resource tables (indexed by RG*Handle::m_Index)
    std::vector<RenderGraphInternal::TransientTexture> m_Textures;
    std::vector<RenderGraphInternal::TransientBuffer> m_Buffers;
    std::vector<const char*> m_PassNames;

    // Deferred release queue (see FlushDeferredReleases)
    std::vector<nvrhi::TextureHandle> m_DeferredReleaseTextures;
    std::vector<nvrhi::BufferHandle>  m_DeferredReleaseBuffers;

    // When m_AliasingEnabled is true, records which passes contain at least one aliased resource
    // that needs a global sync barrier before it's first used.
    // Set during Compile() by checking each aliased resource's first pass.
    // Indexed by pass index (1-based, pass 0 is unused).
    std::vector<bool> m_PassNeedsGlobalSyncBarrier;

    // Setup-phase state
    bool m_IsInsideSetup = false;
    PassAccess m_PendingPassAccess;
    std::vector<AccessRecord> m_PendingAccessRecords;
    std::vector<uint32_t> m_PendingDeclaredTextures;
    std::vector<uint32_t> m_PendingDeclaredBuffers;

    // Typed access records, one list per pass (0-based, parallel to m_PassAccesses).
    std::vector<std::vector<AccessRecord>> m_PassAccessRecords;

    // One resource a pass's command list has to place a barrier for: the state it starts the list in and the one
    // the pass declared. Equal states mean the pass needs no transition, only the seeding.
    struct BarrierOp
    {
        uint32_t m_Resource = 0;
        bool m_bIsBuffer = false;
        nvrhi::ResourceStates m_EntryState = nvrhi::ResourceStates::Common;
        nvrhi::ResourceStates m_State = nvrhi::ResourceStates::Common;
    };

    // Where the declarations say each resource goes: one list per pass (1-based, index 0 unused).
    std::vector<std::vector<BarrierOp>> m_PassBarriers;

    // The state the graph believes each slot's memory is in. With the graph owning barriers these carry over from
    // frame to frame (they describe the GPU, not the frame) and a slot whose handle was just created starts at the
    // layout the descriptor asked to be created in, which is `initialState`. Without it they are rebuilt from the
    // descriptors every frame, because nvrhi restores `initialState` at close().
    std::vector<nvrhi::ResourceStates> m_TextureStates;
    std::vector<nvrhi::ResourceStates> m_BufferStates;
    bool m_bOwnBarriers = false;

    // Declared-vs-actual checker state (ValidateActualAccess, debug runs only). The maps are rebuilt by Compile()
    // from the physical handles it just bound; the per-pass sets are filled while the passes record.
    std::unordered_map<const nvrhi::IResource*, uint32_t> m_TextureSlots;
    std::unordered_map<const nvrhi::IResource*, uint32_t> m_BufferSlots;
    std::vector<std::unordered_set<uint32_t>> m_PassUsedTextures;
    std::vector<std::unordered_set<uint32_t>> m_PassUsedBuffers;
    std::vector<bool> m_PassAccessChecked;
    // Every violation already reported, so an audit run prints each one once instead of once per frame.
    std::unordered_set<uint64_t> m_ReportedAccessViolations;
    // Set by Renderer when --relayout was passed; consumed by the next Compile().
    bool m_bRebuildLayoutPending = false;

    // Frame state
    Stats m_Stats;
    bool m_AliasingEnabled = true;
    // One-time log when aliasing was requested but the device has no enhanced barriers (see Reset).
    bool m_bLoggedAliasingWithoutEnhancedBarriers = false;
    bool m_IsCompiled = false;
    uint16_t m_CurrentPassIndex = 0;
    // The heap set this frame allocates into (see kFrameSetCount). 0 when frame pipelining is off.
    uint32_t m_CurrentFrameSet = 0;
    bool m_bForceInvalidateAllResources = false;
    // When true, informational SDL_Log messages (pool-reuse, aliasing, eviction,
    // desc-change, etc.) are emitted.  Kept false in production to avoid log spam.
    bool m_bVerboseLogging = false;
    // After Shutdown(), we need to force-invalidate handles for TWO consecutive
    // frames, not just one.  Frame 1 only invalidates handles that are actually
    // declared in that frame's rendering mode (e.g. PT mode skips depth/GBuffers).
    // Frame 2 must also invalidate those skipped handles before they can be
    // re-declared in the new mode.  This counter is set to 2 by Shutdown() and
    // decremented by Reset(); m_bForceInvalidateAllResources stays true while > 0.
    uint32_t m_ForceInvalidateFramesRemaining = 0;
};
