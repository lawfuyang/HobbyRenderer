#include "RenderGraph.h"
#include "Renderer.h"
#include "Config.h"
#include "Utilities.h"

#include "imgui.h"

using namespace RenderGraphInternal;

static thread_local uint16_t t_ActivePassIndex = 0;

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
    hash_combine(seed, (uint32_t)m_NvrhiDesc.initialState);
    hash_combine(seed, m_NvrhiDesc.useClearValue);
    if (m_NvrhiDesc.useClearValue)
    {
        hash_combine(seed, m_NvrhiDesc.clearValue.r);
        hash_combine(seed, m_NvrhiDesc.clearValue.g);
        hash_combine(seed, m_NvrhiDesc.clearValue.b);
        hash_combine(seed, m_NvrhiDesc.clearValue.a);
    }
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
    hash_combine(seed, (uint32_t)m_NvrhiDesc.initialState);
    return seed;
}

nvrhi::MemoryRequirements RGTextureDesc::GetMemoryRequirements() const
{
    nvrhi::TextureDesc tempDesc = m_NvrhiDesc;
    tempDesc.isVirtual = true;
    nvrhi::TextureHandle tempTex = Renderer::GetInstance()->m_RHI->m_NvrhiDevice->createTexture(tempDesc);
    return Renderer::GetInstance()->m_RHI->m_NvrhiDevice->getTextureMemoryRequirements(tempTex);
}

nvrhi::MemoryRequirements RGBufferDesc::GetMemoryRequirements() const
{
    nvrhi::BufferDesc tempDesc = m_NvrhiDesc;
    tempDesc.isVirtual = true;
    nvrhi::BufferHandle tempBuf = Renderer::GetInstance()->m_RHI->m_NvrhiDevice->createBuffer(tempDesc);
    return Renderer::GetInstance()->m_RHI->m_NvrhiDevice->getBufferMemoryRequirements(tempBuf);
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
    
    m_AliasingEnabled = Config::Get().m_EnableRenderGraphAliasing;
    
    const uint32_t kMaxTransientResourceLifetimeFrames = 3;

    m_FrameIndex++;
    m_CurrentPassIndex = 0;
    m_IsCompiled = false;
    m_Stats = Stats{};
    m_PassNames.clear();
    m_PassAccesses.clear();
    m_PerPassAliasBarriers.clear();

    // Mark all resources as not declared this frame and reset lifetimes
    for (TransientTexture& texture : m_Textures)
    {
        texture.m_IsDeclaredThisFrame = false;
        texture.m_Lifetime = {};
        texture.m_AliasedFromIndex = UINT32_MAX;
        texture.m_PhysicalLastPass = 0;
        texture.m_DeclarationPass = 0;

        // Cleanup physical resources not used for > 3 frames
        if (texture.m_PhysicalTexture && (m_FrameIndex - texture.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            //SDL_Log("[RenderGraph] Freeing texture '%s' due to inactivity", texture.m_Desc.m_NvrhiDesc.debugName.c_str());
            if (texture.m_IsPhysicalOwner)
            {
                FreeBlock(texture.m_HeapIndex, texture.m_BlockOffset);
            }
            texture.m_PhysicalTexture = nullptr;
            texture.m_Heap = nullptr;
            texture.m_HeapIndex = UINT32_MAX;
            texture.m_IsAllocated = false;
        }
    }

    for (TransientBuffer& buffer : m_Buffers)
    {
        buffer.m_IsDeclaredThisFrame = false;
        buffer.m_Lifetime = {};
        buffer.m_AliasedFromIndex = UINT32_MAX;
        buffer.m_PhysicalLastPass = 0;
        buffer.m_DeclarationPass = 0;

        if (buffer.m_PhysicalBuffer && (m_FrameIndex - buffer.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            //SDL_Log("[RenderGraph] Freeing buffer '%s' due to inactivity", buffer.m_Desc.m_NvrhiDesc.debugName.c_str());
            if (buffer.m_IsPhysicalOwner)
            {
                FreeBlock(buffer.m_HeapIndex, buffer.m_BlockOffset);
            }
            buffer.m_PhysicalBuffer = nullptr;
            buffer.m_Heap = nullptr;
            buffer.m_HeapIndex = UINT32_MAX;
            buffer.m_IsAllocated = false;
        }
    }

    // Heaps are kept in the vector for index stability
    for (HeapEntry& heapEntry : m_Heaps)
    {
        if (heapEntry.m_Heap && (m_FrameIndex - heapEntry.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            //SDL_Log("[RenderGraph] Freeing heap slot %u due to inactivity", heapEntry.m_HeapIdx);
            heapEntry.m_Heap = nullptr;
            heapEntry.m_Blocks.clear();
            heapEntry.m_Size = 0;
        }
    }
}

void RenderGraph::BeginPass(const char* name)
{
    PROFILE_FUNCTION();
    
    SDL_assert(name);
    m_CurrentPassIndex++;
    m_PassNames.push_back(name);
    m_PassAccesses.push_back(m_PendingPassAccess); // Copy pending accesses from Setup
    
    // Update resources declared in Setup with the correct pass index
    for (uint32_t texIdx : m_PendingDeclaredTextures)
    {
        m_Textures[texIdx].m_DeclarationPass = m_CurrentPassIndex;
        UpdateResourceLifetime(m_Textures[texIdx].m_Lifetime, m_CurrentPassIndex);
    }
    for (uint32_t bufIdx : m_PendingDeclaredBuffers)
    {
        m_Buffers[bufIdx].m_DeclarationPass = m_CurrentPassIndex;
        UpdateResourceLifetime(m_Buffers[bufIdx].m_Lifetime, m_CurrentPassIndex);
    }
    
    // Also update any read/write resources that were just registered in Setup
    for (uint32_t texIdx : m_PendingPassAccess.m_ReadTextures) UpdateResourceLifetime(m_Textures[texIdx].m_Lifetime, m_CurrentPassIndex);
    for (uint32_t texIdx : m_PendingPassAccess.m_WriteTextures) UpdateResourceLifetime(m_Textures[texIdx].m_Lifetime, m_CurrentPassIndex);
    for (uint32_t bufIdx : m_PendingPassAccess.m_ReadBuffers) UpdateResourceLifetime(m_Buffers[bufIdx].m_Lifetime, m_CurrentPassIndex);
    for (uint32_t bufIdx : m_PendingPassAccess.m_WriteBuffers) UpdateResourceLifetime(m_Buffers[bufIdx].m_Lifetime, m_CurrentPassIndex);

    m_PendingPassAccess = {};
    m_PendingDeclaredTextures.clear();
    m_PendingDeclaredBuffers.clear();
}

void RenderGraph::ScheduleRenderer(IRenderer* pRenderer)
{
    Renderer* renderer = Renderer::GetInstance();
    const int readIndex = renderer->m_FrameNumber % 2;
    const int writeIndex = (renderer->m_FrameNumber + 1) % 2;

    BeginSetup();
    bool bPassEnabled = false;
    {
        PROFILE_SCOPED("SetupRenderer");
        bPassEnabled = pRenderer->Setup(*this);
    }
    EndSetup(bPassEnabled);

    if (bPassEnabled)
    {
        BeginPass(pRenderer->GetName());
        const uint16_t passIndex = GetCurrentPassIndex();

        nvrhi::CommandListHandle cmd = renderer->AcquireCommandList();

        const bool bImmediateExecute = false; /* defer execution until after render graph compiles */
        renderer->m_TaskScheduler->ScheduleTask([renderer, pRenderer, cmd, readIndex, writeIndex, passIndex]() {
            PROFILE_SCOPED(pRenderer->GetName());
            SimpleTimer cpuTimer;
            ScopedCommandList scopedCmd{ cmd, pRenderer->GetName() };
            
            renderer->m_RenderGraph.SetActivePass(passIndex);
            
            if (renderer->m_RHI->m_NvrhiDevice->pollTimerQuery(pRenderer->m_GPUQueries[readIndex]))
            {
                pRenderer->m_GPUTime = SimpleTimer::SecondsToMilliseconds(renderer->m_RHI->m_NvrhiDevice->getTimerQueryTime(pRenderer->m_GPUQueries[readIndex]));
            }
            renderer->m_RHI->m_NvrhiDevice->resetTimerQuery(pRenderer->m_GPUQueries[readIndex]);
            
            renderer->m_RenderGraph.InsertAliasBarriers(passIndex, scopedCmd);
            scopedCmd->beginTimerQuery(pRenderer->m_GPUQueries[writeIndex]);
            pRenderer->Render(scopedCmd, renderer->m_RenderGraph);
            renderer->m_RenderGraph.SetActivePass(0);
            scopedCmd->endTimerQuery(pRenderer->m_GPUQueries[writeIndex]);
            pRenderer->m_CPUTime = static_cast<float>(cpuTimer.TotalMilliseconds());
        }, bImmediateExecute);
    }
    else
    {
        pRenderer->m_CPUTime = 0.0f;
        pRenderer->m_GPUTime = 0.0f;
    }
}

void RenderGraph::BeginSetup()
{
    SDL_assert(!m_IsInsideSetup);
    m_IsInsideSetup = true;
    m_DidAccessInSetup = false;
    m_PendingPassAccess = {};
    m_PendingDeclaredTextures.clear();
    m_PendingDeclaredBuffers.clear();
}

void RenderGraph::EndSetup(bool bEnabled)
{
    SDL_assert(m_IsInsideSetup);
    if (!bEnabled)
    {
        if (m_DidAccessInSetup)
        {
            SDL_assert(false && "Renderer returned false in Setup but accessed/declared RG resources");
        }
        
        // Safety: clear any pending state just in case
        for (uint32_t texIdx : m_PendingDeclaredTextures) m_Textures[texIdx].m_IsDeclaredThisFrame = false;
        for (uint32_t bufIdx : m_PendingDeclaredBuffers) m_Buffers[bufIdx].m_IsDeclaredThisFrame = false;

        m_PendingPassAccess = {};
        m_PendingDeclaredTextures.clear();
        m_PendingDeclaredBuffers.clear();
    }
    m_IsInsideSetup = false;
}

void RenderGraph::SetActivePass(uint16_t passIndex)
{
    t_ActivePassIndex = passIndex;
}

uint16_t RenderGraph::GetActivePassIndex() const
{
    // If t_ActivePassIndex is set, we use it (during Render phase).
    // Otherwise, we use m_CurrentPassIndex (during Setup phase).
    return (t_ActivePassIndex != 0) ? t_ActivePassIndex : m_CurrentPassIndex;
}

bool RenderGraph::DeclareTexture(const RGTextureDesc& desc, RGTextureHandle& outputHandle)
{
    SDL_assert(m_IsInsideSetup && "DeclareTexture must be called during Setup phase");
    m_DidAccessInSetup = true;

    size_t hash = desc.ComputeHash();

    if (outputHandle.IsValid() && outputHandle.m_Index < m_Textures.size())
    {
        TransientTexture& texture = m_Textures[outputHandle.m_Index];
        
        SDL_assert(!texture.m_IsDeclaredThisFrame && "Texture already declared this frame! Only one pass should declare a resource.");

        bool isNewlyAllocated = false;
        if (texture.m_Hash != hash)
        {
            //SDL_Log("[RenderGraph] Texture desc mismatch for handle %u, freeing old resource", outputHandle.m_Index);
            texture.m_PhysicalTexture = nullptr;
            texture.m_IsAllocated = false;
            isNewlyAllocated = true;
        }

        texture.m_Desc = desc;
        texture.m_Hash = hash;
        texture.m_IsDeclaredThisFrame = true;
        texture.m_IsPersistent = false;
        texture.m_LastFrameUsed = m_FrameIndex;
        
        m_PendingDeclaredTextures.push_back(outputHandle.m_Index);
        WriteTexture(outputHandle); // Implicitly mark as written in the declaring pass, since they start with undefined contents
        return isNewlyAllocated;
    }

    // Try to find a matching unused resource in the pool
    for (uint32_t i = 0; i < m_Textures.size(); ++i)
    {
        if (!m_Textures[i].m_IsDeclaredThisFrame && m_Textures[i].m_Hash == hash)
        {
            m_Textures[i].m_Desc = desc; // Ensure metadata like debugName is updated
            m_Textures[i].m_IsDeclaredThisFrame = true;
            m_Textures[i].m_IsPersistent = false;
            m_Textures[i].m_LastFrameUsed = m_FrameIndex;
            
            m_PendingDeclaredTextures.push_back(i);
            outputHandle = { i };
            WriteTexture(outputHandle); // Implicitly mark as written in the declaring pass, since they start with undefined contents
            return true;
        }
    }

    RGTextureHandle handle;
    handle.m_Index = static_cast<uint32_t>(m_Textures.size());
    
    TransientTexture texture;
    texture.m_Desc = desc;
    texture.m_Hash = hash;
    texture.m_IsDeclaredThisFrame = true;
    texture.m_IsPersistent = false;
    texture.m_LastFrameUsed = m_FrameIndex;
    
    m_Textures.push_back(texture);
    m_PendingDeclaredTextures.push_back(handle.m_Index);

    outputHandle = handle;
    WriteTexture(outputHandle); // Implicitly mark new textures as written in the declaring pass, since they start with undefined contents
    return true;
}

bool RenderGraph::DeclareBuffer(const RGBufferDesc& desc, RGBufferHandle& outputHandle)
{
    SDL_assert(m_IsInsideSetup && "DeclareBuffer must be called during Setup phase");
    m_DidAccessInSetup = true;

    size_t hash = desc.ComputeHash();

    if (outputHandle.IsValid() && outputHandle.m_Index < m_Buffers.size())
    {
        TransientBuffer& buffer = m_Buffers[outputHandle.m_Index];
        
        SDL_assert(!buffer.m_IsDeclaredThisFrame && "Buffer already declared this frame! Only one pass should declare a resource.");

        bool isNewlyAllocated = false;
        if (buffer.m_Hash != hash)
        {
            //SDL_Log("[RenderGraph] Buffer desc mismatch for handle %u, freeing old resource", outputHandle.m_Index);
            buffer.m_PhysicalBuffer = nullptr;
            buffer.m_IsAllocated = false;
            isNewlyAllocated = true;
        }

        buffer.m_Desc = desc;
        buffer.m_Hash = hash;
        buffer.m_IsDeclaredThisFrame = true;
        buffer.m_IsPersistent = false;
        buffer.m_LastFrameUsed = m_FrameIndex;

        m_PendingDeclaredBuffers.push_back(outputHandle.m_Index);
        WriteBuffer(outputHandle); // Implicitly mark as written in the declaring pass, since they start with undefined contents
        return isNewlyAllocated;
    }

    for (uint32_t i = 0; i < m_Buffers.size(); ++i)
    {
        if (!m_Buffers[i].m_IsDeclaredThisFrame && m_Buffers[i].m_Hash == hash)
        {
            m_Buffers[i].m_Desc = desc; // Ensure metadata like debugName is updated
            m_Buffers[i].m_IsDeclaredThisFrame = true;
            m_Buffers[i].m_IsPersistent = false;
            m_Buffers[i].m_LastFrameUsed = m_FrameIndex;

            m_PendingDeclaredBuffers.push_back(i);
            outputHandle = { i };
            WriteBuffer(outputHandle); // Implicitly mark as written in the declaring pass, since they start with undefined contents
            return true;
        }
    }

    RGBufferHandle handle;
    handle.m_Index = static_cast<uint32_t>(m_Buffers.size());
    
    TransientBuffer buffer;
    buffer.m_Desc = desc;
    buffer.m_Hash = hash;
    buffer.m_IsDeclaredThisFrame = true;
    buffer.m_IsPersistent = false;
    buffer.m_LastFrameUsed = m_FrameIndex;
    
    m_Buffers.push_back(buffer);
    m_PendingDeclaredBuffers.push_back(handle.m_Index);

    outputHandle = handle;
    WriteBuffer(outputHandle); // Implicitly mark new buffers as written in the declaring pass, since they start with undefined contents    
    return true;
}

bool RenderGraph::DeclarePersistentTexture(const RGTextureDesc& desc, RGTextureHandle& outputHandle)
{
    bool newlyAllocated = DeclareTexture(desc, outputHandle);
    m_Textures[outputHandle.m_Index].m_IsPersistent = true;
    return newlyAllocated;
}

bool RenderGraph::DeclarePersistentBuffer(const RGBufferDesc& desc, RGBufferHandle& outputHandle)
{
    bool newlyAllocated = DeclareBuffer(desc, outputHandle);
    m_Buffers[outputHandle.m_Index].m_IsPersistent = true;
    return newlyAllocated;
}

// ============================================================================
// RenderGraph - Resource Access
// ============================================================================

void RenderGraph::ReadTexture(RGTextureHandle handle)
{
    SDL_assert(m_IsInsideSetup && "ReadTexture must be called during Setup phase");
    m_DidAccessInSetup = true;

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

    m_PendingPassAccess.m_ReadTextures.insert(handle.m_Index);
}

void RenderGraph::WriteTexture(RGTextureHandle handle)
{
    SDL_assert(m_IsInsideSetup && "WriteTexture must be called during Setup phase");
    m_DidAccessInSetup = true;

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

    m_PendingPassAccess.m_WriteTextures.insert(handle.m_Index);
}

void RenderGraph::ReadBuffer(RGBufferHandle handle)
{
    SDL_assert(m_IsInsideSetup && "ReadBuffer must be called during Setup phase");
    m_DidAccessInSetup = true;

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

    m_PendingPassAccess.m_ReadBuffers.insert(handle.m_Index);
}

void RenderGraph::WriteBuffer(RGBufferHandle handle)
{
    SDL_assert(m_IsInsideSetup && "WriteBuffer must be called during Setup phase");
    m_DidAccessInSetup = true;

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

    m_PendingPassAccess.m_WriteBuffers.insert(handle.m_Index);
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

    // Resource validation
    for (uint32_t i = 0; i < (uint32_t)m_Textures.size(); ++i)
    {
        const TransientTexture& tex = m_Textures[i];
        if (!tex.m_IsDeclaredThisFrame) continue;

        uint16_t firstAccessPass = UINT16_MAX;

        for (uint16_t passIdx = 1; passIdx <= (uint16_t)m_PassAccesses.size(); ++passIdx)
        {
            const PassAccess& access = m_PassAccesses[passIdx - 1];
            if (access.m_ReadTextures.count(i) || access.m_WriteTextures.count(i))
            {
                if (firstAccessPass == UINT16_MAX)
                {
                    firstAccessPass = passIdx;
                }
            }
        }

        if (firstAccessPass == UINT16_MAX)
        {
            SDL_Log("[RenderGraph] ERROR: Texture '%s' (index %u) declared but never accessed", tex.m_Desc.m_NvrhiDesc.debugName.c_str(), i);
            SDL_assert(false && "Resource declared but never accessed");
        }

        if (tex.m_DeclarationPass > firstAccessPass)
        {
            SDL_Log("[RenderGraph] ERROR: Texture '%s' (index %u) accessed in pass %u but only declared in pass %u", 
                tex.m_Desc.m_NvrhiDesc.debugName.c_str(), i, firstAccessPass, tex.m_DeclarationPass);
            SDL_assert(false && "Resource accessed before it was declared");
        }
    }

    for (uint32_t i = 0; i < (uint32_t)m_Buffers.size(); ++i)
    {
        const TransientBuffer& buf = m_Buffers[i];
        if (!buf.m_IsDeclaredThisFrame) continue;

        uint16_t firstAccessPass = UINT16_MAX;

        for (uint16_t passIdx = 1; passIdx <= (uint16_t)m_PassAccesses.size(); ++passIdx)
        {
            const PassAccess& access = m_PassAccesses[passIdx - 1];
            if (access.m_ReadBuffers.count(i) || access.m_WriteBuffers.count(i))
            {
                if (firstAccessPass == UINT16_MAX)
                {
                    firstAccessPass = passIdx;
                }
            }
        }

        if (firstAccessPass == UINT16_MAX)
        {
            SDL_Log("[RenderGraph] ERROR: Buffer '%s' (index %u) declared but never accessed", buf.m_Desc.m_NvrhiDesc.debugName.c_str(), i);
            SDL_assert(false && "Resource declared but never accessed");
        }

        if (buf.m_DeclarationPass > firstAccessPass)
        {
            SDL_Log("[RenderGraph] ERROR: Buffer '%s' (index %u) accessed in pass %u but only declared in pass %u", 
                buf.m_Desc.m_NvrhiDesc.debugName.c_str(), i, firstAccessPass, buf.m_DeclarationPass);
            SDL_assert(false && "Resource accessed before it was declared");
        }
    }

    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();

    m_Stats.m_NumTextures = m_Stats.m_NumBuffers = 0;
    for (const TransientTexture& tex : m_Textures) if (tex.m_IsDeclaredThisFrame) m_Stats.m_NumTextures++;
    for (const TransientBuffer& buf : m_Buffers) if (buf.m_IsDeclaredThisFrame) m_Stats.m_NumBuffers++;

    AllocateResourcesInternal(
        false,  // bIsBuffer
        [this, device](uint32_t idx, nvrhi::HeapHandle heap, uint64_t offset)
        {
            TransientTexture& texture = m_Textures[idx];

            if (texture.m_PhysicalTexture && 
                texture.m_Heap == heap && 
                texture.m_Offset == offset)
            {
                // Already bound correctly to the right heap at the right offset, and metadata matches
                return;
            }

            PROFILE_SCOPED("CreateTextureAndBindMemory");

            texture.m_Desc.m_NvrhiDesc.isVirtual = true;
            texture.m_PhysicalTexture = device->createTexture(texture.m_Desc.m_NvrhiDesc);
            device->bindTextureMemory(texture.m_PhysicalTexture, heap, offset);
            texture.m_Heap = heap;
            texture.m_Offset = offset;
        }
    );

    AllocateResourcesInternal(
        true,  // bIsBuffer
        [this, device](uint32_t idx, nvrhi::HeapHandle heap, uint64_t offset)
        {
            TransientBuffer& buffer = m_Buffers[idx];

            if (buffer.m_PhysicalBuffer && 
                buffer.m_Heap == heap && 
                buffer.m_Offset == offset)
            {
                // Already bound correctly to the right heap at the right offset, and metadata matches
                return;
            }

            PROFILE_SCOPED("CreateBufferAndBindMemory");

            buffer.m_Desc.m_NvrhiDesc.isVirtual = true;
            buffer.m_PhysicalBuffer = device->createBuffer(buffer.m_Desc.m_NvrhiDesc);
            device->bindBufferMemory(buffer.m_PhysicalBuffer, heap, offset);
            buffer.m_Heap = heap;
            buffer.m_Offset = offset;
        }
    );

    // Build per-pass aliasing barrier info.
    // For each aliased resource, insert an aliasing barrier at the pass where it's first used.
    // This ensures the GPU flushes caches for the shared heap memory before the new resource accesses it.
    if (m_AliasingEnabled)
    {
        m_PerPassAliasBarriers.clear();
        m_PerPassAliasBarriers.resize(m_CurrentPassIndex + 1); // pass indices are 1-based

        auto addBarrier = [&](bool isBuffer, uint32_t index, const RenderGraphInternal::ResourceLifetime& lifetime, const char* debugName)
        {
            if (!lifetime.IsValid() || lifetime.m_FirstPass == 0 || lifetime.m_FirstPass > m_PassAccesses.size())
                return;

            const PassAccess& firstAccess = m_PassAccesses[lifetime.m_FirstPass - 1];

            bool hasWrite = false;
            bool hasRead = false;
            if (isBuffer)
            {
                hasWrite = firstAccess.m_WriteBuffers.count(index) > 0;
                hasRead = firstAccess.m_ReadBuffers.count(index) > 0;
            }
            else
            {
                hasWrite = firstAccess.m_WriteTextures.count(index) > 0;
                hasRead = firstAccess.m_ReadTextures.count(index) > 0;
            }

            if (!hasWrite || hasRead)
            {
                SDL_Log("[RenderGraph] ERROR: Aliased %s '%s' first used in pass %u must be write-only. hasWrite=%d hasRead=%d",
                    isBuffer ? "buffer" : "texture",
                    debugName,
                    lifetime.m_FirstPass,
                    hasWrite ? 1 : 0,
                    hasRead ? 1 : 0);
                SDL_assert(false && "Aliased resource first use must be write-only");
            }

            const uint16_t passIdx = lifetime.m_FirstPass;
            if (passIdx > 0 && passIdx < m_PerPassAliasBarriers.size())
            {
                m_PerPassAliasBarriers[passIdx].push_back({ isBuffer, index });
            }
        };

        for (uint32_t i = 0; i < (uint32_t)m_Textures.size(); ++i)
        {
            const TransientTexture& tex = m_Textures[i];
            if (!tex.m_IsDeclaredThisFrame)
                continue;

            if (tex.m_AliasedFromIndex != UINT32_MAX)
            {
                addBarrier(false, i, tex.m_Lifetime, tex.m_Desc.m_NvrhiDesc.debugName.c_str());
            }
        }

        for (uint32_t i = 0; i < (uint32_t)m_Buffers.size(); ++i)
        {
            const TransientBuffer& buf = m_Buffers[i];
            if (!buf.m_IsDeclaredThisFrame)
                continue;

            if (buf.m_AliasedFromIndex != UINT32_MAX)
            {
                addBarrier(true, i, buf.m_Lifetime, buf.m_Desc.m_NvrhiDesc.debugName.c_str());
            }
        }
    }

    m_IsCompiled = true;
}

// ============================================================================
// RenderGraph - Aliasing Barriers
// ============================================================================

void RenderGraph::InsertAliasBarriers(uint16_t passIndex, nvrhi::ICommandList* commandList) const
{
    if (!m_AliasingEnabled || passIndex >= m_PerPassAliasBarriers.size())
        return;

    const std::vector<AliasBarrierEntry>& barriers = m_PerPassAliasBarriers[passIndex];
    for (const AliasBarrierEntry& entry : barriers)
    {
        if (entry.m_IsBuffer)
        {
            const TransientBuffer& buffer = m_Buffers[entry.m_ResourceIndex];
            if (buffer.m_PhysicalBuffer)
            {
                commandList->insertAliasingBarrier(buffer.m_PhysicalBuffer);
            }
        }
        else
        {
            const TransientTexture& texture = m_Textures[entry.m_ResourceIndex];
            if (texture.m_PhysicalTexture)
            {
                commandList->insertAliasingBarrier(texture.m_PhysicalTexture);
            }
        }
    }
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
            
            //SDL_Log("[RenderGraph] Reused heap slot %u for new heap of size %.2f MB", i, size / (1024.0 * 1024.0));
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
    const nvrhi::MemoryRequirements memReq = resource->GetMemoryRequirements();
    size_t size = memReq.size;

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

    // 2. No fit found, create a new heap (at least 1 MB)
    size_t heapSize = NextPow2(std::max(size_t(1024 * 1024), memReq.size));
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
        const nvrhi::MemoryRequirements memReq = resource->GetMemoryRequirements();
        
        // Trivial reuse: if already allocated and was an owner, skip logic
        if (resource->m_IsAllocated && resource->m_IsPhysicalOwner && resource->m_HeapIndex != UINT32_MAX)
        {
            HeapEntry& heapEntry = m_Heaps[resource->m_HeapIndex];
            SDL_assert(heapEntry.m_Heap == resource->m_Heap);

            heapEntry.m_LastFrameUsed = m_FrameIndex;
            resource->m_PhysicalLastPass = resource->m_Lifetime.m_LastPass;

            if (bIsBuffer) m_Stats.m_NumAllocatedBuffers++;
            else m_Stats.m_NumAllocatedTextures++;

            const size_t bufferMemory = bIsBuffer ? memReq.size : 0;
            const size_t textureMemory = !bIsBuffer ? memReq.size : 0;

            m_Stats.m_TotalBufferMemory += bufferMemory;
            m_Stats.m_TotalTextureMemory += textureMemory;
            continue;
        }

        bool aliased = false;
        if (m_AliasingEnabled && !bIsBuffer && !resource->m_IsPersistent) // TODO: figure out why the fuck aliasing buffers is buggy
        {
            for (uint32_t candidateIdx : sortedIndices)
            {
                if (candidateIdx == idx) break;
                TransientResourceBase* candidate = bIsBuffer ? (TransientResourceBase*)&m_Buffers[candidateIdx] : (TransientResourceBase*)&m_Textures[candidateIdx];
                if (!candidate->m_IsAllocated || !candidate->m_IsPhysicalOwner || candidate->m_IsPersistent)
                    continue;

                bool bCanAlias = resource->m_Lifetime.m_FirstPass > candidate->m_PhysicalLastPass;
                if (bCanAlias)
                {
                    const nvrhi::MemoryRequirements candidateMemReq = candidate->GetMemoryRequirements();
                    bCanAlias = memReq.size <= candidateMemReq.size;

                    if (bCanAlias && memReq.alignment > 0)
                    {
                        bCanAlias = (candidate->m_Offset % memReq.alignment) == 0;
                    }
                }
                
                if (bCanAlias)
                {
                    resource->m_Heap = candidate->m_Heap;
                    resource->m_Offset = candidate->m_Offset;
                    resource->m_HeapIndex = candidate->m_HeapIndex;
                    resource->m_BlockOffset = candidate->m_BlockOffset;
                    resource->m_AliasedFromIndex = candidateIdx;
                    resource->m_IsAllocated = true;
                    resource->m_IsPhysicalOwner = false;
                    
                    candidate->m_PhysicalLastPass = std::max(candidate->m_PhysicalLastPass, resource->m_Lifetime.m_LastPass);

                    createAndBindResource(idx, resource->m_Heap, resource->m_Offset);

                    if (bIsBuffer) m_Stats.m_NumAliasedBuffers++;
                    else m_Stats.m_NumAliasedTextures++;
                    
                    aliased = true;
                    break;
                }
            }
        }
        
        if (!aliased)
        {
            SubAllocateResource(resource, memReq.alignment);
            
            resource->m_IsAllocated = true;
            resource->m_IsPhysicalOwner = true;
            resource->m_AliasedFromIndex = UINT32_MAX;
            resource->m_PhysicalLastPass = resource->m_Lifetime.m_LastPass;

            createAndBindResource(idx, resource->m_Heap, resource->m_Offset);

            if (bIsBuffer)
                m_Stats.m_NumAllocatedBuffers++;
            else
                m_Stats.m_NumAllocatedTextures++;

            const size_t bufferMemory = bIsBuffer ? memReq.size : 0;
            const size_t textureMemory = !bIsBuffer ? memReq.size : 0;

            m_Stats.m_TotalBufferMemory += bufferMemory;
            m_Stats.m_TotalTextureMemory += textureMemory;
        }
    }
}

// ============================================================================
// RenderGraph - Resource Retrieval
// ============================================================================

nvrhi::TextureHandle RenderGraph::GetTexture(RGTextureHandle handle, RGResourceAccessMode access) const
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

    // Validate that the current pass declared this access
    uint16_t activePassIdx = GetActivePassIndex();
    if (activePassIdx > 0 && activePassIdx <= m_PassAccesses.size())
    {
        const PassAccess& passAccess = m_PassAccesses[activePassIdx - 1];
        bool found = false;
        
        if (access == RGResourceAccessMode::Read)
        {
            found = passAccess.m_ReadTextures.count(handle.m_Index) > 0;
        }
        else if (access == RGResourceAccessMode::Write)
        {
            found = passAccess.m_WriteTextures.count(handle.m_Index) > 0;
        }

        if (!found)
        {
            SDL_Log("[RenderGraph] ERROR: Pass '%s' attempted to %s texture '%s' without declaring dependency in Setup()",
                m_PassNames[activePassIdx - 1], (access == RGResourceAccessMode::Read ? "READ" : "WRITE"),
                texture.m_Desc.m_NvrhiDesc.debugName.c_str());
            SDL_assert(false && "Resource access dependency not declared");
        }
    }

    SDL_assert(texture.m_PhysicalTexture);
    SDL_assert(texture.m_IsAllocated && "Texture not allocated");
    
    return texture.m_PhysicalTexture;
}

nvrhi::BufferHandle RenderGraph::GetBuffer(RGBufferHandle handle, RGResourceAccessMode access) const
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

    // Validate that the current pass declared this access
    uint16_t activePassIdx = GetActivePassIndex();
    if (activePassIdx > 0 && activePassIdx <= m_PassAccesses.size())
    {
        const PassAccess& passAccess = m_PassAccesses[activePassIdx - 1];
        bool found = false;

        if (access == RGResourceAccessMode::Read)
        {
            found = passAccess.m_ReadBuffers.count(handle.m_Index) > 0;
        }
        else if (access == RGResourceAccessMode::Write)
        {
            found = passAccess.m_WriteBuffers.count(handle.m_Index) > 0;
        }

        if (!found)
        {
            SDL_Log("[RenderGraph] ERROR: Pass '%s' (index %u) attempted to %s buffer '%s' without declaring dependency in Setup()",
                m_PassNames[activePassIdx - 1], activePassIdx, (access == RGResourceAccessMode::Read ? "READ" : "WRITE"),
                buffer.m_Desc.m_NvrhiDesc.debugName.c_str());
            SDL_assert(false && "Resource access dependency not declared");
        }
    }

    SDL_assert(buffer.m_PhysicalBuffer);
    SDL_assert(buffer.m_IsAllocated && "Buffer not allocated");
    
    return buffer.m_PhysicalBuffer;
}


