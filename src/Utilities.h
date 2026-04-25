#pragma once



static constexpr uint32_t NextLowerPow2(uint32_t v)
{
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v - (v >> 1);
}

static constexpr size_t NextPow2(size_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

static constexpr uint32_t DivideAndRoundUp(uint32_t dividend, uint32_t divisor)
{
    return (dividend + divisor - 1) / divisor;
}

float Halton(uint32_t index, uint32_t base);

Vector3 CalculateGridZParams(float NearPlane, float FarPlane, float DepthDistributionScale, uint32_t GridSizeZ);

Vector2 CreateInvDeviceZToWorldZTransform(const Matrix& ProjMatrix);

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path);

uint32_t HashToUint(size_t hash);

void ChooseWindowSize(int* outWidth, int* outHeight);

struct SimpleTimer
{
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t start = SDL_GetPerformanceCounter();
    uint64_t last = start;

    void Reset()
    {
        start = SDL_GetPerformanceCounter();
        last = start;
    }

    double LapSeconds()
    {
        const uint64_t now = SDL_GetPerformanceCounter();
        const double seconds = static_cast<double>(now - last) / static_cast<double>(freq);
        last = now;
        return seconds;
    }

    double TotalSeconds() const
    {
        const uint64_t now = SDL_GetPerformanceCounter();
        return static_cast<double>(now - start) / static_cast<double>(freq);
    }

    double TotalMilliseconds() const
    {
        return SecondsToMilliseconds(TotalSeconds());
    }

    static float SecondsToMilliseconds(float seconds)
    {
        return seconds * 1000.0f;
    }
};

struct ScopedTimerLog
{
    explicit ScopedTimerLog(const char* labelText) : label(labelText) {}
    ~ScopedTimerLog()
    {
        SDL_Log("%s %.3f s", label, timer.TotalSeconds());
    }

    SimpleTimer timer{};
    const char* label = "[Timing]";
};

#define SCOPED_TIMER(labelLiteral) ScopedTimerLog scopedTimer_##__COUNTER__{labelLiteral}

struct SingleThreadGuard
{
    std::atomic<int>& count;
    SingleThreadGuard(std::atomic<int>& c) : count(c)
    {
        int expected = 0;
        if (!count.compare_exchange_strong(expected, 1))
        {
            SDL_assert(false && "Multiple threads detected in single-threaded function");
        }
    }
    ~SingleThreadGuard()
    {
        count.store(0);
    }
};

#define SINGLE_THREAD_GUARD() static std::atomic<int> _stg_count = 0; SingleThreadGuard _stg{ _stg_count }

// ─── MemoryMappedDataReader ──────────────────────────────────────────────────
// Maps a file read-only using the OS virtual memory system (MapViewOfFile on
// Windows). Only the pages that are actually accessed are loaded from disk,
// so callers pay only for what they touch — ideal for reading a small slice
// out of a large binary file.
//
// An alternative owned-data constructor lets callers wrap heap-allocated
// buffers (e.g. stbi-decoded pixels) through the same interface.
class MemoryMappedDataReader
{
public:
    // Maps filePath read-only.  Check IsValid() before use.
    explicit MemoryMappedDataReader(std::string_view filePath);

    // Takes ownership of externally-allocated data.
    // deleter(ptr) is called in the destructor to release the allocation.
    MemoryMappedDataReader(void* data, size_t size, void (*deleter)(void*) = nullptr);

    ~MemoryMappedDataReader();

    MemoryMappedDataReader(const MemoryMappedDataReader&) = delete;
    MemoryMappedDataReader& operator=(const MemoryMappedDataReader&) = delete;

    bool        IsValid()  const { return m_Data != nullptr; }
    // Returns a pointer at the current offset (see SetOffset).
    const void* GetData()  const { return static_cast<const uint8_t*>(m_Data) + m_Offset; }
    // Returns the number of bytes from the current offset to the end of the mapping.
    size_t      GetSize()  const { return m_Size - m_Offset; }
    // Skip the first `offset` bytes of the mapped region (e.g. to skip a header).
    void        SetOffset(size_t offset) { m_Offset = offset; }

private:
    void*  m_Data    = nullptr;
    size_t m_Size    = 0;
    size_t m_Offset  = 0;
    void (*m_Deleter)(void*) = nullptr; // non-null for owned-data mode
#ifdef _WIN32
    HANDLE m_File    = INVALID_HANDLE_VALUE;
    HANDLE m_Mapping = nullptr;
#endif
};
