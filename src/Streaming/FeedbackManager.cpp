#include "FeedbackManager.h"
#include "Renderer.h"

#include "d3d12.h"

namespace nvfeedback
{
    const uint64_t kHeapSizeInBytes = kHeapSizeInTiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

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

    void FeedbackManager::BeginFrame(nvrhi::ICommandList* commandList, std::vector<FeedbackTextureUpdate>& results, bool allowHeapRelease)
    {
        PROFILE_FUNCTION();
        SimpleTimer timer;

        m_FrameIndex = g_Renderer.m_FrameNumber % kNumFramesInFlight;

        // Drain deferred heap/buffer releases that are now safely past the
        // GPU fence (NumFramesInFlight frames have elapsed).
        m_HeapAllocator->DrainReleaseQueue(m_FrameIndex);

        // Update TTM config
        rtxts::TiledTextureManagerConfig ttmConfig{};
        // Standby tiles buffer: up to 64 tiles can be in standby before
        // TrimStandbyTiles() starts freeing the oldest.  This is necessary
        // for hysteresis (kTileHysteresisSeconds) to work — with 0, standby
        // tiles would be freed immediately and re-requested tiles would have
        // to go through the full allocate+submit+pending+mapping cycle.
        // 64 tiles ≈ 4 MB of VRAM headroom for hysteresis.
        ttmConfig.numExtraStandbyTiles = 64;
        m_TiledTextureManager->SetConfig(ttmConfig);

        // ── Step 1: Read back feedback from N frames ago ──
        std::vector<uint32_t>& readbackTextures = m_TexturesToReadback[m_FrameIndex];
        float timeStamp = static_cast<float>(GetTickCount64()) / 1000.0f;

        if (!readbackTextures.empty())
        {
            PROFILE_SCOPED("Resolve feedback");
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
                    kTileHysteresisSeconds);

                g_Renderer.m_RHI->m_NvrhiDevice->unmapBuffer(readbackTexture->GetFeedbackResolveBuffer(m_FrameIndex));
            }
        }

        // NOTE: No re-submission of cached feedback for non-readback textures.
        // The reference implementations (RTXTS, ToyRenderer) only call UpdateWithSamplerFeedback
        // for the textures in the current readback batch — never for all textures every frame.
        // Re-submitting cached feedback for all textures every frame causes GetNumDesiredHeaps()
        // to reflect the full tile demand of ALL textures simultaneously, driving a burst to
        // 34 heaps instead of the gradual growth to 11 that the references exhibit.
        // Without re-submission, tiles time out after kTileHysteresisSeconds (1s).
        // With 307 textures at 30/frame the ringbuffer cycle is ~10 frames (~167ms at 60fps),
        // giving 6 full cycles of margin before any tile times out.

        // ── Step 2: Collect textures to read back NEXT frame ──
        // Assign to the NEXT slot so that the following frame's Step 1 reads from the
        // correct index (ResolveFeedback also decodes into this next slot).
        {
            PROFILE_SCOPED("Collect textures to read back next frame");

            std::vector<uint32_t>& nextReadbackTextures = m_TexturesToReadback[(m_FrameIndex + 1) % kNumFramesInFlight];
            nextReadbackTextures.clear();
            if (!m_TexturesRingbuffer.empty())
            {
                uint32_t updatesLeft = kFeedbackTexturesToResolvePerFrame;
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
        //
        // GetNumDesiredHeaps() sums requestedTilesNum for every texture (packed + standard).
        // Packed-mip heaps are registered with TTM via MapPackedMips, so they are already
        // counted.  We only add heaps here; we never release them during normal operation.
        // Releasing empty heaps causes a burst-and-shrink pattern (the reference only
        // releases heaps when "compactMemory" is explicitly requested by the user).
        const uint32_t numRequiredHeaps = m_TiledTextureManager->GetNumDesiredHeaps();

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
        else if (allowHeapRelease)
        {
            // Only release empty heaps when there are no pending tile uploads.
            // While m_PendingTileRequests is non-empty, TTM has already allocated
            // heap slots for those tiles; releasing an empty heap now and
            // re-allocating it next frame produces the grow-shrink pattern.
            std::vector<uint32_t> emptyHeaps;
            m_TiledTextureManager->GetEmptyHeaps(emptyHeaps);
            for (uint32_t heapId : emptyHeaps)
            {
                if constexpr (kStreamingDebugLog)
                    SDL_Log("[Streaming][Heap] BeginFrame: releasing empty TTM heapId=%u", heapId);
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

        {
            PROFILE_SCOPED("Collect tiles to unmap/map");

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

                // Collect new standard tiles to stream in.
                // Packed tiles are already mapped by MapPackedMips at scene load and
                // will not appear here during normal operation.
                m_TiledTextureManager->GetTilesToMap(feedbackTexture->GetTiledTextureId(), tilesRequestedNew);
                if (!tilesRequestedNew.empty())
                {
                    FeedbackTextureUpdate update;
                    update.m_TextureIdx = texIdx;
                    for (uint32_t tileIndex : tilesRequestedNew)
                        update.m_TileIndices.push_back(tileIndex);
                    if (!update.m_TileIndices.empty())
                        results.push_back(update);
                }
            }
        }

        // ── Step 7: Defragmentation ──
        // NOTE: cause slight visual "stuttering" when tiles are moved, so disable for now
        //m_TiledTextureManager->DefragmentTiles(16);

        m_BeginFrameCPUTime = timer.LapSeconds();
    }

    void FeedbackManager::UpdateTileMappings(nvrhi::ICommandList* commandList, std::vector<FeedbackTextureUpdate>& tilesReady)
    {
        SimpleTimer timer;

        for (FeedbackTextureUpdate& texUpdate : tilesReady)
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

            for (uint32_t texIdx : m_MinMipDirtyTextures)
            {
                FeedbackTexture* texture = GetTextureByIndex(texIdx);
                rtxts::TextureDesc desc = m_TiledTextureManager->GetTextureDesc(texture->GetTiledTextureId(), rtxts::TextureTypes::eMinMipTexture);

                // R8_UINT: 1 byte per texel. nvrhi's writeTexture calls GetCopyableFootprints
                // internally to compute the D3D12-required destination row pitch (256-byte aligned),
                // so the source rowPitch just needs to be the logical stride.
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
        if (!m_TexturesRingbuffer.empty() && kFeedbackTexturesToResolvePerFrame > 0)
        {
            const uint32_t count = (uint32_t)m_TexturesRingbuffer.size();
            const uint32_t advance = std::min(kFeedbackTexturesToResolvePerFrame, count);
            m_RingbufferCursor = (m_RingbufferCursor + advance) % count;
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
    }

    const FeedbackManagerStats& FeedbackManager::GetStats() const
    {
        return m_StatsLastFrame;
    }

    void FeedbackManager::MapPackedMips(uint32_t textureIdx)
    {
        // Synchronously allocate packed tiles through TTM and set up the GPU tile mapping.
        // This must happen at scene load time, before the caller writes packed mip pixel
        // data via writeTexture — the GPU tile mapping must exist before the copy.
        //
        // Packed tiles are queued in TTM's m_requestedQueue by InitTiledTexture (called
        // from AddTiledTexture).  We ensure TTM has enough heap capacity, then call
        // AllocateRequestedTiles() to drain them, then map the GPU tiles using TTM's
        // slot assignments.
        //
        // These heaps are permanently occupied (packed tiles are never freed), so
        // GetEmptyHeaps() will never return them — they are never released.  This is
        // correct: packed-mip heaps must live for the lifetime of the scene.

        FeedbackTexture* texture = m_Textures.at(textureIdx).get();
        const nvrhi::PackedMipDesc& packedMipDesc = texture->GetPackedMipInfo();
        if (packedMipDesc.numPackedMips == 0)
            return;

        const uint32_t numPackedTiles = packedMipDesc.numTilesForPackedMips;

        // Ensure TTM has enough free slots for this texture's packed tiles.
        {
            const rtxts::Statistics stats = m_TiledTextureManager->GetStatistics();
            if (stats.heapFreeTilesNum < numPackedTiles)
            {
                const uint32_t deficit = numPackedTiles - stats.heapFreeTilesNum;
                const uint32_t heapsNeeded = (deficit + kHeapSizeInTiles - 1) / kHeapSizeInTiles;
                for (uint32_t i = 0; i < heapsNeeded; ++i)
                {
                    uint32_t heapId = m_HeapAllocator->AllocateHeap();
                    m_TiledTextureManager->AddHeap(heapId);
                    m_NumTTMHeaps++;
                }
            }
        }

        // Allocate packed tiles from the front of m_requestedQueue.
        m_TiledTextureManager->AllocateRequestedTiles();

        // Retrieve the tiles TTM just allocated for this texture.
        std::vector<uint32_t> tilesToMap;
        m_TiledTextureManager->GetTilesToMap(texture->GetTiledTextureId(), tilesToMap);

        // Filter to packed tiles only.
        std::vector<uint32_t> packedTiles;
        for (uint32_t idx : tilesToMap)
            if (texture->IsTilePacked(idx))
                packedTiles.push_back(idx);

        if (packedTiles.empty())
            return;

        // Tell TTM these tiles are now permanently mapped.
        m_TiledTextureManager->UpdateTilesMapping(texture->GetTiledTextureId(), packedTiles);

        // Build the GPU tile mapping using TTM's slot assignments.
        const std::vector<rtxts::TileAllocation>& allocs = m_TiledTextureManager->GetTileAllocations(texture->GetTiledTextureId());
        const std::vector<rtxts::TileCoord>& coords = m_TiledTextureManager->GetTileCoordinates(texture->GetTiledTextureId());

        std::map<nvrhi::HeapHandle, std::vector<uint32_t>> byHeap;
        for (uint32_t ti : packedTiles)
            byHeap[m_HeapAllocator->GetHeapHandle(allocs[ti].heapId)].push_back(ti);

        for (auto& [heap, heapTiles] : byHeap)
        {
            uint32_t n = (uint32_t)heapTiles.size();
            std::vector<nvrhi::TiledTextureCoordinate> tcoords(n);
            std::vector<nvrhi::TiledTextureRegion>     regions(n);
            std::vector<uint64_t>                      offsets(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t ti = heapTiles[i];
                tcoords[i].mipLevel   = coords[ti].mipLevel;
                tcoords[i].arrayLevel = 0;
                tcoords[i].x          = coords[ti].x;
                tcoords[i].y          = 0;
                tcoords[i].z          = 0;
                regions[i].tilesNum   = 1;
                offsets[i] = (uint64_t)allocs[ti].heapTileIndex * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
            }
            nvrhi::TextureTilesMapping mapping{};
            mapping.numTextureRegions       = n;
            mapping.tiledTextureCoordinates = tcoords.data();
            mapping.tiledTextureRegions     = regions.data();
            mapping.byteOffsets             = offsets.data();
            mapping.heap                    = heap;
            g_Renderer.m_RHI->m_NvrhiDevice->updateTextureTileMappings(texture->GetReservedTexture(), &mapping, 1);
        }

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] MapPackedMips: textureIdx=%u mapped %u packed tile(s) (TTM heaps=%u)",
                    textureIdx, (uint32_t)packedTiles.size(), m_NumTTMHeaps);
        }
    }

} // namespace nvfeedback
