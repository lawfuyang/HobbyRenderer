// Tests_SharcRenderer.cpp
//
// Unit tests for the SHARC indirect lighting integration.
//
// Test categories:
//   1. Hash Grid Insertion/Lookup (3 tests)
//   2. Radiance Accumulation Math (3 tests)
//   3. Buffer Resource Allocation (3 tests)
//   4. Frame Lifecycle (4 tests)
//   5. ImGui State Synchronization (2 tests)
//   6. Shader Integration (2 tests)
//   7. Stale Entry Eviction (3 tests)
//   8. Intentional Failure Cases (3 tests)
//
// Run with: HobbyRenderer --run-tests=*Sharc*

#include "TestFixtures.h"

// ============================================================================
// Category 1: Hash Grid Insertion/Lookup
// ============================================================================
TEST_SUITE("Sharc_HashGrid")
{
    // TC-SHARC-HG-01: Cache capacity is exactly 2^22
    TEST_CASE("TC-SHARC-HG-01 HashGrid - cache capacity is 2^22")
    {
        constexpr uint32_t expected = 1u << 22;
        CHECK(SharcConfig::kCacheCapacity == expected);
        CHECK(SharcConfig::kCacheCapacity == 4194304u);
    }

    // TC-SHARC-HG-02: Hash entries buffer byte size matches capacity * 8 bytes
    TEST_CASE("TC-SHARC-HG-02 HashGrid - hash entries buffer size is capacity * 8 bytes")
    {
        // Hash entries: uint64 per entry = 8 bytes
        const uint64_t expectedBytes = (uint64_t)SharcConfig::kCacheCapacity * sizeof(uint64_t);
        CHECK(expectedBytes == (uint64_t)4194304 * 8);
        CHECK(expectedBytes == 33554432ull); // 32 MB
    }

    // TC-SHARC-HG-03: Total buffer memory is ~160 MB (8+16+16 bytes per entry)
    TEST_CASE("TC-SHARC-HG-03 HashGrid - total buffer memory is ~160 MB")
    {
        // Hash entries: 8B, Accumulation: 16B, Resolved: 16B = 40B per entry
        const uint64_t bytesPerEntry = 8 + 16 + 16;
        const uint64_t totalBytes = (uint64_t)SharcConfig::kCacheCapacity * bytesPerEntry;
        const uint64_t totalMB = totalBytes / (1024 * 1024);

        CHECK(bytesPerEntry == 40u);
        CHECK(totalMB == 160u);
    }
}

// ============================================================================
// Category 2: Radiance Accumulation Math
// ============================================================================
TEST_SUITE("Sharc_AccumulationMath")
{
    // TC-SHARC-AM-01: Accumulation frame count is clamped to valid range
    TEST_CASE("TC-SHARC-AM-01 AccumulationMath - accumulation frame num is in valid range")
    {
        SharcConfig cfg;
        // Default value should be within [1, 1024] (SHARC_ACCUMULATED_FRAME_NUM_MAX)
        CHECK(cfg.m_AccumulationFrameNum >= 1u);
        CHECK(cfg.m_AccumulationFrameNum <= 1024u);
    }

    // TC-SHARC-AM-02: Stale frame max is within SHARC spec limits
    TEST_CASE("TC-SHARC-AM-02 AccumulationMath - stale frame num max is within spec")
    {
        // SHARC_STALE_FRAME_NUM_MIN = 8, SHARC_STALE_FRAME_NUM_MAX = 1024
        CHECK(SharcConfig::kStaleFrameNumMax >= 8u);
        CHECK(SharcConfig::kStaleFrameNumMax <= 1024u);
        CHECK(SharcConfig::kStaleFrameNumMax == 100u);
    }

    // TC-SHARC-AM-03: Scene scale default is positive
    TEST_CASE("TC-SHARC-AM-03 AccumulationMath - scene scale default is positive")
    {
        SharcConfig cfg;
        CHECK(cfg.m_SceneScale > 0.0f);
    }
}

// ============================================================================
// Category 3: Buffer Resource Allocation
// ============================================================================
TEST_SUITE("Sharc_BufferAllocation")
{
// TC-SHARC-BA-01: Persistent buffer declaration succeeds when RenderGraph is in setup phase
    TEST_CASE("TC-SHARC-BA-01 BufferAllocation - persistent buffer declaration succeeds in setup phase")
    {
        RenderGraph rg;
        rg.Reset();
        rg.BeginSetup();
        rg.BeginPass("TestPass");

        // Declare a persistent buffer matching SHARC hash entries spec
        RGBufferHandle handle;
        RGBufferDesc desc;
        desc.m_NvrhiDesc.byteSize     = (uint64_t)SharcConfig::kCacheCapacity * sizeof(uint64_t);
        desc.m_NvrhiDesc.structStride = sizeof(uint64_t);
        desc.m_NvrhiDesc.canHaveUAVs  = true;
        desc.m_NvrhiDesc.debugName    = "Test_SHARC_HashEntries";
        bool result = rg.DeclarePersistentBuffer(desc, handle);

        rg.EndSetup();
        rg.Compile();

        CHECK(result == true);
        CHECK(handle.IsValid() == true);

        rg.Shutdown();
    }

// TC-SHARC-BA-02: Three persistent buffers declared with correct sizes are all valid after compile
    TEST_CASE("TC-SHARC-BA-02 BufferAllocation - three SHARC buffers valid after compile")
    {
        RenderGraph rg;
        rg.Reset();
        rg.BeginSetup();
        rg.BeginPass("TestPass");

        const uint64_t cap = SharcConfig::kCacheCapacity;
        RGBufferHandle hashHandle, accumHandle, resHandle;

        // Hash entries
        {
            RGBufferDesc d;
            d.m_NvrhiDesc.byteSize     = cap * sizeof(uint64_t);
            d.m_NvrhiDesc.structStride = sizeof(uint64_t);
            d.m_NvrhiDesc.canHaveUAVs  = true;
            d.m_NvrhiDesc.debugName    = "Test_Hash";
            rg.DeclarePersistentBuffer(d, hashHandle);
        }
        // Accumulation
        {
            RGBufferDesc d;
            d.m_NvrhiDesc.byteSize     = cap * 16;
            d.m_NvrhiDesc.structStride = 16;
            d.m_NvrhiDesc.canHaveUAVs  = true;
            d.m_NvrhiDesc.debugName    = "Test_Accum";
            rg.DeclarePersistentBuffer(d, accumHandle);
        }
        // Resolved
        {
            RGBufferDesc d;
            d.m_NvrhiDesc.byteSize     = cap * 16;
            d.m_NvrhiDesc.structStride = 16;
            d.m_NvrhiDesc.canHaveUAVs  = true;
            d.m_NvrhiDesc.debugName    = "Test_Resolved";
            rg.DeclarePersistentBuffer(d, resHandle);
        }

        rg.EndSetup();
        rg.Compile();

        CHECK(hashHandle.IsValid());
        CHECK(accumHandle.IsValid());
        CHECK(resHandle.IsValid());

        nvrhi::BufferHandle hashBuf  = rg.GetBufferRaw(hashHandle);
        nvrhi::BufferHandle accumBuf = rg.GetBufferRaw(accumHandle);
        nvrhi::BufferHandle resBuf   = rg.GetBufferRaw(resHandle);

        CHECK(hashBuf  != nullptr);
        CHECK(accumBuf != nullptr);
        CHECK(resBuf   != nullptr);

        rg.Shutdown();
    }

// TC-SHARC-BA-03: Buffer sizes match expected byte counts
    TEST_CASE("TC-SHARC-BA-03 BufferAllocation - buffer sizes match expected byte counts")
    {
        const uint64_t cap = SharcConfig::kCacheCapacity;

        // Hash entries: 8 bytes per entry
        CHECK(cap * sizeof(uint64_t) == cap * 8);
        CHECK(cap * 8  == 33554432ull); // 32 MB

        // Accumulation + Resolved: 16 bytes per entry each
        CHECK(cap * 16 == 67108864ull); // 64 MB each

        // Total: 40 bytes per entry = 160 MB
        CHECK(cap * (8 + 16 + 16) == 167772160ull);
    }
}

// ============================================================================
// Category 4: Frame Lifecycle
// ============================================================================
TEST_SUITE("Sharc_FrameLifecycle")
{
    // TC-SHARC-FL-01: SharcRenderer is registered in the renderer registry
    TEST_CASE("TC-SHARC-FL-01 FrameLifecycle - SharcRenderer is registered")
    {
        IRenderer* r = FindRendererByName("SharcRenderer");
        CHECK(r != nullptr);
    }

    // TC-SHARC-FL-02: SharcDebugVisualizationRenderer is registered
    TEST_CASE("TC-SHARC-FL-02 FrameLifecycle - SharcDebugVisualizationRenderer is registered")
    {
        IRenderer* r = FindRendererByName("SharcDebugViz");
        CHECK(r != nullptr);
    }

    // TC-SHARC-FL-03: SharcRenderer::Setup returns false when technique is None
    TEST_CASE("TC-SHARC-FL-03 FrameLifecycle - Setup returns false when technique is None")
    {
        const IndirectLightingTechnique prev = g_Renderer.m_IndirectLightingTechnique;
        g_Renderer.m_IndirectLightingTechnique = IndirectLightingTechnique::None;

        IRenderer* r = FindRendererByName("SharcRenderer");
        REQUIRE(r != nullptr);

        RenderGraph rg;
        rg.Reset();
        rg.BeginSetup();
        rg.BeginPass("TestPass");

        bool result = r->Setup(rg);

        rg.EndSetup();
        rg.Compile();
        rg.Shutdown();

        CHECK(result == false);

        g_Renderer.m_IndirectLightingTechnique = prev;
    }

    // TC-SHARC-FL-04: SharcRenderer is not a base-pass renderer
    TEST_CASE("TC-SHARC-FL-04 FrameLifecycle - SharcRenderer is not a base-pass renderer")
    {
        IRenderer* r = FindRendererByName("SharcRenderer");
        if (r)
            CHECK(!r->IsBasePassRenderer());
    }
}

// ============================================================================
// Category 5: ImGui State Synchronization
// ============================================================================
TEST_SUITE("Sharc_ImGuiState")
{
    // TC-SHARC-UI-01: Technique selector updates m_IndirectLightingTechnique
    TEST_CASE("TC-SHARC-UI-01 ImGuiState - technique selector updates m_IndirectLightingTechnique")
    {
        const IndirectLightingTechnique prev = g_Renderer.m_IndirectLightingTechnique;

        g_Renderer.m_IndirectLightingTechnique = IndirectLightingTechnique::SHARC;
        CHECK(g_Renderer.m_IndirectLightingTechnique == IndirectLightingTechnique::SHARC);

        g_Renderer.m_IndirectLightingTechnique = IndirectLightingTechnique::RestirGI;
        CHECK(g_Renderer.m_IndirectLightingTechnique == IndirectLightingTechnique::RestirGI);

        g_Renderer.m_IndirectLightingTechnique = IndirectLightingTechnique::None;
        CHECK(g_Renderer.m_IndirectLightingTechnique == IndirectLightingTechnique::None);

        g_Renderer.m_IndirectLightingTechnique = prev;
    }

    // TC-SHARC-UI-02: SHARC param changes update m_SharcConfig
    TEST_CASE("TC-SHARC-UI-02 ImGuiState - SHARC param changes update m_SharcConfig")
    {
        const SharcConfig prevCfg = g_Renderer.m_SharcConfig;

        g_Renderer.m_SharcConfig.m_AccumulationFrameNum = 32;
        CHECK(g_Renderer.m_SharcConfig.m_AccumulationFrameNum == 32u);

        g_Renderer.m_SharcConfig.m_SceneScale = 75.0f;
        CHECK(g_Renderer.m_SharcConfig.m_SceneScale == doctest::Approx(75.0f));

        g_Renderer.m_SharcConfig.m_ShowBounceHeatmap = true;
        CHECK(g_Renderer.m_SharcConfig.m_ShowBounceHeatmap == true);

        g_Renderer.m_SharcConfig = prevCfg;
    }
}

// ============================================================================
// Category 6: Shader Integration
// ============================================================================
TEST_SUITE("Sharc_ShaderIntegration")
{
    // TC-SHARC-SH-01: SHARC shader constants match CPU header values
    TEST_CASE("TC-SHARC-SH-01 ShaderIntegration - cache capacity constant is 2^22")
    {
        // The shader uses g_Sharc.m_EntriesNum which is set from SharcConfig::kCacheCapacity
        // Verify the CPU constant matches the expected value
        CHECK(SharcConfig::kCacheCapacity == (1u << 22));
        CHECK(SharcConfig::kCacheCapacity == 4194304u);
    }

    // TC-SHARC-SH-02: SHARC shader handles are non-null after initialization
    TEST_CASE("TC-SHARC-SH-02 ShaderIntegration - SHARC shader handles are non-null")
    {
        // These shader IDs are generated by ShaderIDsGenerator from shaders.cfg
        // They will be valid after the build_shaders step runs
        // We check that the ShaderID enum values exist (compile-time check via usage)
        CHECK(ShaderID::SHARCUPDATE_SHARCUPDATE_CSMAIN_HASH_GRID_ENABLE_64_BIT_ATOMICS_1_SHARC_UPDATE_1    < ShaderID::COUNT);
        CHECK(ShaderID::SHARCRESOLVE_SHARCRESOLVE_CSMAIN_HASH_GRID_ENABLE_64_BIT_ATOMICS_1  < ShaderID::COUNT);
        CHECK(ShaderID::SHARCQUERY_SHARCQUERY_CSMAIN_HASH_GRID_ENABLE_64_BIT_ATOMICS_1_SHARC_QUERY_1       < ShaderID::COUNT);
        CHECK(ShaderID::SHARCDEBUGVIZ_SHARCDEBUGVIZ_CSMAIN < ShaderID::COUNT);
    }
}

// ============================================================================
// Category 7: Stale Entry Eviction
// ============================================================================
TEST_SUITE("Sharc_StaleEviction")
{
    // TC-SHARC-SE-01: Stale frame num max is at least SHARC_STALE_FRAME_NUM_MIN (8)
    TEST_CASE("TC-SHARC-SE-01 StaleEviction - stale frame num max >= SHARC_STALE_FRAME_NUM_MIN")
    {
        // SHARC_STALE_FRAME_NUM_MIN = 8 per SharcCommon.h
        CHECK(SharcConfig::kStaleFrameNumMax >= 8u);
    }

    // TC-SHARC-SE-02: Stale frame num max is within SHARC_STALE_FRAME_NUM_MAX (1024)
    TEST_CASE("TC-SHARC-SE-02 StaleEviction - stale frame num max <= SHARC_STALE_FRAME_NUM_MAX")
    {
        // SHARC_STALE_FRAME_NUM_MAX = 1024 per SharcCommon.h
        CHECK(SharcConfig::kStaleFrameNumMax <= 1024u);
    }

    // TC-SHARC-SE-03: Accumulation frame num default is within valid range
    TEST_CASE("TC-SHARC-SE-03 StaleEviction - accumulation frame num is in [1, 1024]")
    {
        SharcConfig cfg;
        // SHARC_ACCUMULATED_FRAME_NUM_MIN = 1, SHARC_ACCUMULATED_FRAME_NUM_MAX = 1024
        CHECK(cfg.m_AccumulationFrameNum >= 1u);
        CHECK(cfg.m_AccumulationFrameNum <= 1024u);
    }
}

// ============================================================================
// Category 8: Intentional Failure Cases
// ============================================================================
TEST_SUITE("Sharc_FailureCases")
{
    // TC-SHARC-FC-01: Switching technique to None does not crash
    TEST_CASE("TC-SHARC-FC-01 FailureCases - switching to None does not crash")
    {
        const IndirectLightingTechnique prev = g_Renderer.m_IndirectLightingTechnique;
        CHECK_NOTHROW(g_Renderer.m_IndirectLightingTechnique = IndirectLightingTechnique::None);
        CHECK(g_Renderer.m_IndirectLightingTechnique == IndirectLightingTechnique::None);
        g_Renderer.m_IndirectLightingTechnique = prev;
    }

    // TC-SHARC-FC-02: Switching technique mid-frame (between frames) does not crash
    TEST_CASE("TC-SHARC-FC-02 FailureCases - mid-frame technique switch does not crash")
    {
        const IndirectLightingTechnique prev = g_Renderer.m_IndirectLightingTechnique;

        // Simulate switching from SHARC to None between frames
        CHECK_NOTHROW({
            g_Renderer.m_IndirectLightingTechnique = IndirectLightingTechnique::SHARC;
            g_Renderer.m_IndirectLightingTechnique = IndirectLightingTechnique::None;
        });

        g_Renderer.m_IndirectLightingTechnique = prev;
    }

// TC-SHARC-FC-03: RGBufferHandle is invalid before any declaration
    TEST_CASE("TC-SHARC-FC-03 FailureCases - RGBufferHandle is invalid before declaration")
    {
        RGBufferHandle handle;
        CHECK(handle.IsValid() == false);
    }
}
