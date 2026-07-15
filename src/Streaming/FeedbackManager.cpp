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

    void FeedbackManager::BeginFrame(nvrhi::ICommandList* commandList, std::vector<FeedbackTextureUpdate>& results)
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

        // Build a fast lookup set of which textures are being read back this frame
        // so we can skip them in the re-submission loop below.
        std::unordered_set<uint32_t> readbackSet(readbackTextures.begin(), readbackTextures.end());

        if (!readbackTextures.empty())
        {
            PROFILE_SCOPED("Resolve feedback");
            for (uint32_t texIdx : readbackTextures)
            {
                FeedbackTexture* readbackTexture = GetTextureByIndex(texIdx);
                uint8_t* pReadbackData = static_cast<uint8_t*>(
                    g_Renderer.m_RHI->m_NvrhiDevice->mapBuffer(readbackTexture->GetFeedbackResolveBuffer(m_FrameIndex),
                                        nvrhi::CpuAccessMode::Read));

                // Cache the fresh feedback data so we can re-submit it every frame
                // for frames where this texture is not in the readback batch.
                const nvrhi::BufferDesc& bufDesc = readbackTexture->GetFeedbackResolveBuffer(m_FrameIndex)->getDesc();
                readbackTexture->m_CachedFeedbackData.assign(pReadbackData, pReadbackData + bufDesc.byteSize);

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

        // ── Step 1b: Re-submit cached feedback for all textures NOT in the readback batch ──
        // TTM's timeout is measured from the last time UpdateWithSamplerFeedback was called
        // for a texture.  With a ringbuffer of N textures and only K resolved per frame,
        // each texture is only read back once every N/K frames.  Without re-submission,
        // tiles for non-readback textures would time out after kTileHysteresisSeconds even
        // though the scene is completely static.
        // Re-submitting the cached data refreshes lastRequestedTime for all currently-mapped
        // tiles without changing which tiles are requested, so the hysteresis timeout only
        // fires when a tile genuinely disappears from the feedback across multiple readback cycles.
        {
            PROFILE_SCOPED("Re-submit cached feedback");

            for (uint32_t texIdx = 0; texIdx < (uint32_t)m_Textures.size(); ++texIdx)
            {
                if (readbackSet.count(texIdx)) continue; // already processed above with fresh data

                FeedbackTexture* tex = GetTextureByIndex(texIdx);
                if (tex->m_CachedFeedbackData.empty()) continue; // never been read back yet — skip

                // Skip textures that have no standard tiles allocated yet.
                // TTM::UpdateTiledTexture does an O(regularTilesNum) scan once any tile is
                // allocated; for textures still at packed-mip-only residency the scan is
                // skipped internally, but we still pay the call overhead + BitArray construction
                // for every one of the ~3000 textures.  Checking our own O(1) counter avoids
                // that cost entirely during the initial burst period.
                if (!tex->HasAllocatedStandardTiles()) continue;

                rtxts::SamplerFeedbackDesc samplerFeedbackDesc{};
                samplerFeedbackDesc.pMinMipData = tex->m_CachedFeedbackData.data();
                m_TiledTextureManager->UpdateWithSamplerFeedback(
                    tex->GetTiledTextureId(),
                    samplerFeedbackDesc,
                    timeStamp,
                    kTileHysteresisSeconds);
            }
        }

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
        // TTM's GetNumDesiredHeaps() sums requestedTilesNum for every texture.
        // UpdateWithSamplerFeedback resets requestedTilesNum = packedTilesNum at the
        // start of every call, so the returned value always includes ALL packed mip
        // tiles even though packed mips are never allocated through TTM's heap pool
        // (they live on dedicated packed-mip heaps not registered with TTM).
        //
        // Without correction this causes us to allocate N heaps just to satisfy the
        // packed-mip tile count, leaving those heaps permanently full (heapFreeTilesNum=0)
        // because the packed-mip "requested" tiles are never actually allocated into them.
        //
        // Fix: subtract the packed-mip heap count from TTM's desired heap count so that
        // we only allocate heaps for actual streaming tiles.
        const uint32_t packedMipHeapCount = (m_PackedMipTileCursor + kHeapSizeInTiles - 1) / kHeapSizeInTiles;
        const uint32_t ttmDesiredHeaps    = m_TiledTextureManager->GetNumDesiredHeaps();
        const uint32_t numRequiredHeaps   = (ttmDesiredHeaps > packedMipHeapCount)
                                          ? (ttmDesiredHeaps - packedMipHeapCount) : 0;

        if constexpr (kStreamingDebugLog)
        {
            SDL_Log("[Streaming][Heap] BeginFrame heap check: ttmDesired=%u packedMipHeaps=%u corrected=%u TTMRegistered=%u totalHeaps=%u packedMipCursor=%u",
                    ttmDesiredHeaps, packedMipHeapCount, numRequiredHeaps, m_NumTTMHeaps, m_HeapAllocator->GetNumHeaps(),
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
                        // Decrement the allocated standard tile counter so Step 1b can skip
                        // this texture once all its standard tiles have been evicted.
                        if (!feedbackTexture->IsTilePacked(tileIndex))
                        {
                            SDL_assert(feedbackTexture->m_AllocatedStandardTileCount > 0);
                            feedbackTexture->m_AllocatedStandardTileCount--;
                        }

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
                        results.push_back(update);
                }
            }
        }

        // ── Step 7: Defragmentation ──
        m_TiledTextureManager->DefragmentTiles(16);

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

            // Increment the allocated standard tile counter for each non-packed tile that
            // was just mapped.  This enables the Step 1b fast-skip path.
            for (uint32_t tileIndex : texUpdate.m_TileIndices)
            {
                if (!texture->IsTilePacked(tileIndex))
                    texture->m_AllocatedStandardTileCount++;
            }

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
