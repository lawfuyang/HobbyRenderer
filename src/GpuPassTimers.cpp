#include "GpuPassTimers.h"

namespace
{
thread_local std::string_view tl_CurrentGpuTimerGroup;
}

void GpuPassTimers::Initialize(nvrhi::IDevice* device)
{
    SDL_assert(m_Slots.empty() && "GpuPassTimers::Initialize called twice");
    SDL_assert(device);

    m_Slots.resize(kMaxTimedScopes * 2);
    for (Slot& slot : m_Slots)
    {
        slot.m_Query = device->createTimerQuery();
        // createTimerQuery returns null once the device's query pool is used up, which would leave the
        // last scopes of every frame untimed without saying so.
        SDL_assert(slot.m_Query && "timer query pool exhausted - see nvrhi::DeviceDesc::maxTimerQueries");
    }
}

void GpuPassTimers::Shutdown()
{
    m_Slots.clear();
    m_Entries.clear();
    m_NextSlot.store(0, std::memory_order_relaxed);
    m_FrameIndex = 0;
    m_LastResolvedFrame = 0;
}

void GpuPassTimers::BeginFrame()
{
    m_FrameIndex++;
    m_NextSlot.store(0, std::memory_order_relaxed);
}

void GpuPassTimers::Resolve(nvrhi::IDevice* device)
{
    SDL_assert(device);

    for (Slot& slot : m_Slots)
    {
        // Only what an earlier frame recorded: the half this frame is writing belongs to it and has not
        // been submitted yet, so its timestamps do not exist.
        if (!slot.m_bPending || slot.m_Frame >= m_FrameIndex)
            continue;

        slot.m_bPending = false;

        if (device->pollTimerQuery(slot.m_Query))
            Publish(slot, device->getTimerQueryTime(slot.m_Query) * 1000.0f);

        // Free for the next frame's scope in this slot. A query that had not completed is dropped rather
        // than waited for, and its row keeps the value it already had.
        device->resetTimerQuery(slot.m_Query);
    }

    m_LastResolvedFrame = m_FrameIndex - 1;
}

nvrhi::ITimerQuery* GpuPassTimers::Begin(nvrhi::ICommandList* commandList, std::string_view name)
{
    SDL_assert(commandList);

    if (m_Slots.empty())
        return nullptr;

    const uint32_t index = m_NextSlot.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMaxTimedScopes)
    {
        if (!m_bBudgetLogged.exchange(true, std::memory_order_relaxed))
        {
            SDL_Log("[Profiler] a frame recorded more than %u GPU scopes; the rest are not timed", kMaxTimedScopes);
        }
        return nullptr;
    }

    // Each frame writes its own half of the pool, so the frame being read is never the frame being written.
    Slot& slot = m_Slots[(m_FrameIndex & 1) * kMaxTimedScopes + index];
    slot.m_Name.assign(name);
    slot.m_Group.assign(GetCurrentGpuTimerGroup());
    slot.m_Frame = m_FrameIndex;
    slot.m_bPending = true;

    commandList->beginTimerQuery(slot.m_Query);
    return slot.m_Query;
}

void GpuPassTimers::End(nvrhi::ICommandList* commandList, nvrhi::ITimerQuery* query)
{
    SDL_assert(commandList && query);
    commandList->endTimerQuery(query);
}

void GpuPassTimers::Publish(const Slot& slot, float milliseconds)
{
    for (Entry& entry : m_Entries)
    {
        if (entry.m_Name != slot.m_Name || entry.m_Group != slot.m_Group)
            continue;

        // The same name recorded twice in one frame (a mip chain per texture, say) reads as one row that
        // is the sum; a value from an earlier frame replaces it.
        if (entry.m_Frame == slot.m_Frame)
            entry.m_Milliseconds += milliseconds;
        else
            entry.m_Milliseconds = milliseconds;

        entry.m_Frame = slot.m_Frame;
        return;
    }

    m_Entries.push_back(Entry{ slot.m_Group, slot.m_Name, milliseconds, slot.m_Frame });
}

ScopedGpuTimerGroup::ScopedGpuTimerGroup(std::string_view group)
    : m_Previous(tl_CurrentGpuTimerGroup)
{
    tl_CurrentGpuTimerGroup = group;
}

ScopedGpuTimerGroup::~ScopedGpuTimerGroup()
{
    tl_CurrentGpuTimerGroup = m_Previous;
}

std::string_view GetCurrentGpuTimerGroup()
{
    return tl_CurrentGpuTimerGroup;
}
