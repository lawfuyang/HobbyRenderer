#include "GeometryStreaming.h"

#include "Config.h"
#include "Renderer.h"
#include "Streaming/FeedbackManager.h"

#include "shaders/srrhi/cpp/Cluster.h"

namespace
{
    // Every blob starts on this boundary. The plan asks for 16 bytes, which is also what the arena free list
    // splits on, so an allocation can never leave an unaligned block behind.
    constexpr uint32_t kBlobAlignmentBytes = 16;
    constexpr uint32_t kBlobAlignmentElements = kBlobAlignmentBytes / sizeof(uint32_t);
}

uint32_t GeometryStreaming::CapacityElementsForBudget() const
{
    const uint64_t budgetBytes = g_Renderer.GetStreamingBudgetBytes();
    uint64_t elements = m_TotalElements;
    if (budgetBytes != 0)
    {
        // The two streaming systems share one cap and the tile heaps have already claimed part of it by the time
        // the arena is sized, so the arena gets what is left. The reverse direction closes the loop: the heaps are
        // only offered what the arena does not hold (Renderer::GetTileStreamingBudgetBytes).
        const uint64_t tileBytes = g_Renderer.m_FeedbackManager->GetAllocatedHeapBytes();
        const uint64_t remainingBytes = budgetBytes > tileBytes ? budgetBytes - tileBytes : 0;
        elements = std::min(elements, remainingBytes / sizeof(uint32_t));
    }
    // Never below one complete cover of the scene - every primitive's coarsest level - even if the cap is smaller: a
    // budget below the scene's cheapest representation is better served by coarse geometry than by no geometry, and
    // the tile heaps are the side that has already claimed its share by then.
    elements = std::max(elements, std::min<uint64_t>(m_TotalElements, m_CoarsestElements));
    elements = std::max<uint64_t>(elements, kBlobAlignmentElements);
    // An address is a 31-bit element offset; anything past that would collide with the "not resident" tag.
    SDL_assert(elements < srrhi::ClusterStream::kInvalidAddressStart);
    return uint32_t(elements);
}

nvrhi::BufferHandle GeometryStreaming::CreateArenaBuffer(uint32_t elements) const
{
    nvrhi::BufferDesc desc;
    desc.byteSize = std::max<uint32_t>(elements * uint32_t(sizeof(uint32_t)), uint32_t(sizeof(uint32_t)));
    desc.structStride = sizeof(uint32_t);
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Scene_ClusterStreamData";
    return g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(desc);
}

const uint8_t* GeometryStreaming::GetGroupBlob(uint32_t groupIndex, uint32_t& outByteSize) const
{
    outByteSize = 0;
    if (!m_BlobFile || !m_BlobFile->IsValid() || groupIndex >= m_GroupFiles.size())
        return nullptr;

    const ClusterLod::GroupBlobRef& ref = m_GroupFiles[groupIndex];
    SDL_assert(uint64_t(ref.m_FileOffset) + ref.m_ByteSize <= m_BlobFile->GetSize());
    outByteSize = ref.m_ByteSize;
    return static_cast<const uint8_t*>(m_BlobFile->GetDataAt(size_t(ref.m_FileOffset)));
}

void GeometryStreaming::Initialize(nvrhi::ICommandList* cmd, ClusterLod::GpuData& data)
{
    SDL_assert(cmd != nullptr);

    m_GroupBlobs = std::move(data.m_StreamGroups);
    m_GroupCount = uint32_t(m_GroupBlobs.size());
    m_FrameIndex = 0;

    // The blobs stay in the cooked mesh file. Map it read-only and take the per-group table; the table is in the
    // same group order BuildGpuData flattened them, so entry i is m_GroupBlobs[i]'s bytes.
    m_GroupFiles = std::move(g_Renderer.m_ClusterBlobFile.m_Groups);
    const std::string blobPath = g_Renderer.m_ClusterBlobFile.m_Path.string();
    g_Renderer.m_ClusterBlobFile.Clear();

    m_BlobFile = std::make_unique<MemoryMappedDataReader>(blobPath);
    SDL_assert(m_BlobFile->IsValid());
    if (!m_BlobFile->IsValid())
    {
        SDL_Log("[ClusterLod] geometry streaming: cannot map the cooked mesh file %s; nothing will stream in",
            blobPath.c_str());
    }

    // A table that does not line up with the flattened groups is a bug in the cook: refuse to draw another
    // group's bytes rather than trusting the offsets.
    SDL_assert(m_GroupFiles.size() == m_GroupBlobs.size());
    if (m_GroupFiles.size() != m_GroupBlobs.size())
    {
        SDL_Log("[ClusterLod] geometry streaming: %zu group blobs for %u groups; streaming disabled",
            m_GroupFiles.size(), m_GroupCount);
        m_BlobFile.reset();
        m_GroupFiles.clear();
    }

    if (m_BlobFile && m_BlobFile->IsValid())
    {
        const uint64_t fileBytes = m_BlobFile->GetSize();
        for (uint32_t g = 0; g < m_GroupCount; ++g)
        {
            const uint64_t bytes = uint64_t(m_GroupBlobs[g].GetBlobElements()) * sizeof(uint32_t);
            const uint64_t end   = uint64_t(m_GroupFiles[g].m_FileOffset) + bytes;
            SDL_assert(m_GroupFiles[g].m_ByteSize == bytes);
            SDL_assert(end <= fileBytes);
            if (m_GroupFiles[g].m_ByteSize != bytes || end > fileBytes)
            {
                SDL_Log("[ClusterLod] geometry streaming: group %u blob %u B at %llu does not fit the cooked file "
                        "(%llu B, expected %llu B); streaming disabled",
                    g, m_GroupFiles[g].m_ByteSize, static_cast<unsigned long long>(m_GroupFiles[g].m_FileOffset),
                    static_cast<unsigned long long>(fileBytes), static_cast<unsigned long long>(bytes));
                m_BlobFile.reset();
                break;
            }
        }
    }

    // The geometry slices are the per-MeshData group ranges the discrete-LOD levels are derived per geometry.
    m_Geometries.clear();
    m_Geometries.reserve(data.m_Geometry.size());
    for (const srrhi::ClusterGeometry& geometry : data.m_Geometry)
    {
        m_Geometries.push_back(GeometryRange{ geometry.m_GroupOffset, geometry.m_GroupCount, geometry.m_LODLevelCount });
    }

    m_TotalElements = 0;
    m_CoarsestElements = 0;
    uint32_t maxBlobElements = 0;
    for (const ClusterLod::StreamGroup& blob : m_GroupBlobs)
    {
        m_TotalElements += blob.GetBlobElements();
        if (blob.m_bCoarsestLevel)
            m_CoarsestElements += blob.GetBlobElements();
        maxBlobElements = std::max(maxBlobElements, blob.GetBlobElements());
    }
    m_ArenaElements = CapacityElementsForBudget();
    m_ResizedBudgetBytes = g_Renderer.GetStreamingBudgetBytes();

    nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice;
    SDL_assert(device != nullptr);

    auto createStreamBuffer = [device](uint32_t byteSize, uint32_t stride, bool bUAV, const char* name)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = std::max<uint32_t>(byteSize, stride);
        desc.structStride = stride;
        desc.canHaveUAVs = bUAV;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = name;
        return device->createBuffer(desc);
    };

    // The arena itself is created by Pack(); only the fixed-size tables live here.
    m_GroupStates = createStreamBuffer(std::max<uint32_t>(m_GroupCount, 1) * uint32_t(sizeof(srrhi::ClusterGroupState)),
        sizeof(srrhi::ClusterGroupState), true, "Scene_ClusterGroupStates");
    m_LoadRequests = createStreamBuffer(kLoadListCapacity * uint32_t(sizeof(uint32_t)), sizeof(uint32_t), true,
        "Scene_ClusterStreamLoadRequests");
    m_UnloadRequests = createStreamBuffer(kUnloadListCapacity * uint32_t(sizeof(uint32_t)), sizeof(uint32_t), true,
        "Scene_ClusterStreamUnloadRequests");
    m_StreamCounters = createStreamBuffer(sizeof(srrhi::ClusterStreamCounters), sizeof(srrhi::ClusterStreamCounters),
        true, "Scene_ClusterStreamCounters");
    m_ActiveGroupList = createStreamBuffer(std::max<uint32_t>(m_GroupCount, 1) * uint32_t(sizeof(uint32_t)),
        sizeof(uint32_t), false, "Scene_ClusterStreamActiveGroups");
    m_PatchList = createStreamBuffer(kMaxPatches * uint32_t(sizeof(srrhi::ClusterStreamPatch)),
        sizeof(srrhi::ClusterStreamPatch), false, "Scene_ClusterStreamPatches");
    m_DiscreteLevels = createStreamBuffer(std::max<uint32_t>(uint32_t(m_Geometries.size()), 1) * uint32_t(sizeof(uint32_t)),
        sizeof(uint32_t), false, "Scene_ClusterDiscreteLevels");

    for (ReadbackSlot& slot : m_Slots)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = sizeof(srrhi::ClusterStreamCounters) +
            (kLoadListCapacity + kUnloadListCapacity) * uint32_t(sizeof(uint32_t));
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.debugName = "Scene_ClusterStreamRequestReadback";
        slot.m_Staging = device->createBuffer(desc);
        slot.m_Event = device->createEventQuery();
        slot.m_bInFlight = false;
    }

    m_Stats = Stats{};
    m_Stats.m_GroupCount = m_GroupCount;
    m_Stats.m_LoadBudgetBytes = Config::Get().m_ClusterLoadBytesPerFrame;
    m_Stats.m_BlobFileBytes = (m_BlobFile && m_BlobFile->IsValid()) ? m_BlobFile->GetSize() : 0;

    // Per-level totals for the residency histogram; the resident half is refreshed every frame in PreRender.
    for (const ClusterLod::StreamGroup& blob : m_GroupBlobs)
    {
        const uint32_t level = std::min(blob.m_LODLevel, ClusterLod::kMaxLODLevels - 1);
        m_Stats.m_GroupsPerLevel[level]++;
        m_Stats.m_LevelCount = std::max(m_Stats.m_LevelCount, level + 1);
    }

    BuildInitialResidency(cmd);
    UpdateDiscreteLevels(cmd);

    SDL_Log("[ClusterLod] geometry streaming: %u groups, %.2f MB of blobs (largest %.1f KB), arena %.2f MB of a "
            "%llu MB budget, age threshold %u, blob file %s (%.2f MB), load budget %llu MB/frame",
        m_GroupCount, double(m_TotalElements * sizeof(uint32_t)) / (1024.0 * 1024.0),
        double(maxBlobElements * sizeof(uint32_t)) / 1024.0,
        double(m_ArenaElements * sizeof(uint32_t)) / (1024.0 * 1024.0),
        static_cast<unsigned long long>(m_Stats.m_BudgetBytes / (1024ull * 1024ull)),
        Config::Get().m_ClusterStreamAgeThreshold,
        (m_BlobFile && m_BlobFile->IsValid()) ? "mapped" : "MISSING",
        double(m_Stats.m_BlobFileBytes) / (1024.0 * 1024.0),
        static_cast<unsigned long long>(m_Stats.m_LoadBudgetBytes / (1024ull * 1024ull)));
}

void GeometryStreaming::BuildInitialResidency(nvrhi::ICommandList* cmd)
{
    m_Residency.assign(std::max<uint32_t>(m_GroupCount, 1), Residency{});
    m_ActiveGroups.clear();

    if (m_ArenaElements < m_TotalElements)
    {
        // The budget cannot hold every level: start from each primitive's coarsest level, which is the cheapest
        // complete cover of the scene, and let the traversal fill the rest with what is actually on screen.
        for (uint32_t g = 0; g < m_GroupCount; ++g)
        {
            if (m_GroupBlobs[g].m_bCoarsestLevel)
                MarkResident(g);
        }
    }
    else
    {
        for (uint32_t g = 0; g < m_GroupCount; ++g)
            MarkResident(g);
    }

    TrimToCapacity();
    Pack(cmd);

    uint32_t residentGroups = 0;
    for (const Residency& residency : m_Residency)
        residentGroups += residency.m_bResident ? 1u : 0u;

    SDL_Log("[ClusterLod] geometry streaming: %u of %u groups resident after load, %u of %u elements used",
        residentGroups, m_GroupCount, m_Stats.m_ResidentBytes / uint32_t(sizeof(uint32_t)), m_ArenaElements);
}

void GeometryStreaming::TrimToCapacity()
{
    uint64_t residentElements = 0;
    std::vector<uint32_t> resident;
    resident.reserve(m_GroupCount);
    for (uint32_t g = 0; g < m_GroupCount; ++g)
    {
        if (!m_Residency[g].m_bResident)
            continue;
        resident.push_back(g);
        residentElements += m_Residency[g].m_Elements;
    }
    if (residentElements <= m_ArenaElements)
        return;

    // Coarsest level first: it is the cheapest cover of the scene, so losing it costs the least detail per byte,
    // and the finest levels - which is what the cut picks near the camera - are the ones that have to survive.
    std::sort(resident.begin(), resident.end(), [this](uint32_t a, uint32_t b)
    {
        const uint32_t levelA = m_GroupBlobs[a].m_LODLevel;
        const uint32_t levelB = m_GroupBlobs[b].m_LODLevel;
        return levelA != levelB ? levelA > levelB : a > b;
    });

    uint32_t dropped = 0;
    for (uint32_t groupIndex : resident)
    {
        if (residentElements <= m_ArenaElements)
            break;
        residentElements -= m_Residency[groupIndex].m_Elements;
        MarkUnresident(groupIndex);
        dropped++;
    }
    m_Stats.m_EvictedByBudget += dropped;
}

void GeometryStreaming::Pack(nvrhi::ICommandList* cmd)
{
    std::vector<srrhi::ClusterGroupState> states(std::max<uint32_t>(m_GroupCount, 1));

    // An address of zero is a valid arena offset, so a default-initialised table would claim every group is
    // resident at the start of the arena. Everything starts missing and the pack below publishes what exists.
    for (srrhi::ClusterGroupState& state : states)
    {
        state.m_Address = srrhi::ClusterStream::kInvalidAddressStart;
        state.m_Age = 0;
    }

    m_FreeBlocks.clear();
    m_ActiveGroups.clear();
    m_StreamData = CreateArenaBuffer(m_ArenaElements);

    uint32_t cursor = 0;
    for (uint32_t g = 0; g < m_GroupCount; ++g)
    {
        if (!m_Residency[g].m_bResident)
            continue;

        const ClusterLod::StreamGroup& blob = m_GroupBlobs[g];
        const uint32_t elements = blob.GetBlobElements();
        SDL_assert(cursor + elements <= m_ArenaElements);
        if (cursor + elements > m_ArenaElements)
        {
            // Only reachable with a budget smaller than one blob; TrimToCapacity should have removed it.
            MarkUnresident(g);
            continue;
        }

        // The blob is one contiguous range in the cooked file: triangles first, then the vertex indices.
        uint32_t byteSize = 0;
        const uint8_t* bytes = GetGroupBlob(g, byteSize);
        SDL_assert(bytes != nullptr && byteSize == uint64_t(elements) * sizeof(uint32_t));
        if (bytes == nullptr || byteSize != uint64_t(elements) * sizeof(uint32_t))
        {
            MarkUnresident(g);
            continue;
        }

        cmd->writeBuffer(m_StreamData, bytes, byteSize, uint64_t(cursor) * sizeof(uint32_t));

        m_Residency[g].m_Offset = cursor;
        m_Residency[g].m_Elements = elements;
        states[g].m_Address = cursor;
        m_ActiveGroups.push_back(g);

        cursor += elements;
    }

    if (cursor < m_ArenaElements)
        m_FreeBlocks.push_back(FreeBlock{ cursor, m_ArenaElements - cursor });

    if (m_GroupCount != 0)
        cmd->writeBuffer(m_GroupStates, states.data(), size_t(m_GroupCount) * sizeof(srrhi::ClusterGroupState), 0);

    m_Stats.m_ArenaBytes = uint64_t(m_ArenaElements) * sizeof(uint32_t);
    m_Stats.m_BudgetBytes = g_Renderer.GetStreamingBudgetBytes();
    m_Stats.m_ResidentBytes = uint64_t(cursor) * sizeof(uint32_t);
    m_Stats.m_WastedBytes = m_Stats.m_ArenaBytes - m_Stats.m_ResidentBytes;

    // The whole table was rewritten, so the active list has to be re-uploaded even if it looks unchanged.
    m_bForceActiveListUpload = true;
}

void GeometryStreaming::Resize(nvrhi::ICommandList* cmd)
{
    const uint64_t budgetBytes = g_Renderer.GetStreamingBudgetBytes();
    m_Stats.m_BudgetBytes = budgetBytes;

    // Only a change of the budget is worth a re-pack. The share is settled then, so a heap the tile manager adds
    // afterwards must not move the arena - it is capped by what the arena holds instead. A budget above the
    // scene's own size leaves the capacity where it is, so dragging the slider through that range costs nothing.
    if (budgetBytes == m_ResizedBudgetBytes)
        return;
    m_ResizedBudgetBytes = budgetBytes;

    const uint32_t capacity = CapacityElementsForBudget();
    if (capacity == m_ArenaElements)
        return;

    m_ArenaElements = capacity;
    TrimToCapacity();
    Pack(cmd);

    SDL_Log("[ClusterLod] geometry streaming: budget changed, arena %.2f MB, %u groups kept",
        double(m_ArenaElements * sizeof(uint32_t)) / (1024.0 * 1024.0), uint32_t(m_ActiveGroups.size()));
}

void GeometryStreaming::Shutdown()
{
    for (ReadbackSlot& slot : m_Slots)
    {
        slot.m_Staging = nullptr;
        slot.m_Event = nullptr;
        slot.m_bInFlight = false;
    }

    m_StreamData = nullptr;
    m_GroupStates = nullptr;
    m_LoadRequests = nullptr;
    m_UnloadRequests = nullptr;
    m_StreamCounters = nullptr;
    m_ActiveGroupList = nullptr;
    m_PatchList = nullptr;
    m_DiscreteLevels = nullptr;

    m_BlobFile.reset();
    m_GroupFiles.clear();
    m_GroupBlobs.clear();
    m_Geometries.clear();
    m_DiscreteLevelsHost.clear();
    m_Residency.clear();
    m_FreeBlocks.clear();
    m_ActiveGroups.clear();
    m_UploadedActiveGroups.clear();
    m_PendingLoads.clear();
    m_PendingUnloads.clear();
    m_Patches.clear();
    m_LoadedThisFrame.clear();
    m_GroupCount = 0;
    m_TotalElements = 0;
    m_ArenaElements = 0;
}

uint32_t GeometryStreaming::Allocate(uint32_t elements)
{
    SDL_assert(elements != 0 && (elements % kBlobAlignmentElements) == 0);

    // First fit over an offset-sorted list: with a few thousand blocks and a per-frame load budget this is far
    // cheaper than the memory it protects.
    for (size_t i = 0; i < m_FreeBlocks.size(); ++i)
    {
        FreeBlock& block = m_FreeBlocks[i];
        if (block.m_Elements < elements)
            continue;

        const uint32_t offset = block.m_Offset;
        if (block.m_Elements == elements)
        {
            m_FreeBlocks.erase(m_FreeBlocks.begin() + i);
        }
        else
        {
            block.m_Offset += elements;
            block.m_Elements -= elements;
        }
        return offset;
    }
    return srrhi::ClusterStream::kInvalidAddressStart;
}

void GeometryStreaming::Free(uint32_t offset, uint32_t elements)
{
    if (elements == 0)
        return;

    size_t insertAt = 0;
    while (insertAt < m_FreeBlocks.size() && m_FreeBlocks[insertAt].m_Offset < offset)
        ++insertAt;
    m_FreeBlocks.insert(m_FreeBlocks.begin() + insertAt, FreeBlock{ offset, elements });

    // Coalesce with the neighbours so a long-lived arena does not decay into a list of one-block holes.
    if (insertAt + 1 < m_FreeBlocks.size())
    {
        FreeBlock& block = m_FreeBlocks[insertAt];
        const FreeBlock& next = m_FreeBlocks[insertAt + 1];
        if (block.m_Offset + block.m_Elements == next.m_Offset)
        {
            block.m_Elements += next.m_Elements;
            m_FreeBlocks.erase(m_FreeBlocks.begin() + insertAt + 1);
        }
    }
    if (insertAt > 0)
    {
        FreeBlock& previous = m_FreeBlocks[insertAt - 1];
        const FreeBlock& block = m_FreeBlocks[insertAt];
        if (previous.m_Offset + previous.m_Elements == block.m_Offset)
        {
            previous.m_Elements += block.m_Elements;
            m_FreeBlocks.erase(m_FreeBlocks.begin() + insertAt);
        }
    }
}

void GeometryStreaming::MarkResident(uint32_t groupIndex)
{
    SDL_assert(!m_Residency[groupIndex].m_bResident);
    m_Residency[groupIndex].m_Elements = m_GroupBlobs[groupIndex].GetBlobElements();
    m_Residency[groupIndex].m_bResident = true;
}

void GeometryStreaming::MarkResident(uint32_t groupIndex, uint32_t offset, uint32_t elements)
{
    SDL_assert(!m_Residency[groupIndex].m_bResident);
    m_Residency[groupIndex].m_Offset = offset;
    m_Residency[groupIndex].m_Elements = elements;
    m_Residency[groupIndex].m_bResident = true;
    m_ActiveGroups.push_back(groupIndex);
}

void GeometryStreaming::MarkUnresident(uint32_t groupIndex)
{
    SDL_assert(m_Residency[groupIndex].m_bResident);
    m_Residency[groupIndex].m_bResident = false;
    m_Residency[groupIndex].m_Elements = 0;

    // The list is only read by the age filter, so the swap-with-last removal can run on the host where there is no
    // race to lose.
    for (size_t i = 0; i < m_ActiveGroups.size(); ++i)
    {
        if (m_ActiveGroups[i] != groupIndex)
            continue;
        m_ActiveGroups[i] = m_ActiveGroups.back();
        m_ActiveGroups.pop_back();
        return;
    }
}

void GeometryStreaming::ConsumeCompletedRequests()
{
    for (ReadbackSlot& slot : m_Slots)
    {
        if (!slot.m_bInFlight || !g_Renderer.m_RHI->m_NvrhiDevice->pollEventQuery(slot.m_Event.Get()))
            continue;

        const uint8_t* data = static_cast<const uint8_t*>(
            g_Renderer.m_RHI->m_NvrhiDevice->mapBuffer(slot.m_Staging, nvrhi::CpuAccessMode::Read));
        SDL_assert(data != nullptr);
        if (data != nullptr)
        {
            srrhi::ClusterStreamCounters counters{};
            memcpy(&counters, data, sizeof(counters));

            const uint32_t* loads = reinterpret_cast<const uint32_t*>(data + sizeof(counters));
            const uint32_t* unloads = loads + kLoadListCapacity;

            // The shaders only write the first m_MaxLoads / m_MaxUnloads entries; the counters keep counting past
            // that, so the read range is the budget, not the list capacity (the rest of the list is last frame's).
            const uint32_t loadCount = std::min(counters.m_LoadCount, kMaxLoadsPerFrame);
            const uint32_t unloadCount = std::min(counters.m_UnloadCount, kMaxUnloadsPerFrame);
            m_PendingLoads.insert(m_PendingLoads.end(), loads, loads + loadCount);
            m_PendingUnloads.insert(m_PendingUnloads.end(), unloads, unloads + unloadCount);

            m_Stats.m_RequestsLastFrame = loadCount;
            m_Stats.m_DroppedLoads += counters.m_LoadCount - loadCount;

            g_Renderer.m_RHI->m_NvrhiDevice->unmapBuffer(slot.m_Staging);
        }
        slot.m_bInFlight = false;
    }
}

void GeometryStreaming::ApplyUnloads(std::vector<srrhi::ClusterStreamPatch>& patches)
{
    // Every request that arrived is honoured: a dropped unload would leave a group resident with an age past the
    // threshold, and the age filter only asks once per idle period.
    for (uint32_t groupIndex : m_PendingUnloads)
    {
        if (groupIndex >= m_GroupCount || !m_Residency[groupIndex].m_bResident)
            continue;

        Free(m_Residency[groupIndex].m_Offset, m_Residency[groupIndex].m_Elements);
        MarkUnresident(groupIndex);
        m_Stats.m_UnloadedThisFrame++;

        patches.push_back(srrhi::ClusterStreamPatch{ groupIndex, srrhi::ClusterStream::kInvalidAddressStart });
    }
    m_PendingUnloads.clear();
}

void GeometryStreaming::ApplyLoads(std::vector<srrhi::ClusterStreamPatch>& patches)
{
    // Coarse to fine: a fine group that lands before the levels between it renders the same surface twice for a
    // frame or two, so the coarser levels of this frame's requests go first. The order is stable, so groups of
    // the same level keep the traversal's order (which is roughly front to back).
    std::stable_sort(m_PendingLoads.begin(), m_PendingLoads.end(), [this](uint32_t a, uint32_t b)
    {
        const uint32_t levelA = a < m_GroupCount ? m_GroupBlobs[a].m_LODLevel : 0u;
        const uint32_t levelB = b < m_GroupCount ? m_GroupBlobs[b].m_LODLevel : 0u;
        return levelA > levelB;
    });

    const uint64_t budget = Config::Get().m_ClusterLoadBytesPerFrame;
    uint64_t bytesThisFrame = 0;
    uint32_t loadsThisFrame = 0;
    for (uint32_t groupIndex : m_PendingLoads)
    {
        if (loadsThisFrame >= kMaxLoadsPerFrame)
            break;
        if (groupIndex >= m_GroupCount || m_Residency[groupIndex].m_bResident)
            continue;

        const uint32_t elements = m_GroupBlobs[groupIndex].GetBlobElements();
        const uint64_t blobBytes = uint64_t(elements) * sizeof(uint32_t);

        // The byte budget bounds how much of the file is pulled per frame. Always let one blob through, so a
        // budget smaller than a single blob cannot stall the stream entirely.
        if (budget != 0 && loadsThisFrame != 0 && bytesThisFrame + blobBytes > budget)
            break;

        const uint32_t offset = Allocate(elements);
        if (offset >= srrhi::ClusterStream::kInvalidAddressStart)
        {
            // No hole big enough. The traversal asks again next frame, and the age filter will have handed back
            // something by then.
            m_Stats.m_DroppedLoads++;
            continue;
        }

        MarkResident(groupIndex, offset, elements);
        m_LoadedThisFrame.push_back(groupIndex);
        loadsThisFrame++;
        bytesThisFrame += blobBytes;
        m_Stats.m_LoadedThisFrame++;

        patches.push_back(srrhi::ClusterStreamPatch{ groupIndex, offset });
    }
    m_PendingLoads.clear();
    m_Stats.m_LoadBytesThisFrame = bytesThisFrame;
}

void GeometryStreaming::DispatchPatches(nvrhi::ICommandList* cmd, uint32_t unloadCount)
{
    SDL_assert(!m_Patches.empty());

    cmd->writeBuffer(m_PatchList, m_Patches.data(), m_Patches.size() * sizeof(srrhi::ClusterStreamPatch), 0);

    const nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(
        uint32_t(sizeof(srrhi::ClusterStreamUpdateConstants)), "ClusterStreamUpdateCB", 1);
    const nvrhi::BufferHandle cb = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(cbd);

    srrhi::ClusterStreamUpdateConstants constants{};
    constants.SetPatchCount(uint32_t(m_Patches.size()));
    constants.SetPatchUnloadCount(unloadCount);
    constants.SetFrameIndex(m_FrameIndex);
    cmd->writeBuffer(cb, &constants, sizeof(constants), 0);

    srrhi::ClusterStreamUpdateInputs inputs;
    inputs.SetStreamCB(cb);
    inputs.SetPatches(m_PatchList);
    inputs.SetGroupStates(m_GroupStates);

    Renderer::RenderPassParams params;
    params.commandList = cmd;
    params.shaderID = ShaderID::CLUSTERSTREAMING_CLUSTERSTREAMUPDATE_CSMAIN;
    params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
    params.dispatchParams = {
        .x = (uint32_t(m_Patches.size()) + srrhi::CommonConsts::kThreadsPerGroup - 1) / srrhi::CommonConsts::kThreadsPerGroup,
        .y = 1, .z = 1 };
    g_Renderer.AddComputePass(params);
}

void GeometryStreaming::UploadLoadedBlobs(nvrhi::ICommandList* cmd)
{
    for (uint32_t groupIndex : m_LoadedThisFrame)
    {
        const Residency& residency = m_Residency[groupIndex];
        SDL_assert(residency.m_bResident);

        uint32_t byteSize = 0;
        const uint8_t* bytes = GetGroupBlob(groupIndex, byteSize);
        SDL_assert(bytes != nullptr && byteSize == uint64_t(residency.m_Elements) * sizeof(uint32_t));
        if (bytes == nullptr)
            continue;

        cmd->writeBuffer(m_StreamData, bytes, byteSize, uint64_t(residency.m_Offset) * sizeof(uint32_t));
    }
    m_LoadedThisFrame.clear();
}

void GeometryStreaming::UploadActiveGroups(nvrhi::ICommandList* cmd)
{
    if (!m_bForceActiveListUpload && m_ActiveGroups == m_UploadedActiveGroups)
        return;

    if (!m_ActiveGroups.empty())
        cmd->writeBuffer(m_ActiveGroupList, m_ActiveGroups.data(), m_ActiveGroups.size() * sizeof(uint32_t), 0);

    m_UploadedActiveGroups = m_ActiveGroups;
    m_bForceActiveListUpload = false;
}

void GeometryStreaming::DispatchAgeFilter(nvrhi::ICommandList* cmd)
{
    if (m_ActiveGroups.empty())
        return;

    // The sweep is only worth its latency when the budget cannot hold every blob. With room to spare the preload
    // already made the whole scene resident, and unloading what this frame's cut skipped only guarantees the next
    // camera move has to ask for it again: a turn then draws missing props and half-covered surfaces for as long
    // as the requests take to come back. A budget smaller than the scene keeps the sweep, which is what makes the
    // arena converge on what is on screen when it cannot hold everything.
    if (m_ArenaElements >= m_TotalElements)
        return;

    const nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(
        uint32_t(sizeof(srrhi::ClusterStreamAgeConstants)), "ClusterStreamAgeCB", 1);
    const nvrhi::BufferHandle cb = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(cbd);

    srrhi::ClusterStreamAgeConstants constants{};
    constants.SetActiveGroupCount(uint32_t(m_ActiveGroups.size()));
    constants.SetAgeThreshold(std::max(2u, Config::Get().m_ClusterStreamAgeThreshold));
    constants.SetFrameIndex(m_FrameIndex);
    constants.SetMaxUnloads(kMaxUnloadsPerFrame);
    cmd->writeBuffer(cb, &constants, sizeof(constants), 0);

    srrhi::ClusterStreamAgeInputs inputs;
    inputs.SetStreamCB(cb);
    inputs.SetActiveGroups(m_ActiveGroupList);
    inputs.SetGroupStates(m_GroupStates);
    inputs.SetUnloadRequests(m_UnloadRequests);
    inputs.SetStreamCounters(m_StreamCounters);

    Renderer::RenderPassParams params;
    params.commandList = cmd;
    params.shaderID = ShaderID::CLUSTERSTREAMING_CLUSTERSTREAMAGEFILTER_CSMAIN;
    params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
    params.dispatchParams = {
        .x = (uint32_t(m_ActiveGroups.size()) + srrhi::CommonConsts::kThreadsPerGroup - 1) / srrhi::CommonConsts::kThreadsPerGroup,
        .y = 1, .z = 1 };
    g_Renderer.AddComputePass(params);
}

void GeometryStreaming::PreRender(nvrhi::ICommandList* cmd)
{
    SDL_assert(cmd != nullptr);

    PROFILE_GPU_SCOPED("Cluster Stream Update", cmd);

    ++m_FrameIndex;

    m_Stats.m_LoadedThisFrame = 0;
    m_Stats.m_UnloadedThisFrame = 0;
    m_Stats.m_LoadBytesThisFrame = 0;
    m_Stats.m_LoadBudgetBytes = Config::Get().m_ClusterLoadBytesPerFrame;   // the panel slider is live

    // A budget change (the panel slider or --stream-budget) re-packs the arena before anything else this frame,
    // so the address table the traversal is about to read already matches the new reservation.
    Resize(cmd);

    // The traversal and the age filter only increment the counters, so the host rewrites the whole record once a
    // frame. It also carries the per-frame request budget the shaders clamp against.
    srrhi::ClusterStreamCounters counters{};
    counters.m_MaxLoads = kMaxLoadsPerFrame;
    counters.m_MaxUnloads = kMaxUnloadsPerFrame;
    cmd->writeBuffer(m_StreamCounters, &counters, sizeof(counters), 0);

    ConsumeCompletedRequests();

    m_Patches.clear();
    m_LoadedThisFrame.clear();

    ApplyUnloads(m_Patches);
    const size_t unloadPatchCount = m_Patches.size();
    ApplyLoads(m_Patches);

    // Patch first: the address table is what the next traversal reads, and the uploads below must not land in a
    // block whose previous owner is still in the table.
    if (!m_Patches.empty())
        DispatchPatches(cmd, uint32_t(unloadPatchCount));

    UploadLoadedBlobs(cmd);
    UploadActiveGroups(cmd);

    DispatchAgeFilter(cmd);

    uint32_t residentGroups = 0;
    uint64_t residentBytes = 0;
    memset(m_Stats.m_ResidentPerLevel, 0, sizeof(m_Stats.m_ResidentPerLevel));
    for (uint32_t groupIndex = 0; groupIndex < m_GroupCount; ++groupIndex)
    {
        const Residency& residency = m_Residency[groupIndex];
        if (!residency.m_bResident)
            continue;
        residentGroups++;
        residentBytes += uint64_t(residency.m_Elements) * sizeof(uint32_t);
        m_Stats.m_ResidentPerLevel[std::min(m_GroupBlobs[groupIndex].m_LODLevel, ClusterLod::kMaxLODLevels - 1)]++;
    }

    m_Stats.m_ResidentGroups = residentGroups;
    m_Stats.m_ResidentBytes = residentBytes;
    m_Stats.m_ActiveGroups = uint32_t(m_ActiveGroups.size());
    m_Stats.m_WastedBytes = m_Stats.m_ArenaBytes - residentBytes;
}

void GeometryStreaming::UpdateDiscreteLevels(nvrhi::ICommandList* cmd)
{
    SDL_assert(cmd != nullptr);
    if (m_Geometries.empty())
        return;

    // Per geometry: the first level of the suffix that is entirely resident. That suffix is what the discrete-LOD
    // path may seed a raster traversal at, so it can never ask for geometry the arena does not hold.
    std::vector<uint32_t> levels(m_Geometries.size(), 0u);
    uint32_t groupsPerLevel[ClusterLod::kMaxLODLevels];
    uint32_t residentPerLevel[ClusterLod::kMaxLODLevels];

    for (size_t geometryIndex = 0; geometryIndex < m_Geometries.size(); ++geometryIndex)
    {
        const GeometryRange& geometry = m_Geometries[geometryIndex];
        SDL_assert(geometry.m_GroupOffset + geometry.m_GroupCount <= m_GroupCount);

        memset(groupsPerLevel, 0, sizeof(groupsPerLevel));
        memset(residentPerLevel, 0, sizeof(residentPerLevel));

        for (uint32_t g = geometry.m_GroupOffset; g < geometry.m_GroupOffset + geometry.m_GroupCount; ++g)
        {
            const uint32_t level = std::min(m_GroupBlobs[g].m_LODLevel, ClusterLod::kMaxLODLevels - 1);
            groupsPerLevel[level]++;
            residentPerLevel[level] += m_Residency[g].m_bResident ? 1u : 0u;
        }

        // Every primitive's coarsest level is resident (it is the cheapest cover the arena always keeps), so the
        // scan starts there and walks towards higher detail while the levels it passes are complete.
        const uint32_t levelCount = std::min(geometry.m_LODLevelCount, ClusterLod::kMaxLODLevels);
        uint32_t discrete = levelCount > 0 ? levelCount - 1 : 0;
        while (discrete > 0 && residentPerLevel[discrete - 1] == groupsPerLevel[discrete - 1])
            --discrete;

        levels[geometryIndex] = discrete;
    }

    const bool bSizeChanged = m_DiscreteLevelsHost.size() != levels.size();
    const bool bValueChanged = !bSizeChanged &&
        memcmp(levels.data(), m_DiscreteLevelsHost.data(), levels.size() * sizeof(uint32_t)) != 0;
    if (!bSizeChanged && !bValueChanged)
        return;

    m_DiscreteLevelsHost = levels;
    cmd->writeBuffer(m_DiscreteLevels, levels.data(), levels.size() * sizeof(uint32_t), 0);
}

void GeometryStreaming::PostRender(nvrhi::ICommandList* cmd)
{
    SDL_assert(cmd != nullptr);

    // Pick a slot that is not waiting for its event; if all are, this frame's requests are skipped and the
    // traversal simply asks again next frame.
    ReadbackSlot* slot = nullptr;
    for (uint32_t i = 0; i < kReadbackSlots; ++i)
    {
        ReadbackSlot& candidate = m_Slots[(m_NextSlot + i) % kReadbackSlots];
        if (!candidate.m_bInFlight)
        {
            slot = &candidate;
            m_NextSlot = (m_NextSlot + i + 1) % kReadbackSlots;
            break;
        }
    }
    if (slot == nullptr)
        return;

    const uint32_t loadBytes = kLoadListCapacity * uint32_t(sizeof(uint32_t));
    cmd->copyBuffer(slot->m_Staging, 0, m_StreamCounters, 0, uint32_t(sizeof(srrhi::ClusterStreamCounters)));
    cmd->copyBuffer(slot->m_Staging, sizeof(srrhi::ClusterStreamCounters), m_LoadRequests, 0, loadBytes);
    cmd->copyBuffer(slot->m_Staging, sizeof(srrhi::ClusterStreamCounters) + loadBytes, m_UnloadRequests, 0,
        kUnloadListCapacity * uint32_t(sizeof(uint32_t)));

    slot->m_bInFlight = true;
    g_Renderer.SignalEventAfterFrame(slot->m_Event);
}
