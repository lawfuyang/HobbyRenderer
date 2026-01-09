#pragma once

#include "pch.h"

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
