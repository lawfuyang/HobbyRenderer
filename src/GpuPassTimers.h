#pragma once

// GPU time for every scope the frame records, one nvrhi timer query per scope.
//
// The queries are created up front: the D3D12 backend sizes its timestamp heap once, from the device's
// maxTimerQueries, and never grows it, so a scope needs a query that already exists. They are handed out in
// recording order, which means a frame can time at most kMaxTimedScopes scopes; anything past that is
// recorded but not timed. A query's timestamps cannot be read until the submission that wrote them has
// finished, so BeginFrame() publishes the frame before and the panel is always one frame behind.
class GpuPassTimers
{
public:
    struct Entry
    {
        std::string m_Group;                  // the renderer that recorded the scope; empty for frame-level work
        std::string m_Name;                   // the scope name, as given to PROFILE_GPU_SCOPED
        float       m_Milliseconds = 0.0f;    // summed over the frame when one name is recorded more than once
        uint32_t    m_Frame = 0;              // the frame index this value was measured in
    };

    // Scopes a single frame can time. The pool is double buffered - the frame being recorded uses one half
    // while the other half is read - so it holds twice this many queries, which is why D3D12RHI raises the
    // device's maxTimerQueries above nvrhi's default of 256.
    static constexpr uint32_t kMaxTimedScopes = 256;

    void Initialize(nvrhi::IDevice* device);
    void Shutdown();

    // Starts a frame: the scopes it records take their queries from the start of the pool again.
    void BeginFrame();

    // Publishes the values of every scope recorded by an earlier frame. Has to be called at a point where
    // the submissions that wrote those timestamps have completed - the render loop's wait-for-idle before
    // it submits the next frame is exactly that point.
    void Resolve(nvrhi::IDevice* device);

    // Recording side, called through ScopedGpuProfile from the thread recording that pass. Begin returns
    // null when there is no query left, in which case End is not called for it.
    nvrhi::ITimerQuery* Begin(nvrhi::ICommandList* commandList, std::string_view name);
    void End(nvrhi::ICommandList* commandList, nvrhi::ITimerQuery* query);

    const std::vector<Entry>& GetEntries() const { return m_Entries; }
    uint32_t GetLastResolvedFrame() const        { return m_LastResolvedFrame; }

private:
    struct Slot
    {
        nvrhi::TimerQueryHandle m_Query;
        std::string m_Name;
        std::string m_Group;
        uint32_t    m_Frame = 0;
        bool        m_bPending = false;       // started, timestamps not read yet
    };

    void Publish(const Slot& slot, float milliseconds);

    std::vector<Slot> m_Slots;
    std::vector<Entry> m_Entries;
    std::atomic<uint32_t> m_NextSlot{ 0 };
    std::atomic<bool> m_bBudgetLogged{ false };
    uint32_t m_FrameIndex = 0;
    uint32_t m_LastResolvedFrame = 0;
};

// Names the renderer that is recording, so the panel can list each of its dispatches underneath it. One
// renderer's passes are recorded by one thread, which is what makes the thread-local safe.
class ScopedGpuTimerGroup
{
public:
    explicit ScopedGpuTimerGroup(std::string_view group);
    ~ScopedGpuTimerGroup();

    ScopedGpuTimerGroup(const ScopedGpuTimerGroup&) = delete;
    ScopedGpuTimerGroup& operator=(const ScopedGpuTimerGroup&) = delete;

private:
    std::string_view m_Previous;
};

// The group named by the innermost ScopedGpuTimerGroup on this thread, empty outside one.
std::string_view GetCurrentGpuTimerGroup();
