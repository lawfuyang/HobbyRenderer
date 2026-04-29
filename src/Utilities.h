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

// Returns true if the 4×4 matrix has no NaN or Inf entries.
inline bool MatrixIsFinite(const Matrix& m)
{
    const float* p = reinterpret_cast<const float*>(&m);
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

// Returns true if the 4×4 matrix diagonal is not all-zero.
inline bool MatrixIsNonZero(const Matrix& m)
{
    return (m._11 != 0.0f || m._22 != 0.0f || m._33 != 0.0f || m._44 != 0.0f);
}

// Compute the length of a Vector3.
inline float Vec3Length(const Vector3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Extract the 3×3 rotation part of a 4×4 matrix as a row vector (first row).
inline Vector3 MatrixRow0(const Matrix& m)
{
    return Vector3{ m._11, m._12, m._13 };
}

// Returns true if |a - b| <= eps for every element of two 4x4 matrices.
inline bool MatrixNearEqual(const Matrix& a, const Matrix& b, float eps = 1e-4f)
{
    const float* pa = reinterpret_cast<const float*>(&a);
    const float* pb = reinterpret_cast<const float*>(&b);
    for (int i = 0; i < 16; ++i)
        if (std::fabs(pa[i] - pb[i]) > eps)
            return false;
    return true;
}

// Returns the 4x4 identity matrix.
inline Matrix IdentityMatrix()
{
    Matrix m{};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

// Multiply two Matrix values via DirectXMath.
inline Matrix MatMul(const Matrix& a, const Matrix& b)
{
    using namespace DirectX;
    XMMATRIX xa = XMLoadFloat4x4(&a);
    XMMATRIX xb = XMLoadFloat4x4(&b);
    Matrix out{};
    XMStoreFloat4x4(&out, XMMatrixMultiply(xa, xb));
    return out;
}

// Invert a Matrix via DirectXMath.
inline Matrix MatInv(const Matrix& m)
{
    using namespace DirectX;
    XMMATRIX xm = XMLoadFloat4x4(&m);
    Matrix out{};
    XMStoreFloat4x4(&out, XMMatrixInverse(nullptr, xm));
    return out;
}

inline float LengthSq(const Vector3& v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

// EV100 → linear exposure multiplier (photographic formula)
// exposure = 1 / (2^EV * 1.2)
inline float EV100ToExposure(float ev) { return 1.0f / (std::powf(2.0f, ev) * 1.2f); }

// Bloom mip count for a given base dimension
inline uint32_t ComputeMipCount(uint32_t dim)
{
    uint32_t count = 1;
    while (dim > 1) { dim >>= 1; ++count; }
    return count;
}

// Next power-of-two >= v
inline uint32_t NextPow2(uint32_t v)
{
    if (v == 0) return 1;
    --v;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

// Returns true if v is a power of two
inline bool IsPow2(uint32_t v) { return v > 0 && (v & (v - 1)) == 0; }
