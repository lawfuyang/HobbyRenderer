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
    const uint32_t kMaxTransientResourceLifetimeFrames = 3;

    m_FrameIndex++;
    m_CurrentPassIndex = 0;
    m_IsCompiled = false;
    m_Stats = Stats{};

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
            texture.m_PhysicalTexture = nullptr;
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
            buffer.m_PhysicalBuffer = nullptr;
            buffer.m_IsAllocated = false;
            buffer.m_Desc.m_CachedMemorySize = 0;
        }
    }

    // Heaps can be fully removed from vector since they aren't indexed by external handles
    auto itHeap = m_Heaps.begin();
    while (itHeap != m_Heaps.end())
    {
        if (m_FrameIndex - itHeap->m_LastFrameUsed > kMaxTransientResourceLifetimeFrames)
        {
            itHeap = m_Heaps.erase(itHeap);
        }
        else
        {
            ++itHeap;
        }
    }
}

void RenderGraph::BeginPass()
{
    m_CurrentPassIndex++;
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

    ComputeLifetimes();
    AllocateTextures();
    AllocateBuffers();
    
    m_IsCompiled = true;
}

void RenderGraph::ComputeLifetimes()
{
    // Lifetimes are already computed during resource access registration
    // This is a no-op, but kept for future extensions
}

// ============================================================================
// RenderGraph - Memory Management
// ============================================================================

nvrhi::HeapHandle RenderGraph::GetOrCreateHeap(size_t size, const std::string& debugName)
{
    PROFILE_FUNCTION();

    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();
    int bestHeapIdx = -1;
    size_t bestSize = std::numeric_limits<size_t>::max();
    
    for (int i = 0; i < (int)m_Heaps.size(); ++i)
    {
        // Find best fitting heap that hasn't been used this frame
        if (m_Heaps[i].m_Size >= size && m_Heaps[i].m_Size < bestSize && m_Heaps[i].m_LastFrameUsed < m_FrameIndex)
        {
            bestSize = m_Heaps[i].m_Size;
            bestHeapIdx = i;
        }
    }
    
    if (bestHeapIdx != -1)
    {
        m_Heaps[bestHeapIdx].m_LastFrameUsed = m_FrameIndex;
        return m_Heaps[bestHeapIdx].m_Heap;
    }
    
    nvrhi::HeapDesc heapDesc;
    heapDesc.capacity = size;
    heapDesc.debugName = debugName;
    heapDesc.type = nvrhi::HeapType::DeviceLocal;
    
    HeapEntry entry;
    entry.m_Heap = device->createHeap(heapDesc);
    entry.m_Size = size;
    entry.m_LastFrameUsed = m_FrameIndex;
    m_Heaps.push_back(entry);
    
    SDL_Log("[RenderGraph] Allocated new heap of size %.2f MB", size / (1024.0 * 1024.0));
    
    return entry.m_Heap;
}

void RenderGraph::AllocateTextures()
{
    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();
    
    m_Stats.m_NumTextures = 0;
    for (const TransientTexture& tex : m_Textures) if (tex.m_IsDeclaredThisFrame) m_Stats.m_NumTextures++;
    
    AllocateResourcesInternal(
        false,  // bIsBuffer
        [this, device](uint32_t idx)
        {
            TransientTexture& texture = m_Textures[idx];
            texture.m_Desc.m_NvrhiDesc.isVirtual = true;
            texture.m_PhysicalTexture = device->createTexture(texture.m_Desc.m_NvrhiDesc);
            
            nvrhi::HeapHandle heap = GetOrCreateHeap(texture.m_Desc.GetMemorySize(), texture.m_Desc.m_NvrhiDesc.debugName);
            device->bindTextureMemory(texture.m_PhysicalTexture, heap, 0);
        }
    );
}

void RenderGraph::AllocateBuffers()
{
    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();

    m_Stats.m_NumBuffers = 0;
    for (const TransientBuffer& buf : m_Buffers) if (buf.m_IsDeclaredThisFrame) m_Stats.m_NumBuffers++;

    AllocateResourcesInternal(
        true,  // bIsBuffer
        [this, device](uint32_t idx)
        {
            TransientBuffer& buffer = m_Buffers[idx];
            buffer.m_Desc.m_NvrhiDesc.isVirtual = true;
            buffer.m_PhysicalBuffer = device->createBuffer(buffer.m_Desc.m_NvrhiDesc);

            nvrhi::MemoryRequirements memReq = device->getBufferMemoryRequirements(buffer.m_PhysicalBuffer);
            nvrhi::HeapHandle heap = GetOrCreateHeap(memReq.size, buffer.m_Desc.m_NvrhiDesc.debugName);
            device->bindBufferMemory(buffer.m_PhysicalBuffer, heap, 0);
        }
    );
}

// ============================================================================
// RenderGraph - Resource Aliasing & Allocation
// ============================================================================

// Helper for generic resource allocation - abstracts over textures and buffers
void RenderGraph::AllocateResourcesInternal(bool bIsBuffer, std::function<void(uint32_t)> createAndBindResource)
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
                    resource->m_AliasedFromIndex = candidateIdx;
                    resource->m_IsAllocated = true;
                    aliased = true;
                    break;
                }
            }
        }
        
        if (!aliased)
        {
            createAndBindResource(idx);
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

void RenderGraph::Cleanup()
{
    // Transient resources are freed when their handles go out of scope
    // We keep the allocations for next frame (deterministic reuse)
    m_IsCompiled = false;
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
        
        bool aliasingEnabled = m_AliasingEnabled;
        if (ImGui::Checkbox("Enable Aliasing", &aliasingEnabled))
        {
            m_AliasingEnabled = aliasingEnabled;
        }
        
        ImGui::Separator();
        
        if (ImGui::TreeNode("Textures"))
        {
            if (ImGui::BeginTable("Textures", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("Format");
                ImGui::TableSetupColumn("First Pass");
                ImGui::TableSetupColumn("Last Pass");
                ImGui::TableSetupColumn("Memory (MB)");
                ImGui::TableSetupColumn("Aliased From");
                ImGui::TableHeadersRow();
                
                for (uint32_t i = 0; i < m_Textures.size(); ++i)
                {
                    const TransientTexture& texture = m_Textures[i];
                    
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", texture.m_Desc.m_NvrhiDesc.debugName);
                    
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
                    if (texture.m_AliasedFromIndex != UINT32_MAX)
                        ImGui::Text("%s", m_Textures[texture.m_AliasedFromIndex].m_Desc.m_NvrhiDesc.debugName);
                    else
                        ImGui::Text("-");
                }
                
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        
        if (ImGui::TreeNode("Buffers"))
        {
            if (ImGui::BeginTable("Buffers", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Size (MB)");
                ImGui::TableSetupColumn("First Pass");
                ImGui::TableSetupColumn("Last Pass");
                ImGui::TableSetupColumn("Aliased From");
                ImGui::TableHeadersRow();
                
                for (uint32_t i = 0; i < m_Buffers.size(); ++i)
                {
                    const TransientBuffer& buffer = m_Buffers[i];
                    
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", buffer.m_Desc.m_NvrhiDesc.debugName);
                    
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
                    if (buffer.m_AliasedFromIndex != UINT32_MAX)
                        ImGui::Text("%s", m_Buffers[buffer.m_AliasedFromIndex].m_Desc.m_NvrhiDesc.debugName);
                    else
                        ImGui::Text("-");
                }
                
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        
        ImGui::TreePop();
    }
}
