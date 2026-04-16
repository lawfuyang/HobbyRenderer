// Tests_CoreBoot.cpp — Core Boot Tests
//
// Systems under test: TaskScheduler, Config, Utilities (timer, math)
// Setup required: None (CPU-only, no GPU/RHI)
//
// Run with: HobbyRenderer --run-tests=*CoreBoot*
// ============================================================================

// TestFixtures.h includes doctest.h (without DOCTEST_CONFIG_IMPLEMENT) and
// all shared headers. DOCTEST_CONFIG_IMPLEMENT lives in TestMain.cpp.
#include "TestFixtures.h"

// ============================================================================
// TEST SUITE: TaskScheduler
// ============================================================================
TEST_SUITE("TaskScheduler")
{
    // ------------------------------------------------------------------
    // TC-TS-01: Default construction creates kRuntimeThreadCount workers
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-01 Thread pool creation — default thread count")
    {
        TaskScheduler scheduler;
        CHECK(scheduler.GetThreadCount() == TaskScheduler::kRuntimeThreadCount);
    }

    // ------------------------------------------------------------------
    // TC-TS-02: SetThreadCount reduces the pool
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-02 Thread pool resize — reduce thread count")
    {
        TaskScheduler scheduler;
        scheduler.SetThreadCount(4);
        CHECK(scheduler.GetThreadCount() == 4);
    }

    // ------------------------------------------------------------------
    // TC-TS-03: SetThreadCount increases the pool
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-03 Thread pool resize — increase thread count")
    {
        TaskScheduler scheduler;
        scheduler.SetThreadCount(2);
        scheduler.SetThreadCount(8);
        CHECK(scheduler.GetThreadCount() == 8);
    }

    // ------------------------------------------------------------------
    // TC-TS-04: ParallelFor executes all items exactly once
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-04 ParallelFor — all items executed exactly once")
    {
        TaskScheduler scheduler;
        constexpr uint32_t kCount = 1000;
        std::vector<std::atomic<int>> counters(kCount);
        for (auto& c : counters) c.store(0);

        scheduler.ParallelFor(kCount, [&](uint32_t index, uint32_t /*threadIndex*/)
        {
            counters[index].fetch_add(1);
        });

        for (uint32_t i = 0; i < kCount; ++i)
        {
            CHECK(counters[i].load() == 1);
        }
    }

    // ------------------------------------------------------------------
    // TC-TS-05: ParallelFor with count=0 is a no-op
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-05 ParallelFor — zero count is a no-op")
    {
        TaskScheduler scheduler;
        bool called = false;
        scheduler.ParallelFor(0, [&](uint32_t, uint32_t)
        {
            called = true;
        });
        CHECK_FALSE(called);
    }

    // ------------------------------------------------------------------
    // TC-TS-06: ScheduleTask (immediate) executes the task
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-06 ScheduleTask — immediate execution")
    {
        TaskScheduler scheduler;
        std::atomic<int> counter{ 0 };

        scheduler.ScheduleTask([&counter]()
        {
            counter.fetch_add(1);
        }, /*bImmediateExecute=*/true);

        // Wait for the task to complete
        scheduler.ExecuteAllScheduledTasks();

        CHECK(counter.load() == 1);
    }

    // ------------------------------------------------------------------
    // TC-TS-07: ScheduleTask (deferred) executes on ExecuteAllScheduledTasks
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-07 ScheduleTask — deferred execution via ExecuteAllScheduledTasks")
    {
        TaskScheduler scheduler;
        std::atomic<int> counter{ 0 };

        scheduler.ScheduleTask([&counter]()
        {
            counter.fetch_add(1);
        }, /*bImmediateExecute=*/false);

        // Should not have run yet
        CHECK(counter.load() == 0);

        scheduler.ExecuteAllScheduledTasks();

        CHECK(counter.load() == 1);
    }

    // ------------------------------------------------------------------
    // TC-TS-08: Multiple deferred tasks all execute
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-08 ScheduleTask — multiple deferred tasks all execute")
    {
        TaskScheduler scheduler;
        constexpr int kTaskCount = 50;
        std::atomic<int> counter{ 0 };

        for (int i = 0; i < kTaskCount; ++i)
        {
            scheduler.ScheduleTask([&counter]()
            {
                counter.fetch_add(1);
            }, /*bImmediateExecute=*/false);
        }

        scheduler.ExecuteAllScheduledTasks();

        CHECK(counter.load() == kTaskCount);
    }

    // ------------------------------------------------------------------
    // TC-TS-09: ParallelFor accumulates a sum correctly (data race check)
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-09 ParallelFor — atomic accumulation is correct")
    {
        TaskScheduler scheduler;
        constexpr uint32_t kCount = 500;
        std::atomic<uint64_t> sum{ 0 };

        scheduler.ParallelFor(kCount, [&](uint32_t index, uint32_t)
        {
            sum.fetch_add(index);
        });

        // Expected: 0 + 1 + ... + (kCount-1) = kCount*(kCount-1)/2
        const uint64_t expected = static_cast<uint64_t>(kCount) * (kCount - 1) / 2;
        CHECK(sum.load() == expected);
    }

    // ------------------------------------------------------------------
    // TC-TS-10: Thread indices are within valid range
    // ------------------------------------------------------------------
    TEST_CASE("TC-TS-10 ParallelFor — thread indices are within valid range")
    {
        TaskScheduler scheduler;
        const uint32_t threadCount = scheduler.GetThreadCount();
        std::atomic<bool> outOfRange{ false };

        scheduler.ParallelFor(200, [&](uint32_t, uint32_t threadIndex)
        {
            // threadIndex can be 0..threadCount (main thread uses threadCount as its index)
            if (threadIndex > threadCount)
                outOfRange.store(true);
        });

        CHECK_FALSE(outOfRange.load());
    }
}

// ============================================================================
// TEST SUITE: Config
// ============================================================================
TEST_SUITE("Config")
{
    // ------------------------------------------------------------------
    // TC-CFG-01: Default config values are sane
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-01 Default config — validation disabled by default")
    {
        // We read the singleton; it was already parsed by main() with no
        // test-specific flags, so validation should be off.
        CHECK_FALSE(Config::Get().m_EnableValidation);
        CHECK_FALSE(Config::Get().m_EnableGPUAssistedValidation);
    }

    // ------------------------------------------------------------------
    // TC-CFG-02: Default scene path is empty
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-02 Default config — scene path is empty")
    {
        CHECK(Config::Get().m_ScenePath.empty());
    }

    // ------------------------------------------------------------------
    // TC-CFG-03: Default skip-textures is false
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-03 Default config — skip-textures is false")
    {
        CHECK_FALSE(Config::Get().m_SkipTextures);
    }

    // ------------------------------------------------------------------
    // TC-CFG-04: Default skip-cache is false
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-04 Default config — skip-cache is false")
    {
        CHECK_FALSE(Config::Get().m_SkipCache);
    }

    // ------------------------------------------------------------------
    // TC-CFG-05: Default render-graph aliasing is enabled
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-05 Default config — render graph aliasing enabled")
    {
        CHECK(Config::Get().m_EnableRenderGraphAliasing);
    }

    // ------------------------------------------------------------------
    // TC-CFG-06: ParseCommandLine sets --rhidebug
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-06 ParseCommandLine — --rhidebug enables validation")
    {
        ConfigGuard guard; // restore on exit

        const char* argv[] = { "HobbyRenderer", "--rhidebug" };
        Config::ParseCommandLine(2, const_cast<char**>(argv));

        CHECK(Config::Get().m_EnableValidation);
    }

    // ------------------------------------------------------------------
    // TC-CFG-07: ParseCommandLine sets --skip-textures
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-07 ParseCommandLine — --skip-textures sets flag")
    {
        ConfigGuard guard;

        const char* argv[] = { "HobbyRenderer", "--skip-textures" };
        Config::ParseCommandLine(2, const_cast<char**>(argv));

        CHECK(Config::Get().m_SkipTextures);
    }

    // ------------------------------------------------------------------
    // TC-CFG-08: ParseCommandLine sets --skip-cache
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-08 ParseCommandLine — --skip-cache sets flag")
    {
        ConfigGuard guard;

        const char* argv[] = { "HobbyRenderer", "--skip-cache" };
        Config::ParseCommandLine(2, const_cast<char**>(argv));

        CHECK(Config::Get().m_SkipCache);
    }

    // ------------------------------------------------------------------
    // TC-CFG-09: ParseCommandLine sets --disable-rendergraph-aliasing
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-09 ParseCommandLine — --disable-rendergraph-aliasing clears flag")
    {
        ConfigGuard guard;

        const char* argv[] = { "HobbyRenderer", "--disable-rendergraph-aliasing" };
        Config::ParseCommandLine(2, const_cast<char**>(argv));

        CHECK_FALSE(Config::Get().m_EnableRenderGraphAliasing);
    }

    // ------------------------------------------------------------------
    // TC-CFG-10: ParseCommandLine sets --execute-per-pass
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-10 ParseCommandLine — --execute-per-pass sets flag")
    {
        ConfigGuard guard;

        const char* argv[] = { "HobbyRenderer", "--execute-per-pass" };
        Config::ParseCommandLine(2, const_cast<char**>(argv));

        CHECK(Config::Get().ExecutePerPass);
    }

    // ------------------------------------------------------------------
    // TC-CFG-11: ParseCommandLine sets --execute-per-pass-and-wait
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-11 ParseCommandLine — --execute-per-pass-and-wait sets flag")
    {
        ConfigGuard guard;

        const char* argv[] = { "HobbyRenderer", "--execute-per-pass-and-wait" };
        Config::ParseCommandLine(2, const_cast<char**>(argv));

        CHECK(Config::Get().ExecutePerPassAndWait);
    }

    // ------------------------------------------------------------------
    // TC-CFG-12: Unknown arguments are silently ignored (no crash)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CFG-12 ParseCommandLine — unknown arguments do not crash")
    {
        ConfigGuard guard;

        const char* argv[] = { "HobbyRenderer", "--totally-unknown-flag-xyz" };
        // Should not throw or assert
        Config::ParseCommandLine(2, const_cast<char**>(argv));
        CHECK(true); // reached here = no crash
    }
}

// ============================================================================
// TEST SUITE: Timer
// ============================================================================
TEST_SUITE("Timer")
{
    // ------------------------------------------------------------------
    // TC-TMR-01: SimpleTimer TotalSeconds is non-negative immediately after construction
    // ------------------------------------------------------------------
    TEST_CASE("TC-TMR-01 SimpleTimer — TotalSeconds is non-negative")
    {
        SimpleTimer t;
        CHECK(t.TotalSeconds() >= 0.0);
    }

    // ------------------------------------------------------------------
    // TC-TMR-02: SimpleTimer measures elapsed time with reasonable accuracy
    //            We sleep 50 ms and expect 40–200 ms (generous bounds for CI).
    // ------------------------------------------------------------------
    TEST_CASE("TC-TMR-02 SimpleTimer — measures elapsed time accurately")
    {
        SimpleTimer t;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double elapsed = t.TotalMilliseconds();

        // Allow generous bounds: 40 ms – 500 ms
        CHECK(elapsed >= 40.0);
        CHECK(elapsed <= 500.0);
    }

    // ------------------------------------------------------------------
    // TC-TMR-03: SimpleTimer Reset restarts the clock
    // ------------------------------------------------------------------
    TEST_CASE("TC-TMR-03 SimpleTimer — Reset restarts elapsed time")
    {
        SimpleTimer t;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        t.Reset();
        const double afterReset = t.TotalMilliseconds();

        // After reset the elapsed should be very small (< 30 ms)
        CHECK(afterReset < 30.0);
    }

    // ------------------------------------------------------------------
    // TC-TMR-04: SimpleTimer LapSeconds returns positive value and advances
    // ------------------------------------------------------------------
    TEST_CASE("TC-TMR-04 SimpleTimer — LapSeconds returns positive value")
    {
        SimpleTimer t;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double lap = t.LapSeconds();
        CHECK(lap >= 0.0);
    }

    // ------------------------------------------------------------------
    // TC-TMR-05: SimpleTimer LapSeconds resets the lap counter
    // ------------------------------------------------------------------
    TEST_CASE("TC-TMR-05 SimpleTimer — consecutive LapSeconds are independent")
    {
        SimpleTimer t;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double lap1 = t.LapSeconds();

        // Immediately take another lap — should be very small
        const double lap2 = t.LapSeconds();

        CHECK(lap1 >= 0.0);
        // lap2 should be much smaller than lap1 (< 10 ms)
        CHECK(lap2 < 0.010);
    }

    // ------------------------------------------------------------------
    // TC-TMR-06: SecondsToMilliseconds conversion is correct
    // ------------------------------------------------------------------
    TEST_CASE("TC-TMR-06 SimpleTimer — SecondsToMilliseconds conversion")
    {
        CHECK(SimpleTimer::SecondsToMilliseconds(1.0f) == doctest::Approx(1000.0f));
        CHECK(SimpleTimer::SecondsToMilliseconds(0.5f) == doctest::Approx(500.0f));
        CHECK(SimpleTimer::SecondsToMilliseconds(0.0f) == doctest::Approx(0.0f));
    }
}

// ============================================================================
// TEST SUITE: Math
// ============================================================================
TEST_SUITE("Math")
{
    // ------------------------------------------------------------------
    // TC-MATH-01: NextLowerPow2 — basic cases
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-01 NextLowerPow2 — basic cases")
    {
        CHECK(NextLowerPow2(1u)   == 1u);
        CHECK(NextLowerPow2(2u)   == 2u);
        CHECK(NextLowerPow2(3u)   == 2u);
        CHECK(NextLowerPow2(4u)   == 4u);
        CHECK(NextLowerPow2(5u)   == 4u);
        CHECK(NextLowerPow2(7u)   == 4u);
        CHECK(NextLowerPow2(8u)   == 8u);
        CHECK(NextLowerPow2(255u) == 128u);
        CHECK(NextLowerPow2(256u) == 256u);
        CHECK(NextLowerPow2(1024u) == 1024u);
        CHECK(NextLowerPow2(1025u) == 1024u);
    }

    // ------------------------------------------------------------------
    // TC-MATH-02: NextPow2 — basic cases
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-02 NextPow2 — basic cases")
    {
        CHECK(NextPow2(1)   == 1);
        CHECK(NextPow2(2)   == 2);
        CHECK(NextPow2(3)   == 4);
        CHECK(NextPow2(4)   == 4);
        CHECK(NextPow2(5)   == 8);
        CHECK(NextPow2(7)   == 8);
        CHECK(NextPow2(8)   == 8);
        CHECK(NextPow2(9)   == 16);
        CHECK(NextPow2(255) == 256);
        CHECK(NextPow2(256) == 256);
        CHECK(NextPow2(257) == 512);
        CHECK(NextPow2(1000) == 1024);
    }

    // ------------------------------------------------------------------
    // TC-MATH-03: DivideAndRoundUp — basic cases
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-03 DivideAndRoundUp — basic cases")
    {
        CHECK(DivideAndRoundUp(0u, 4u)  == 0u);
        CHECK(DivideAndRoundUp(1u, 4u)  == 1u);
        CHECK(DivideAndRoundUp(4u, 4u)  == 1u);
        CHECK(DivideAndRoundUp(5u, 4u)  == 2u);
        CHECK(DivideAndRoundUp(8u, 4u)  == 2u);
        CHECK(DivideAndRoundUp(9u, 4u)  == 3u);
        CHECK(DivideAndRoundUp(100u, 7u) == 15u);  // 100/7 = 14.28... → 15
        CHECK(DivideAndRoundUp(7u, 7u)  == 1u);
        CHECK(DivideAndRoundUp(1u, 1u)  == 1u);
    }

    // ------------------------------------------------------------------
    // TC-MATH-04: DivideAndRoundUp — exact multiples
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-04 DivideAndRoundUp — exact multiples produce no rounding")
    {
        for (uint32_t d = 1; d <= 16; ++d)
        {
            for (uint32_t n = 0; n <= 64; n += d)
            {
                CHECK(DivideAndRoundUp(n, d) == n / d);
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-MATH-05: Halton sequence — base 2 first few values
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-05 Halton — base-2 sequence values")
    {
        // Known Halton base-2: 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8, ...
        CHECK(Halton(1, 2) == doctest::Approx(0.5f));
        CHECK(Halton(2, 2) == doctest::Approx(0.25f));
        CHECK(Halton(3, 2) == doctest::Approx(0.75f));
        CHECK(Halton(4, 2) == doctest::Approx(0.125f));
        CHECK(Halton(5, 2) == doctest::Approx(0.625f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-06: Halton sequence — base 3 first few values
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-06 Halton — base-3 sequence values")
    {
        // Known Halton base-3: 1/3, 2/3, 1/9, 4/9, 7/9, ...
        CHECK(Halton(1, 3) == doctest::Approx(1.0f / 3.0f));
        CHECK(Halton(2, 3) == doctest::Approx(2.0f / 3.0f));
        CHECK(Halton(3, 3) == doctest::Approx(1.0f / 9.0f));
        CHECK(Halton(4, 3) == doctest::Approx(4.0f / 9.0f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-07: Halton values are in [0, 1)
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-07 Halton — all values are in [0, 1)")
    {
        for (uint32_t i = 1; i <= 64; ++i)
        {
            const float v2 = Halton(i, 2);
            const float v3 = Halton(i, 3);
            CHECK(v2 >= 0.0f);
            CHECK(v2 < 1.0f);
            CHECK(v3 >= 0.0f);
            CHECK(v3 < 1.0f);
        }
    }

    // ------------------------------------------------------------------
    // TC-MATH-08: DirectXMath — XMVector basic arithmetic
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-08 DirectXMath — XMVector basic arithmetic")
    {
        using namespace DirectX;

        const XMVECTOR a = XMVectorSet(1.0f, 2.0f, 3.0f, 0.0f);
        const XMVECTOR b = XMVectorSet(4.0f, 5.0f, 6.0f, 0.0f);
        const XMVECTOR sum = XMVectorAdd(a, b);

        XMFLOAT4 result;
        XMStoreFloat4(&result, sum);

        CHECK(result.x == doctest::Approx(5.0f));
        CHECK(result.y == doctest::Approx(7.0f));
        CHECK(result.z == doctest::Approx(9.0f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-09: DirectXMath — XMVector3Dot
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-09 DirectXMath — XMVector3Dot")
    {
        using namespace DirectX;

        const XMVECTOR a = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        const XMVECTOR b = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR c = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);

        // Perpendicular vectors → dot = 0
        float dotAB = XMVectorGetX(XMVector3Dot(a, b));
        CHECK(dotAB == doctest::Approx(0.0f));

        // Parallel vectors → dot = 1
        float dotAC = XMVectorGetX(XMVector3Dot(a, c));
        CHECK(dotAC == doctest::Approx(1.0f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-10: DirectXMath — XMVector3Normalize
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-10 DirectXMath — XMVector3Normalize produces unit vector")
    {
        using namespace DirectX;

        const XMVECTOR v = XMVectorSet(3.0f, 4.0f, 0.0f, 0.0f);
        const XMVECTOR n = XMVector3Normalize(v);
        const float len = XMVectorGetX(XMVector3Length(n));

        CHECK(len == doctest::Approx(1.0f).epsilon(0.0001f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-11: DirectXMath — XMMatrixIdentity
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-11 DirectXMath — XMMatrixIdentity is correct")
    {
        using namespace DirectX;

        const XMMATRIX I = XMMatrixIdentity();
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, I);

        // Diagonal should be 1, off-diagonal 0
        CHECK(m._11 == doctest::Approx(1.0f));
        CHECK(m._22 == doctest::Approx(1.0f));
        CHECK(m._33 == doctest::Approx(1.0f));
        CHECK(m._44 == doctest::Approx(1.0f));
        CHECK(m._12 == doctest::Approx(0.0f));
        CHECK(m._13 == doctest::Approx(0.0f));
        CHECK(m._21 == doctest::Approx(0.0f));
        CHECK(m._31 == doctest::Approx(0.0f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-12: DirectXMath — XMMatrixMultiply with identity
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-12 DirectXMath — XMMatrixMultiply with identity is no-op")
    {
        using namespace DirectX;

        const XMMATRIX T = XMMatrixTranslation(1.0f, 2.0f, 3.0f);
        const XMMATRIX I = XMMatrixIdentity();
        const XMMATRIX result = XMMatrixMultiply(T, I);

        XMFLOAT4X4 mT, mR;
        XMStoreFloat4x4(&mT, T);
        XMStoreFloat4x4(&mR, result);

        CHECK(mR._41 == doctest::Approx(mT._41));
        CHECK(mR._42 == doctest::Approx(mT._42));
        CHECK(mR._43 == doctest::Approx(mT._43));
    }

    // ------------------------------------------------------------------
    // TC-MATH-13: DirectXMath — XMMatrixInverse
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-13 DirectXMath — XMMatrixInverse of translation")
    {
        using namespace DirectX;

        const XMMATRIX T = XMMatrixTranslation(5.0f, -3.0f, 2.0f);
        XMVECTOR det;
        const XMMATRIX Tinv = XMMatrixInverse(&det, T);
        const XMMATRIX product = XMMatrixMultiply(T, Tinv);

        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, product);

        // T * T^-1 should be identity
        CHECK(m._11 == doctest::Approx(1.0f).epsilon(0.0001f));
        CHECK(m._22 == doctest::Approx(1.0f).epsilon(0.0001f));
        CHECK(m._33 == doctest::Approx(1.0f).epsilon(0.0001f));
        CHECK(m._44 == doctest::Approx(1.0f).epsilon(0.0001f));
        CHECK(m._41 == doctest::Approx(0.0f).epsilon(0.0001f));
        CHECK(m._42 == doctest::Approx(0.0f).epsilon(0.0001f));
        CHECK(m._43 == doctest::Approx(0.0f).epsilon(0.0001f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-14: DirectXMath — XMQuaternionRotationAxis
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-14 DirectXMath — XMQuaternionRotationAxis produces unit quaternion")
    {
        using namespace DirectX;

        const XMVECTOR axis = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR q = XMQuaternionRotationAxis(axis, XM_PIDIV2);
        const float len = XMVectorGetX(XMQuaternionLength(q));

        CHECK(len == doctest::Approx(1.0f).epsilon(0.0001f));
    }

    // ------------------------------------------------------------------
    // TC-MATH-15: DirectXMath — BoundingFrustum contains/intersects
    // ------------------------------------------------------------------
    TEST_CASE("TC-MATH-15 DirectXMath — BoundingFrustum contains a point inside")
    {
        using namespace DirectX;

        // Build a simple perspective frustum
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.0f, 0.1f, 100.0f);
        BoundingFrustum frustum;
        BoundingFrustum::CreateFromMatrix(frustum, proj);

        // A point directly in front of the camera at z=10 should be inside
        const XMVECTOR pointInside = XMVectorSet(0.0f, 0.0f, 10.0f, 1.0f);
        XMFLOAT3 p;
        XMStoreFloat3(&p, pointInside);

        const ContainmentType ct = frustum.Contains(XMLoadFloat3(&p));
        CHECK(ct != DISJOINT);
    }
}
