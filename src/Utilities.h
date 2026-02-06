#pragma once

#include "pch.h"

static constexpr uint32_t NextLowerPow2(uint32_t v)
{
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v - (v >> 1);
}

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
