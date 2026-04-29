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
    nvrhi::TextureHandle tempTex = g_Renderer.m_RHI->m_NvrhiDevice->createTexture(tempDesc);
    return g_Renderer.m_RHI->m_NvrhiDevice->getTextureMemoryRequirements(tempTex);
}

nvrhi::MemoryRequirements RGBufferDesc::GetMemoryRequirements() const
{
    nvrhi::BufferDesc tempDesc = m_NvrhiDesc;
    tempDesc.isVirtual = true;
    nvrhi::BufferHandle tempBuf = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(tempDesc);
    return g_Renderer.m_RHI->m_NvrhiDevice->getBufferMemoryRequirements(tempBuf);
}

// ============================================================================
// RenderGraph - Deferred Release
// ============================================================================

// FlushDeferredReleases — called at the top of Reset() to safely drop any GPU
// resource handles that were queued for release during the previous frame.
//
// Background: dropping an nvrhi RefCountPtr whose refcount reaches zero triggers
// ID3D12Resource::Release() immediately on the calling thread.  If the GPU is
// still executing work that references that resource, D3D12 fires:
//   ERROR #921: OBJECT_DELETED_WHILE_STILL_IN_USE
//
// The fix is to never drop handles inline (mid-Compile, mid-Reset eviction, or
// mid-DeclareTexture desc-change).  Instead, move them into these lists and
// flush once per frame at the top of Reset(), after the previous frame's GPU
// work has been waited on by ExecutePendingCommandLists / waitForIdle.
//
// If the lists are non-empty we call waitForIdle() + runGarbageCollection()
// defensively before clearing, so this function is safe even if the caller
// forgot to wait.  In the normal path (RunOneFrame calls waitForIdle before
// the next Reset) the wait is a no-op.
void RenderGraph::FlushDeferredReleases()
{
    if (m_DeferredReleaseTextures.empty() && m_DeferredReleaseBuffers.empty())
        return;

    if (m_bVerboseLogging)
        SDL_Log("[RenderGraph] FlushDeferredReleases: %zu texture(s), %zu buffer(s) pending",
                m_DeferredReleaseTextures.size(), m_DeferredReleaseBuffers.size());

    // Ensure the GPU has finished with these resources before we drop them.
    if (g_Renderer.m_RHI && g_Renderer.m_RHI->m_NvrhiDevice)
    {
        g_Renderer.m_RHI->m_NvrhiDevice->waitForIdle();
        g_Renderer.m_RHI->m_NvrhiDevice->runGarbageCollection();
    }

    // Now it is safe to drop the handles — refcounts reach zero here.
    m_DeferredReleaseTextures.clear();
    m_DeferredReleaseBuffers.clear();
}

// ============================================================================
// RenderGraph - Resource Declaration
// ============================================================================

void RenderGraph::Shutdown()
{
    // Transient textures and buffers hold nvrhi GPU resource handles.  Dropping
    // those handles while the GPU is still executing work that references them
    // causes D3D12 ERROR 921 (OBJECT_DELETED_WHILE_STILL_IN_USE).  Always
    // drain the GPU before releasing any physical resources.
    //
    // In normal engine operation the caller (Renderer::Shutdown, scene reload,
    // etc.) is responsible for calling waitForIdle first.  We assert here so
    // that any future caller that forgets gets an immediate, actionable failure
    // rather than a silent GPU corruption or a deferred D3D12 validation error.
    //
    // We only do this when the RHI is fully initialised (m_RHI != nullptr and
    // the device exists) so that Shutdown() is still safe to call during early
    // startup or in unit-test teardown before the GPU stack is up.
    if (g_Renderer.m_RHI && g_Renderer.m_RHI->m_NvrhiDevice)
    {
        // If the graph is still compiled (PostRender() was not called), log a
        // warning but continue — Shutdown() is a recovery/teardown path and must
        // not itself assert.  We call waitForIdle() defensively to ensure GPU
        // resources are not freed while in-flight.
        if (m_IsCompiled)
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] WARNING: Shutdown() called while m_IsCompiled==true. "
                        "PostRender() was not called after the last Compile(). "
                        "Calling waitForIdle() defensively.");
        }
        g_Renderer.m_RHI->m_NvrhiDevice->waitForIdle();
        g_Renderer.m_RHI->m_NvrhiDevice->runGarbageCollection();
    }

    // Drop any deferred-release handles now that the GPU is idle.
    m_DeferredReleaseTextures.clear();
    m_DeferredReleaseBuffers.clear();

    m_Textures.clear();
    m_Buffers.clear();
    m_Heaps.clear();
    m_PassNames.clear();
    m_PassAccesses.clear();
    m_PerPassAliasBarriers.clear();
    m_PendingPassAccess = {};
    m_PendingDeclaredTextures.clear();
    m_PendingDeclaredBuffers.clear();
    m_Stats = {};
    m_IsInsideSetup = false;
    m_IsCompiled = false;
    m_CurrentPassIndex = 0;
    t_ActivePassIndex = 0;
    m_bForceInvalidateAllResources = true;
    // Keep the flag true for two full frames after Shutdown() so that handles
    // skipped in frame 1 (e.g. depth/GBuffers in PT mode) are also invalidated
    // when they are first declared again in frame 2.
    m_ForceInvalidateFramesRemaining = 2;
}

void RenderGraph::Reset()
{
    PROFILE_FUNCTION();

    // Flush any GPU resource handles that were queued for deferred release
    // during the previous frame's Compile() or DeclareTexture/DeclareBuffer
    // desc-change paths.  This must happen before we touch m_Textures /
    // m_Buffers so that the waitForIdle() inside FlushDeferredReleases() runs
    // while the previous frame's command lists are still the most-recently-
    // submitted work.  After this call it is safe to null out physical handles.
    FlushDeferredReleases();

    m_AliasingEnabled = Config::Get().m_EnableRenderGraphAliasing;
    
    const uint32_t kMaxTransientResourceLifetimeFrames = 3;

    m_CurrentPassIndex = 0;
    m_IsCompiled = false;
    m_IsInsideSetup = false; // safety: ensure setup state is clean at frame start
    m_Stats = Stats{};
    m_PassNames.clear();
    m_PassAccesses.clear();
    m_PerPassAliasBarriers.clear();
    // Clear any pending state that may have been left over if a previous frame
    // was interrupted (e.g. a renderer's Setup() threw or returned early after
    // declaring resources).  BeginSetup() also clears these, but Reset() is the
    // authoritative frame-start reset and must leave the graph in a fully clean
    // state regardless of what happened last frame.
    m_PendingPassAccess = {};
    m_PendingDeclaredTextures.clear();
    m_PendingDeclaredBuffers.clear();

    // Mark all resources as not declared this frame and reset lifetimes
    for (TransientTexture& texture : m_Textures)
    {
        texture.m_IsDeclaredThisFrame = false;
        texture.m_Lifetime = {};
        texture.m_AliasedFromIndex = UINT32_MAX;
        texture.m_PhysicalLastPass = 0;
        texture.m_DeclarationPass = 0;

        // Cleanup physical resources not used for > 3 frames
        if (texture.m_PhysicalTexture && (g_Renderer.m_FrameNumber - texture.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] Evicting texture '%s' (slot %u): inactive for %llu frames (lastUsed=%llu, now=%u, isOwner=%d)",
                    texture.m_Desc.m_NvrhiDesc.debugName.c_str(),
                    (uint32_t)(&texture - m_Textures.data()),
                    (unsigned long long)(g_Renderer.m_FrameNumber - texture.m_LastFrameUsed),
                    (unsigned long long)texture.m_LastFrameUsed,
                    g_Renderer.m_FrameNumber,
                    (int)texture.m_IsPhysicalOwner);
            if (texture.m_IsPhysicalOwner)
            {
                FreeBlock(texture.m_HeapIndex, texture.m_BlockOffset);
            }
            // Defer the handle drop — FlushDeferredReleases() (called at the
            // top of the *next* Reset()) will wait for GPU idle before the
            // refcount reaches zero, preventing ERROR #921.
            m_DeferredReleaseTextures.push_back(std::move(texture.m_PhysicalTexture));
            texture.m_PhysicalTexture = nullptr;
            texture.m_Heap = nullptr;
            texture.m_HeapIndex = UINT32_MAX;
            texture.m_IsAllocated = false;
            texture.m_IsPhysicalOwner = false;
        }
    }

    for (TransientBuffer& buffer : m_Buffers)
    {
        buffer.m_IsDeclaredThisFrame = false;
        buffer.m_Lifetime = {};
        buffer.m_AliasedFromIndex = UINT32_MAX;
        buffer.m_PhysicalLastPass = 0;
        buffer.m_DeclarationPass = 0;

        if (buffer.m_PhysicalBuffer && (g_Renderer.m_FrameNumber - buffer.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] Evicting buffer '%s' (slot %u): inactive for %llu frames (lastUsed=%llu, now=%u, isOwner=%d)",
                    buffer.m_Desc.m_NvrhiDesc.debugName.c_str(),
                    (uint32_t)(&buffer - m_Buffers.data()),
                    (unsigned long long)(g_Renderer.m_FrameNumber - buffer.m_LastFrameUsed),
                    (unsigned long long)buffer.m_LastFrameUsed,
                    g_Renderer.m_FrameNumber,
                    (int)buffer.m_IsPhysicalOwner);
            if (buffer.m_IsPhysicalOwner)
            {
                FreeBlock(buffer.m_HeapIndex, buffer.m_BlockOffset);
            }
            // Defer the handle drop — same reasoning as for textures above.
            m_DeferredReleaseBuffers.push_back(std::move(buffer.m_PhysicalBuffer));
            buffer.m_PhysicalBuffer = nullptr;
            buffer.m_Heap = nullptr;
            buffer.m_HeapIndex = UINT32_MAX;
            buffer.m_IsAllocated = false;
            buffer.m_IsPhysicalOwner = false;
        }
    }

    // Heaps are kept in the vector for index stability
    for (HeapEntry& heapEntry : m_Heaps)
    {
        if (heapEntry.m_Heap && (g_Renderer.m_FrameNumber - heapEntry.m_LastFrameUsed > kMaxTransientResourceLifetimeFrames))
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
    SDL_assert(m_IsInsideSetup && "ScheduleRenderer() called outside of BeginSetup()/EndSetup() block");

    const int readIndex = g_Renderer.m_FrameNumber % 2;
    const int writeIndex = (g_Renderer.m_FrameNumber + 1) % 2;

    pRenderer->m_bPassEnabled = false;
    {
        PROFILE_SCOPED("SetupRenderer");
        pRenderer->m_bPassEnabled = pRenderer->Setup(*this);
    }

    if (pRenderer->m_bPassEnabled)
    {
        BeginPass(pRenderer->GetName());
    }
    else
    {
        // Renderer was disabled: roll back any resources it declared during Setup()
        // so they don't pollute the frame's allocation.
        if (!m_PendingDeclaredTextures.empty())
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] Renderer '%s' disabled after Setup() but declared %zu texture(s) - rolling back.",
                        pRenderer->GetName(), m_PendingDeclaredTextures.size());
            SDL_assert(false && "Renderer declared textures but returned false from Setup() - contract violation");
            for (uint32_t idx : m_PendingDeclaredTextures)
                if (idx < m_Textures.size())
                    m_Textures[idx].m_IsDeclaredThisFrame = false;
        }
        if (!m_PendingDeclaredBuffers.empty())
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] Renderer '%s' disabled after Setup() but declared %zu buffer(s) - rolling back.",
                        pRenderer->GetName(), m_PendingDeclaredBuffers.size());
            SDL_assert(false && "Renderer declared buffers but returned false from Setup() - contract violation");
            for (uint32_t idx : m_PendingDeclaredBuffers)
                if (idx < m_Buffers.size())
                    m_Buffers[idx].m_IsDeclaredThisFrame = false;
        }
        m_PendingDeclaredTextures.clear();
        m_PendingDeclaredBuffers.clear();
        m_PendingPassAccess = {};
        pRenderer->m_CPUTime = 0.0f;
        pRenderer->m_GPUTime = 0.0f;
        return;
    }

    // pRenderer->m_bPassEnabled is true here (disabled path returned early above).
    const uint16_t passIndex = GetCurrentPassIndex();

    nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();

    const bool bImmediateExecute = false; /* defer execution until after render graph compiles */
    g_Renderer.m_TaskScheduler->ScheduleTask([pRenderer, cmd, readIndex, writeIndex, passIndex]() {
        PROFILE_SCOPED(pRenderer->GetName());
        SimpleTimer cpuTimer;
        ScopedCommandList scopedCmd{ cmd, pRenderer->GetName() };
        PROFILE_GPU_SCOPED(pRenderer->GetName(), cmd);

        g_Renderer.m_RenderGraph.SetActivePass(passIndex);

        if (g_Renderer.m_RHI->m_NvrhiDevice->pollTimerQuery(pRenderer->m_GPUQueries[readIndex]))
        {
            pRenderer->m_GPUTime = SimpleTimer::SecondsToMilliseconds(g_Renderer.m_RHI->m_NvrhiDevice->getTimerQueryTime(pRenderer->m_GPUQueries[readIndex]));
        }
        g_Renderer.m_RHI->m_NvrhiDevice->resetTimerQuery(pRenderer->m_GPUQueries[readIndex]);

        g_Renderer.m_RenderGraph.InsertAliasBarriers(passIndex, scopedCmd);
        scopedCmd->beginTimerQuery(pRenderer->m_GPUQueries[writeIndex]);
        pRenderer->Render(scopedCmd, g_Renderer.m_RenderGraph);
        g_Renderer.m_RenderGraph.SetActivePass(0);
        scopedCmd->endTimerQuery(pRenderer->m_GPUQueries[writeIndex]);
        pRenderer->m_CPUTime = static_cast<float>(cpuTimer.TotalMilliseconds());
    }, bImmediateExecute);
}

void RenderGraph::BeginSetup()
{
    SDL_assert(!m_IsInsideSetup);

    // BeginSetup() is called exactly once per frame, immediately after Reset().
    // A non-zero m_CurrentPassIndex means Reset() was not called - the previous
    // frame's pass list is still live, which would corrupt aliasing and lifetime
    // decisions in Compile().
    //
    // Correct usage:  Reset() → BeginSetup()
    //                   → renderer1.Setup(rg) → BeginPass("r1")
    //                   → renderer2.Setup(rg) → BeginPass("r2")  …
    //                 → EndSetup() → Compile() → PostRender()
    if (m_CurrentPassIndex != 0)
    {
        if (m_bVerboseLogging)
            SDL_Log("[RenderGraph] BeginSetup() called with m_CurrentPassIndex=%u - "
                    "Reset() was not called before this frame's BeginSetup().",
                    m_CurrentPassIndex);
        SDL_assert(false && "BeginSetup() called without a preceding Reset() - "
                   "call Reset() at the start of every new frame");
    }

    // A second misuse pattern: calling BeginSetup() while the graph is still
    // compiled (Compile() was called but PostRender() was not).  This would
    // start a new setup phase while GPU resources from the previous Compile()
    // are still live, leading to double-allocation and heap corruption.
    if (m_IsCompiled)
    {
        if (m_bVerboseLogging)
            SDL_Log("[RenderGraph] BeginSetup() called while m_IsCompiled==true - "
                    "PostRender() was not called after the previous Compile().");
        SDL_assert(false && "BeginSetup() called while graph is still compiled - "
                   "call PostRender() before starting a new frame");
    }

    m_IsInsideSetup = true;
    m_PendingPassAccess = {};
    m_PendingDeclaredTextures.clear();
    m_PendingDeclaredBuffers.clear();
}

void RenderGraph::EndSetup()
{
    SDL_assert(m_IsInsideSetup);
    // Any pending state that was not committed by a BeginPass() call belongs to
    // a renderer whose Setup() ran but was not followed by BeginPass() (i.e. the
    // renderer was disabled).  Roll it back so those slots are not poisoned.
    if (!m_PendingDeclaredTextures.empty())
    {
        if (m_bVerboseLogging)
            SDL_Log("[RenderGraph] EndSetup(): %zu uncommitted texture declaration(s) - "
                    "rolling back (renderer was disabled after Setup()).",
                    m_PendingDeclaredTextures.size());
        for (uint32_t idx : m_PendingDeclaredTextures)
            if (idx < m_Textures.size())
                m_Textures[idx].m_IsDeclaredThisFrame = false;
    }
    if (!m_PendingDeclaredBuffers.empty())
    {
        if (m_bVerboseLogging)
            SDL_Log("[RenderGraph] EndSetup(): %zu uncommitted buffer declaration(s) - "
                    "rolling back (renderer was disabled after Setup()).",
                    m_PendingDeclaredBuffers.size());
        for (uint32_t idx : m_PendingDeclaredBuffers)
            if (idx < m_Buffers.size())
                m_Buffers[idx].m_IsDeclaredThisFrame = false;
    }
    m_PendingDeclaredTextures.clear();
    m_PendingDeclaredBuffers.clear();
    m_PendingPassAccess = {};
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

    if (m_bForceInvalidateAllResources)
    {
        outputHandle.Invalidate();
    }

    size_t hash = desc.ComputeHash();

    if (outputHandle.IsValid() && outputHandle.m_Index < m_Textures.size())
    {
        TransientTexture& texture = m_Textures[outputHandle.m_Index];

        if (texture.m_IsDeclaredThisFrame)
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] DOUBLE-DECLARE: handle index=%u, existing name='%s', existing hash=%zu, "
                        "new name='%s', new hash=%zu, frame=%u, lastFrameUsed=%llu, isPersistent=%d",
                        outputHandle.m_Index,
                        texture.m_Desc.m_NvrhiDesc.debugName.c_str(),
                        texture.m_Hash,
                        desc.m_NvrhiDesc.debugName.c_str(),
                        hash,
                        g_Renderer.m_FrameNumber,
                        (unsigned long long)texture.m_LastFrameUsed,
                        (int)texture.m_IsPersistent);
        }
        SDL_assert(!texture.m_IsDeclaredThisFrame && "Texture already declared this frame! Only one pass should declare a resource.");

        bool isNewlyAllocated = false;
        if (texture.m_Hash != hash)
        {
            // Desc changed: free the old heap block so SubAllocateResource can reuse
            // it for the new allocation.  Without this the old block stays occupied
            // and SubAllocateResource is forced to find a different region, causing
            // the createAndBindResource callback to recreate the nvrhi object even
            // when the caller expects a stable pointer (e.g. MF-03, OC-10).
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] DESC-CHANGE texture: slot %u '%s' hash %zu->%zu "
                        "(isOwner=%d heapIdx=%u blockOffset=%llu) - freeing old block",
                        outputHandle.m_Index,
                        texture.m_Desc.m_NvrhiDesc.debugName.c_str(),
                        texture.m_Hash, hash,
                        (int)texture.m_IsPhysicalOwner,
                        texture.m_HeapIndex,
                        (unsigned long long)texture.m_BlockOffset);
            if (texture.m_IsPhysicalOwner && texture.m_HeapIndex != UINT32_MAX)
            {
                FreeBlock(texture.m_HeapIndex, texture.m_BlockOffset);
            }
            // Defer the handle drop so the GPU finishes before the D3D12
            // resource is released (prevents ERROR #921).
            if (texture.m_PhysicalTexture)
                m_DeferredReleaseTextures.push_back(std::move(texture.m_PhysicalTexture));
            texture.m_PhysicalTexture = nullptr;
            texture.m_IsAllocated     = false;
            texture.m_IsPhysicalOwner = false;
            texture.m_Heap            = nullptr;
            texture.m_HeapIndex       = UINT32_MAX;
            isNewlyAllocated = true;
        }

        texture.m_Desc = desc;
        texture.m_Hash = hash;
        texture.m_IsDeclaredThisFrame = true;
        texture.m_IsPersistent = false;
        texture.m_LastFrameUsed = g_Renderer.m_FrameNumber;
        
        m_PendingDeclaredTextures.push_back(outputHandle.m_Index);
        WriteTexture(outputHandle); // Implicitly mark as written in the declaring pass, since they start with undefined contents
        return isNewlyAllocated;
    }

    // Try to find a matching unused resource in the pool
    for (uint32_t i = 0; i < m_Textures.size(); ++i)
    {
        if (!m_Textures[i].m_IsDeclaredThisFrame
            && m_Textures[i].m_Hash == hash
            && (g_Renderer.m_FrameNumber - m_Textures[i].m_LastFrameUsed > 1))
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] POOL-REUSE texture: slot %u (was '%s', isOwner=%d, isAllocated=%d) "
                        "reassigned to '%s' (frame=%u, lastUsed=%llu)",
                        i,
                        m_Textures[i].m_Desc.m_NvrhiDesc.debugName.c_str(),
                        (int)m_Textures[i].m_IsPhysicalOwner,
                        (int)m_Textures[i].m_IsAllocated,
                        desc.m_NvrhiDesc.debugName.c_str(),
                        g_Renderer.m_FrameNumber,
                        (unsigned long long)m_Textures[i].m_LastFrameUsed);
            m_Textures[i].m_Desc = desc; // Ensure metadata like debugName is updated
            m_Textures[i].m_IsDeclaredThisFrame = true;
            m_Textures[i].m_IsPersistent = false;
            m_Textures[i].m_LastFrameUsed = g_Renderer.m_FrameNumber;
            
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
    texture.m_LastFrameUsed = g_Renderer.m_FrameNumber;
    
    m_Textures.push_back(texture);
    m_PendingDeclaredTextures.push_back(handle.m_Index);

    outputHandle = handle;
    WriteTexture(outputHandle); // Implicitly mark new textures as written in the declaring pass, since they start with undefined contents
    return true;
}

bool RenderGraph::DeclareBuffer(const RGBufferDesc& desc, RGBufferHandle& outputHandle)
{
    SDL_assert(m_IsInsideSetup && "DeclareBuffer must be called during Setup phase");

    if (m_bForceInvalidateAllResources)
    {
        outputHandle.Invalidate();
    }

    size_t hash = desc.ComputeHash();

    if (outputHandle.IsValid() && outputHandle.m_Index < m_Buffers.size())
    {
        TransientBuffer& buffer = m_Buffers[outputHandle.m_Index];
        
        SDL_assert(!buffer.m_IsDeclaredThisFrame && "Buffer already declared this frame! Only one pass should declare a resource.");

        bool isNewlyAllocated = false;
        if (buffer.m_Hash != hash)
        {
            // Desc changed: free the old heap block so SubAllocateResource can reuse it.
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] DESC-CHANGE buffer: slot %u '%s' hash %zu->%zu "
                        "(isOwner=%d heapIdx=%u blockOffset=%llu) - freeing old block",
                        outputHandle.m_Index,
                        buffer.m_Desc.m_NvrhiDesc.debugName.c_str(),
                        buffer.m_Hash, hash,
                        (int)buffer.m_IsPhysicalOwner,
                        buffer.m_HeapIndex,
                        (unsigned long long)buffer.m_BlockOffset);
            if (buffer.m_IsPhysicalOwner && buffer.m_HeapIndex != UINT32_MAX)
            {
                FreeBlock(buffer.m_HeapIndex, buffer.m_BlockOffset);
            }
            // Defer the handle drop — same reasoning as for textures above.
            if (buffer.m_PhysicalBuffer)
                m_DeferredReleaseBuffers.push_back(std::move(buffer.m_PhysicalBuffer));
            buffer.m_PhysicalBuffer = nullptr;
            buffer.m_IsAllocated    = false;
            buffer.m_IsPhysicalOwner = false;
            buffer.m_Heap           = nullptr;
            buffer.m_HeapIndex      = UINT32_MAX;
            isNewlyAllocated = true;
        }

        buffer.m_Desc = desc;
        buffer.m_Hash = hash;
        buffer.m_IsDeclaredThisFrame = true;
        buffer.m_IsPersistent = false;
        buffer.m_LastFrameUsed = g_Renderer.m_FrameNumber;

        m_PendingDeclaredBuffers.push_back(outputHandle.m_Index);
        WriteBuffer(outputHandle); // Implicitly mark as written in the declaring pass, since they start with undefined contents
        return isNewlyAllocated;
    }

    for (uint32_t i = 0; i < m_Buffers.size(); ++i)
    {
        if (!m_Buffers[i].m_IsDeclaredThisFrame
            && m_Buffers[i].m_Hash == hash
            && (g_Renderer.m_FrameNumber - m_Buffers[i].m_LastFrameUsed > 1))
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] POOL-REUSE buffer: slot %u (was '%s', isOwner=%d, isAllocated=%d) "
                        "reassigned to '%s' (frame=%u, lastUsed=%llu)",
                        i,
                        m_Buffers[i].m_Desc.m_NvrhiDesc.debugName.c_str(),
                        (int)m_Buffers[i].m_IsPhysicalOwner,
                        (int)m_Buffers[i].m_IsAllocated,
                        desc.m_NvrhiDesc.debugName.c_str(),
                        g_Renderer.m_FrameNumber,
                        (unsigned long long)m_Buffers[i].m_LastFrameUsed);
            m_Buffers[i].m_Desc = desc; // Ensure metadata like debugName is updated
            m_Buffers[i].m_IsDeclaredThisFrame = true;
            m_Buffers[i].m_IsPersistent = false;
            m_Buffers[i].m_LastFrameUsed = g_Renderer.m_FrameNumber;

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
    buffer.m_LastFrameUsed = g_Renderer.m_FrameNumber;
    
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

    if (!handle.IsValid() || handle.m_Index >= m_Textures.size())
    {
        SDL_assert(false && "Invalid texture handle");
        return;
    }
    
    TransientTexture& texture = m_Textures[handle.m_Index];
    if (!texture.m_IsDeclaredThisFrame)
    {
        if (m_bVerboseLogging)
            SDL_Log("[RenderGraph] ERROR: Attempting to read undeclared texture '%s' (handle index %u)", texture.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Texture not declared this frame");
        return;
    }

    m_PendingPassAccess.m_ReadTextures.insert(handle.m_Index);
}

void RenderGraph::WriteTexture(RGTextureHandle handle)
{
    SDL_assert(m_IsInsideSetup && "WriteTexture must be called during Setup phase");

    if (!handle.IsValid() || handle.m_Index >= m_Textures.size())
    {
        SDL_assert(false && "Invalid texture handle");
        return;
    }
    
    TransientTexture& texture = m_Textures[handle.m_Index];
    if (!texture.m_IsDeclaredThisFrame)
    {
        if (m_bVerboseLogging)
            SDL_Log("[RenderGraph] ERROR: Attempting to write undeclared texture '%s' (handle index %u)", texture.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Texture not declared this frame");
        return;
    }

    m_PendingPassAccess.m_WriteTextures.insert(handle.m_Index);
}

void RenderGraph::ReadBuffer(RGBufferHandle handle)
{
    SDL_assert(m_IsInsideSetup && "ReadBuffer must be called during Setup phase");

    if (!handle.IsValid() || handle.m_Index >= m_Buffers.size())
    {
        SDL_assert(false && "Invalid buffer handle");
        return;
    }
    
    TransientBuffer& buffer = m_Buffers[handle.m_Index];
    if (!buffer.m_IsDeclaredThisFrame)
    {
        if (m_bVerboseLogging)
            SDL_Log("[RenderGraph] ERROR: Attempting to read undeclared buffer '%s' (handle index %u)", buffer.m_Desc.m_NvrhiDesc.debugName.c_str(), handle.m_Index);
        SDL_assert(false && "Buffer not declared this frame");
        return;
    }

    m_PendingPassAccess.m_ReadBuffers.insert(handle.m_Index);
}

void RenderGraph::WriteBuffer(RGBufferHandle handle)
{
    SDL_assert(m_IsInsideSetup && "WriteBuffer must be called during Setup phase");

    if (!handle.IsValid() || handle.m_Index >= m_Buffers.size())
    {
        SDL_assert(false && "Invalid buffer handle");
        return;
    }
    
    TransientBuffer& buffer = m_Buffers[handle.m_Index];
    if (!buffer.m_IsDeclaredThisFrame)
    {
        if (m_bVerboseLogging)
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
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] ERROR: Texture '%s' (index %u) declared but never accessed", tex.m_Desc.m_NvrhiDesc.debugName.c_str(), i);
            SDL_assert(false && "Resource declared but never accessed");
        }

        if (tex.m_DeclarationPass > firstAccessPass)
        {
            if (m_bVerboseLogging)
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
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] ERROR: Buffer '%s' (index %u) declared but never accessed", buf.m_Desc.m_NvrhiDesc.debugName.c_str(), i);
            SDL_assert(false && "Resource declared but never accessed");
        }

        if (buf.m_DeclarationPass > firstAccessPass)
        {
            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] ERROR: Buffer '%s' accessed in pass '%s' but only declared in pass '%s'", 
                    buf.m_Desc.m_NvrhiDesc.debugName.c_str(), m_PassNames.at(firstAccessPass), m_PassNames.at(buf.m_DeclarationPass));
            SDL_assert(false && "Resource accessed before it was declared");
        }
    }

    nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice.Get();

    m_Stats.m_NumTextures = m_Stats.m_NumBuffers = 0;
    for (const TransientTexture& tex : m_Textures) if (tex.m_IsDeclaredThisFrame) m_Stats.m_NumTextures++;
    for (const TransientBuffer& buf : m_Buffers) if (buf.m_IsDeclaredThisFrame) m_Stats.m_NumBuffers++;

    AllocateResourcesInternal(
        false,  // bIsBuffer
        [this, device](uint32_t idx, nvrhi::HeapHandle heap, uint64_t offset)
        {
            TransientTexture& texture = m_Textures[idx];

            // Aliased (non-owner) resources must ALWAYS recreate their nvrhi handle
            // each frame.  They are virtual textures re-bound to the owner's heap
            // region, and callers must not cache raw pointers across frames.
            // Only physical owners may use the stable-pointer fast path.
            const bool isAliased = !texture.m_IsPhysicalOwner;

            if (!isAliased &&
                texture.m_PhysicalTexture && 
                texture.m_Heap == heap && 
                texture.m_Offset == offset)
            {
                // Physical owner already bound correctly — reuse the existing handle.
                return;
            }

            PROFILE_SCOPED("CreateTextureAndBindMemory");

            // Capture old pointer so we can assert it changed for aliased resources.
            const nvrhi::ITexture* oldRawPtr = texture.m_PhysicalTexture
                                                ? texture.m_PhysicalTexture.Get() : nullptr;

            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] CREATE-TEXTURE slot %u '%s': isAliased=%d "
                        "oldPtr=%p heap=%p offset=%llu",
                        idx,
                        texture.m_Desc.m_NvrhiDesc.debugName.c_str(),
                        (int)isAliased,
                        (void*)oldRawPtr,
                        (void*)heap.Get(),
                        (unsigned long long)offset);

            texture.m_Desc.m_NvrhiDesc.isVirtual = true;
            // Defer the old handle before overwriting — the move-assign would
            // otherwise drop the refcount inline, potentially triggering a
            // final-release while the GPU is still in-flight (ERROR #921).
            // INVARIANT: after this push_back, texture.m_PhysicalTexture must be
            // null so the subsequent assignment cannot double-release.
            if (texture.m_PhysicalTexture)
            {
                SDL_assert(texture.m_PhysicalTexture.Get() != nullptr &&
                           "Compile: texture handle is non-null but Get() returns null — "
                           "RefCountPtr is in an inconsistent state");
                m_DeferredReleaseTextures.push_back(std::move(texture.m_PhysicalTexture));
                // After std::move the source must be null.
                SDL_assert(texture.m_PhysicalTexture == nullptr &&
                           "Compile: std::move did not null the source texture handle — "
                           "deferred-release invariant violated; old handle may be double-freed");
            }
            texture.m_PhysicalTexture = device->createTexture(texture.m_Desc.m_NvrhiDesc);
            SDL_assert(texture.m_PhysicalTexture != nullptr &&
                       "Compile: device->createTexture returned null — "
                       "out of GPU memory or invalid texture descriptor");
            device->bindTextureMemory(texture.m_PhysicalTexture, heap, offset);
            texture.m_Heap = heap;
            texture.m_Offset = offset;

            // Aliased resources must always produce a fresh handle — if the pointer
            // is identical to the previous frame's handle the driver may have
            // returned a cached object, which violates the aliasing contract.
            SDL_assert((!isAliased || texture.m_PhysicalTexture.Get() != oldRawPtr ||
                        oldRawPtr == nullptr)
                       && "Aliased texture: nvrhi returned the same pointer as last frame "
                          "(handle was not recreated)");
        }
    );

    AllocateResourcesInternal(
        true,  // bIsBuffer
        [this, device](uint32_t idx, nvrhi::HeapHandle heap, uint64_t offset)
        {
            TransientBuffer& buffer = m_Buffers[idx];

            // Aliased (non-owner) resources must ALWAYS recreate their nvrhi handle
            // each frame — same reasoning as for textures above.
            const bool isAliased = !buffer.m_IsPhysicalOwner;

            if (!isAliased &&
                buffer.m_PhysicalBuffer && 
                buffer.m_Heap == heap && 
                buffer.m_Offset == offset)
            {
                // Physical owner already bound correctly — reuse the existing handle.
                return;
            }

            PROFILE_SCOPED("CreateBufferAndBindMemory");

            // Capture old pointer so we can assert it changed for aliased resources.
            const nvrhi::IBuffer* oldRawPtr = buffer.m_PhysicalBuffer
                                               ? buffer.m_PhysicalBuffer.Get() : nullptr;

            if (m_bVerboseLogging)
                SDL_Log("[RenderGraph] CREATE-BUFFER slot %u '%s': isAliased=%d "
                        "oldPtr=%p heap=%p offset=%llu",
                        idx,
                        buffer.m_Desc.m_NvrhiDesc.debugName.c_str(),
                        (int)isAliased,
                        (void*)oldRawPtr,
                        (void*)heap.Get(),
                        (unsigned long long)offset);

            buffer.m_Desc.m_NvrhiDesc.isVirtual = true;
            // Defer the old handle before overwriting — same reasoning as for
            // textures above.
            if (buffer.m_PhysicalBuffer)
            {
                SDL_assert(buffer.m_PhysicalBuffer.Get() != nullptr &&
                           "Compile: buffer handle is non-null but Get() returns null — "
                           "RefCountPtr is in an inconsistent state");
                m_DeferredReleaseBuffers.push_back(std::move(buffer.m_PhysicalBuffer));
                SDL_assert(buffer.m_PhysicalBuffer == nullptr &&
                           "Compile: std::move did not null the source buffer handle — "
                           "deferred-release invariant violated; old handle may be double-freed");
            }
            buffer.m_PhysicalBuffer = device->createBuffer(buffer.m_Desc.m_NvrhiDesc);
            SDL_assert(buffer.m_PhysicalBuffer != nullptr &&
                       "Compile: device->createBuffer returned null — "
                       "out of GPU memory or invalid buffer descriptor");
            device->bindBufferMemory(buffer.m_PhysicalBuffer, heap, offset);
            buffer.m_Heap = heap;
            buffer.m_Offset = offset;

            // Aliased resources must always produce a fresh handle.
            SDL_assert((!isAliased || buffer.m_PhysicalBuffer.Get() != oldRawPtr ||
                        oldRawPtr == nullptr)
                       && "Aliased buffer: nvrhi returned the same pointer as last frame "
                          "(handle was not recreated)");
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
                if (m_bVerboseLogging)
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

void RenderGraph::PostRender()
{
    m_IsCompiled = false;
    // Decrement the post-Shutdown() invalidation countdown here (end of frame)
    // so the flag stays true for the *entire* current frame's DeclareTexture
    // calls.  Decrementing in Reset() would clear the flag one frame too early:
    //   Shutdown() -> counter=2, flag=true
    //   Frame 1 Reset()  -> flag still true  -> DeclareTexture invalidates PT handles
    //   Frame 1 PostRender() -> counter 2->1, flag still true
    //   Frame 2 Reset()  -> flag still true  -> DeclareTexture invalidates depth/GBuffer handles
    //   Frame 2 PostRender() -> counter 1->0, flag=false
    //   Frame 3+ -> normal operation
    if (m_ForceInvalidateFramesRemaining > 0)
    {
        --m_ForceInvalidateFramesRemaining;
        m_bForceInvalidateAllResources = (m_ForceInvalidateFramesRemaining > 0);
    }
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

    nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice.Get();
    
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
            m_Heaps[i].m_LastFrameUsed = g_Renderer.m_FrameNumber;
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
    entry.m_LastFrameUsed = g_Renderer.m_FrameNumber;
    entry.m_HeapIdx = static_cast<uint32_t>(m_Heaps.size());
    
    HeapBlock block;
    block.m_Offset = 0;
    block.m_Size = size;
    block.m_IsFree = true;
    entry.m_Blocks.push_back(block);
    
    m_Heaps.push_back(entry);
    
    //SDL_Log("[RenderGraph] Allocated new heap of size %.2f MB", size / (1024.0 * 1024.0));
    
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

                    heapEntry.m_LastFrameUsed = g_Renderer.m_FrameNumber;
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
    nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice.Get();

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
        
        // Trivial reuse: if already allocated and was an owner, skip logic.
        // Non-owners (aliased resources) must go through the aliasing/allocation
        // path each frame because their nvrhi handle is recreated against the
        // owner's heap region.
        if (resource->m_IsAllocated && resource->m_IsPhysicalOwner && resource->m_HeapIndex != UINT32_MAX)
        {
            HeapEntry& heapEntry = m_Heaps[resource->m_HeapIndex];

            // Heap handle must match what the resource recorded at allocation time.
            // A mismatch means the heap vector was reallocated or the index is stale.
            if (heapEntry.m_Heap != resource->m_Heap)
            {
                if (m_bVerboseLogging)
                    SDL_Log("[RenderGraph] HEAP-MISMATCH %s slot %u ('%s'): "
                            "heapEntry.m_Heap=%p resource->m_Heap=%p heapIdx=%u "
                            "(heap vector may have been reallocated)",
                            bIsBuffer ? "buffer" : "texture",
                            idx,
                            bIsBuffer ? m_Buffers[idx].m_Desc.m_NvrhiDesc.debugName.c_str()
                                      : m_Textures[idx].m_Desc.m_NvrhiDesc.debugName.c_str(),
                            (void*)heapEntry.m_Heap.Get(),
                            (void*)resource->m_Heap.Get(),
                            resource->m_HeapIndex);
                SDL_assert(false && "Heap handle mismatch in trivial-reuse path - "
                           "resource->m_HeapIndex is stale or heap vector was reallocated");
            }

            heapEntry.m_LastFrameUsed = g_Renderer.m_FrameNumber;
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
        if (m_AliasingEnabled && !resource->m_IsPersistent)
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
                    // Store candidate's heap info in locals BEFORE overwriting resource fields,
                    // so the createAndBindResource callback can correctly detect if the physical
                    // resource needs to be recreated (by comparing old m_Heap/m_Offset with new).
                    nvrhi::HeapHandle aliasHeap = candidate->m_Heap;
                    uint64_t aliasOffset = candidate->m_Offset;
                    uint32_t aliasHeapIndex = candidate->m_HeapIndex;
                    uint64_t aliasBlockOffset = candidate->m_BlockOffset;

                resource->m_AliasedFromIndex = candidateIdx;
                resource->m_IsAllocated = true;
                resource->m_IsPhysicalOwner = false;
                
                candidate->m_PhysicalLastPass = std::max(candidate->m_PhysicalLastPass, resource->m_Lifetime.m_LastPass);

                // Log aliasing decisions so they are visible in test output and
                // can be correlated with pool-reuse logs when debugging pointer
                // stability failures.
                if (m_bVerboseLogging)
                    SDL_Log("[RenderGraph] ALIAS %s: slot %u ('%s') aliases slot %u ('%s') "
                            "at heap=%u offset=%llu (passes %u-%u over %u-%u)",
                            bIsBuffer ? "buffer" : "texture",
                            idx,
                            bIsBuffer ? m_Buffers[idx].m_Desc.m_NvrhiDesc.debugName.c_str()
                                      : m_Textures[idx].m_Desc.m_NvrhiDesc.debugName.c_str(),
                            candidateIdx,
                            bIsBuffer ? m_Buffers[candidateIdx].m_Desc.m_NvrhiDesc.debugName.c_str()
                                      : m_Textures[candidateIdx].m_Desc.m_NvrhiDesc.debugName.c_str(),
                            aliasHeapIndex,
                            (unsigned long long)aliasOffset,
                            resource->m_Lifetime.m_FirstPass,
                            resource->m_Lifetime.m_LastPass,
                            bIsBuffer ? m_Buffers[candidateIdx].m_Lifetime.m_FirstPass
                                      : m_Textures[candidateIdx].m_Lifetime.m_FirstPass,
                            bIsBuffer ? m_Buffers[candidateIdx].m_Lifetime.m_LastPass
                                      : m_Textures[candidateIdx].m_Lifetime.m_LastPass);

                createAndBindResource(idx, aliasHeap, aliasOffset);

                // Update resource's heap/offset AFTER the callback so the callback
                // could compare old values against new to decide if rebinding is needed.
                resource->m_Heap = aliasHeap;
                resource->m_Offset = aliasOffset;
                resource->m_HeapIndex = aliasHeapIndex;
                resource->m_BlockOffset = aliasBlockOffset;

                // Sanity: aliased resource must have a valid physical handle after binding.
                SDL_assert((bIsBuffer ? (m_Buffers[idx].m_PhysicalBuffer != nullptr)
                                      : (m_Textures[idx].m_PhysicalTexture != nullptr))
                           && "Aliased resource has null physical handle after createAndBindResource");

                if (bIsBuffer) m_Stats.m_NumAliasedBuffers++;
                else m_Stats.m_NumAliasedTextures++;
                    
                    aliased = true;
                    break;
                }
            }
        }
        
        if (!aliased)
        {
            // Save old heap/offset so the callback can detect changes after SubAllocateResource
            // overwrites them. This ensures the callback correctly recreates the physical resource
            // when the allocation location changes.
            nvrhi::HeapHandle oldHeap = resource->m_Heap;
            uint64_t oldOffset = resource->m_Offset;
            const bool wasPreviouslyAllocated = resource->m_IsAllocated;

            SubAllocateResource(resource, memReq.alignment);

            // SubAllocateResource must always produce a valid heap assignment.
            SDL_assert(resource->m_HeapIndex != UINT32_MAX
                       && "SubAllocateResource failed to assign a heap slot");
            SDL_assert(resource->m_Heap != nullptr
                       && "SubAllocateResource produced a null heap handle");

            resource->m_IsAllocated = true;
            resource->m_IsPhysicalOwner = true;
            resource->m_AliasedFromIndex = UINT32_MAX;
            resource->m_PhysicalLastPass = resource->m_Lifetime.m_LastPass;

            // Pass new heap/offset from SubAllocateResource. The callback compares against
            // the resource's stored values, but SubAllocateResource already overwrote them.
            // We need to temporarily restore old values so the callback can detect changes.
            nvrhi::HeapHandle newHeap = resource->m_Heap;
            uint64_t newOffset = resource->m_Offset;
            resource->m_Heap = oldHeap;
            resource->m_Offset = oldOffset;

            const bool sameLocation = (oldHeap == newHeap && oldOffset == newOffset);
            if (m_bVerboseLogging)
            {
                if (wasPreviouslyAllocated && sameLocation)
                {
                    // Trivial re-bind: same heap block reused after a desc change that freed
                    // and immediately re-allocated the same region.  The callback's early-return
                    // guard will fire and the nvrhi handle will be preserved.
                    SDL_Log("[RenderGraph] TRIVIAL-REBIND %s slot %u ('%s'): "
                            "same heap=%u offset=%llu after desc change",
                            bIsBuffer ? "buffer" : "texture",
                            idx,
                            bIsBuffer ? m_Buffers[idx].m_Desc.m_NvrhiDesc.debugName.c_str()
                                      : m_Textures[idx].m_Desc.m_NvrhiDesc.debugName.c_str(),
                            resource->m_HeapIndex,
                            (unsigned long long)newOffset);
                }
                else if (wasPreviouslyAllocated && !sameLocation)
                {
                    // Location changed: the callback will recreate the nvrhi object.
                    SDL_Log("[RenderGraph] RELOC %s slot %u ('%s'): "
                            "heap %u->%u offset %llu->%llu (desc change or heap pressure)",
                            bIsBuffer ? "buffer" : "texture",
                            idx,
                            bIsBuffer ? m_Buffers[idx].m_Desc.m_NvrhiDesc.debugName.c_str()
                                      : m_Textures[idx].m_Desc.m_NvrhiDesc.debugName.c_str(),
                            (oldHeap ? UINT32_MAX : UINT32_MAX), // heap identity logged via index below
                            resource->m_HeapIndex,
                            (unsigned long long)oldOffset,
                            (unsigned long long)newOffset);
                }
            }

            createAndBindResource(idx, newHeap, newOffset);
            resource->m_Heap = newHeap;
            resource->m_Offset = newOffset;

            // After createAndBindResource the physical handle must be non-null.
            SDL_assert((bIsBuffer ? (m_Buffers[idx].m_PhysicalBuffer  != nullptr)
                                  : (m_Textures[idx].m_PhysicalTexture != nullptr))
                       && "Physical handle is null after createAndBindResource (non-aliased path)");

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
    SDL_assert(m_IsCompiled && "GetTexture cannot be called before Compile() or after PostRender()");

    if (!handle.IsValid() || handle.m_Index >= m_Textures.size())
    {
        SDL_assert(false && "Invalid texture handle");
        return nullptr;
    }
    
    const TransientTexture& texture = m_Textures[handle.m_Index];
    if (!texture.m_IsDeclaredThisFrame)
    {
        if (m_bVerboseLogging)
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
            found |= passAccess.m_WriteTextures.count(handle.m_Index) > 0; // Allow read access if the pass declared write access, since that implies read access as well
        }
        else if (access == RGResourceAccessMode::Write)
        {
            found = passAccess.m_WriteTextures.count(handle.m_Index) > 0;
        }

        if (!found)
        {
            if (m_bVerboseLogging)
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
    SDL_assert(m_IsCompiled && "GetBuffer cannot be called before Compile() or after PostRender()");

    if (!handle.IsValid() || handle.m_Index >= m_Buffers.size())
    {
        SDL_assert(false && "Invalid buffer handle");
        return nullptr;
    }
    
    const TransientBuffer& buffer = m_Buffers[handle.m_Index];
    if (!buffer.m_IsDeclaredThisFrame)
    {
        if (m_bVerboseLogging)
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
            found |= passAccess.m_WriteBuffers.count(handle.m_Index) > 0; // Allow read access if the pass declared write access, since that implies read access as well
        }
        else if (access == RGResourceAccessMode::Write)
        {
            found = passAccess.m_WriteBuffers.count(handle.m_Index) > 0;
        }

        if (!found)
        {
            if (m_bVerboseLogging)
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

nvrhi::TextureHandle RenderGraph::GetTextureRaw(RGTextureHandle handle) const
{
    if (!handle.IsValid() || handle.m_Index >= m_Textures.size())
    {
        return nullptr;
    }
    // Only return the physical texture if it was declared (active) this frame.
    // Stale handles for resources not scheduled this frame must appear as nullptr.
    const auto& tex = m_Textures[handle.m_Index];
    if (!tex.m_IsDeclaredThisFrame)
    {
        return nullptr;
    }
    return tex.m_PhysicalTexture;
}

nvrhi::BufferHandle RenderGraph::GetBufferRaw(RGBufferHandle handle) const
{
    if (!handle.IsValid() || handle.m_Index >= m_Buffers.size())
    {
        return nullptr;
    }
    // Same active-this-frame guard as GetTextureRaw
    const auto& buf = m_Buffers[handle.m_Index];
    if (!buf.m_IsDeclaredThisFrame)
    {
        return nullptr;
    }
    return buf.m_PhysicalBuffer;
}

uint16_t RenderGraph::GetPassIndex(const char* passName) const
{
    if (!passName) return 0;
    for (uint16_t i = 0; i < static_cast<uint16_t>(m_PassNames.size()); ++i)
    {
        if (m_PassNames[i] && std::string_view(m_PassNames[i]) == std::string_view(passName))
            return static_cast<uint16_t>(i + 1); // pass indices are 1-based
    }
    return 0; // not found / pass was disabled this frame
}

RGBufferDesc RenderGraph::GetSPDAtomicCounterDesc(const char* debugName)
{
    RGBufferDesc desc;
    desc.m_NvrhiDesc.structStride = sizeof(uint32_t);
    desc.m_NvrhiDesc.byteSize = sizeof(uint32_t);
    desc.m_NvrhiDesc.canHaveUAVs = true;
    desc.m_NvrhiDesc.debugName = debugName;
    desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    return desc;
}
