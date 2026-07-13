#include "FeedbackManager.h"
#include "Renderer.h"

#include "d3d12.h"

namespace nvfeedback
{
    const uint64_t kHeapSizeInBytes = kHeapSizeInTiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    static constexpr bool kStreamingDebugLog = false;

    // ─── HeapAllocator ───────────────────────────────────────────────────────
    uint32_t HeapAllocator::AllocateHeap()
    {
        nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice;

        nvrhi::HeapDesc heapDesc{};
        heapDesc.capacity = kHeapSizeInBytes;
        heapDesc.type = nvrhi::HeapType::DeviceLocal;
        nvrhi::HeapHandle heap = device->createHeap(heapDesc);

        nvrhi::BufferDesc bufferDesc{};
        bufferDesc.byteSize = kHeapSizeInBytes;
        bufferDesc.isVirtual = true;
        bufferDesc.initialState = nvrhi::ResourceStates::CopySource;
        bufferDesc.keepInitialState = true;
        nvrhi::BufferHandle buffer = device->createBuffer(bufferDesc);

        device->bindBufferMemory(buffer, heap, 0);

        uint32_t heapId;
        if (m_FreeHeapIds.empty())
        {
            heapId = (uint32_t)m_Heaps.size();
            m_Heaps.push_back(heap);
            m_Buffers.push_back(buffer);

            if constexpr (kStreamingDebugLog)
            {
                SDL_Log("[Streaming][Heap] AllocateHeap: new slot %u (total heaps: %u, VRAM: %.2f MB)",
                        heapId, m_NumHeaps + 1, BYTES_TO_MB(m_TotalAllocatedBytes + kHeapSizeInBytes));
            }
        }
        else
        {
            heapId = m_FreeHeapIds.back();
            m_FreeHeapIds.pop_back();
            m_Heaps[heapId] = heap;
            m_Buffers[heapId] = buffer;

            if constexpr (kStreamingDebugLog)
            {
                SDL_Log("[Streaming][Heap] AllocateHeap: reuse slot %u from free list (total heaps: %u, VRAM: %.2f MB)",
                        heapId, m_NumHeaps + 1, BYTES_TO_MB(m_TotalAllocatedBytes + kHeapSizeInBytes));
            }
        }

        m_TotalAllocatedBytes += kHeapSizeInBytes;
        m_NumHeaps++;

        return heapId;
    }

    void HeapAllocator::ReleaseHeap(uint32_t heapId, uint32_t frameIndex)
    {
        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] ReleaseHeap: heapId=%u frameIndex=%u bucket=%u "
                    "(old Heaps[%u]=%p, VRAM before: %.2f MB) — deferred destroy, "
                    "WARNING: if any tiles are still mapped to this heap the GPU will TDR!",
                    heapId, frameIndex, frameIndex % kNumFramesInFlight,
                    heapId, (void*)m_Heaps[heapId].Get(),
                    BYTES_TO_MB(m_TotalAllocatedBytes));
        }

        m_FreeHeapIds.push_back(heapId);

        // Defer actual destruction
        uint32_t bucket = frameIndex % kNumFramesInFlight;
        m_BuffersToRelease[bucket].push_back(m_Buffers[heapId]);
        m_HeapsToRelease[bucket].push_back(m_Heaps[heapId]);

        m_Heaps[heapId] = nullptr;
        m_Buffers[heapId] = nullptr;

        m_TotalAllocatedBytes -= kHeapSizeInBytes;
        m_NumHeaps--;

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] ReleaseHeap: heapId=%u done (VRAM after: %.2f MB, in-use heaps: %u)",
                    heapId, BYTES_TO_MB(m_TotalAllocatedBytes), m_NumHeaps);
        }
    }

    void HeapAllocator::DrainReleaseQueue(uint32_t frameIndex)
    {
        uint32_t bucket = frameIndex % kNumFramesInFlight;
        size_t numHeaps = m_HeapsToRelease[bucket].size();

        if (numHeaps > 0)
        {
            if constexpr (kStreamingDebugLog)
            {
                SDL_Log("[Streaming][Heap] DrainReleaseQueue: frameIndex=%u bucket=%u — destroying %zu deferred heap(s) "
                        "(these were released %u frames ago, GPU should be done with them)",
                        frameIndex, bucket, numHeaps, kNumFramesInFlight);
            }

            for (size_t i = 0; i < numHeaps; ++i)
            {
                if constexpr (kStreamingDebugLog)
                {
                    SDL_Log("[Streaming][Heap] DrainReleaseQueue:   heap handle=%p buffer handle=%p",
                            (void*)m_HeapsToRelease[bucket][i].Get(),
                            (void*)m_BuffersToRelease[bucket][i].Get());
                }
            }
        }

        m_HeapsToRelease[bucket].clear();
        m_BuffersToRelease[bucket].clear();
    }

    // ─── FeedbackManager ─────────────────────────────────────────────────────

    FeedbackManager::FeedbackManager()
    {
        m_HeapAllocator = std::make_unique<HeapAllocator>();

        rtxts::TiledTextureManagerDesc tiledTextureManagerDesc{};
        tiledTextureManagerDesc.heapTilesCapacity = kHeapSizeInTiles;
        m_TiledTextureManager = std::unique_ptr<rtxts::TiledTextureManager>(
            rtxts::CreateTiledTextureManager(tiledTextureManagerDesc));
    }

    FeedbackTexture* FeedbackManager::CreateTexture(const nvrhi::TextureDesc& desc)
    {
        std::unique_ptr<FeedbackTexture> feedbackTexture = std::make_unique<FeedbackTexture>(desc, m_TiledTextureManager.get());
        FeedbackTexture* rawPtr = feedbackTexture.get();
        uint32_t idx = (uint32_t)m_Textures.size();
        rawPtr->SetManagerIndex(idx);
        m_Textures.push_back(std::move(feedbackTexture));
        m_TexturesRingbuffer.push_back(idx);
        return rawPtr;
    }

    void FeedbackManager::UnregisterTexture(uint32_t textureIdx)
    {
        // Remove from readback slots
        for (std::vector<uint32_t>& vec : m_TexturesToReadback)
        {
            auto it = std::find(vec.begin(), vec.end(), textureIdx);
            if (it != vec.end()) vec.erase(it);
        }

        // Remove from ringbuffer
        {
            auto it = std::find(m_TexturesRingbuffer.begin(), m_TexturesRingbuffer.end(), textureIdx);
            if (it != m_TexturesRingbuffer.end())
            {
                const size_t pos = it - m_TexturesRingbuffer.begin();
                m_TexturesRingbuffer.erase(it);
                if (pos < m_RingbufferCursor && m_RingbufferCursor > 0)
                    --m_RingbufferCursor;
                if (!m_TexturesRingbuffer.empty())
                    m_RingbufferCursor %= (uint32_t)m_TexturesRingbuffer.size();
                else
                    m_RingbufferCursor = 0;
            }
        }

        m_MinMipDirtyTextures.erase(textureIdx);

        // Release ownership — this destroys the FeedbackTexture (and its nvrhi resources)
        m_Textures.erase(m_Textures.begin() + textureIdx);

        // Fix up m_ManagerIndex for textures that shifted
        for (uint32_t i = textureIdx; i < (uint32_t)m_Textures.size(); ++i)
            m_Textures[i]->SetManagerIndex(i);
    }

    void FeedbackManager::UpdateTextureRingBufferState(uint32_t textureIdx, bool bIncludeInRingBuffer)
    {
        auto it = std::find(m_TexturesRingbuffer.begin(), m_TexturesRingbuffer.end(), textureIdx);
        if (bIncludeInRingBuffer && it == m_TexturesRingbuffer.end())
        {
            m_TexturesRingbuffer.push_back(textureIdx);
        }
        else if (!bIncludeInRingBuffer && it != m_TexturesRingbuffer.end())
        {
            const size_t pos = it - m_TexturesRingbuffer.begin();
            m_TexturesRingbuffer.erase(it);
            if (pos < m_RingbufferCursor && m_RingbufferCursor > 0)
                --m_RingbufferCursor;
            if (!m_TexturesRingbuffer.empty())
                m_RingbufferCursor %= (uint32_t)m_TexturesRingbuffer.size();
            else
                m_RingbufferCursor = 0;
        }
    }

    void FeedbackManager::BeginFrame(nvrhi::ICommandList* commandList, FeedbackTextureCollection& results)
    {
        SimpleTimer timer;

        m_FrameIndex = g_Renderer.m_FrameNumber % kNumFramesInFlight;

        // Drain deferred heap/buffer releases that are now safely past the
        // GPU fence (NumFramesInFlight frames have elapsed).
        m_HeapAllocator->DrainReleaseQueue(m_FrameIndex);

        // Update TTM config
        rtxts::TiledTextureManagerConfig ttmConfig{};
        ttmConfig.numExtraStandbyTiles = 0;
        m_TiledTextureManager->SetConfig(ttmConfig);

        // ── Step 1: Read back feedback from N frames ago ──
        std::vector<uint32_t>& readbackTextures = m_TexturesToReadback[m_FrameIndex];
        if (!readbackTextures.empty())
        {
            float timeStamp = static_cast<float>(GetTickCount64()) / 1000.0f;
            for (uint32_t texIdx : readbackTextures)
            {
                FeedbackTexture* readbackTexture = GetTextureByIndex(texIdx);
                uint8_t* pReadbackData = static_cast<uint8_t*>(
                    g_Renderer.m_RHI->m_NvrhiDevice->mapBuffer(readbackTexture->GetFeedbackResolveBuffer(m_FrameIndex),
                                        nvrhi::CpuAccessMode::Read));

                rtxts::SamplerFeedbackDesc samplerFeedbackDesc{};
                samplerFeedbackDesc.pMinMipData = pReadbackData;
                m_TiledTextureManager->UpdateWithSamplerFeedback(
                    readbackTexture->GetTiledTextureId(),
                    samplerFeedbackDesc,
                    timeStamp,
                    0.0f);

                g_Renderer.m_RHI->m_NvrhiDevice->unmapBuffer(readbackTexture->GetFeedbackResolveBuffer(m_FrameIndex));

                // Propagate to follower textures in the same set
                if (readbackTexture->IsPrimaryTexture())
                {
                    for (FeedbackTextureSet* textureSet : readbackTexture->GetPrimaryTextureSets())
                    {
                        uint32_t numTextures = textureSet->GetNumTextures();
                        uint32_t primaryIdx  = textureSet->GetPrimaryTextureIndex();
                        for (uint32_t i = 0; i < numTextures; ++i)
                        {
                            if (i == primaryIdx) continue;
                            FeedbackTexture* follower = textureSet->GetTexture(i);
                            m_TiledTextureManager->MatchPrimaryTexture(
                                readbackTexture->GetTiledTextureId(),
                                follower->GetTiledTextureId(),
                                timeStamp,
                                0.0f);
                        }
                    }
                }
            }
        }

        // ── Step 2: Collect textures to read back NEXT frame ──
        // Assign to the NEXT slot so that the following frame's Step 1 reads from the
        // correct index (ResolveFeedback also decodes into this next slot).
        {
            std::vector<uint32_t>& nextReadbackTextures = m_TexturesToReadback[(m_FrameIndex + 1) % kNumFramesInFlight];
            nextReadbackTextures.clear();
            if (!m_TexturesRingbuffer.empty())
            {
                uint32_t updatesLeft = UINT32_MAX;
                const uint32_t count = (uint32_t)m_TexturesRingbuffer.size();
                for (uint32_t i = 0; i < count && updatesLeft > 0; ++i)
                {
                    uint32_t texIdx = m_TexturesRingbuffer[(m_RingbufferCursor + i) % count];
                    FeedbackTexture* feedbackTexture = GetTextureByIndex(texIdx);
                    commandList->clearSamplerFeedbackTexture(feedbackTexture->GetSamplerFeedbackTexture());
                    nextReadbackTextures.push_back(texIdx);
                    updatesLeft--;
                }
            }
        }

        // ── Step 3: Trim standby tiles ──
        m_TiledTextureManager->TrimStandbyTiles();

        // ── Step 4: Heap management ──
        uint32_t numRequiredHeaps = m_TiledTextureManager->GetNumDesiredHeaps();

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] BeginFrame heap check: required=%u TTMRegistered=%u totalHeaps=%u packedMipCursor=%u",
                    numRequiredHeaps, m_NumTTMHeaps, m_HeapAllocator->GetNumHeaps(),
                    m_PackedMipTileCursor);
        }

        if (numRequiredHeaps > m_NumTTMHeaps)
        {
            if constexpr (kStreamingDebugLog)
            {
                SDL_Log("[Streaming][Heap] BeginFrame: need %u more TTM heap(s) (required=%u > registered=%u)",
                        numRequiredHeaps - m_NumTTMHeaps, numRequiredHeaps, m_NumTTMHeaps);
            }

            while (m_NumTTMHeaps < numRequiredHeaps)
            {
                uint32_t heapId = m_HeapAllocator->AllocateHeap();
                m_TiledTextureManager->AddHeap(heapId);
                m_NumTTMHeaps++;

                if constexpr (kStreamingDebugLog)
                {
                    SDL_Log("[Streaming][Heap] BeginFrame: added heapId=%u to TTM (TTM-registered=%u, total=%u)",
                            heapId, m_NumTTMHeaps, m_HeapAllocator->GetNumHeaps());
                }
            }
        }
        else
        {
            std::vector<uint32_t> emptyHeaps;
            m_TiledTextureManager->GetEmptyHeaps(emptyHeaps);

            if (!emptyHeaps.empty())
            {
                if constexpr (kStreamingDebugLog)
                {
                    SDL_Log("[Streaming][Heap] BeginFrame: TTM reports %zu empty heap(s) — releasing:",
                            emptyHeaps.size());
                }
            }

            for (uint32_t heapId : emptyHeaps)
            {
                if constexpr (kStreamingDebugLog)
                {
                    SDL_Log("[Streaming][Heap] BeginFrame: releasing empty TTM heapId=%u "
                            "(only TTM-registered heaps appear here; packed-mip heaps are invisible to TTM)",
                            heapId);
                }
                m_TiledTextureManager->RemoveHeap(heapId);
                m_HeapAllocator->ReleaseHeap(heapId, m_FrameIndex);
                m_NumTTMHeaps--;
            }
        }

        // ── Step 5: Allocate requested tiles ──
        m_TiledTextureManager->AllocateRequestedTiles();

        // ── Step 6: Collect tiles to unmap and map ──
        std::vector<uint32_t> tilesRequestedNew;
        std::vector<uint32_t> tilesToUnmap;

        for (uint32_t texIdx = 0; texIdx < (uint32_t)m_Textures.size(); ++texIdx)
        {
            FeedbackTexture* feedbackTexture = m_Textures.at(texIdx).get();
            // Unmap tiles
            m_TiledTextureManager->GetTilesToUnmap(feedbackTexture->GetTiledTextureId(), tilesToUnmap);
            if (!tilesToUnmap.empty())
            {
                const std::vector<rtxts::TileCoord>& tileCoords = m_TiledTextureManager->GetTileCoordinates(feedbackTexture->GetTiledTextureId());
                uint32_t tileToUnmapNum = (uint32_t)tilesToUnmap.size();

                nvrhi::TiledTextureRegion tiledTextureRegion{};
                tiledTextureRegion.tilesNum = 1;

                nvrhi::TextureTilesMapping textureTilesMapping{};
                textureTilesMapping.numTextureRegions = tileToUnmapNum;
                std::vector<nvrhi::TiledTextureCoordinate> tiledTextureCoordinates(tileToUnmapNum);
                std::vector<nvrhi::TiledTextureRegion> tiledTextureRegions(tileToUnmapNum, tiledTextureRegion);
                textureTilesMapping.tiledTextureCoordinates = tiledTextureCoordinates.data();
                textureTilesMapping.tiledTextureRegions = tiledTextureRegions.data();

                uint32_t tilesProcessedNum = 0;
                for (uint32_t tileIndex : tilesToUnmap)
                {
                    nvrhi::TiledTextureCoordinate& coord = tiledTextureCoordinates[tilesProcessedNum];
                    coord.mipLevel = tileCoords[tileIndex].mipLevel;
                    coord.arrayLevel = 0;
                    coord.x = tileCoords[tileIndex].x;
                    coord.y = tileCoords[tileIndex].y;
                    coord.z = 0;
                    tilesProcessedNum++;
                }

                g_Renderer.m_RHI->m_NvrhiDevice->updateTextureTileMappings(feedbackTexture->GetReservedTexture(), &textureTilesMapping, 1);
                m_MinMipDirtyTextures.insert(texIdx);
            }

            // Collect new tiles to stream in (skip packed mips — handled at scene load)
            m_TiledTextureManager->GetTilesToMap(feedbackTexture->GetTiledTextureId(), tilesRequestedNew);
            if (!tilesRequestedNew.empty())
            {
                FeedbackTextureUpdate update;
                update.m_TextureIdx = texIdx;
                for (uint32_t tileIndex : tilesRequestedNew)
                {
                    if (feedbackTexture->IsTilePacked(tileIndex))
                        continue; // packed mips are permanently mapped at scene load
                    update.m_TileIndices.push_back(tileIndex);
                }
                if (!update.m_TileIndices.empty())
                    results.m_Textures.push_back(update);
            }
        }

        // ── Step 7: Defragmentation ──
        m_TiledTextureManager->DefragmentTiles(16);

        m_BeginFrameCPUTime = timer.LapSeconds();
    }

    void FeedbackManager::UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection& tilesReady)
    {
        SimpleTimer timer;

        for (FeedbackTextureUpdate& texUpdate : tilesReady.m_Textures)
        {
            FeedbackTexture* texture = GetTextureByIndex(texUpdate.m_TextureIdx);
            m_MinMipDirtyTextures.insert(texUpdate.m_TextureIdx);

            uint32_t tiledTextureId = texture->GetTiledTextureId();
            m_TiledTextureManager->UpdateTilesMapping(tiledTextureId, texUpdate.m_TileIndices);

            const std::vector<rtxts::TileCoord>& tileCoords     = m_TiledTextureManager->GetTileCoordinates(tiledTextureId);
            const std::vector<rtxts::TileAllocation>& tileAllocations = m_TiledTextureManager->GetTileAllocations(tiledTextureId);

            // Group tiles by heap for batched updateTextureTileMappings calls
            std::map<nvrhi::HeapHandle, std::vector<uint32_t>> heapTilesMapping;
            for (uint32_t tileIndex : texUpdate.m_TileIndices)
            {
                if (texture->IsTilePacked(tileIndex))
                    continue; // packed mips are permanently mapped at scene load
                nvrhi::HeapHandle heap = m_HeapAllocator->GetHeapHandle(tileAllocations[tileIndex].heapId);
                heapTilesMapping[heap].push_back(tileIndex);
            }

            for (auto& [heap, heapTiles] : heapTilesMapping)
            {
                uint32_t numTiles = (uint32_t)heapTiles.size();

                if constexpr (kStreamingDebugLog)
                {
                    SDL_Log("[Streaming][Heap] UpdateTileMappings: textureIdx=%u mapping %u tiles to heap=%p "
                            "(verify heap is not in release queue!)",
                            texUpdate.m_TextureIdx, numTiles, (void*)heap.Get());
                }

                std::vector<nvrhi::TiledTextureCoordinate> tiledTextureCoordinates;
                std::vector<nvrhi::TiledTextureRegion>     tiledTextureRegions;
                std::vector<uint64_t>                      byteOffsets;

                tiledTextureCoordinates.reserve(numTiles);
                tiledTextureRegions.reserve(numTiles);
                byteOffsets.reserve(numTiles);

                for (uint32_t i = 0; i < numTiles; i++)
                {
                    uint32_t tileIndex = heapTiles[i];

                    nvrhi::TiledTextureCoordinate coord{};
                    coord.mipLevel   = tileCoords[tileIndex].mipLevel;
                    coord.x          = tileCoords[tileIndex].x;
                    coord.y          = tileCoords[tileIndex].y;
                    coord.z          = 0;
                    tiledTextureCoordinates.push_back(coord);

                    nvrhi::TiledTextureRegion region{};
                    region.tilesNum = 1;
                    tiledTextureRegions.push_back(region);

                    byteOffsets.push_back(static_cast<uint64_t>(tileAllocations[tileIndex].heapTileIndex) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
                }

                nvrhi::TextureTilesMapping textureTilesMapping{};
                textureTilesMapping.numTextureRegions       = numTiles;
                textureTilesMapping.tiledTextureCoordinates = tiledTextureCoordinates.data();
                textureTilesMapping.tiledTextureRegions     = tiledTextureRegions.data();
                textureTilesMapping.byteOffsets             = byteOffsets.data();
                textureTilesMapping.heap                    = heap;

                g_Renderer.m_RHI->m_NvrhiDevice->updateTextureTileMappings(texture->GetReservedTexture(), &textureTilesMapping, 1);
            }
        }

        // Upload MinMip data for dirty textures
        if (!m_MinMipDirtyTextures.empty())
        {
            const bool bUseAutomaticBarriers = false;
            commandList->setEnableAutomaticBarriers(bUseAutomaticBarriers);

            if (!bUseAutomaticBarriers)
            {
                for (uint32_t texIdx : m_MinMipDirtyTextures)
                {
                    commandList->setTextureState(GetTextureByIndex(texIdx)->GetMinMipTexture(), nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
                }
            }

            std::vector<uint8_t> minMipData;
            std::vector<uint8_t> uploadData;

            for (uint32_t texIdx : m_MinMipDirtyTextures)
            {
                FeedbackTexture* texture = GetTextureByIndex(texIdx);
                rtxts::TextureDesc desc = m_TiledTextureManager->GetTextureDesc(texture->GetTiledTextureId(), rtxts::TextureTypes::eMinMipTexture);

                // R8_UINT: 1 byte per texel, rowPitch = width (no padding needed for writeTexture)
                uint32_t numElements = desc.textureOrMipRegionWidth * desc.textureOrMipRegionHeight;
                uint32_t rowPitch = desc.textureOrMipRegionWidth;
                minMipData.resize(numElements);

                m_TiledTextureManager->WriteMinMipData(texture->GetTiledTextureId(), minMipData.data());

                commandList->writeTexture(texture->GetMinMipTexture(), 0, 0, minMipData.data(), rowPitch);
            }

            if (!bUseAutomaticBarriers)
            {
                for (uint32_t texIdx : m_MinMipDirtyTextures)
                {
                    commandList->setTextureState(GetTextureByIndex(texIdx)->GetMinMipTexture(), nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
                }
            }

            m_MinMipDirtyTextures.clear();
            commandList->setEnableAutomaticBarriers(true);
        }

        m_UpdateTileMappingsCPUTime = timer.LapSeconds();
    }

    void FeedbackManager::ResolveFeedback(nvrhi::ICommandList* commandList)
    {
        // Use the NEXT slot — matches BeginFrame Step 2 which also assigns to (m_FrameIndex+1)%F
        std::vector<uint32_t>& readbackTextures = m_TexturesToReadback[(m_FrameIndex + 1) % kNumFramesInFlight];
        if (readbackTextures.empty())
        {
            m_ResolveCPUTime = 0.0;
            return;
        }

        SimpleTimer timer;

        const bool bUseAutomaticBarriers = false;
        commandList->setEnableAutomaticBarriers(bUseAutomaticBarriers);

        if (!bUseAutomaticBarriers)
        {
            for (uint32_t texIdx : readbackTextures)
                commandList->setSamplerFeedbackTextureState(
                    GetTextureByIndex(texIdx)->GetSamplerFeedbackTexture(),
                    nvrhi::ResourceStates::ResolveSource);
        }

        for (uint32_t texIdx : readbackTextures)
        {
            FeedbackTexture* texture = GetTextureByIndex(texIdx);
            // Decode into the NEXT slot to match BeginFrame Step 2's assignment
            const uint32_t resolveSlot = (m_FrameIndex + 1) % kNumFramesInFlight;
            commandList->decodeSamplerFeedbackTexture(
                texture->GetFeedbackResolveBuffer(resolveSlot),
                texture->GetSamplerFeedbackTexture(),
                nvrhi::Format::R8_UINT);
        }

        if (!bUseAutomaticBarriers)
        {
            for (uint32_t texIdx : readbackTextures)
                commandList->setSamplerFeedbackTextureState(
                    GetTextureByIndex(texIdx)->GetSamplerFeedbackTexture(),
                    nvrhi::ResourceStates::UnorderedAccess);
        }

        commandList->setEnableAutomaticBarriers(true);

        m_ResolveCPUTime = timer.LapSeconds();
    }

    void FeedbackManager::EndFrame()
    {
        // Advance ring buffer cursor by the number of textures processed this frame
        if (!m_TexturesRingbuffer.empty() && UINT32_MAX > 0)
        {
            const uint32_t count = (uint32_t)m_TexturesRingbuffer.size();
            // Clamp to count; use 64-bit arithmetic to safely handle UINT32_MAX
            const uint64_t advance = std::min<uint64_t>(UINT32_MAX, count);
            m_RingbufferCursor = (uint32_t)(((uint64_t)m_RingbufferCursor + advance) % count);
        }

        // Update stats
        m_StatsLastFrame.m_HeapAllocationInBytes = m_HeapAllocator->GetTotalAllocatedBytes();
        m_StatsLastFrame.m_NumHeaps    = m_HeapAllocator->GetNumHeaps();
        m_StatsLastFrame.m_NumTTMHeaps = m_NumTTMHeaps;
        m_StatsLastFrame.m_CpuTimeBeginFrame = m_BeginFrameCPUTime;
        m_StatsLastFrame.m_CpuTimeUpdateTileMappings = m_UpdateTileMappingsCPUTime;
        m_StatsLastFrame.m_CpuTimeResolve = m_ResolveCPUTime;

        rtxts::Statistics stats = m_TiledTextureManager->GetStatistics();
        m_StatsLastFrame.m_TilesAllocated = stats.allocatedTilesNum;
        m_StatsLastFrame.m_TilesTotal     = stats.totalTilesNum;
        m_StatsLastFrame.m_HeapTilesFree  = stats.heapFreeTilesNum;
        m_StatsLastFrame.m_TilesStandby   = stats.standbyTilesNum;

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] EndFrame summary: totalHeaps=%u TTMHeaps=%u VRAM=%.2fMB tiles(alloc=%u total=%u free=%u stby=%u) packedMipCursor=%u",
                    m_HeapAllocator->GetNumHeaps(),
                    m_NumTTMHeaps,
                    BYTES_TO_MB(m_HeapAllocator->GetTotalAllocatedBytes()),
                    stats.allocatedTilesNum,
                    stats.totalTilesNum,
                    stats.heapFreeTilesNum,
                    stats.standbyTilesNum,
                    m_PackedMipTileCursor);
        }
    }

    const FeedbackManagerStats& FeedbackManager::GetStats() const
    {
        return m_StatsLastFrame;
    }

    void FeedbackManager::MapPackedMips(uint32_t textureIdx)
    {
        FeedbackTexture* texture = m_Textures.at(textureIdx).get();
        const nvrhi::PackedMipDesc& packedMipDesc = texture->GetPackedMipInfo();
        if (packedMipDesc.numPackedMips == 0)
            return;

        const uint32_t numPackedTiles = packedMipDesc.numTilesForPackedMips;
        const uint32_t firstSubresource = packedMipDesc.numStandardMips;

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] MapPackedMips: textureIdx=%u needs %u packed tile(s) "
                    "(cursor before=%u, current heaps=%u)",
                    textureIdx, numPackedTiles, m_PackedMipTileCursor,
                    m_HeapAllocator->GetNumHeaps());
        }

        // Ensure enough heaps are allocated for these packed mips.
        // These heaps are NOT registered with TTM (no AddHeap) because TTM
        // would otherwise allocate streaming tiles into packed-mip physical
        // slots via AllocateRequestedTiles().  Packed mips use their own
        // dedicated heaps managed purely through updateTextureTileMappings.
        {
            const uint32_t totalNeeded = m_PackedMipTileCursor + numPackedTiles;
            const uint32_t heapsNeeded = (totalNeeded + kHeapSizeInTiles - 1) / kHeapSizeInTiles;

            if constexpr (kStreamingDebugLog)
            {
                SDL_Log("[Streaming][Heap] MapPackedMips: cursor after=%u, heapsNeeded=%u, currentHeaps=%u",
                        totalNeeded, heapsNeeded, m_HeapAllocator->GetNumHeaps());
            }

            while (m_HeapAllocator->GetNumHeaps() < heapsNeeded)
            {
                uint32_t heapId = m_HeapAllocator->AllocateHeap();
                // Intentionally NOT calling AddHeap — packed-mip heaps are
                // invisible to TTM so GetEmptyHeaps() never reports them.
                if constexpr (kStreamingDebugLog)
                {
                    SDL_Log("[Streaming][Heap] MapPackedMips: allocated heapId=%u for packed mips "
                            "(NOT added to TTM, total heaps=%u)",
                            heapId, m_HeapAllocator->GetNumHeaps());
                }
            }
        }

        // Suballocate these packed mip tiles from the shared heap pool.
        const uint32_t startHeapId    = m_PackedMipTileCursor / kHeapSizeInTiles;
        const uint32_t startLocalTile = m_PackedMipTileCursor % kHeapSizeInTiles;

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] MapPackedMips: suballocating %u tiles at heapId=%u localTile=%u "
                    "(heaps total=%u, TTM-registered=%u)",
                    numPackedTiles, startHeapId, startLocalTile,
                    m_HeapAllocator->GetNumHeaps(), m_NumTTMHeaps);
        }

        m_PackedMipTileCursor += numPackedTiles;

        // Map packed mips as a single contiguous tile region starting at the first
        // packed subresource.  In nvrhi, TiledTextureCoordinate::mipLevel maps to
        // D3D12's Subresource index.
        nvrhi::TiledTextureCoordinate startCoord{};
        startCoord.mipLevel   = static_cast<uint16_t>(firstSubresource);
        startCoord.arrayLevel = 0;
        startCoord.x = 0;
        startCoord.y = 0;
        startCoord.z = 0;

        nvrhi::TiledTextureRegion region{};
        region.tilesNum = numPackedTiles;

        std::vector<uint64_t> byteOffsets(numPackedTiles);
        for (uint32_t i = 0; i < numPackedTiles; ++i)
            byteOffsets[i] = static_cast<uint64_t>(startLocalTile + i) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

        nvrhi::TextureTilesMapping mapping{};
        mapping.numTextureRegions       = 1;
        mapping.tiledTextureCoordinates = &startCoord;
        mapping.tiledTextureRegions     = &region;
        mapping.byteOffsets             = byteOffsets.data();
        mapping.heap                    = m_HeapAllocator->GetHeapHandle(startHeapId);

        g_Renderer.m_RHI->m_NvrhiDevice->updateTextureTileMappings(texture->GetReservedTexture(), &mapping, 1);

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming] MapPackedMips: mapped %u packed mip tile(s) for subresources %u-%u "
                    "onto heap %u at local tile %u",
                    numPackedTiles, firstSubresource,
                    firstSubresource + packedMipDesc.numPackedMips - 1,
                    startHeapId, startLocalTile);
        }
    }

} // namespace nvfeedback
