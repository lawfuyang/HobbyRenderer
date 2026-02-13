#include "RenderGraph.h"
#include "Renderer.h"
#include "Utilities.h"

#include "imgui.h"

using namespace RenderGraphInternal;

template <class T>
inline void hash_combine(std::size_t& seed, const T& v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

size_t RGTextureDesc::ComputeHash() const
{
    size_t seed = 0;
    hash_combine(seed, m_NvrhiDesc.width);
    hash_combine(seed, m_NvrhiDesc.height);
    hash_combine(seed, m_NvrhiDesc.depth);
    hash_combine(seed, m_NvrhiDesc.arraySize);
    hash_combine(seed, m_NvrhiDesc.mipLevels);
    hash_combine(seed, m_NvrhiDesc.sampleCount);
    hash_combine(seed, (uint32_t)m_NvrhiDesc.format);
    hash_combine(seed, m_NvrhiDesc.isRenderTarget);
    hash_combine(seed, m_NvrhiDesc.isUAV);
    hash_combine(seed, m_NvrhiDesc.isTypeless);
    return seed;
}

size_t RGBufferDesc::ComputeHash() const
{
    size_t seed = 0;
    hash_combine(seed, m_NvrhiDesc.byteSize);
    hash_combine(seed, m_NvrhiDesc.structStride);
    hash_combine(seed, (uint32_t)m_NvrhiDesc.format);
    hash_combine(seed, m_NvrhiDesc.canHaveUAVs);
    hash_combine(seed, m_NvrhiDesc.isVertexBuffer);
    hash_combine(seed, m_NvrhiDesc.isIndexBuffer);
    return seed;
}

size_t RGTextureDesc::GetMemorySize() const
{
    if (m_CachedMemorySize == 0)
    {
        nvrhi::TextureDesc tempDesc = m_NvrhiDesc;
        tempDesc.isVirtual = true;
        nvrhi::TextureHandle tempTex = Renderer::GetInstance()->m_RHI->m_NvrhiDevice->createTexture(tempDesc);
        nvrhi::MemoryRequirements memReq = Renderer::GetInstance()->m_RHI->m_NvrhiDevice->getTextureMemoryRequirements(tempTex);
        m_CachedMemorySize = memReq.size;
    }
    return m_CachedMemorySize;
}

size_t RGBufferDesc::GetMemorySize() const
{
    if (m_CachedMemorySize == 0)
    {
        nvrhi::BufferDesc tempDesc = m_NvrhiDesc;
        tempDesc.isVirtual = true;
        nvrhi::BufferHandle tempBuf = Renderer::GetInstance()->m_RHI->m_NvrhiDevice->createBuffer(tempDesc);
        nvrhi::MemoryRequirements memReq = Renderer::GetInstance()->m_RHI->m_NvrhiDevice->getBufferMemoryRequirements(tempBuf);
        m_CachedMemorySize = memReq.size;
    }
    return m_CachedMemorySize;
}

// ============================================================================
// RenderGraph - Resource Declaration
// ============================================================================

void RenderGraph::Shutdown()
{
    m_Textures.clear();
    m_Buffers.clear();
    m_Heaps.clear();
}

void RenderGraph::Reset()
{
    PROFILE_FUNCTION();
    
    const uint32_t kMaxTransientResourceLifetimeFrames = 3;

    m_FrameIndex++;
    m_CurrentPassIndex = 0;
    m_IsCompiled = false;
    m_Stats = Stats{};
    m_PassNames.clear();

    // Mark all resources as not declared this frame and reset lifetimes
    for (TransientTexture& texture : m_Textures)
    {
        texture.m_IsDeclaredThisFrame = false;
        texture.m_Lifetime = {};
        texture.m_AliasedFromIndex = UINT32_MAX;

        // Cleanup physical resources not used for > 3 frames
        if (texture.m_PhysicalTexture && (m_FrameIndex - texture.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            SDL_Log("[RenderGraph] Freeing texture '%s' due to inactivity", texture.m_Desc.m_NvrhiDesc.debugName.c_str());
            if (texture.m_IsPhysicalOwner)
            {
                FreeBlock(texture.m_HeapIndex, texture.m_BlockOffset);
            }
            texture.m_PhysicalTexture = nullptr;
            texture.m_Heap = nullptr;
            texture.m_HeapIndex = UINT32_MAX;
            texture.m_IsAllocated = false;
            texture.m_Desc.m_CachedMemorySize = 0;
        }
    }

    for (TransientBuffer& buffer : m_Buffers)
    {
        buffer.m_IsDeclaredThisFrame = false;
        buffer.m_Lifetime = {};
        buffer.m_AliasedFromIndex = UINT32_MAX;

        if (buffer.m_PhysicalBuffer && (m_FrameIndex - buffer.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            SDL_Log("[RenderGraph] Freeing buffer '%s' due to inactivity", buffer.m_Desc.m_NvrhiDesc.debugName.c_str());
            if (buffer.m_IsPhysicalOwner)
            {
                FreeBlock(buffer.m_HeapIndex, buffer.m_BlockOffset);
            }
            buffer.m_PhysicalBuffer = nullptr;
            buffer.m_Heap = nullptr;
            buffer.m_HeapIndex = UINT32_MAX;
            buffer.m_IsAllocated = false;
            buffer.m_Desc.m_CachedMemorySize = 0;
        }
    }

    // Heaps are kept in the vector for index stability
    for (HeapEntry& heapEntry : m_Heaps)
    {
        if (heapEntry.m_Heap && (m_FrameIndex - heapEntry.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            SDL_Log("[RenderGraph] Freeing heap slot %u due to inactivity", heapEntry.m_HeapIdx);
            heapEntry.m_Heap = nullptr;
            heapEntry.m_Blocks.clear();
            heapEntry.m_Size = 0;
        }
    }
}

void RenderGraph::BeginPass(const char* name)
{
    SDL_assert(name);
    m_CurrentPassIndex++;
    m_PassNames.push_back(name);
}

RGTextureHandle RenderGraph::DeclareTexture(const RGTextureDesc& desc, RGTextureHandle existing)
{
    size_t hash = desc.ComputeHash();

    if (existing.IsValid() && existing.m_Index < m_Textures.size())
    {
        TransientTexture& texture = m_Textures[existing.m_Index];
        
        if (texture.m_Hash != hash)
        {
            SDL_Log("[RenderGraph] Texture desc mismatch for handle %u, freeing old resource", existing.m_Index);
            texture.m_PhysicalTexture = nullptr;
            texture.m_IsAllocated = false;
            texture.m_Desc.m_CachedMemorySize = 0;
        }

        texture.m_Desc = desc;
        texture.m_Hash = hash;
        texture.m_IsDeclaredThisFrame = true;
        texture.m_LastFrameUsed = m_FrameIndex;
        UpdateResourceLifetime(texture.m_Lifetime, m_CurrentPassIndex);
        return existing;
    }

    // Try to find a matching unused resource in the pool
    for (uint32_t i = 0; i < m_Textures.size(); ++i)
    {
        if (!m_Textures[i].m_IsDeclaredThisFrame && m_Textures[i].m_Hash == hash)
        {
            m_Textures[i].m_IsDeclaredThisFrame = true;
            m_Textures[i].m_LastFrameUsed = m_FrameIndex;
            UpdateResourceLifetime(m_Textures[i].m_Lifetime, m_CurrentPassIndex);
            return { i };
        }
    }

    RGTextureHandle handle;
    handle.m_Index = static_cast<uint32_t>(m_Textures.size());
    
    TransientTexture texture;
    texture.m_Desc = desc;
    texture.m_Hash = hash;
    texture.m_IsDeclaredThisFrame = true;
    texture.m_LastFrameUsed = m_FrameIndex;
    UpdateResourceLifetime(texture.m_Lifetime, m_CurrentPassIndex);
    
    m_Textures.push_back(texture);
    
    return handle;
}

RGBufferHandle RenderGraph::DeclareBuffer(const RGBufferDesc& desc, RGBufferHandle existing)
{
    size_t hash = desc.ComputeHash();

    if (existing.IsValid() && existing.m_Index < m_Buffers.size())
    {
        TransientBuffer& buffer = m_Buffers[existing.m_Index];
        
        if (buffer.m_Hash != hash)
        {
            SDL_Log("[RenderGraph] Buffer desc mismatch for handle %u, freeing old resource", existing.m_Index);
            buffer.m_PhysicalBuffer = nullptr;
            buffer.m_IsAllocated = false;
            buffer.m_Desc.m_CachedMemorySize = 0;
        }

        buffer.m_Desc = desc;
        buffer.m_Hash = hash;
        buffer.m_IsDeclaredThisFrame = true;
        buffer.m_LastFrameUsed = m_FrameIndex;
        UpdateResourceLifetime(buffer.m_Lifetime, m_CurrentPassIndex);
        return existing;
    }

    for (uint32_t i = 0; i < m_Buffers.size(); ++i)
    {
        if (!m_Buffers[i].m_IsDeclaredThisFrame && m_Buffers[i].m_Hash == hash)
        {
            m_Buffers[i].m_IsDeclaredThisFrame = true;
            m_Buffers[i].m_LastFrameUsed = m_FrameIndex;
            UpdateResourceLifetime(m_Buffers[i].m_Lifetime, m_CurrentPassIndex);
            return { i };
        }
    }

    RGBufferHandle handle;
    handle.m_Index = static_cast<uint32_t>(m_Buffers.size());
    
    TransientBuffer buffer;
    buffer.m_Desc = desc;
    buffer.m_Hash = hash;
    buffer.m_IsDeclaredThisFrame = true;
    buffer.m_LastFrameUsed = m_FrameIndex;
    UpdateResourceLifetime(buffer.m_Lifetime, m_CurrentPassIndex);
    
    m_Buffers.push_back(buffer);
    
    return handle;
}

// ============================================================================
// RenderGraph - Resource Access
// ============================================================================

void RenderGraph::ReadTexture(RGTextureHandle handle)
{
    if (!handle.IsValid() || handle.m_Index >= m_Textures.size())
    {
        SDL_assert(false && "Invalid texture handle");
        return;
    }
    
    TransientTexture& texture = m_Textures[handle.m_Index];
    if (!texture.m_IsDeclaredThisFrame)
    {
        SDL_Log("[RenderGraph] ERROR: Attempting to read undeclared texture '%s' (handle index %u)", texture.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Texture not declared this frame");
        return;
    }

    UpdateResourceLifetime(texture.m_Lifetime, m_CurrentPassIndex);
}

void RenderGraph::WriteTexture(RGTextureHandle handle)
{
    if (!handle.IsValid() || handle.m_Index >= m_Textures.size())
    {
        SDL_assert(false && "Invalid texture handle");
        return;
    }
    
    TransientTexture& texture = m_Textures[handle.m_Index];
    if (!texture.m_IsDeclaredThisFrame)
    {
        SDL_Log("[RenderGraph] ERROR: Attempting to write undeclared texture '%s' (handle index %u)", texture.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Texture not declared this frame");
        return;
    }

    UpdateResourceLifetime(texture.m_Lifetime, m_CurrentPassIndex);
}

void RenderGraph::ReadBuffer(RGBufferHandle handle)
{
    if (!handle.IsValid() || handle.m_Index >= m_Buffers.size())
    {
        SDL_assert(false && "Invalid buffer handle");
        return;
    }
    
    TransientBuffer& buffer = m_Buffers[handle.m_Index];
    if (!buffer.m_IsDeclaredThisFrame)
    {
        SDL_Log("[RenderGraph] ERROR: Attempting to read undeclared buffer '%s' (handle index %u)", buffer.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Buffer not declared this frame");
        return;
    }

    UpdateResourceLifetime(buffer.m_Lifetime, m_CurrentPassIndex);
}

void RenderGraph::WriteBuffer(RGBufferHandle handle)
{
    if (!handle.IsValid() || handle.m_Index >= m_Buffers.size())
    {
        SDL_assert(false && "Invalid buffer handle");
        return;
    }
    
    TransientBuffer& buffer = m_Buffers[handle.m_Index];
    if (!buffer.m_IsDeclaredThisFrame)
    {
        SDL_Log("[RenderGraph] ERROR: Attempting to write undeclared buffer '%s' (handle index %u)", buffer.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Buffer not declared this frame");
        return;
    }

    UpdateResourceLifetime(buffer.m_Lifetime, m_CurrentPassIndex);
}

// ============================================================================
// RenderGraph - Compilation
// ============================================================================

void RenderGraph::UpdateResourceLifetime(RenderGraphInternal::ResourceLifetime& lifetime, uint16_t currentPass)
{
    if (lifetime.m_FirstPass == UINT16_MAX)
    {
        lifetime.m_FirstPass = currentPass;
    }
    else
    {
        lifetime.m_FirstPass = std::min(lifetime.m_FirstPass, currentPass);
    }
    lifetime.m_LastPass = std::max(lifetime.m_LastPass, currentPass);
}

void RenderGraph::Compile()
{
    PROFILE_FUNCTION();

    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();

    m_Stats.m_NumTextures = m_Stats.m_NumBuffers = 0;
    for (const TransientTexture& tex : m_Textures) if (tex.m_IsDeclaredThisFrame) m_Stats.m_NumTextures++;
    for (const TransientBuffer& buf : m_Buffers) if (buf.m_IsDeclaredThisFrame) m_Stats.m_NumBuffers++;

    AllocateResourcesInternal(
        false,  // bIsBuffer
        [this, device](uint32_t idx, nvrhi::HeapHandle heap, uint64_t offset)
        {
            TransientTexture& texture = m_Textures[idx];

            if (texture.m_PhysicalTexture && texture.m_Heap == heap && texture.m_Offset == offset)
            {
                // Already bound correctly to the right heap at the right offset
                texture.m_IsPhysicalOwner = true;
                return;
            }

            PROFILE_SCOPED("CreateTextureAndBindMemory");

            texture.m_Desc.m_NvrhiDesc.isVirtual = true;
            texture.m_PhysicalTexture = device->createTexture(texture.m_Desc.m_NvrhiDesc);
            device->bindTextureMemory(texture.m_PhysicalTexture, heap, offset);
            texture.m_Heap = heap;
            texture.m_Offset = offset;
            texture.m_IsPhysicalOwner = true;
        }
    );

    AllocateResourcesInternal(
        true,  // bIsBuffer
        [this, device](uint32_t idx, nvrhi::HeapHandle heap, uint64_t offset)
        {
            TransientBuffer& buffer = m_Buffers[idx];

            if (buffer.m_PhysicalBuffer && buffer.m_Heap == heap && buffer.m_Offset == offset)
            {
                // Already bound correctly to the right heap at the right offset
                buffer.m_IsPhysicalOwner = true;
                return;
            }

            PROFILE_SCOPED("CreateBufferAndBindMemory");

            buffer.m_Desc.m_NvrhiDesc.isVirtual = true;
            buffer.m_PhysicalBuffer = device->createBuffer(buffer.m_Desc.m_NvrhiDesc);
            device->bindBufferMemory(buffer.m_PhysicalBuffer, heap, offset);
            buffer.m_Heap = heap;
            buffer.m_Offset = offset;
            buffer.m_IsPhysicalOwner = true;
        }
    );

    m_IsCompiled = true;
}

// ============================================================================
// RenderGraph - Memory Management
// ============================================================================

nvrhi::HeapHandle RenderGraph::CreateHeap(size_t size)
{
    PROFILE_FUNCTION();

    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();
    
    nvrhi::HeapDesc heapDesc;
    heapDesc.capacity = size;
    heapDesc.debugName = "RenderGraph Managed Heap";
    heapDesc.type = nvrhi::HeapType::DeviceLocal;
    
    nvrhi::HeapHandle heap = device->createHeap(heapDesc);

    // Try to reuse an empty slot
    for (uint32_t i = 0; i < m_Heaps.size(); ++i)
    {
        if (!m_Heaps[i].m_Heap)
        {
            m_Heaps[i].m_Heap = heap;
            m_Heaps[i].m_Size = size;
            m_Heaps[i].m_LastFrameUsed = m_FrameIndex;
            m_Heaps[i].m_HeapIdx = i;
            
            HeapBlock block;
            block.m_Offset = 0;
            block.m_Size = size;
            block.m_IsFree = true;
            m_Heaps[i].m_Blocks.push_back(block);
            
            SDL_Log("[RenderGraph] Reused heap slot %u for new heap of size %.2f MB", i, size / (1024.0 * 1024.0));
            return heap;
        }
    }

    HeapEntry entry;
    entry.m_Heap = heap;
    entry.m_Size = size;
    entry.m_LastFrameUsed = m_FrameIndex;
    entry.m_HeapIdx = static_cast<uint32_t>(m_Heaps.size());
    
    HeapBlock block;
    block.m_Offset = 0;
    block.m_Size = size;
    block.m_IsFree = true;
    entry.m_Blocks.push_back(block);
    
    m_Heaps.push_back(entry);
    
    SDL_Log("[RenderGraph] Allocated new heap of size %.2f MB", size / (1024.0 * 1024.0));
    
    return heap;
}

void RenderGraph::SubAllocateResource(RenderGraphInternal::TransientResourceBase* resource, uint64_t alignment)
{
    size_t size = resource->GetMemorySize();

    // 1. Try to find a free block in existing heaps
    for (HeapEntry& heapEntry : m_Heaps)
    {
        if (!heapEntry.m_Heap) continue;

        for (size_t i = 0; i < heapEntry.m_Blocks.size(); ++i)
        {
            HeapBlock& block = heapEntry.m_Blocks[i];
            if (block.m_IsFree)
            {
                uint64_t alignedOffset = (block.m_Offset + alignment - 1) & ~(alignment - 1);
                uint64_t blockEnd = block.m_Offset + block.m_Size;

                if (alignedOffset + size <= blockEnd)
                {
                    uint64_t blockOriginalOffset = block.m_Offset;
                    uint64_t blockOriginalSize = block.m_Size;

                    // Prefix block if needed
                    if (alignedOffset > blockOriginalOffset)
                    {
                        HeapBlock prefix;
                        prefix.m_Offset = blockOriginalOffset;
                        prefix.m_Size = alignedOffset - blockOriginalOffset;
                        prefix.m_IsFree = true;
                        heapEntry.m_Blocks.insert(heapEntry.m_Blocks.begin() + i, prefix);
                        i++; // Current block is now at i+1
                    }

                    heapEntry.m_Blocks[i].m_Offset = alignedOffset;
                    heapEntry.m_Blocks[i].m_Size = size;
                    heapEntry.m_Blocks[i].m_IsFree = false;

                    resource->m_Heap = heapEntry.m_Heap;
                    resource->m_HeapIndex = heapEntry.m_HeapIdx;
                    resource->m_Offset = alignedOffset;
                    resource->m_BlockOffset = alignedOffset;

                    // Suffix block if needed
                    uint64_t suffixStart = alignedOffset + size;
                    if (suffixStart < blockEnd)
                    {
                        HeapBlock suffix;
                        suffix.m_Offset = suffixStart;
                        suffix.m_Size = blockEnd - suffixStart;
                        suffix.m_IsFree = true;
                        heapEntry.m_Blocks.insert(heapEntry.m_Blocks.begin() + i + 1, suffix);
                    }

                    heapEntry.m_LastFrameUsed = m_FrameIndex;
                    return;
                }
            }
        }
    }

    // 2. No fit found, create a new heap
    size_t heapSize = std::max(size_t(1024 * 1024), size + alignment);
    CreateHeap(heapSize);

    // Retry allocation now that we have a new heap
    SubAllocateResource(resource, alignment);
}

void RenderGraph::FreeBlock(uint32_t heapIdx, uint64_t blockOffset)
{
    if (heapIdx >= m_Heaps.size()) return;

    HeapEntry& heapEntry = m_Heaps[heapIdx];
    for (size_t i = 0; i < heapEntry.m_Blocks.size(); ++i)
    {
        if (heapEntry.m_Blocks[i].m_Offset == blockOffset)
        {
            heapEntry.m_Blocks[i].m_IsFree = true;

            // Coalesce with next block if free
            if (i + 1 < heapEntry.m_Blocks.size() && heapEntry.m_Blocks[i + 1].m_IsFree)
            {
                heapEntry.m_Blocks[i].m_Size += heapEntry.m_Blocks[i + 1].m_Size;
                heapEntry.m_Blocks.erase(heapEntry.m_Blocks.begin() + i + 1);
            }

            // Coalesce with previous block if free
            if (i > 0 && heapEntry.m_Blocks[i - 1].m_IsFree)
            {
                heapEntry.m_Blocks[i - 1].m_Size += heapEntry.m_Blocks[i].m_Size;
                heapEntry.m_Blocks.erase(heapEntry.m_Blocks.begin() + i);
            }

            return;
        }
    }
}

// ============================================================================
// RenderGraph - Resource Aliasing & Allocation
// ============================================================================

// Helper for generic resource allocation - abstracts over textures and buffers
void RenderGraph::AllocateResourcesInternal(bool bIsBuffer, std::function<void(uint32_t, nvrhi::HeapHandle, uint64_t)> createAndBindResource)
{
    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();

    std::vector<uint32_t> sortedIndices;
    for (uint32_t i = 0; i < (bIsBuffer ? m_Buffers.size() : m_Textures.size()); ++i)
    {
        if ((bIsBuffer ? m_Buffers[i].m_IsDeclaredThisFrame : m_Textures[i].m_IsDeclaredThisFrame) &&
            (bIsBuffer ? m_Buffers[i].m_Lifetime.IsValid() : m_Textures[i].m_Lifetime.IsValid()))
        {
            sortedIndices.push_back(i);
        }
    }

    std::sort(sortedIndices.begin(), sortedIndices.end(), [this, bIsBuffer](uint32_t a, uint32_t b) {
        return (bIsBuffer ? m_Buffers[a].m_Lifetime.m_FirstPass : m_Textures[a].m_Lifetime.m_FirstPass) <
            (bIsBuffer ? m_Buffers[b].m_Lifetime.m_FirstPass : m_Textures[b].m_Lifetime.m_FirstPass);
        });
    
    // Attempt aliasing and allocate
    for (uint32_t idx : sortedIndices)
    {
        TransientResourceBase* resource = bIsBuffer ? (TransientResourceBase*)&m_Buffers[idx] : (TransientResourceBase*)&m_Textures[idx];
        
        // Trivial reuse: if already allocated and was an owner, skip logic
        if (resource->m_IsAllocated && resource->m_IsPhysicalOwner && resource->m_HeapIndex != UINT32_MAX)
        {
            HeapEntry& heapEntry = m_Heaps[resource->m_HeapIndex];
            SDL_assert(heapEntry.m_Heap == resource->m_Heap);

            heapEntry.m_LastFrameUsed = m_FrameIndex;

            if (bIsBuffer) m_Stats.m_NumAllocatedBuffers++;
            else m_Stats.m_NumAllocatedTextures++;

            const size_t bufferMemory = bIsBuffer ? m_Buffers.at(idx).m_Desc.GetMemorySize() : 0;
            const size_t textureMemory = !bIsBuffer ? m_Textures.at(idx).m_Desc.GetMemorySize() : 0;

            m_Stats.m_TotalBufferMemory += bufferMemory;
            m_Stats.m_TotalTextureMemory += textureMemory;
            continue;
        }

        bool aliased = false;
        if (m_AliasingEnabled)
        {
            for (uint32_t candidateIdx : sortedIndices)
            {
                if (candidateIdx == idx) break;
                TransientResourceBase* candidate = bIsBuffer ? (TransientResourceBase*)&m_Buffers[candidateIdx] : (TransientResourceBase*)&m_Textures[candidateIdx];
                
                if (!candidate->m_IsAllocated)
                    continue;

                bool bCanAlias = !candidate->m_Lifetime.Overlaps(resource->m_Lifetime);
                if (bCanAlias)
                {
                    bCanAlias = resource->GetMemorySize() <= candidate->GetMemorySize();
                }
                
                if (bCanAlias)
                {
                    if (bIsBuffer)
                    {
                        m_Buffers[idx].m_PhysicalBuffer = m_Buffers[candidateIdx].m_PhysicalBuffer;
                        m_Stats.m_NumAliasedBuffers++;
                    }
                    else
                    {
                        m_Textures[idx].m_PhysicalTexture = m_Textures[candidateIdx].m_PhysicalTexture;
                        m_Stats.m_NumAliasedTextures++;
                    }
                    resource->m_Heap = candidate->m_Heap;
                    resource->m_Offset = candidate->m_Offset;
                    resource->m_BlockOffset = candidate->m_BlockOffset;
                    resource->m_AliasedFromIndex = candidateIdx;
                    resource->m_IsAllocated = true;
                    resource->m_IsPhysicalOwner = false;
                    aliased = true;
                    break;
                }
            }
        }
        
        if (!aliased)
        {
            const uint64_t alignment = bIsBuffer ? Renderer::GetInstance()->m_RHI->GetBufferAlignment() : Renderer::GetInstance()->m_RHI->GetTextureAlignment();
            SubAllocateResource(resource, alignment);
            createAndBindResource(idx, resource->m_Heap, resource->m_Offset);
            resource->m_IsAllocated = true;
            if (bIsBuffer)
                m_Stats.m_NumAllocatedBuffers++;
            else
                m_Stats.m_NumAllocatedTextures++;

            const size_t bufferMemory = bIsBuffer ? m_Buffers.at(idx).m_Desc.GetMemorySize() : 0;
            const size_t textureMemory = !bIsBuffer ? m_Textures.at(idx).m_Desc.GetMemorySize() : 0;

            m_Stats.m_TotalBufferMemory += bufferMemory;
            m_Stats.m_TotalTextureMemory += textureMemory;
        }
    }
    
    // Memory events for peak calculation
    std::vector<std::pair<uint16_t, int64_t>> memoryEvents;
    for (uint32_t idx : sortedIndices)
    {
        TransientResourceBase* resource = bIsBuffer ? (TransientResourceBase*)&m_Buffers[idx] : (TransientResourceBase*)&m_Textures[idx];
        if (!resource->m_IsAllocated || resource->m_AliasedFromIndex != UINT32_MAX)
            continue;
        
        const size_t size = bIsBuffer ? m_Buffers.at(idx).m_Desc.GetMemorySize() : m_Textures.at(idx).m_Desc.GetMemorySize();
        memoryEvents.push_back({ resource->m_Lifetime.m_FirstPass, static_cast<int64_t>(size) });
        memoryEvents.push_back({ static_cast<uint16_t>(resource->m_Lifetime.m_LastPass + 1), -static_cast<int64_t>(size) });
    }
    
    std::sort(memoryEvents.begin(), memoryEvents.end());
    
    size_t currentMemory = 0;
    size_t& peakMemory = bIsBuffer ? m_Stats.m_PeakBufferMemory : m_Stats.m_PeakTextureMemory;
    peakMemory = 0;
    for (const std::pair<uint16_t, int64_t>& event : memoryEvents)
    {
        currentMemory += event.second;
        peakMemory = std::max(peakMemory, currentMemory);
    }
}

// ============================================================================
// RenderGraph - Execution & Cleanup
// ============================================================================

void RenderGraph::Execute()
{
    SDL_assert(m_IsCompiled && "RenderGraph must be compiled before execution");
    // Physical resources are already allocated in Compile()
    // This is here for future extensions (e.g., resource barriers)
}

// ============================================================================
// RenderGraph - Resource Retrieval
// ============================================================================

nvrhi::TextureHandle RenderGraph::GetTexture(RGTextureHandle handle) const
{
    if (!handle.IsValid() || handle.m_Index >= m_Textures.size())
    {
        SDL_assert(false && "Invalid texture handle");
        return nullptr;
    }
    
    const TransientTexture& texture = m_Textures[handle.m_Index];
    if (!texture.m_IsDeclaredThisFrame)
    {
        SDL_Log("[RenderGraph] ERROR: Attempting to get undeclared texture '%s' (handle index %u)", texture.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Texture not declared this frame");
        return nullptr;
    }
    SDL_assert(texture.m_PhysicalTexture);
    SDL_assert(texture.m_IsAllocated && "Texture not allocated");
    
    return texture.m_PhysicalTexture;
}

nvrhi::BufferHandle RenderGraph::GetBuffer(RGBufferHandle handle) const
{
    if (!handle.IsValid() || handle.m_Index >= m_Buffers.size())
    {
        SDL_assert(false && "Invalid buffer handle");
        return nullptr;
    }
    
    const TransientBuffer& buffer = m_Buffers[handle.m_Index];
    if (!buffer.m_IsDeclaredThisFrame)
    {
        SDL_Log("[RenderGraph] ERROR: Attempting to get undeclared buffer '%s' (handle index %u)", buffer.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Buffer not declared this frame");
        return nullptr;
    }
    SDL_assert(buffer.m_PhysicalBuffer);
    SDL_assert(buffer.m_IsAllocated && "Buffer not allocated");
    
    return buffer.m_PhysicalBuffer;
}

// ============================================================================
// RenderGraph - Debug UI
// ============================================================================

void RenderGraph::RenderDebugUI()
{
    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();
    if (ImGui::TreeNode("Render Graph"))
    {
        ImGui::Text("Textures: %u (Allocated: %u, Aliased: %u)", 
                   m_Stats.m_NumTextures, 
                   m_Stats.m_NumAllocatedTextures, 
                   m_Stats.m_NumAliasedTextures);
        
        ImGui::Text("Buffers: %u (Allocated: %u, Aliased: %u)", 
                   m_Stats.m_NumBuffers, 
                   m_Stats.m_NumAllocatedBuffers, 
                   m_Stats.m_NumAliasedBuffers);
        
        ImGui::Text("Texture Memory: %.2f MB (Peak: %.2f MB)", 
                   m_Stats.m_TotalTextureMemory / (1024.0 * 1024.0),
                   m_Stats.m_PeakTextureMemory / (1024.0 * 1024.0));
        
        ImGui::Text("Buffer Memory: %.2f MB (Peak: %.2f MB)", 
                   m_Stats.m_TotalBufferMemory / (1024.0 * 1024.0),
                   m_Stats.m_PeakBufferMemory / (1024.0 * 1024.0));
        
        ImGui::Separator();
        
        if (ImGui::TreeNode("Lifetime Visualization"))
        {
            const float kPassWidth = 25.0f;
            const float kRowHeight = 22.0f;
            const float kNameWidth = 200.0f;
            
            uint32_t numPasses = (uint32_t)m_PassNames.size();
            
            if (numPasses > 0)
            {
                ImGui::BeginChild("LifetimeScroll", ImVec2(0, 400), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize, ImGuiWindowFlags_HorizontalScrollbar);
                
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 startPos = ImGui::GetCursorScreenPos();
                
                // Header - Pass names (vertical or angled if many)
                float headerHeight = 100.0f;
                ImGui::Dummy(ImVec2(kNameWidth + numPasses * kPassWidth, headerHeight));
                
                for (uint32_t i = 0; i < numPasses; ++i)
                {
                    ImVec2 textPos = ImVec2(startPos.x + kNameWidth + i * kPassWidth + kPassWidth * 0.5f, startPos.y + headerHeight - 5.0f);
                    // Draw pass number and name rotated or vertically
                    char passLabel[16];
                    sprintf(passLabel, "%u", i + 1);
                    drawList->AddText(ImVec2(textPos.x - 5, startPos.y), ImGui::GetColorU32(ImGuiCol_Text), passLabel);
                    
                    // Simple vertical line
                    drawList->AddLine(ImVec2(startPos.x + kNameWidth + i * kPassWidth, startPos.y + 20), 
                                      ImVec2(startPos.x + kNameWidth + i * kPassWidth, startPos.y + 1000), 
                                      ImGui::GetColorU32(ImGuiCol_Separator, 0.5f));
                }

                auto drawResourceRow = [&](const auto& resource, const char* typeName, ImU32 color)
                {
                    if (!resource.m_IsDeclaredThisFrame || !resource.m_Lifetime.IsValid())
                        return;

                    ImVec2 rowStart = ImGui::GetCursorScreenPos();
                    const std::string& name = resource.m_Desc.m_NvrhiDesc.debugName;
                    
                    ImGui::Text("%s", name.c_str());
                    
                    float barStart = kNameWidth + (resource.m_Lifetime.m_FirstPass - 1) * kPassWidth;
                    float barEnd = kNameWidth + (resource.m_Lifetime.m_LastPass) * kPassWidth;
                    
                    ImVec2 pMin = ImVec2(rowStart.x + barStart, rowStart.y);
                    ImVec2 pMax = ImVec2(rowStart.x + barEnd, rowStart.y + kRowHeight - 2.0f);
                    
                    drawList->AddRectFilled(pMin, pMax, color, 4.0f);
                    if (resource.m_AliasedFromIndex != UINT32_MAX)
                    {
                         drawList->AddRect(pMin, pMax, ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), 4.0f, 0, 2.0f);
                    }

                    ImGui::SetCursorScreenPos(rowStart);
                    ImGui::InvisibleButton(name.c_str(), ImVec2(kNameWidth + numPasses * kPassWidth, kRowHeight));
                    
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("Resource: %s", name.c_str());
                        ImGui::Text("Type: %s", typeName);
                        ImGui::Text("Size: %.2f MB", resource.m_Desc.GetMemorySize() / (1024.0 * 1024.0));
                        ImGui::Text("Lifetime: Pass %u to %u", resource.m_Lifetime.m_FirstPass, resource.m_Lifetime.m_LastPass);
                        
                        if (resource.m_Lifetime.m_FirstPass > 0 && resource.m_Lifetime.m_FirstPass <= m_PassNames.size())
                            ImGui::Text("First Pass: %s", m_PassNames[resource.m_Lifetime.m_FirstPass - 1]);
                        
                        if (resource.m_Lifetime.m_LastPass > 0 && resource.m_Lifetime.m_LastPass <= m_PassNames.size())
                            ImGui::Text("Last Pass: %s", m_PassNames[resource.m_Lifetime.m_LastPass - 1]);

                        ImGui::Text("Heap Index: %d", resource.m_HeapIndex != UINT32_MAX ? (int)resource.m_HeapIndex : -1);
                        ImGui::Text("Offset: %llu", resource.m_Offset);
                        
                        if (resource.m_AliasedFromIndex != UINT32_MAX)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
                            ImGui::Text("Aliased from: index %u", resource.m_AliasedFromIndex);
                            ImGui::PopStyleColor();
                        }
                        else
                        {
                            ImGui::Text("Physical Owner: %s", resource.m_IsPhysicalOwner ? "Yes" : "No");
                        }
                        
                        ImGui::EndTooltip();
                    }
                };

                for (const auto& tex : m_Textures)
                    drawResourceRow(tex, "Texture", ImGui::GetColorU32(ImVec4(0.2f, 0.5f, 0.8f, 0.8f)));

                for (const auto& buf : m_Buffers)
                    drawResourceRow(buf, "Buffer", ImGui::GetColorU32(ImVec4(0.2f, 0.7f, 0.3f, 0.8f)));

                ImGui::EndChild();
            }
            else
            {
                ImGui::Text("No passes recorded.");
            }
            
            ImGui::TreePop();
        }

        ImGui::Separator();
        
        bool aliasingEnabled = m_AliasingEnabled;
        if (ImGui::Checkbox("Enable Aliasing", &aliasingEnabled))
        {
            m_AliasingEnabled = aliasingEnabled;
        }
        
        ImGui::Separator();
        
        if (ImGui::TreeNode("Textures"))
        {
            if (ImGui::BeginTable("Textures", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("Format");
                ImGui::TableSetupColumn("First Pass");
                ImGui::TableSetupColumn("Last Pass");
                ImGui::TableSetupColumn("Memory (MB)");
                ImGui::TableSetupColumn("Offset");
                ImGui::TableSetupColumn("Aliased From");
                ImGui::TableHeadersRow();
                
                for (uint32_t i = 0; i < m_Textures.size(); ++i)
                {
                    const TransientTexture& texture = m_Textures[i];
                    
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", texture.m_Desc.m_NvrhiDesc.debugName.c_str());
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%ux%u", texture.m_Desc.m_NvrhiDesc.width, texture.m_Desc.m_NvrhiDesc.height);
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", nvrhi::utils::FormatToString(texture.m_Desc.m_NvrhiDesc.format));
                    
                    ImGui::TableNextColumn();
                    if (texture.m_Lifetime.IsValid())
                        ImGui::Text("%u", texture.m_Lifetime.m_FirstPass);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    if (texture.m_Lifetime.IsValid())
                        ImGui::Text("%u", texture.m_Lifetime.m_LastPass);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", texture.m_Desc.GetMemorySize() / (1024.0 * 1024.0));

                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", texture.m_Offset);
                    
                    ImGui::TableNextColumn();
                    if (texture.m_AliasedFromIndex != UINT32_MAX)
                        ImGui::Text("%s", m_Textures[texture.m_AliasedFromIndex].m_Desc.m_NvrhiDesc.debugName.c_str());
                    else
                        ImGui::Text("-");
                }
                
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        
        if (ImGui::TreeNode("Buffers"))
        {
            if (ImGui::BeginTable("Buffers", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Size (MB)");
                ImGui::TableSetupColumn("First Pass");
                ImGui::TableSetupColumn("Last Pass");
                ImGui::TableSetupColumn("Offset");
                ImGui::TableSetupColumn("Aliased From");
                ImGui::TableHeadersRow();
                
                for (uint32_t i = 0; i < m_Buffers.size(); ++i)
                {
                    const TransientBuffer& buffer = m_Buffers[i];
                    
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", buffer.m_Desc.m_NvrhiDesc.debugName.c_str());
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", buffer.m_Desc.GetMemorySize() / (1024.0 * 1024.0));
                    
                    ImGui::TableNextColumn();
                    if (buffer.m_Lifetime.IsValid())
                        ImGui::Text("%u", buffer.m_Lifetime.m_FirstPass);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    if (buffer.m_Lifetime.IsValid())
                        ImGui::Text("%u", buffer.m_Lifetime.m_LastPass);
                    else
                        ImGui::Text("N/A");

                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", buffer.m_Offset);
                    
                    ImGui::TableNextColumn();
                    if (buffer.m_AliasedFromIndex != UINT32_MAX)
                        ImGui::Text("%s", m_Buffers[buffer.m_AliasedFromIndex].m_Desc.m_NvrhiDesc.debugName.c_str());
                    else
                        ImGui::Text("-");
                }
                
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Heaps"))
        {
            for (size_t i = 0; i < m_Heaps.size(); ++i)
            {
                const HeapEntry& heap = m_Heaps[i];
                if (!heap.m_Heap) continue;

                if (ImGui::TreeNode((void*)(intptr_t)i, "Heap %zu (%.2f MB)", i, heap.m_Size / (1024.0 * 1024.0)))
                {
                    if (ImGui::BeginTable("HeapBlocks", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableSetupColumn("Offset");
                        ImGui::TableSetupColumn("Size");
                        ImGui::TableSetupColumn("Status");
                        ImGui::TableHeadersRow();
                        for (const HeapBlock& block : heap.m_Blocks)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%llu", block.m_Offset);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f KB", block.m_Size / 1024.0);
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", block.m_IsFree ? "Free" : "Allocated");
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        
        ImGui::TreePop();
    }
}
