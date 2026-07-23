#pragma once


#include "GraphicRHI.h"

class RenderGraph;

// ============================================================================
// Resource Handles (opaque indices)
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
    bool Overlaps(const ResourceLifetime& other) const
    {
        if (!IsValid() || !other.IsValid())
            return false;
        return !(m_LastPass < other.m_FirstPass || other.m_LastPass < m_FirstPass);
    }
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
    bool m_IsAllocated = false;
    bool m_IsDeclaredThisFrame = false;
    bool m_IsPersistent = false;
    bool m_IsPhysicalOwner = false;
    nvrhi::HeapHandle m_Heap;

    virtual nvrhi::MemoryRequirements GetMemoryRequirements() const = 0;
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

class RenderGraph
{
public:
    void Shutdown();

    // Reset graph for new frame (doesn't free physical resources)
    void Reset();
    
    // general purpose transient resource declaration (called during Setup phase)
    // underlying memory is not guaranteed to be retained across the whole frame due to aliasing
    bool DeclareTexture(const RGTextureDesc& desc, RGTextureHandle& outputHandle);
    bool DeclareBuffer(const RGBufferDesc& desc, RGBufferHandle& outputHandle);

    // "persistent" resources are guaranteed to NOT be aliased with any other resource
    // they are ideal for long-term caching of resources across frames
    // they are only freed when the "owner" does not declare them for several frames
    bool DeclarePersistentTexture(const RGTextureDesc& desc, RGTextureHandle& outputHandle);
    bool DeclarePersistentBuffer(const RGBufferDesc& desc, RGBufferHandle& outputHandle);

    // Resource Access Registration (called during Setup phase)
    void ReadTexture(RGTextureHandle handle);
    void WriteTexture(RGTextureHandle handle);
    
    void ReadBuffer(RGBufferHandle handle);
    void WriteBuffer(RGBufferHandle handle);
    
    // Pass management (internal use by render loop)
    // BeginSetup() is called exactly once per frame (after Reset()), before any
    // ScheduleRenderer() calls.  EndSetup() is called once after all renderers
    // have been scheduled, before Compile().
    void BeginSetup();
    void EndSetup();
    void BeginPass(const char* name);
    
    void ScheduleRenderer(class IRenderer* pRenderer);
    
    void Compile();
    void PostRender();
    
    // Resource Retrieval (only valid after Compile and before Cleanup)
    nvrhi::TextureHandle GetTexture(RGTextureHandle handle, RGResourceAccessMode access) const;
    nvrhi::BufferHandle GetBuffer(RGBufferHandle handle, RGResourceAccessMode access) const;

    // Raw retrieval without access validation (only for internal use by render loop, not safe for general use)
    nvrhi::TextureHandle GetTextureRaw(RGTextureHandle handle) const;
    nvrhi::BufferHandle GetBufferRaw(RGBufferHandle handle) const;

    // Returns the 1-based pass index for the named pass as recorded during the last frame's
    // Setup/ScheduleRenderer phase, or 0 if no enabled pass with that name was found.
    // Pass names come directly from IRenderer::GetName(), so they match the renderer's name.
    // Only valid after at least one frame has been rendered (i.e. after ScheduleAndRunAllRenderers).
    uint16_t GetPassIndex(const char* passName) const;
    
    // Set the current active pass for validation (used during Render phase)
    void SetActivePass(uint16_t passIndex);

    // Insert global sync barriers for a given pass into the command list.
    // Must be called at the start of each pass's command list recording, before any GPU work.
    void InsertGlobalSyncBarriers(uint16_t passIndex, nvrhi::ICommandList* commandList) const;

    // Returns the pass index for the current pass (valid after BeginPass, before Compile)
    uint16_t GetCurrentPassIndex() const { return m_CurrentPassIndex; }

    // Debug & Stats
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
    };
    
    void RenderDebugUI();
    std::string ExportToString() const;

    static RGBufferDesc GetSPDAtomicCounterDesc(const char* debugName, uint32_t numElements = 1);
    
private:
    // Generic resource allocation helper (avoids code duplication)
    void AllocateResourcesInternal(bool bIsBuffer, std::function<void(uint32_t, nvrhi::HeapHandle, uint64_t)> createAndBindResource);
    
    // Heap management
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
        std::vector<HeapBlock> m_Blocks;
    };
    std::vector<HeapEntry> m_Heaps;
    nvrhi::HeapHandle CreateHeap(size_t size);
    void SubAllocateResource(RenderGraphInternal::TransientResourceBase* resource, uint64_t alignment);
    void FreeBlock(uint32_t heapIdx, uint64_t blockOffset);
    
    // Helper for updating resource lifetimes
    void UpdateResourceLifetime(RenderGraphInternal::ResourceLifetime& lifetime, uint16_t currentPass);

    // When m_AliasingEnabled is true, records which passes contain at least one aliased resource
    // that needs a global sync barrier before it's first used.
    // Set during Compile() by checking each aliased resource's first pass.
    // Indexed by pass index (1-based, pass 0 is unused).
    std::vector<bool> m_PassNeedsGlobalSyncBarrier;

    // Deferred release: when aliasing replaces a physical resource handle,
    // we must not immediately destroy the old D3D12 object because the GPU
    // may still have work in-flight from the previous frame referencing it.
    // Old handles are moved here and released at the start of the next
    // Compile(), which runs after the previous frame's ExecutePendingCommandLists
    // has synced the GPU (via waitForIdle).
    void FlushDeferredReleases();
    std::vector<nvrhi::TextureHandle> m_DeferredReleaseTextures;
    std::vector<nvrhi::BufferHandle>  m_DeferredReleaseBuffers;
    
private:
    uint16_t GetActivePassIndex() const;

    struct PassAccess
    {
        std::unordered_set<uint32_t> m_ReadTextures;
        std::unordered_set<uint32_t> m_WriteTextures;
        std::unordered_set<uint32_t> m_ReadBuffers;
        std::unordered_set<uint32_t> m_WriteBuffers;
    };
    std::vector<PassAccess> m_PassAccesses;

    std::vector<RenderGraphInternal::TransientTexture> m_Textures;
    std::vector<RenderGraphInternal::TransientBuffer> m_Buffers;
    std::vector<const char*> m_PassNames;

    // Setup state
    bool m_IsInsideSetup = false;
    PassAccess m_PendingPassAccess;
    std::vector<uint32_t> m_PendingDeclaredTextures;
    std::vector<uint32_t> m_PendingDeclaredBuffers;

    Stats m_Stats;
    bool m_AliasingEnabled = true;
    bool m_IsCompiled = false;
    uint16_t m_CurrentPassIndex = 0;
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

public:
    // Exposed for diagnostics only — do not use in production code.
    uint32_t GetForceInvalidateFramesRemaining() const { return m_ForceInvalidateFramesRemaining; }

    // Enable/disable verbose informational logging (pool-reuse, aliasing, eviction …).
    void SetVerboseLogging(bool enabled) { m_bVerboseLogging = enabled; }
    bool IsVerboseLogging() const { return m_bVerboseLogging; }

    const std::vector<RenderGraphInternal::TransientTexture>& GetTextures() const { return m_Textures; }
    const std::vector<RenderGraphInternal::TransientBuffer>& GetBuffers() const { return m_Buffers; }
    const std::vector<HeapEntry>& GetHeaps() const { return m_Heaps; }
    const Stats& GetStats() const { return m_Stats; }
};
