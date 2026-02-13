#pragma once

#include "pch.h"
#include "GraphicRHI.h"

class RenderGraph;

// ============================================================================
// Resource Handles (opaque indices)
// ============================================================================

struct RGTextureHandle
{
    uint32_t m_Index = UINT32_MAX;
    
    bool IsValid() const { return m_Index != UINT32_MAX; }
    void Invalidate() { m_Index = UINT32_MAX; }
    bool operator==(const RGTextureHandle& other) const { return m_Index == other.m_Index; }
    bool operator!=(const RGTextureHandle& other) const { return m_Index != other.m_Index; }
};

struct RGBufferHandle
{
    uint32_t m_Index = UINT32_MAX;
    
    bool IsValid() const { return m_Index != UINT32_MAX; }
    void Invalidate() { m_Index = UINT32_MAX; }
    bool operator==(const RGBufferHandle& other) const { return m_Index == other.m_Index; }
    bool operator!=(const RGBufferHandle& other) const { return m_Index != other.m_Index; }
};

// ============================================================================
// Resource Descriptors
// ============================================================================

struct RGResourceDescBase
{
    virtual size_t ComputeHash() const = 0;
    virtual size_t GetMemorySize() const = 0;

    mutable size_t m_CachedMemorySize = 0;
};

struct RGTextureDesc : public RGResourceDescBase
{
    nvrhi::TextureDesc m_NvrhiDesc;
    
    size_t ComputeHash() const override;
    size_t GetMemorySize() const override;
};

struct RGBufferDesc : public RGResourceDescBase
{
    nvrhi::BufferDesc m_NvrhiDesc;
    
    size_t ComputeHash() const override;
    size_t GetMemorySize() const override;
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
    uint64_t m_LastFrameUsed = 0;
    uint64_t m_Offset = 0;
    uint64_t m_BlockOffset = 0;
    uint32_t m_HeapIndex = UINT32_MAX;
    bool m_IsAllocated = false;
    bool m_IsDeclaredThisFrame = false;
    bool m_IsPhysicalOwner = false;
    nvrhi::HeapHandle m_Heap;

    virtual size_t GetMemorySize() const = 0;
    virtual ~TransientResourceBase() = default;
};

struct TransientTexture : public TransientResourceBase
{
    RGTextureDesc m_Desc;
    nvrhi::TextureHandle m_PhysicalTexture;

    size_t GetMemorySize() const override
    {
        return m_Desc.GetMemorySize();
    }
};

struct TransientBuffer : public TransientResourceBase
{
    RGBufferDesc m_Desc;
    nvrhi::BufferHandle m_PhysicalBuffer;

    size_t GetMemorySize() const override
    {
        return m_Desc.GetMemorySize();
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
    
    // Resource Declaration (called during Setup phase)
    RGTextureHandle DeclareTexture(const RGTextureDesc& desc, RGTextureHandle existing = {});
    RGBufferHandle DeclareBuffer(const RGBufferDesc& desc, RGBufferHandle existing = {});
    
    // Resource Access Registration (called during Setup phase)
    void ReadTexture(RGTextureHandle handle);
    void WriteTexture(RGTextureHandle handle);
    
    void ReadBuffer(RGBufferHandle handle);
    void WriteBuffer(RGBufferHandle handle);
    
    // Pass management (internal use by render loop)
    void BeginPass(const char* name);
    
    // Compilation & Execution
    void Compile();
    void Execute();
    
    // Resource Retrieval (only valid after Compile and before Cleanup)
    nvrhi::TextureHandle GetTexture(RGTextureHandle handle) const;
    nvrhi::BufferHandle GetBuffer(RGBufferHandle handle) const;
    
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
        size_t m_PeakTextureMemory = 0;
        size_t m_PeakBufferMemory = 0;
    };
    
    void RenderDebugUI();
    
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
    
private:
    std::vector<RenderGraphInternal::TransientTexture> m_Textures;
    std::vector<RenderGraphInternal::TransientBuffer> m_Buffers;
    std::vector<const char*> m_PassNames;
    
    Stats m_Stats;
    bool m_AliasingEnabled = false;
    bool m_IsCompiled = false;
    uint16_t m_CurrentPassIndex = 0;
    uint64_t m_FrameIndex = 0;
};

