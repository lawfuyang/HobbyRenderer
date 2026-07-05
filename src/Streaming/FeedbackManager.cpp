#include "FeedbackManager.h"
#include "../Renderer.h"

// D3D12 tile size constant (64 KB)
#ifndef D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES
#define D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES 65536u
#endif

namespace nvfeedback
{
    // ─── HeapAllocator ───────────────────────────────────────────────────────

    HeapAllocator::HeapAllocator(uint64_t heapSizeInBytes)
        : m_HeapSizeInBytes(heapSizeInBytes)
        , m_NumHeaps(0)
        , m_TotalAllocatedBytes(0)
    {
    }

    uint32_t HeapAllocator::AllocateHeap()
    {
        nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice;

        nvrhi::HeapDesc heapDesc{};
        heapDesc.capacity = m_HeapSizeInBytes;
        heapDesc.type = nvrhi::HeapType::DeviceLocal;
        nvrhi::HeapHandle heap = device->createHeap(heapDesc);

        nvrhi::BufferDesc bufferDesc{};
        bufferDesc.byteSize = m_HeapSizeInBytes;
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
        }
        else
        {
            heapId = m_FreeHeapIds.back();
            m_FreeHeapIds.pop_back();
            m_Heaps[heapId] = heap;
            m_Buffers[heapId] = buffer;
        }

        m_TotalAllocatedBytes += m_HeapSizeInBytes;
        m_NumHeaps++;

        return heapId;
    }

    void HeapAllocator::ReleaseHeap(uint32_t heapId)
    {
        m_FreeHeapIds.push_back(heapId);

        m_Heaps[heapId] = nullptr;
        m_Buffers[heapId] = nullptr;

        m_TotalAllocatedBytes -= m_HeapSizeInBytes;
        m_NumHeaps--;
    }

    // ─── FeedbackManager ─────────────────────────────────────────────────────

    FeedbackManager::FeedbackManager(const FeedbackManagerDesc& desc)
        : m_Desc(desc)
        , m_NumFramesInFlight(desc.m_NumFramesInFlight)
        , m_FrameIndex(0)
    {
        m_TexturesToReadback.resize(m_NumFramesInFlight);

        m_HeapAllocator = std::make_unique<HeapAllocator>(
            static_cast<uint64_t>(desc.m_HeapSizeInTiles) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);

        rtxts::TiledTextureManagerDesc tiledTextureManagerDesc{};
        tiledTextureManagerDesc.heapTilesCapacity = desc.m_HeapSizeInTiles;
        m_TiledTextureManager = std::unique_ptr<rtxts::TiledTextureManager>(
            rtxts::CreateTiledTextureManager(tiledTextureManagerDesc));
    }

    FeedbackTexture* FeedbackManager::CreateTexture(const nvrhi::TextureDesc& desc)
    {
        auto feedbackTexture = std::make_unique<FeedbackTexture>(
            desc, m_TiledTextureManager.get(), m_NumFramesInFlight);
        FeedbackTexture* rawPtr = feedbackTexture.get();
        m_Textures.push_back(std::move(feedbackTexture));
        m_TexturesRingbuffer.push_back(rawPtr);
        return rawPtr;
    }

    std::unique_ptr<FeedbackTextureSet> FeedbackManager::CreateTextureSet()
    {
        return std::make_unique<FeedbackTextureSet>(m_NumFramesInFlight);
    }

    void FeedbackManager::UnregisterTexture(FeedbackTexture* feedbackTexture)
    {
        {
            auto it = std::find_if(m_Textures.begin(), m_Textures.end(),
                [feedbackTexture](const std::unique_ptr<FeedbackTexture>& p) { return p.get() == feedbackTexture; });
            if (it != m_Textures.end())
                m_Textures.erase(it);
        }

        {
            auto it = std::find(m_TexturesRingbuffer.begin(), m_TexturesRingbuffer.end(), feedbackTexture);
            if (it != m_TexturesRingbuffer.end())
            {
                size_t idx = std::distance(m_TexturesRingbuffer.begin(), it);
                m_TexturesRingbuffer.erase(it);
                if (idx < m_RingbufferCursor)
                    m_RingbufferCursor--;
            }
        }

        for (auto& vec : m_TexturesToReadback)
        {
            auto it = std::find(vec.begin(), vec.end(), feedbackTexture);
            if (it != vec.end()) vec.erase(it);
        }

        m_MinMipDirtyTextures.erase(feedbackTexture);
    }

    void FeedbackManager::UpdateTextureRingBufferState(FeedbackTexture* pTex, bool bIncludeInRingBuffer)
    {
        auto it = std::find(m_TexturesRingbuffer.begin(), m_TexturesRingbuffer.end(), pTex);
        if (bIncludeInRingBuffer && it == m_TexturesRingbuffer.end())
        {
            m_TexturesRingbuffer.push_back(pTex);
        }
        else if (!bIncludeInRingBuffer && it != m_TexturesRingbuffer.end())
        {
            size_t idx = std::distance(m_TexturesRingbuffer.begin(), it);
            if (idx < m_RingbufferCursor)
                m_RingbufferCursor--;
            m_TexturesRingbuffer.erase(it);
        }
    }

    void FeedbackManager::BeginFrame(
        nvrhi::ICommandList* commandList,
        const FeedbackUpdateConfig& config,
        FeedbackTextureCollection* results)
    {
        SimpleTimer timer;

        m_FrameIndex = config.m_FrameIndex % m_NumFramesInFlight;
        m_UpdateConfigThisFrame = config;

        // Update TTM config
        rtxts::TiledTextureManagerConfig ttmConfig{};
        ttmConfig.numExtraStandbyTiles = config.m_NumExtraStandbyTiles;
        m_TiledTextureManager->SetConfig(ttmConfig);

        // ── Step 1: Read back feedback from N frames ago ──
        std::vector<FeedbackTexture*>& readbackTextures = m_TexturesToReadback[m_FrameIndex];
        if (!readbackTextures.empty())
        {
            float timeStamp = static_cast<float>(GetTickCount64()) / 1000.0f;
            for (FeedbackTexture* readbackTexture : readbackTextures)
            {
                uint8_t* pReadbackData = static_cast<uint8_t*>(
                    g_Renderer.m_RHI->m_NvrhiDevice->mapBuffer(readbackTexture->GetFeedbackResolveBuffer(m_FrameIndex),
                                        nvrhi::CpuAccessMode::Read));

                rtxts::SamplerFeedbackDesc samplerFeedbackDesc{};
                samplerFeedbackDesc.pMinMipData = pReadbackData;
                m_TiledTextureManager->UpdateWithSamplerFeedback(
                    readbackTexture->GetTiledTextureId(),
                    samplerFeedbackDesc,
                    timeStamp,
                    m_UpdateConfigThisFrame.m_TileTimeoutSeconds);

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
                                m_UpdateConfigThisFrame.m_TileTimeoutSeconds);
                        }
                    }
                }
            }
        }

        // ── Step 2: Collect textures to read back this frame ──
        readbackTextures.clear();
        {
            uint32_t updatesLeft = m_UpdateConfigThisFrame.m_MaxTexturesToUpdate;
            size_t count = m_TexturesRingbuffer.size();
            for (size_t i = 0; i < count && updatesLeft > 0; i++)
            {
                size_t idx = (m_RingbufferCursor + i) % count;
                FeedbackTexture* feedbackTexture = m_TexturesRingbuffer[idx];
                commandList->clearSamplerFeedbackTexture(feedbackTexture->GetSamplerFeedbackTexture());
                readbackTextures.push_back(feedbackTexture);
                updatesLeft--;
            }
        }

        // ── Step 3: Trim standby tiles ──
        if (m_UpdateConfigThisFrame.m_bTrimStandbyTiles)
            m_TiledTextureManager->TrimStandbyTiles();

        // ── Step 4: Heap management ──
        uint32_t numRequiredHeaps = m_TiledTextureManager->GetNumDesiredHeaps();
        if (numRequiredHeaps > m_HeapAllocator->GetNumHeaps())
        {
            while (m_HeapAllocator->GetNumHeaps() < numRequiredHeaps)
            {
                uint32_t heapId = m_HeapAllocator->AllocateHeap();
                m_TiledTextureManager->AddHeap(heapId);
            }
        }
        else if (m_UpdateConfigThisFrame.m_bReleaseEmptyHeaps)
        {
            std::vector<uint32_t> emptyHeaps;
            m_TiledTextureManager->GetEmptyHeaps(emptyHeaps);
            for (uint32_t heapId : emptyHeaps)
            {
                m_TiledTextureManager->RemoveHeap(heapId);
                m_HeapAllocator->ReleaseHeap(heapId);
            }
        }

        // ── Step 5: Allocate requested tiles ──
        m_TiledTextureManager->AllocateRequestedTiles();

        // ── Step 6: Collect tiles to unmap and map ──
        std::vector<uint32_t> tilesRequestedNew;
        std::vector<uint32_t> tilesToUnmap;

        for (const auto& feedbackTexturePtr : m_Textures)
        {
            FeedbackTexture* feedbackTexture = feedbackTexturePtr.get();
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
                m_MinMipDirtyTextures.insert(feedbackTexture);
            }

            // Collect new tiles to stream in
            m_TiledTextureManager->GetTilesToMap(feedbackTexture->GetTiledTextureId(), tilesRequestedNew);
            if (!tilesRequestedNew.empty())
            {
                FeedbackTextureUpdate update;
                update.m_Texture = feedbackTexture;
                for (uint32_t tileIndex : tilesRequestedNew)
                    update.m_TileIndices.push_back(tileIndex);
                results->m_Textures.push_back(update);
            }
        }

        // ── Step 7: Defragmentation ──
        if (m_UpdateConfigThisFrame.m_bDefragmentHeaps)
            m_TiledTextureManager->DefragmentTiles(16);

        m_BeginFrameCPUTime = timer.LapSeconds();
    }

    void FeedbackManager::UpdateTileMappings(
        nvrhi::ICommandList* commandList,
        FeedbackTextureCollection* tilesReady)
    {
        SimpleTimer timer;

        for (FeedbackTextureUpdate& texUpdate : tilesReady->m_Textures)
        {
            FeedbackTexture* texture = texUpdate.m_Texture;
            m_MinMipDirtyTextures.insert(texture);

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

                    byteOffsets.push_back(
                        static_cast<uint64_t>(tileAllocations[tileIndex].heapTileIndex) *
                        D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
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
                for (FeedbackTexture* feedbackTexture : m_MinMipDirtyTextures)
                    commandList->setTextureState(feedbackTexture->GetMinMipTexture(),
                                                 nvrhi::AllSubresources,
                                                 nvrhi::ResourceStates::CopyDest);
            }

            std::vector<uint8_t> minMipData;
            std::vector<uint8_t> uploadData;

            for (FeedbackTexture* texture : m_MinMipDirtyTextures)
            {
                rtxts::TextureDesc desc = m_TiledTextureManager->GetTextureDesc(
                    texture->GetTiledTextureId(), rtxts::TextureTypes::eMinMipTexture);

                // Size scratch buffers for this texture's MinMip dimensions
                uint32_t numElements = desc.textureOrMipRegionWidth * desc.textureOrMipRegionHeight;
                uint32_t rowPitch = (desc.textureOrMipRegionWidth * sizeof(float) + 0xFF) & ~0xFF;
                minMipData.resize(numElements);
                uploadData.resize(rowPitch * desc.textureOrMipRegionHeight);

                m_TiledTextureManager->WriteMinMipData(texture->GetTiledTextureId(), minMipData.data());

                uint8_t* pUploadData = uploadData.data();
                for (uint32_t y = 0; y < desc.textureOrMipRegionHeight; ++y)
                {
                    float* pDataFloat = reinterpret_cast<float*>(pUploadData);
                    for (uint32_t x = 0; x < desc.textureOrMipRegionWidth; ++x)
                        pDataFloat[x] = static_cast<float>(minMipData[y * desc.textureOrMipRegionWidth + x]);
                    pUploadData += rowPitch;
                }

                commandList->writeTexture(texture->GetMinMipTexture(), 0, 0, uploadData.data(), rowPitch);
            }

            if (!bUseAutomaticBarriers)
            {
                for (FeedbackTexture* feedbackTexture : m_MinMipDirtyTextures)
                    commandList->setTextureState(feedbackTexture->GetMinMipTexture(),
                                                 nvrhi::AllSubresources,
                                                 nvrhi::ResourceStates::ShaderResource);
            }

            m_MinMipDirtyTextures.clear();
            commandList->setEnableAutomaticBarriers(true);
        }

        m_UpdateTileMappingsCPUTime = timer.LapSeconds();
    }

    void FeedbackManager::ResolveFeedback(nvrhi::ICommandList* commandList)
    {
        std::vector<FeedbackTexture*>& readbackTextures = m_TexturesToReadback[m_FrameIndex];
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
            for (FeedbackTexture* feedbackTexture : readbackTextures)
                commandList->setSamplerFeedbackTextureState(
                    feedbackTexture->GetSamplerFeedbackTexture(),
                    nvrhi::ResourceStates::ResolveSource);
        }

        for (FeedbackTexture* feedbackTexture : readbackTextures)
        {
            commandList->decodeSamplerFeedbackTexture(
                feedbackTexture->GetFeedbackResolveBuffer(m_FrameIndex),
                feedbackTexture->GetSamplerFeedbackTexture(),
                nvrhi::Format::R8_UINT);
        }

        if (!bUseAutomaticBarriers)
        {
            for (FeedbackTexture* feedbackTexture : readbackTextures)
                commandList->setSamplerFeedbackTextureState(
                    feedbackTexture->GetSamplerFeedbackTexture(),
                    nvrhi::ResourceStates::UnorderedAccess);
        }

        commandList->setEnableAutomaticBarriers(true);

        m_ResolveCPUTime = timer.LapSeconds();
    }

    void FeedbackManager::EndFrame()
    {
        // Advance ring buffer cursor
        if (!m_TexturesRingbuffer.empty() && m_UpdateConfigThisFrame.m_MaxTexturesToUpdate > 0)
        {
            uint32_t numToRotate = m_UpdateConfigThisFrame.m_MaxTexturesToUpdate;
            m_RingbufferCursor = (m_RingbufferCursor + numToRotate) % uint32_t(m_TexturesRingbuffer.size());
        }

        // Update stats
        m_StatsLastFrame.m_HeapAllocationInBytes = m_HeapAllocator->GetTotalAllocatedBytes();
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

} // namespace nvfeedback
