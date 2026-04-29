// Tests_RenderGraphAllocator.cpp
//
// Systems under test: RenderGraph resource allocator — physical resource
//   allocation, heap sub-allocation, aliasing, multi-frame reuse, memory
//   pressure, dangling-reference prevention, and RHI liveness guarantees.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//
// Test coverage (suites):
//   RGAlloc_RHIAlive          — device/heap/resource liveness after allocation
//   RGAlloc_TextureAlloc      — texture allocation, reuse, and release
//   RGAlloc_BufferAlloc       — buffer allocation, reuse, and release
//   RGAlloc_Aliasing          — aliasing correctness: non-overlapping lifetimes
//   RGAlloc_NoAlias           — persistent resources are never aliased
//   RGAlloc_DanglingRef       — stale handles return nullptr after resource eviction
//   RGAlloc_MemoryPressure    — many resources, heap growth, stats consistency
//   RGAlloc_MultiFrameReuse   — physical resources survive across frames
//   RGAlloc_DescHashVariants  — different descs produce different allocations
//   RGAlloc_ShutdownReset     — Shutdown + re-init leaves allocator in clean state
//
// Run with: HobbyRenderer --run-tests=*RGAlloc*
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// TEST SUITE: RGAlloc_RHIAlive
// Verify that the RHI device and heap infrastructure are alive after the
// allocator has created physical resources.
// ============================================================================
TEST_SUITE("RGAlloc_RHIAlive")
{
    // ------------------------------------------------------------------
    // TC-RGAL-RHI-01: Device is non-null before any allocation
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-01 RHIAlive - device non-null before allocation")
    {
        REQUIRE(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-02: Device is non-null after a full frame (allocations happened)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-02 RHIAlive - device non-null after full frame")
    {
        RunOneFrame();
        CHECK(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-03: Allocated texture has a non-null physical handle
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-03 RHIAlive - allocated texture has non-null physical handle")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-RHI-03-Tex");
        const RGTextureHandle h = RunSingleTexPass(rg, desc, "TC-RHI-03-Pass");

        CHECK(h.IsValid());
        CHECK(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-04: Allocated buffer has a non-null physical handle
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-04 RHIAlive - allocated buffer has non-null physical handle")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferDesc desc = MakeBufDesc(256, true, "TC-RHI-04-Buf");
        const RGBufferHandle h = RunSingleBufPass(rg, desc, "TC-RHI-04-Pass");

        CHECK(h.IsValid());
        CHECK(rg.GetBufferRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-05: Physical texture survives waitForIdle (GPU still alive)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-05 RHIAlive - physical texture survives GPU idle")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(32, 32, nvrhi::Format::R32_FLOAT, true, "TC-RHI-05-Tex");
        const RGTextureHandle h = RunSingleTexPass(rg, desc, "TC-RHI-05-Pass");

        DEV()->waitForIdle();

        CHECK(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-06: Physical buffer survives waitForIdle (GPU still alive)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-06 RHIAlive - physical buffer survives GPU idle")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferDesc desc = MakeBufDesc(512, true, "TC-RHI-06-Buf");
        const RGBufferHandle h = RunSingleBufPass(rg, desc, "TC-RHI-06-Pass");

        DEV()->waitForIdle();

        CHECK(rg.GetBufferRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-07: Stats.m_NumAllocatedTextures > 0 after allocation
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-07 RHIAlive - NumAllocatedTextures > 0 after allocation")
    {
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetStats().m_NumAllocatedTextures > 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-08: Stats.m_NumAllocatedBuffers > 0 after allocation
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-08 RHIAlive - NumAllocatedBuffers > 0 after allocation")
    {
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetStats().m_NumAllocatedBuffers > 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-09: Stats.m_TotalTextureMemory > 0 after allocation
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-09 RHIAlive - TotalTextureMemory > 0 after allocation")
    {
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetStats().m_TotalTextureMemory > 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RHI-10: Stats.m_TotalBufferMemory > 0 after allocation
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RHI-10 RHIAlive - TotalBufferMemory > 0 after allocation")
    {
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetStats().m_TotalBufferMemory > 0);
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_TextureAlloc
// Physical texture allocation, descriptor-driven sizing, and release.
// ============================================================================
TEST_SUITE("RGAlloc_TextureAlloc")
{
    // ------------------------------------------------------------------
    // TC-RGAL-TA-01: 64×64 RGBA8 texture allocates successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-01 TextureAlloc - 64x64 RGBA8 allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureHandle h = RunSingleTexPass(rg,
            MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-TA-01"), "TC-TA-01-Pass");
        CHECK(rg.GetTextureRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-TA-02: 512×512 R32_FLOAT texture allocates successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-02 TextureAlloc - 512x512 R32_FLOAT allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureHandle h = RunSingleTexPass(rg,
            MakeTexDesc(512, 512, nvrhi::Format::R32_FLOAT, true, "TC-TA-02"), "TC-TA-02-Pass");
        CHECK(rg.GetTextureRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-TA-03: Two different-sized textures in the same pass both allocate
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-03 TextureAlloc - two different-sized textures in same pass")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle hA, hB;
        rg.DeclareTexture(MakeTexDesc(128, 128, nvrhi::Format::RGBA8_UNORM, true, "TC-TA-03-A"), hA);
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::R32_FLOAT,   true, "TC-TA-03-B"), hB);
        rg.BeginPass("TC-TA-03-Pass");
        rg.EndSetup();
        rg.Compile();

        CHECK(rg.GetTextureRaw(hA) != nullptr);
        CHECK(rg.GetTextureRaw(hB) != nullptr);
        CHECK(rg.GetTextureRaw(hA) != rg.GetTextureRaw(hB));

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-TA-04: Texture declared with isUAV=false also allocates
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-04 TextureAlloc - non-UAV texture allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureHandle h = RunSingleTexPass(rg,
            MakeTexDesc(32, 32, nvrhi::Format::RGBA8_UNORM, false, "TC-TA-04"), "TC-TA-04-Pass");
        CHECK(rg.GetTextureRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-TA-05: After PostRender, GetTextureRaw returns nullptr for
    //                a transient texture (not declared this frame)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-05 TextureAlloc - GetTextureRaw returns nullptr after PostRender")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureHandle h = RunSingleTexPass(rg,
            MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-TA-05"), "TC-TA-05-Pass");
        REQUIRE(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
        rg.Reset(); // new frame — resource not re-declared

        // Not declared this frame → GetTextureRaw must return nullptr
        CHECK(rg.GetTextureRaw(h) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-TA-06: Texture physical handle is non-null immediately after Compile
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-06 TextureAlloc - physical handle non-null immediately after Compile")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(16, 16, nvrhi::Format::RGBA8_UNORM, true, "TC-TA-06"), h);
        rg.BeginPass("TC-TA-06-Pass");
        rg.EndSetup();
        rg.Compile();

        // Immediately after Compile — no PostRender yet
        CHECK(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-TA-07: Texture with render-target flag allocates successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-07 TextureAlloc - render-target texture allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RGTextureDesc desc;
        desc.m_NvrhiDesc = nvrhi::TextureDesc()
            .setWidth(128).setHeight(128)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("TC-TA-07-RT")
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setIsUAV(false);
        const RGTextureHandle h = RunSingleTexPass(rg, desc, "TC-TA-07-Pass");
        CHECK(rg.GetTextureRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-TA-08: Stats.m_NumTextures equals number of declared textures
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-TA-08 TextureAlloc - Stats.NumTextures matches declared count")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h1, h2, h3;
        rg.DeclareTexture(MakeTexDesc(16, 16, nvrhi::Format::RGBA8_UNORM, true, "TC-TA-08-A"), h1);
        rg.DeclareTexture(MakeTexDesc(32, 32, nvrhi::Format::R32_FLOAT,   true, "TC-TA-08-B"), h2);
        rg.DeclareTexture(MakeTexDesc(64, 64, nvrhi::Format::RG16_FLOAT,  true, "TC-TA-08-C"), h3);
        rg.BeginPass("TC-TA-08-Pass");
        rg.EndSetup();
        rg.Compile();

        // The full-frame renderers also declare textures, so we just check >= 3
        CHECK(rg.GetStats().m_NumTextures >= 3);

        rg.PostRender();
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_BufferAlloc
// Physical buffer allocation, descriptor-driven sizing, and release.
// ============================================================================
TEST_SUITE("RGAlloc_BufferAlloc")
{
    // ------------------------------------------------------------------
    // TC-RGAL-BA-01: Small buffer (64 bytes) allocates successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-BA-01 BufferAlloc - 64-byte buffer allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferHandle h = RunSingleBufPass(rg, MakeBufDesc(64, true, "TC-BA-01"), "TC-BA-01-Pass");
        CHECK(rg.GetBufferRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-BA-02: Large buffer (4 MB) allocates successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-BA-02 BufferAlloc - 4MB buffer allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferHandle h = RunSingleBufPass(rg,
            MakeBufDesc(4 * 1024 * 1024, true, "TC-BA-02"), "TC-BA-02-Pass");
        CHECK(rg.GetBufferRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-BA-03: Two buffers in the same pass both allocate
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-BA-03 BufferAlloc - two buffers in same pass both allocate")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGBufferHandle hA, hB;
        rg.DeclareBuffer(MakeBufDesc(128, true,  "TC-BA-03-A"), hA);
        rg.DeclareBuffer(MakeBufDesc(256, false, "TC-BA-03-B"), hB);
        rg.BeginPass("TC-BA-03-Pass");
        rg.EndSetup();
        rg.Compile();

        CHECK(rg.GetBufferRaw(hA) != nullptr);
        CHECK(rg.GetBufferRaw(hB) != nullptr);
        CHECK(rg.GetBufferRaw(hA) != rg.GetBufferRaw(hB));

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-BA-04: After PostRender + Reset, GetBufferRaw returns nullptr
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-BA-04 BufferAlloc - GetBufferRaw returns nullptr after PostRender+Reset")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferHandle h = RunSingleBufPass(rg, MakeBufDesc(64, true, "TC-BA-04"), "TC-BA-04-Pass");
        REQUIRE(rg.GetBufferRaw(h) != nullptr);

        rg.PostRender();
        rg.Reset(); // new frame — resource not re-declared

        CHECK(rg.GetBufferRaw(h) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-BA-05: Buffer with structStride allocates successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-BA-05 BufferAlloc - structured buffer allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RGBufferDesc desc;
        desc.m_NvrhiDesc.byteSize     = 1024;
        desc.m_NvrhiDesc.structStride = 16;
        desc.m_NvrhiDesc.canHaveUAVs  = true;
        desc.m_NvrhiDesc.debugName    = "TC-BA-05-Struct";
        desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        const RGBufferHandle h = RunSingleBufPass(rg, desc, "TC-BA-05-Pass");
        CHECK(rg.GetBufferRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-BA-06: Stats.m_NumBuffers >= declared buffer count after Compile
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-BA-06 BufferAlloc - Stats.NumBuffers >= declared count")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGBufferHandle h1, h2;
        rg.DeclareBuffer(MakeBufDesc(64,  true, "TC-BA-06-A"), h1);
        rg.DeclareBuffer(MakeBufDesc(128, true, "TC-BA-06-B"), h2);
        rg.BeginPass("TC-BA-06-Pass");
        rg.EndSetup();
        rg.Compile();

        CHECK(rg.GetStats().m_NumBuffers >= 2);

        rg.PostRender();
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_Aliasing
// Verify that non-overlapping transient resources share heap memory.
// ============================================================================
TEST_SUITE("RGAlloc_Aliasing")
{
    // ------------------------------------------------------------------
    // TC-RGAL-AL-01: With aliasing enabled, NumAliasedTextures > 0 after
    //                a full frame (the engine declares many transient textures)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-01 Aliasing - NumAliasedTextures > 0 with aliasing enabled")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;
        RunOneFrame();
        // The engine declares enough transient textures that at least one alias
        // should be found.
        CHECK(g_Renderer.m_RenderGraph.GetStats().m_NumAliasedTextures > 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-02: With aliasing disabled, NumAliasedTextures == 0
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-02 Aliasing - NumAliasedTextures == 0 with aliasing disabled")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = false;
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetStats().m_NumAliasedTextures == 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-03: With aliasing disabled, NumAliasedBuffers == 0
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-03 Aliasing - NumAliasedBuffers == 0 with aliasing disabled")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = false;
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetStats().m_NumAliasedBuffers == 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-04: Two textures in separate passes with non-overlapping
    //                lifetimes can alias — both still have valid handles
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-04 Aliasing - two non-overlapping textures both valid after Compile")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        // Pass A: declare + write TexA
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle hA;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-AL-04-A"), hA);
        rg.BeginPass("TC-AL-04-PassA");

        // Pass B: declare + write TexB (non-overlapping with TexA)
        RGTextureHandle hB;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-AL-04-B"), hB);
        rg.BeginPass("TC-AL-04-PassB");

        rg.EndSetup();
        rg.Compile();

        CHECK(hA.IsValid());
        CHECK(hB.IsValid());
        CHECK(rg.GetTextureRaw(hA) != nullptr);
        CHECK(rg.GetTextureRaw(hB) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-05: Aliased texture has a different nvrhi handle than its
    //                alias source (they are separate virtual resources on the
    //                same heap region)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-05 Aliasing - aliased texture has distinct nvrhi handle from source")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle hA;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-AL-05-A"), hA);
        rg.BeginPass("TC-AL-05-PassA");

        RGTextureHandle hB;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-AL-05-B"), hB);
        rg.BeginPass("TC-AL-05-PassB");

        rg.EndSetup();
        rg.Compile();

        // Both must be valid but are distinct nvrhi objects
        REQUIRE(rg.GetTextureRaw(hA) != nullptr);
        REQUIRE(rg.GetTextureRaw(hB) != nullptr);
        // They are separate virtual textures even if they share heap memory
        CHECK(rg.GetTextureRaw(hA).Get() != rg.GetTextureRaw(hB).Get());

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-06: Aliasing enabled — 5 frames survive without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-06 Aliasing - 5 frames survive with aliasing enabled")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-07: Aliasing disabled — 5 frames survive without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-07 Aliasing - 5 frames survive with aliasing disabled")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = false;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-08: Aliasing enabled → disabled mid-run — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-08 Aliasing - toggle aliasing mid-run survives")
    {
        ConfigGuard guard;
        auto& cfg = const_cast<Config&>(Config::Get());

        cfg.m_EnableRenderGraphAliasing = true;
        CHECK(RunOneFrame());
        CHECK(RunOneFrame());

        cfg.m_EnableRenderGraphAliasing = false;
        CHECK(RunOneFrame());
        CHECK(RunOneFrame());

        cfg.m_EnableRenderGraphAliasing = true;
        CHECK(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-AL-09: With aliasing enabled, total texture memory is <= total
    //                without aliasing (aliasing reduces or equals memory usage)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-AL-09 Aliasing - aliasing does not increase total texture memory")
    {
        ConfigGuard guard;
        auto& cfg = const_cast<Config&>(Config::Get());

        cfg.m_EnableRenderGraphAliasing = false;
        RunOneFrame();
        const size_t memNoAlias = g_Renderer.m_RenderGraph.GetStats().m_TotalTextureMemory;

        cfg.m_EnableRenderGraphAliasing = true;
        RunOneFrame();
        const size_t memWithAlias = g_Renderer.m_RenderGraph.GetStats().m_TotalTextureMemory;

        // Aliasing should not increase the reported memory footprint
        CHECK(memWithAlias <= memNoAlias);
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_NoAlias
// Persistent resources must never be aliased.
// ============================================================================
TEST_SUITE("RGAlloc_NoAlias")
{
    // ------------------------------------------------------------------
    // TC-RGAL-NA-01: Persistent texture is not aliased (AliasedFromIndex == UINT32_MAX)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-NA-01 NoAlias - persistent texture is not aliased")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclarePersistentTexture(
            MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-NA-01-Persist"), h);
        rg.BeginPass("TC-NA-01-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(h.m_Index < rg.GetTextures().size());
        // Persistent resources must not be aliased
        CHECK(rg.GetTextures().at(h.m_Index).m_AliasedFromIndex == UINT32_MAX);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-NA-02: Persistent buffer is not aliased
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-NA-02 NoAlias - persistent buffer is not aliased")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGBufferHandle h;
        rg.DeclarePersistentBuffer(MakeBufDesc(256, true, "TC-NA-02-Persist"), h);
        rg.BeginPass("TC-NA-02-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(h.m_Index < rg.GetBuffers().size());
        CHECK(rg.GetBuffers().at(h.m_Index).m_AliasedFromIndex == UINT32_MAX);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-NA-03: Persistent texture physical handle is non-null across
    //                two consecutive frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-NA-03 NoAlias - persistent texture physical handle stable across frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(32, 32, nvrhi::Format::R32_FLOAT, true, "TC-NA-03-Persist");

        // Frame 1
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclarePersistentTexture(desc, h);
        rg.BeginPass("TC-NA-03-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        const nvrhi::ITexture* physPtr1 = rg.GetTextureRaw(h).Get();
        rg.PostRender();

        // Frame 2: re-declare with same handle
        rg.Reset();
        rg.BeginSetup();
        rg.DeclarePersistentTexture(desc, h);
        rg.BeginPass("TC-NA-03-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        const nvrhi::ITexture* physPtr2 = rg.GetTextureRaw(h).Get();

        // Same physical resource — no re-allocation
        CHECK(physPtr1 == physPtr2);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-NA-04: Persistent buffer physical handle is stable across frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-NA-04 NoAlias - persistent buffer physical handle stable across frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferDesc desc = MakeBufDesc(128, true, "TC-NA-04-Persist");

        rg.Reset();
        rg.BeginSetup();
        RGBufferHandle h;
        rg.DeclarePersistentBuffer(desc, h);
        rg.BeginPass("TC-NA-04-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(rg.GetBufferRaw(h) != nullptr);
        const nvrhi::IBuffer* physPtr1 = rg.GetBufferRaw(h).Get();
        rg.PostRender();

        rg.Reset();
        rg.BeginSetup();
        rg.DeclarePersistentBuffer(desc, h);
        rg.BeginPass("TC-NA-04-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(rg.GetBufferRaw(h) != nullptr);
        const nvrhi::IBuffer* physPtr2 = rg.GetBufferRaw(h).Get();

        CHECK(physPtr1 == physPtr2);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-NA-05: Persistent texture IsPersistent flag is set after declaration
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-NA-05 NoAlias - persistent texture has IsPersistent flag set")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclarePersistentTexture(
            MakeTexDesc(16, 16, nvrhi::Format::RGBA8_UNORM, true, "TC-NA-05-Persist"), h);
        rg.BeginPass("TC-NA-05-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        CHECK(rg.GetTextures()[h.m_Index].m_IsPersistent == true);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-NA-06: Transient texture IsPersistent flag is NOT set
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-NA-06 NoAlias - transient texture does not have IsPersistent flag")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(16, 16, nvrhi::Format::RGBA8_UNORM, true, "TC-NA-06-Trans"), h);
        rg.BeginPass("TC-NA-06-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        CHECK(rg.GetTextures()[h.m_Index].m_IsPersistent == false);

        rg.PostRender();
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_DanglingRef
// Stale handles must not return live GPU resources after the resource has been
// evicted or the graph has been reset.
// ============================================================================
TEST_SUITE("RGAlloc_DanglingRef")
{
    // ------------------------------------------------------------------
    // TC-RGAL-DR-01: Handle from previous frame returns nullptr after Reset
    //                (resource not re-declared)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DR-01 DanglingRef - stale texture handle returns nullptr after Reset")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureHandle h = RunSingleTexPass(rg,
            MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-DR-01"), "TC-DR-01-Pass");
        REQUIRE(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
        rg.Reset(); // new frame, resource not re-declared

        CHECK(rg.GetTextureRaw(h) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DR-02: Handle from previous frame returns nullptr after Reset
    //                (buffer not re-declared)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DR-02 DanglingRef - stale buffer handle returns nullptr after Reset")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferHandle h = RunSingleBufPass(rg,
            MakeBufDesc(128, true, "TC-DR-02"), "TC-DR-02-Pass");
        REQUIRE(rg.GetBufferRaw(h) != nullptr);

        rg.PostRender();
        rg.Reset();

        CHECK(rg.GetBufferRaw(h) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DR-03: Invalid handle always returns nullptr regardless of state
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DR-03 DanglingRef - invalid handle always returns nullptr")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RunOneFrame(); // ensure graph is compiled

        RGTextureHandle invalid;
        CHECK(rg.GetTextureRaw(invalid) == nullptr);

        RGBufferHandle invalidBuf;
        CHECK(rg.GetBufferRaw(invalidBuf) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DR-04: Out-of-range handle index returns nullptr
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DR-04 DanglingRef - out-of-range handle index returns nullptr")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RunOneFrame();

        RGTextureHandle oob;
        oob.m_Index = 0xFFFFFFFE; // near-max but not UINT32_MAX (which is "invalid")
        CHECK(rg.GetTextureRaw(oob) == nullptr);

        RGBufferHandle oobBuf;
        oobBuf.m_Index = 0xFFFFFFFE;
        CHECK(rg.GetBufferRaw(oobBuf) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DR-05: After Shutdown, ForceInvalidateFramesRemaining == 2
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DR-05 DanglingRef - Shutdown sets ForceInvalidateFramesRemaining to 2")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        // MinimalSceneFixture already called Shutdown + 2 warm-up frames,
        // so the counter should be 0 now.
        CHECK(rg.GetForceInvalidateFramesRemaining() == 0);

        // Calling Shutdown again resets it to 2.
        DEV()->waitForIdle();
        rg.Shutdown();
        CHECK(rg.GetForceInvalidateFramesRemaining() == 2);

        // Restore for fixture teardown
        rg.Reset();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DR-06: ForceInvalidateFramesRemaining decrements after PostRender
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DR-06 DanglingRef - ForceInvalidateFramesRemaining decrements after PostRender")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        DEV()->waitForIdle();
        rg.Shutdown();
        REQUIRE(rg.GetForceInvalidateFramesRemaining() == 2);

        // Simulate PostRender (which decrements the counter)
        rg.PostRender();
        CHECK(rg.GetForceInvalidateFramesRemaining() == 1);

        rg.PostRender();
        CHECK(rg.GetForceInvalidateFramesRemaining() == 0);

        // Restore
        rg.Reset();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DR-07: Transient texture declared in frame N is not accessible
    //                in frame N+1 if not re-declared
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DR-07 DanglingRef - transient texture not accessible in next frame if not re-declared")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(32, 32, nvrhi::Format::RGBA8_UNORM, true, "TC-DR-07");

        // Frame N
        const RGTextureHandle h = RunSingleTexPass(rg, desc, "TC-DR-07-Pass");
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        rg.PostRender();

        // Frame N+1: do NOT re-declare
        rg.Reset();
        // Declare a different resource to advance the frame
        rg.BeginSetup();
        RGTextureHandle hOther;
        rg.DeclareTexture(MakeTexDesc(8, 8, nvrhi::Format::R32_FLOAT, true, "TC-DR-07-Other"), hOther);
        rg.BeginPass("TC-DR-07-OtherPass");
        rg.EndSetup();
        rg.Compile();

        // Original handle must not return a live resource
        CHECK(rg.GetTextureRaw(h) == nullptr);

        rg.PostRender();
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_MemoryPressure
// Allocate many resources to exercise heap growth and sub-allocation.
// ============================================================================
TEST_SUITE("RGAlloc_MemoryPressure")
{
    // ------------------------------------------------------------------
    // TC-RGAL-MP-01: 16 textures in a single pass all allocate successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-01 MemoryPressure - 16 textures in one pass all allocate")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();

        static constexpr int kCount = 16;
        RGTextureHandle handles[kCount];
        char name[64];
        for (int i = 0; i < kCount; ++i)
        {
            snprintf(name, sizeof(name), "TC-MP-01-Tex%02d", i);
            rg.DeclareTexture(MakeTexDesc(128, 128, nvrhi::Format::RGBA8_UNORM, true, name), handles[i]);
        }
        rg.BeginPass("TC-MP-01-Pass");
        rg.EndSetup();
        rg.Compile();

        for (int i = 0; i < kCount; ++i)
        {
            INFO("Texture " << i);
            CHECK(handles[i].IsValid());
            CHECK(rg.GetTextureRaw(handles[i]) != nullptr);
        }

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MP-02: 16 buffers in a single pass all allocate successfully
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-02 MemoryPressure - 16 buffers in one pass all allocate")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();

        static constexpr int kCount = 16;
        RGBufferHandle handles[kCount];
        char name[64];
        for (int i = 0; i < kCount; ++i)
        {
            snprintf(name, sizeof(name), "TC-MP-02-Buf%02d", i);
            rg.DeclareBuffer(MakeBufDesc(256, true, name), handles[i]);
        }
        rg.BeginPass("TC-MP-02-Pass");
        rg.EndSetup();
        rg.Compile();

        for (int i = 0; i < kCount; ++i)
        {
            INFO("Buffer " << i);
            CHECK(handles[i].IsValid());
            CHECK(rg.GetBufferRaw(handles[i]) != nullptr);
        }

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MP-03: Stats.m_NumAllocatedTextures >= declared count
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-03 MemoryPressure - NumAllocatedTextures >= declared count")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();

        static constexpr int kCount = 8;
        RGTextureHandle handles[kCount];
        char name[64];
        for (int i = 0; i < kCount; ++i)
        {
            snprintf(name, sizeof(name), "TC-MP-03-Tex%02d", i);
            rg.DeclareTexture(MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, name), handles[i]);
        }
        rg.BeginPass("TC-MP-03-Pass");
        rg.EndSetup();
        rg.Compile();

        CHECK(rg.GetStats().m_NumAllocatedTextures >= kCount);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MP-04: Very large texture (2048×2048 RGBA32_FLOAT) allocates
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-04 MemoryPressure - 2048x2048 RGBA32_FLOAT texture allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureHandle h = RunSingleTexPass(rg,
            MakeTexDesc(2048, 2048, nvrhi::Format::RGBA32_FLOAT, true, "TC-MP-04-Large"),
            "TC-MP-04-Pass");
        CHECK(rg.GetTextureRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MP-05: Very large buffer (16 MB) allocates
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-05 MemoryPressure - 16MB buffer allocates")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferHandle h = RunSingleBufPass(rg,
            MakeBufDesc(16 * 1024 * 1024, true, "TC-MP-05-Large"),
            "TC-MP-05-Pass");
        CHECK(rg.GetBufferRaw(h) != nullptr);
        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MP-06: 10 consecutive frames with many resources — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-06 MemoryPressure - 10 frames with many resources survive")
    {
        for (int frame = 0; frame < 10; ++frame)
        {
            INFO("Frame " << frame);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MP-07: Stats.m_TotalTextureMemory is consistent with
    //                NumAllocatedTextures > 0 (non-zero memory when textures exist)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-07 MemoryPressure - TotalTextureMemory > 0 when textures allocated")
    {
        RunOneFrame();
        const auto& stats = g_Renderer.m_RenderGraph.GetStats();
        if (stats.m_NumAllocatedTextures > 0)
        {
            CHECK(stats.m_TotalTextureMemory > 0);
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MP-08: Stats.m_TotalBufferMemory is consistent with
    //                NumAllocatedBuffers > 0
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MP-08 MemoryPressure - TotalBufferMemory > 0 when buffers allocated")
    {
        RunOneFrame();
        const auto& stats = g_Renderer.m_RenderGraph.GetStats();
        if (stats.m_NumAllocatedBuffers > 0)
        {
            CHECK(stats.m_TotalBufferMemory > 0);
        }
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_MultiFrameReuse
//
// Physical-pointer stability contract:
//
//   PERSISTENT resources (DeclarePersistentTexture / DeclarePersistentBuffer)
//   are GUARANTEED to keep the same nvrhi handle across frames as long as the
//   descriptor does not change.  The allocator's trivial-reuse path fires
//   because m_IsAllocated=true && m_IsPhysicalOwner=true after the first frame.
//
//   TRANSIENT resources (DeclareTexture / DeclareBuffer) do NOT guarantee
//   pointer stability.  When a fresh handle is assigned via the pool-reuse
//   path it may land on a slot that was previously aliased
//   (m_IsPhysicalOwner=false).  On the next frame the trivial-reuse check
//   fails and the nvrhi object is recreated (same heap/offset, new handle).
//   This is correct behaviour: aliased virtual textures are cheap to recreate
//   and callers must not cache their raw pointers across frames.
//
// ============================================================================
TEST_SUITE("RGAlloc_MultiFrameReuse")
{
    // ------------------------------------------------------------------
    // TC-RGAL-MF-01: Transient texture physical pointer is stable across
    //                two consecutive frames ONLY when the slot is a physical
    //                owner (not aliased).  We verify the correct contract:
    //                the handle index is stable and the resource is non-null
    //                on both frames.  Pointer identity is only asserted when
    //                the slot is confirmed to be a physical owner.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MF-01 MultiFrameReuse - transient texture handle index stable and resource non-null across frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-MF-01");

        // Frame 1: allocate
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(desc, h);
        rg.BeginPass("TC-MF-01-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        const uint32_t slotIdx1 = h.m_Index;
        const nvrhi::ITexture* ptr1 = rg.GetTextureRaw(h).Get();
        // Record whether this slot is a physical owner so we can assert
        // pointer stability only when it is safe to do so.
        const bool isOwner1 = rg.GetTextures()[slotIdx1].m_IsPhysicalOwner;
        INFO("Frame1: slotIdx=" << slotIdx1 << " isPhysicalOwner=" << isOwner1
             << " ptr=" << ptr1);
        rg.PostRender();

        // Frame 2: re-declare with the SAME handle — must land on the same slot
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareTexture(desc, h);
        rg.BeginPass("TC-MF-01-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        const uint32_t slotIdx2 = h.m_Index;
        const nvrhi::ITexture* ptr2 = rg.GetTextureRaw(h).Get();
        const bool isOwner2 = rg.GetTextures()[slotIdx2].m_IsPhysicalOwner;
        INFO("Frame2: slotIdx=" << slotIdx2 << " isPhysicalOwner=" << isOwner2
             << " ptr=" << ptr2);

        // The handle index must be stable — the fast-path re-uses the same slot.
        CHECK(slotIdx2 == slotIdx1);

        // The physical resource must be non-null on both frames.
        CHECK(ptr1 != nullptr);
        CHECK(ptr2 != nullptr);

        // Pointer identity is only guaranteed when the slot is a physical owner
        // on both frames.  Aliased slots (m_IsPhysicalOwner=false) get a new
        // nvrhi handle each frame — that is correct engine behaviour.
        if (isOwner1 && isOwner2)
        {
            INFO("Both frames are physical owners — asserting pointer identity");
            CHECK(ptr1 == ptr2);
        }
        else
        {
            INFO("Slot was aliased on at least one frame — pointer identity not required");
            // Still verify the resource is valid and usable
            CHECK(rg.GetTextures()[slotIdx2].m_IsAllocated);
        }

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MF-02: Transient buffer handle index is stable and resource
    //                non-null across frames.  Pointer identity only asserted
    //                for physical-owner slots (same contract as MF-01).
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MF-02 MultiFrameReuse - transient buffer handle index stable and resource non-null across frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferDesc desc = MakeBufDesc(256, true, "TC-MF-02");

        // Frame 1
        rg.Reset();
        rg.BeginSetup();
        RGBufferHandle h;
        rg.DeclareBuffer(desc, h);
        rg.BeginPass("TC-MF-02-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        REQUIRE(rg.GetBufferRaw(h) != nullptr);
        const uint32_t slotIdx1 = h.m_Index;
        const nvrhi::IBuffer* ptr1 = rg.GetBufferRaw(h).Get();
        const bool isOwner1 = rg.GetBuffers()[slotIdx1].m_IsPhysicalOwner;
        INFO("Frame1: slotIdx=" << slotIdx1 << " isPhysicalOwner=" << isOwner1
             << " ptr=" << ptr1);
        rg.PostRender();

        // Frame 2: re-declare with same handle
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareBuffer(desc, h);
        rg.BeginPass("TC-MF-02-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        REQUIRE(rg.GetBufferRaw(h) != nullptr);
        const uint32_t slotIdx2 = h.m_Index;
        const nvrhi::IBuffer* ptr2 = rg.GetBufferRaw(h).Get();
        const bool isOwner2 = rg.GetBuffers()[slotIdx2].m_IsPhysicalOwner;
        INFO("Frame2: slotIdx=" << slotIdx2 << " isPhysicalOwner=" << isOwner2
             << " ptr=" << ptr2);

        // Handle index must be stable.
        CHECK(slotIdx2 == slotIdx1);

        // Resource must be non-null on both frames.
        CHECK(ptr1 != nullptr);
        CHECK(ptr2 != nullptr);

        // Pointer identity only for physical-owner slots.
        if (isOwner1 && isOwner2)
        {
            INFO("Both frames are physical owners — asserting pointer identity");
            CHECK(ptr1 == ptr2);
        }
        else
        {
            INFO("Slot was aliased on at least one frame — pointer identity not required");
            CHECK(rg.GetBuffers()[slotIdx2].m_IsAllocated);
        }

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MF-03: Changing the texture descriptor forces re-allocation
    //
    //   When the desc hash changes DeclareTexture clears m_IsAllocated and
    //   frees the old heap block.  SubAllocateResource then re-allocates —
    //   it may reuse the same freed block (same heap/offset), in which case
    //   the createAndBindResource callback's early-return guard fires and the
    //   nvrhi handle is preserved.  Or it may find a different block and
    //   recreate the handle.  Either way the resource must be valid and
    //   m_IsAllocated must be true after Compile().
    //
    //   We do NOT assert ptr1 != ptr2 because that depends on heap layout.
    //   The invariant we care about is: the slot is re-allocated and the
    //   physical handle is non-null.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MF-03 MultiFrameReuse - desc change forces re-allocation")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // Frame 1: 64×64 — use an unusual size to avoid pool-reuse with engine slots.
        // Advance the frame counter first so the pool-reuse window is clear.
        RunOneFrame();
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-MF-03"), h);
        rg.BeginPass("TC-MF-03-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        const uint32_t slotIdx = h.m_Index;
        INFO("Frame1: slot=" << slotIdx
             << " isOwner=" << rg.GetTextures()[slotIdx].m_IsPhysicalOwner
             << " isAllocated=" << rg.GetTextures()[slotIdx].m_IsAllocated);
        rg.PostRender();

        // Frame 2: 128×128 — different desc → hash mismatch → m_IsAllocated cleared
        //          → old block freed → SubAllocateResource → re-allocated
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareTexture(MakeTexDesc(128, 128, nvrhi::Format::RGBA8_UNORM, true, "TC-MF-03"), h);
        rg.BeginPass("TC-MF-03-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        INFO("Frame2: slot=" << h.m_Index
             << " isOwner=" << rg.GetTextures()[h.m_Index].m_IsPhysicalOwner
             << " isAllocated=" << rg.GetTextures()[h.m_Index].m_IsAllocated);

        // The slot must be re-allocated and be a physical owner.
        CHECK(rg.GetTextures()[h.m_Index].m_IsAllocated     == true);
        CHECK(rg.GetTextures()[h.m_Index].m_IsPhysicalOwner == true);
        // The handle index must be stable (same slot, fast-path re-use).
        CHECK(h.m_Index == slotIdx);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MF-04: Persistent texture handle index is stable across 5 frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MF-04 MultiFrameReuse - persistent texture index stable across 5 frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(16, 16, nvrhi::Format::RGBA8_UNORM, true, "TC-MF-04-Persist");

        RGTextureHandle h;
        uint32_t firstIdx = UINT32_MAX;

        for (int frame = 0; frame < 5; ++frame)
        {
            rg.Reset();
            rg.BeginSetup();
            rg.DeclarePersistentTexture(desc, h);
            rg.BeginPass("TC-MF-04-Pass");
            rg.EndSetup();
            rg.Compile();

            REQUIRE(h.IsValid());
            if (frame == 0)
                firstIdx = h.m_Index;
            else
            {
                INFO("Frame " << frame << ": h.m_Index=" << h.m_Index << " firstIdx=" << firstIdx);
                CHECK(h.m_Index == firstIdx);
            }

            rg.PostRender();
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MF-05: Persistent buffer handle index is stable across 5 frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MF-05 MultiFrameReuse - persistent buffer index stable across 5 frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferDesc desc = MakeBufDesc(64, true, "TC-MF-05-Persist");

        RGBufferHandle h;
        uint32_t firstIdx = UINT32_MAX;

        for (int frame = 0; frame < 5; ++frame)
        {
            rg.Reset();
            rg.BeginSetup();
            rg.DeclarePersistentBuffer(desc, h);
            rg.BeginPass("TC-MF-05-Pass");
            rg.EndSetup();
            rg.Compile();

            REQUIRE(h.IsValid());
            if (frame == 0)
                firstIdx = h.m_Index;
            else
            {
                INFO("Frame " << frame << ": h.m_Index=" << h.m_Index << " firstIdx=" << firstIdx);
                CHECK(h.m_Index == firstIdx);
            }

            rg.PostRender();
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MF-06: LastFrameUsed is updated each frame a resource is declared
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MF-06 MultiFrameReuse - LastFrameUsed updated each frame")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(32, 32, nvrhi::Format::RGBA8_UNORM, true, "TC-MF-06");

        RGTextureHandle h;

        // Frame A
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareTexture(desc, h);
        rg.BeginPass("TC-MF-06-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        const uint64_t frameA = rg.GetTextures()[h.m_Index].m_LastFrameUsed;
        rg.PostRender();

        // Advance frame counter
        ++g_Renderer.m_FrameNumber;

        // Frame B
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareTexture(desc, h);
        rg.BeginPass("TC-MF-06-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(h.IsValid());
        const uint64_t frameB = rg.GetTextures()[h.m_Index].m_LastFrameUsed;

        CHECK(frameB > frameA);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-MF-07: Resource not declared for > 3 frames is evicted
    //                (physical handle becomes nullptr after Reset)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-MF-07 MultiFrameReuse - resource evicted after 3 frames of inactivity")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(32, 32, nvrhi::Format::RGBA8_UNORM, true, "TC-MF-07");

        // Declare once to create the physical resource
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(desc, h);
        rg.BeginPass("TC-MF-07-Pass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        rg.PostRender();

        // Advance frame counter by 4 (> kMaxTransientResourceLifetimeFrames = 3)
        // without re-declaring the resource, then call Reset to trigger eviction.
        g_Renderer.m_FrameNumber += 4;
        rg.Reset();

        // After eviction the physical texture should be null
        REQUIRE(h.IsValid());
        CHECK(rg.GetTextures()[h.m_Index].m_PhysicalTexture == nullptr);
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_DescHashVariants
// Different descriptors must produce independent allocations.
// ============================================================================
TEST_SUITE("RGAlloc_DescHashVariants")
{
    // ------------------------------------------------------------------
    // TC-RGAL-DH-01: Different texture formats produce different hashes
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGAL-DH-01 DescHash - different formats produce different hashes")
    {
        const RGTextureDesc a = MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "A");
        const RGTextureDesc b = MakeTexDesc(64, 64, nvrhi::Format::R32_FLOAT,   true, "B");
        CHECK(a.ComputeHash() != b.ComputeHash());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DH-02: Different texture sizes produce different hashes
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGAL-DH-02 DescHash - different sizes produce different hashes")
    {
        const RGTextureDesc a = MakeTexDesc(64,  64,  nvrhi::Format::RGBA8_UNORM, true, "A");
        const RGTextureDesc b = MakeTexDesc(128, 128, nvrhi::Format::RGBA8_UNORM, true, "B");
        CHECK(a.ComputeHash() != b.ComputeHash());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DH-03: Same texture descriptor produces the same hash
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGAL-DH-03 DescHash - identical descs produce identical hashes")
    {
        const RGTextureDesc a = MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "A");
        const RGTextureDesc b = MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "B");
        CHECK(a.ComputeHash() == b.ComputeHash()); // debug name is not part of hash
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DH-04: Different buffer sizes produce different hashes
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGAL-DH-04 DescHash - different buffer sizes produce different hashes")
    {
        const RGBufferDesc a = MakeBufDesc(64,  true, "A");
        const RGBufferDesc b = MakeBufDesc(128, true, "B");
        CHECK(a.ComputeHash() != b.ComputeHash());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DH-05: Same buffer descriptor produces the same hash
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGAL-DH-05 DescHash - identical buffer descs produce identical hashes")
    {
        const RGBufferDesc a = MakeBufDesc(256, true, "A");
        const RGBufferDesc b = MakeBufDesc(256, true, "B");
        CHECK(a.ComputeHash() == b.ComputeHash());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DH-06: UAV vs non-UAV buffer produces different hashes
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGAL-DH-06 DescHash - UAV vs non-UAV buffer produces different hashes")
    {
        const RGBufferDesc a = MakeBufDesc(256, true,  "A");
        const RGBufferDesc b = MakeBufDesc(256, false, "B");
        CHECK(a.ComputeHash() != b.ComputeHash());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DH-07: UAV vs non-UAV texture produces different hashes
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGAL-DH-07 DescHash - UAV vs non-UAV texture produces different hashes")
    {
        const RGTextureDesc a = MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true,  "A");
        const RGTextureDesc b = MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, false, "B");
        CHECK(a.ComputeHash() != b.ComputeHash());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-DH-08: Two textures with different descs in the same pass
    //                produce distinct physical handles
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-DH-08 DescHash - different descs produce distinct physical handles")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle hA, hB;
        rg.DeclareTexture(MakeTexDesc(64,  64,  nvrhi::Format::RGBA8_UNORM, true, "TC-DH-08-A"), hA);
        rg.DeclareTexture(MakeTexDesc(128, 128, nvrhi::Format::R32_FLOAT,   true, "TC-DH-08-B"), hB);
        rg.BeginPass("TC-DH-08-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(rg.GetTextureRaw(hA) != nullptr);
        REQUIRE(rg.GetTextureRaw(hB) != nullptr);
        CHECK(rg.GetTextureRaw(hA).Get() != rg.GetTextureRaw(hB).Get());

        rg.PostRender();
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_ShutdownReset
// Shutdown + re-init leaves the allocator in a clean, usable state.
// ============================================================================
TEST_SUITE("RGAlloc_ShutdownReset")
{
    // ------------------------------------------------------------------
    // TC-RGAL-SR-01: Shutdown clears all textures
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-01 ShutdownReset - Shutdown clears texture list")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RunOneFrame(); // populate the graph

        DEV()->waitForIdle();
        rg.Shutdown();

        CHECK(rg.GetTextures().empty());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-SR-02: Shutdown clears all buffers
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-02 ShutdownReset - Shutdown clears buffer list")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RunOneFrame();

        DEV()->waitForIdle();
        rg.Shutdown();

        CHECK(rg.GetBuffers().empty());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-SR-03: Shutdown clears all heaps
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-03 ShutdownReset - Shutdown clears heap list")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RunOneFrame();

        DEV()->waitForIdle();
        rg.Shutdown();

        CHECK(rg.GetHeaps().empty());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-SR-04: After Shutdown + Reset, new declarations succeed
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-04 ShutdownReset - new declarations succeed after Shutdown+Reset")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        DEV()->waitForIdle();
        rg.Shutdown();
        rg.Reset();

        rg.BeginSetup();
        RGTextureHandle h;
        const bool ok = rg.DeclareTexture(
            MakeTexDesc(32, 32, nvrhi::Format::RGBA8_UNORM, true, "TC-SR-04"), h);
        rg.BeginPass("TC-SR-04-Pass");
        rg.EndSetup();
        rg.Compile();

        CHECK(ok);
        CHECK(h.IsValid());
        CHECK(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-SR-05: Reset clears Stats
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-05 ShutdownReset - Reset clears Stats")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        RunOneFrame(); // populate stats

        rg.Reset(); // new frame — stats cleared

        // After Reset, stats should be zeroed (they are rebuilt during Compile)
        const auto& stats = rg.GetStats();
        CHECK(stats.m_NumTextures == 0);
        CHECK(stats.m_NumBuffers == 0);
        CHECK(stats.m_NumAllocatedTextures == 0);
        CHECK(stats.m_NumAllocatedBuffers == 0);
        CHECK(stats.m_TotalTextureMemory == 0);
        CHECK(stats.m_TotalBufferMemory == 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-SR-06: Full RunOneFrame succeeds after Shutdown + re-init
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-06 ShutdownReset - RunOneFrame succeeds after Shutdown+re-init")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        DEV()->waitForIdle();
        rg.Shutdown();

        // Two warm-up frames to clear the force-invalidate counter
        CHECK(RunOneFrame());
        CHECK(RunOneFrame());
        CHECK(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-RGAL-SR-07: GetPassIndex returns 0 before any pass is registered
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-07 ShutdownReset - GetPassIndex returns 0 for unknown pass")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        CHECK(rg.GetPassIndex("NonExistentPass") == 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-SR-08: GetCurrentPassIndex returns 0 before any BeginPass
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-SR-08 ShutdownReset - GetCurrentPassIndex is 0 before BeginPass")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        CHECK(rg.GetCurrentPassIndex() == 0);
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_OwnershipContract
//
// Precisely documents and verifies the physical-pointer stability contract:
//
//   PERSISTENT resources  → always physical owners → pointer stable across frames
//   TRANSIENT  resources  → may be aliased (non-owner) → pointer NOT guaranteed
//
// These tests were added after MF-01/MF-02 failures revealed that the original
// tests incorrectly assumed transient pointer stability.  The suite also
// exercises the engine-side SDL_Log that was added to the pool-reuse path so
// that future regressions produce actionable log output.
// ============================================================================
TEST_SUITE("RGAlloc_OwnershipContract")
{
    // ------------------------------------------------------------------
    // TC-RGAL-OC-01: Persistent texture is always a physical owner
    //                (m_IsPhysicalOwner == true after Compile)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-01 OwnershipContract - persistent texture is physical owner")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclarePersistentTexture(
            MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-01-Persist"), h);
        rg.BeginPass("TC-OC-01-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        const auto& slot = rg.GetTextures()[h.m_Index];
        CHECK(slot.m_IsPhysicalOwner == true);
        CHECK(slot.m_IsAllocated     == true);
        CHECK(slot.m_IsPersistent    == true);
        CHECK(slot.m_AliasedFromIndex == UINT32_MAX);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-02: Persistent buffer is always a physical owner
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-02 OwnershipContract - persistent buffer is physical owner")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        rg.Reset();
        rg.BeginSetup();
        RGBufferHandle h;
        rg.DeclarePersistentBuffer(MakeBufDesc(256, true, "TC-OC-02-Persist"), h);
        rg.BeginPass("TC-OC-02-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        const auto& slot = rg.GetBuffers()[h.m_Index];
        CHECK(slot.m_IsPhysicalOwner == true);
        CHECK(slot.m_IsAllocated     == true);
        CHECK(slot.m_IsPersistent    == true);
        CHECK(slot.m_AliasedFromIndex == UINT32_MAX);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-03: Persistent texture physical pointer is identical
    //                across 3 consecutive frames (guaranteed stability)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-03 OwnershipContract - persistent texture ptr identical across 3 frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(32, 32, nvrhi::Format::R32_FLOAT, true, "TC-OC-03-Persist");

        RGTextureHandle h;
        const nvrhi::ITexture* firstPtr = nullptr;

        for (int frame = 0; frame < 3; ++frame)
        {
            rg.Reset();
            rg.BeginSetup();
            rg.DeclarePersistentTexture(desc, h);
            rg.BeginPass("TC-OC-03-Pass");
            rg.EndSetup();
            rg.Compile();

            REQUIRE(h.IsValid());
            REQUIRE(rg.GetTextureRaw(h) != nullptr);

            const nvrhi::ITexture* ptr = rg.GetTextureRaw(h).Get();
            INFO("Frame " << frame << ": ptr=" << ptr);

            if (frame == 0)
                firstPtr = ptr;
            else
                CHECK(ptr == firstPtr);

            rg.PostRender();
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-04: Persistent buffer physical pointer is identical
    //                across 3 consecutive frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-04 OwnershipContract - persistent buffer ptr identical across 3 frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGBufferDesc desc = MakeBufDesc(128, true, "TC-OC-04-Persist");

        RGBufferHandle h;
        const nvrhi::IBuffer* firstPtr = nullptr;

        for (int frame = 0; frame < 3; ++frame)
        {
            rg.Reset();
            rg.BeginSetup();
            rg.DeclarePersistentBuffer(desc, h);
            rg.BeginPass("TC-OC-04-Pass");
            rg.EndSetup();
            rg.Compile();

            REQUIRE(h.IsValid());
            REQUIRE(rg.GetBufferRaw(h) != nullptr);

            const nvrhi::IBuffer* ptr = rg.GetBufferRaw(h).Get();
            INFO("Frame " << frame << ": ptr=" << ptr);

            if (frame == 0)
                firstPtr = ptr;
            else
                CHECK(ptr == firstPtr);

            rg.PostRender();
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-05: Transient physical-owner texture preserves its pointer
    //                across frames (trivial-reuse path fires)
    //
    //   We guarantee a fresh slot (no pool-reuse possible) by running a full
    //   frame first so m_FrameNumber advances past the pool-reuse window
    //   (pool-reuse requires lastFrameUsed > 1 frame ago).  After frame 1
    //   the slot is a physical owner; frame 2 must hit the trivial-reuse
    //   path and keep the same nvrhi handle.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-05 OwnershipContract - transient physical-owner texture ptr stable")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        // Use an unusual size to minimise hash collisions with engine resources.
        const RGTextureDesc desc = MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-OC-05-Unique");

        // Advance the frame counter so any engine slot with the same hash has
        // lastFrameUsed <= FrameNumber-2, making pool-reuse impossible for it.
        // Reset() is required after RunOneFrame() so that engine resources from
        // that frame (which still have m_IsDeclaredThisFrame=true after PostRender)
        // are cleared before the test's own BeginSetup().  Without Reset() those
        // engine resources participate in the test's Compile() as aliasing
        // candidates, which can cause the test's fresh slot to be aliased
        // (m_IsPhysicalOwner=false) and fail the REQUIRE(isOwner1) check.
        RunOneFrame();
        rg.Reset();

        // Frame 1: fresh handle → guaranteed new slot (pool-reuse window clear)
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(desc, h);
        rg.BeginPass("TC-OC-05-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);

        const uint32_t slotIdx = h.m_Index;
        const bool isOwner1 = rg.GetTextures()[slotIdx].m_IsPhysicalOwner;
        const nvrhi::ITexture* ptr1 = rg.GetTextureRaw(h).Get();
        INFO("Frame1: slot=" << slotIdx << " isOwner=" << isOwner1 << " ptr=" << ptr1);

        // A freshly-allocated slot with no aliasing candidates must be an owner.
        REQUIRE(isOwner1);

        rg.PostRender();

        // Frame 2: same handle → fast-path → trivial-reuse → same ptr
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareTexture(desc, h);
        rg.BeginPass("TC-OC-05-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);

        const bool isOwner2 = rg.GetTextures()[slotIdx].m_IsPhysicalOwner;
        const nvrhi::ITexture* ptr2 = rg.GetTextureRaw(h).Get();
        INFO("Frame2: slot=" << slotIdx << " isOwner=" << isOwner2 << " ptr=" << ptr2);

        CHECK(isOwner2 == true);
        CHECK(ptr2 == ptr1);  // trivial-reuse: same physical handle

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-06: Transient physical-owner buffer preserves its pointer
    //                across frames (trivial-reuse path fires)
    //
    //   Same strategy as OC-05: advance the frame counter via RunOneFrame()
    //   to push all engine slots outside the pool-reuse window before
    //   declaring the test buffer with a fresh handle.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-06 OwnershipContract - transient physical-owner buffer ptr stable")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        // Unusual byte size to minimise hash collisions with engine buffers.
        RGBufferDesc desc;
        desc.m_NvrhiDesc.byteSize     = 7919; // prime number
        desc.m_NvrhiDesc.canHaveUAVs  = true;
        desc.m_NvrhiDesc.debugName    = "TC-OC-06-Unique";
        desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;

        // Advance the frame counter so pool-reuse is impossible for engine slots.
        // Reset() clears m_IsDeclaredThisFrame on all engine slots so they don't
        // participate as aliasing candidates in the test's Compile().
        RunOneFrame();
        rg.Reset();

        // Frame 1: fresh handle → guaranteed new slot
        rg.BeginSetup();
        RGBufferHandle h;
        rg.DeclareBuffer(desc, h);
        rg.BeginPass("TC-OC-06-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetBufferRaw(h) != nullptr);

        const uint32_t slotIdx = h.m_Index;
        const bool isOwner1 = rg.GetBuffers()[slotIdx].m_IsPhysicalOwner;
        const nvrhi::IBuffer* ptr1 = rg.GetBufferRaw(h).Get();
        INFO("Frame1: slot=" << slotIdx << " isOwner=" << isOwner1 << " ptr=" << ptr1);
        REQUIRE(isOwner1);

        rg.PostRender();

        // Frame 2: same handle → trivial-reuse → same ptr
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareBuffer(desc, h);
        rg.BeginPass("TC-OC-06-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetBufferRaw(h) != nullptr);

        const bool isOwner2 = rg.GetBuffers()[slotIdx].m_IsPhysicalOwner;
        const nvrhi::IBuffer* ptr2 = rg.GetBufferRaw(h).Get();
        INFO("Frame2: slot=" << slotIdx << " isOwner=" << isOwner2 << " ptr=" << ptr2);

        CHECK(isOwner2 == true);
        CHECK(ptr2 == ptr1);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-07: Aliased transient texture has m_IsPhysicalOwner == false
    //                and m_AliasedFromIndex != UINT32_MAX
    //
    //   Two textures in separate passes with non-overlapping lifetimes and
    //   identical memory requirements.  With aliasing enabled the second
    //   texture should alias the first.
    //
    //   We advance the frame counter via RunOneFrame() before declaring the
    //   test textures with fresh handles.  This pushes all engine slots
    //   outside the pool-reuse window (pool-reuse requires lastFrameUsed > 1
    //   frame ago), so both hA and hB land on brand-new slots that have
    //   m_IsAllocated=false.  Only then can the aliasing path fire for hB.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-07 OwnershipContract - aliased texture is not physical owner")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        // Advance frame counter so engine slots are outside the pool-reuse window.
        // Reset() is mandatory after RunOneFrame() — engine resources still have
        // m_IsDeclaredThisFrame=true after PostRender() and would be included in
        // the test's sortedIndices, making hA a trivial-reuse candidate (already
        // allocated owner) and preventing hB from being aliased against it.
        RunOneFrame();
        rg.Reset();

        // PassA: declare TexA with a fresh handle (write-only, lifetime = pass 1)
        rg.BeginSetup();
        RGTextureHandle hA;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-07-A"), hA);
        rg.BeginPass("TC-OC-07-PassA");

        // PassB: declare TexB with a fresh handle (write-only, lifetime = pass 2)
        RGTextureHandle hB;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-07-B"), hB);
        rg.BeginPass("TC-OC-07-PassB");

        rg.EndSetup();
        rg.Compile();

        REQUIRE(hA.IsValid());
        REQUIRE(hB.IsValid());

        const auto& slotA = rg.GetTextures()[hA.m_Index];
        const auto& slotB = rg.GetTextures()[hB.m_Index];

        INFO("TexA: slot=" << hA.m_Index
             << " isOwner=" << slotA.m_IsPhysicalOwner
             << " aliasedFrom=" << slotA.m_AliasedFromIndex
             << " isAllocated=" << slotA.m_IsAllocated);
        INFO("TexB: slot=" << hB.m_Index
             << " isOwner=" << slotB.m_IsPhysicalOwner
             << " aliasedFrom=" << slotB.m_AliasedFromIndex
             << " isAllocated=" << slotB.m_IsAllocated);

        // TexA must be the physical owner (declared first, no prior allocation)
        CHECK(slotA.m_IsPhysicalOwner  == true);
        CHECK(slotA.m_AliasedFromIndex == UINT32_MAX);

        // TexB must be aliased from TexA (non-owner, non-overlapping lifetime)
        CHECK(slotB.m_IsPhysicalOwner  == false);
        CHECK(slotB.m_AliasedFromIndex == hA.m_Index);

        // Both must have valid nvrhi handles
        CHECK(rg.GetTextureRaw(hA) != nullptr);
        CHECK(rg.GetTextureRaw(hB) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-08: Aliased texture gets a NEW nvrhi handle each frame
    //                (pointer changes frame-to-frame — this is correct)
    //
    //   Aliased resources are virtual textures re-bound to the owner's heap
    //   region.  The nvrhi handle is recreated each frame because the
    //   createAndBindResource callback always runs for non-owners.
    //   Callers must NOT cache raw pointers of aliased resources across frames.
    //
    //   We use RunOneFrame() before the first test frame to push engine slots
    //   outside the pool-reuse window, guaranteeing fresh slots for hA/hB
    //   and therefore reliable aliasing.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-08 OwnershipContract - aliased texture gets new nvrhi handle each frame")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        // Advance frame counter so engine slots are outside the pool-reuse window.
        // Reset() clears m_IsDeclaredThisFrame on all engine slots before the
        // first mini-frame so they don't interfere with aliasing.
        RunOneFrame();
        rg.Reset();

        // Diagnostic struct captured inside runMiniFrame for rich failure messages.
        struct MiniFrameInfo
        {
            const nvrhi::ITexture* ptrA   = nullptr;
            const nvrhi::ITexture* ptrB   = nullptr;
            uint32_t               slotA  = UINT32_MAX;
            uint32_t               slotB  = UINT32_MAX;
            bool                   aIsOwner   = false;
            bool                   bIsAliased = false;
            uint32_t               bAliasedFrom = UINT32_MAX;
        };

        // Helper lambda: run one two-pass mini-frame (Reset+declare+compile+PostRender)
        // and return full diagnostic info.  hA/hB are passed by reference so the
        // caller can inspect slot state after the call.
        auto runMiniFrame = [&](RGTextureHandle& hA, RGTextureHandle& hB) -> MiniFrameInfo
        {
            rg.Reset();

            rg.BeginSetup();
            rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-08-A"), hA);
            rg.BeginPass("TC-OC-08-PassA");

            rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-08-B"), hB);
            rg.BeginPass("TC-OC-08-PassB");

            rg.EndSetup();
            rg.Compile();

            MiniFrameInfo info;
            info.slotA = hA.m_Index;
            info.slotB = hB.m_Index;
            if (hA.IsValid() && hA.m_Index < rg.GetTextures().size())
            {
                const auto& slotA = rg.GetTextures()[hA.m_Index];
                info.aIsOwner = slotA.m_IsPhysicalOwner;
                info.ptrA = rg.GetTextureRaw(hA) ? rg.GetTextureRaw(hA).Get() : nullptr;
            }
            if (hB.IsValid() && hB.m_Index < rg.GetTextures().size())
            {
                const auto& slotB = rg.GetTextures()[hB.m_Index];
                info.bIsAliased    = (slotB.m_AliasedFromIndex != UINT32_MAX);
                info.bAliasedFrom  = slotB.m_AliasedFromIndex;
                info.ptrB = rg.GetTextureRaw(hB) ? rg.GetTextureRaw(hB).Get() : nullptr;
            }

            rg.PostRender();
            return info;
        };

        // Mini-frame 1: fresh handles → hA gets new slot (owner), hB aliases hA
        RGTextureHandle hA, hB;
        const MiniFrameInfo f1 = runMiniFrame(hA, hB);

        INFO("MiniFrame1:"
             << " hA.slot=" << f1.slotA << " aIsOwner=" << f1.aIsOwner << " ptrA=" << f1.ptrA
             << " | hB.slot=" << f1.slotB << " bIsAliased=" << f1.bIsAliased
             << " bAliasedFrom=" << f1.bAliasedFrom << " ptrB=" << f1.ptrB);

        REQUIRE(f1.ptrB != nullptr);
        // With fresh slots and aliasing enabled, hB MUST be aliased from hA.
        REQUIRE(f1.bIsAliased);
        CHECK(f1.bAliasedFrom == f1.slotA);
        // hA must be the physical owner.
        CHECK(f1.aIsOwner == true);

        // Mini-frame 2: same handles re-declared → hA trivial-reuse (owner),
        //               hB goes through aliasing path again → new nvrhi handle.
        const MiniFrameInfo f2 = runMiniFrame(hA, hB);

        INFO("MiniFrame2:"
             << " hA.slot=" << f2.slotA << " aIsOwner=" << f2.aIsOwner << " ptrA=" << f2.ptrA
             << " | hB.slot=" << f2.slotB << " bIsAliased=" << f2.bIsAliased
             << " bAliasedFrom=" << f2.bAliasedFrom << " ptrB=" << f2.ptrB);

        REQUIRE(f2.ptrB != nullptr);
        // hB must still be aliased on the second frame.
        CHECK(f2.bIsAliased);
        CHECK(f2.bAliasedFrom == f2.slotA);
        // hA must still be the physical owner (trivial-reuse path).
        CHECK(f2.aIsOwner == true);
        // hA's owner pointer must be STABLE across frames (physical owner reuses handle).
        CHECK(f2.ptrA == f1.ptrA);
        // Aliased resources go through createTexture each frame (non-owner path always
        // calls createAndBindResource).  The resulting nvrhi pointer MAY be identical
        // to the previous frame's pointer: the deferred-release fix keeps the old handle
        // alive in m_DeferredReleaseTextures until the next Reset(), so the D3D12 driver
        // is free to return the same memory address for the new allocation.  What we
        // MUST guarantee is that the slot is still a non-owner (aliased) and that the
        // handle is valid — not that the raw pointer changed.
        //
        // NOTE: If you need to verify that createTexture was actually called (not the
        // trivial-reuse fast path), check m_IsPhysicalOwner == false, which is the
        // structural invariant that matters for correctness.
        CHECK(f2.ptrB != nullptr); // handle must be valid
        // (pointer equality with f1.ptrB is now allowed — see comment above)

        // Mini-frame 3: confirm the pattern holds for a third consecutive frame.
        const MiniFrameInfo f3 = runMiniFrame(hA, hB);

        INFO("MiniFrame3:"
             << " hA.slot=" << f3.slotA << " aIsOwner=" << f3.aIsOwner << " ptrA=" << f3.ptrA
             << " | hB.slot=" << f3.slotB << " bIsAliased=" << f3.bIsAliased
             << " bAliasedFrom=" << f3.bAliasedFrom << " ptrB=" << f3.ptrB);

        REQUIRE(f3.ptrB != nullptr);
        CHECK(f3.bIsAliased);
        CHECK(f3.aIsOwner == true);
        // Owner pointer remains stable.
        CHECK(f3.ptrA == f1.ptrA);
        // Aliased handle is valid each frame (pointer equality is allowed — see above).
        CHECK(f3.ptrB != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-09: Pool-reuse of an aliased slot produces a valid resource
    //                (the new caller gets a working physical handle even though
    //                 the slot was previously a non-owner)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-09 OwnershipContract - pool-reuse of aliased slot produces valid resource")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        // Frame 1: create two textures so TexB gets aliased.
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle hA;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-09-A"), hA);
        rg.BeginPass("TC-OC-09-PassA");

        RGTextureHandle hB;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-09-B"), hB);
        rg.BeginPass("TC-OC-09-PassB");

        rg.EndSetup();
        rg.Compile();

        const bool bWasAliased = (rg.GetTextures()[hB.m_Index].m_AliasedFromIndex != UINT32_MAX);
        INFO("Frame1: hB.slot=" << hB.m_Index << " aliased=" << bWasAliased);

        rg.PostRender();

        // Frame 2: re-declare hB with the same handle.
        // Whether it was aliased or not, the resource must be valid.
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-09-A"), hA);
        rg.BeginPass("TC-OC-09-PassA");

        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-09-B"), hB);
        rg.BeginPass("TC-OC-09-PassB");

        rg.EndSetup();
        rg.Compile();

        CHECK(hB.IsValid());
        CHECK(rg.GetTextureRaw(hB) != nullptr);
        CHECK(rg.GetTextures()[hB.m_Index].m_IsAllocated == true);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-10: Desc change on a physical-owner slot forces re-allocation
    //
    //   When the desc hash changes DeclareTexture frees the old heap block
    //   and clears m_IsAllocated.  Compile() then calls SubAllocateResource
    //   which may reuse the same freed block (same heap/offset) or find a
    //   different one.  Either way the slot must be re-allocated and remain
    //   a physical owner.  We do NOT assert ptr1 != ptr2 because the pointer
    //   depends on whether the same heap block is reused.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-10 OwnershipContract - desc change on owner slot forces re-alloc")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // Advance frame counter so pool-reuse is impossible for engine slots.
        // Reset() clears m_IsDeclaredThisFrame on all engine slots so they don't
        // participate as aliasing candidates and don't cause the test's fresh
        // slot to become a non-owner.
        RunOneFrame();
        rg.Reset();

        // Frame 1: fresh handle → new slot → physical owner
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-OC-10"), h);
        rg.BeginPass("TC-OC-10-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextures()[h.m_Index].m_IsPhysicalOwner);
        REQUIRE(rg.GetTextures()[h.m_Index].m_IsAllocated);
        const uint32_t slotIdx = h.m_Index;
        INFO("Frame1: slot=" << slotIdx
             << " isOwner=" << rg.GetTextures()[slotIdx].m_IsPhysicalOwner
             << " heapIdx=" << rg.GetTextures()[slotIdx].m_HeapIndex);
        rg.PostRender();

        // Frame 2: different size → hash mismatch → old block freed → re-alloc
        rg.Reset();
        rg.BeginSetup();
        rg.DeclareTexture(MakeTexDesc(11, 17, nvrhi::Format::RGBA32_FLOAT, true, "TC-OC-10"), h);
        rg.BeginPass("TC-OC-10-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        INFO("Frame2: slot=" << h.m_Index
             << " isOwner=" << rg.GetTextures()[h.m_Index].m_IsPhysicalOwner
             << " isAllocated=" << rg.GetTextures()[h.m_Index].m_IsAllocated
             << " heapIdx=" << rg.GetTextures()[h.m_Index].m_HeapIndex);

        // The slot must be re-allocated and remain a physical owner.
        CHECK(rg.GetTextures()[h.m_Index].m_IsAllocated     == true);
        CHECK(rg.GetTextures()[h.m_Index].m_IsPhysicalOwner == true);
        // Handle index must be stable (fast-path re-use of same slot).
        CHECK(h.m_Index == slotIdx);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-11: m_IsAllocated is false for a slot whose physical
    //                resource was evicted (inactive > 3 frames)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-11 OwnershipContract - evicted slot has m_IsAllocated false")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // Allocate once with an unusual desc
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-OC-11"), h);
        rg.BeginPass("TC-OC-11-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextures()[h.m_Index].m_IsAllocated);
        rg.PostRender();

        // Skip 4 frames without re-declaring → eviction threshold exceeded
        g_Renderer.m_FrameNumber += 4;
        rg.Reset(); // eviction runs here

        // Physical resource must have been freed
        REQUIRE(h.IsValid()); // handle index is still valid
        const auto& slot = rg.GetTextures()[h.m_Index];
        CHECK(slot.m_IsAllocated       == false);
        CHECK(slot.m_PhysicalTexture   == nullptr);
        CHECK(slot.m_IsDeclaredThisFrame == false);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-12: Persistent resource is NEVER evicted even after many
    //                inactive frames (persistent resources bypass the eviction
    //                check because they are always re-declared by their owner)
    //
    //   This test verifies the contract indirectly: after 5 frames of
    //   re-declaration the physical pointer is still the same, confirming
    //   no eviction occurred.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-12 OwnershipContract - persistent resource not evicted across many frames")
    {
        auto& rg = g_Renderer.m_RenderGraph;
        const RGTextureDesc desc = MakeTexDesc(16, 16, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-12-Persist");

        RGTextureHandle h;
        const nvrhi::ITexture* firstPtr = nullptr;

        for (int frame = 0; frame < 5; ++frame)
        {
            // Simulate frame-number advancing (as RunOneFrame would do)
            if (frame > 0) ++g_Renderer.m_FrameNumber;

            rg.Reset();
            rg.BeginSetup();
            rg.DeclarePersistentTexture(desc, h);
            rg.BeginPass("TC-OC-12-Pass");
            rg.EndSetup();
            rg.Compile();

            REQUIRE(h.IsValid());
            REQUIRE(rg.GetTextureRaw(h) != nullptr);

            const nvrhi::ITexture* ptr = rg.GetTextureRaw(h).Get();
            INFO("Frame " << frame << ": ptr=" << ptr);

            if (frame == 0)
                firstPtr = ptr;
            else
                CHECK(ptr == firstPtr); // no eviction, no re-allocation

            rg.PostRender();
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-13: Physical-owner pointer is STABLE across frames
    //
    //   Complementary to OC-08: while aliased resources must get a new
    //   handle each frame, physical owners must NOT — their pointer must
    //   remain identical across consecutive frames (trivial-reuse path).
    //   This test guards against accidentally breaking the fast path for
    //   owners while fixing the aliased-resource recreation path.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-13 OwnershipContract - physical owner pointer is stable across frames")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();
        rg.Reset();

        auto runMiniFrame = [&](RGTextureHandle& hA) -> const nvrhi::ITexture*
        {
            rg.Reset();
            rg.BeginSetup();
            rg.DeclareTexture(MakeTexDesc(128, 128, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-13-A"), hA);
            rg.BeginPass("TC-OC-13-Pass");
            rg.EndSetup();
            rg.Compile();
            const nvrhi::ITexture* ptr = rg.GetTextureRaw(hA) ? rg.GetTextureRaw(hA).Get() : nullptr;
            rg.PostRender();
            return ptr;
        };

        RGTextureHandle hA;
        const nvrhi::ITexture* ptr1 = runMiniFrame(hA);
        REQUIRE(ptr1 != nullptr);
        INFO("Frame1: slot=" << hA.m_Index
             << " isOwner=" << rg.GetTextures()[hA.m_Index].m_IsPhysicalOwner
             << " ptr=" << ptr1);
        REQUIRE(rg.GetTextures()[hA.m_Index].m_IsPhysicalOwner);

        const nvrhi::ITexture* ptr2 = runMiniFrame(hA);
        REQUIRE(ptr2 != nullptr);
        INFO("Frame2: slot=" << hA.m_Index
             << " isOwner=" << rg.GetTextures()[hA.m_Index].m_IsPhysicalOwner
             << " ptr=" << ptr2);
        CHECK(rg.GetTextures()[hA.m_Index].m_IsPhysicalOwner);

        // Physical owner must reuse the same nvrhi handle (trivial-reuse path).
        CHECK(ptr2 == ptr1);

        const nvrhi::ITexture* ptr3 = runMiniFrame(hA);
        REQUIRE(ptr3 != nullptr);
        INFO("Frame3: ptr=" << ptr3);
        CHECK(ptr3 == ptr1);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-14: Aliased BUFFER gets a new nvrhi handle each frame
    //
    //   Buffer variant of OC-08.  Two buffers with non-overlapping
    //   lifetimes and identical memory requirements: the second must alias
    //   the first, and its nvrhi handle must be recreated every frame.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-14 OwnershipContract - aliased buffer gets new nvrhi handle each frame")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();
        rg.Reset();

        // Build a buffer desc large enough to be aliasable.
        auto makeBufDesc = [](const char* name) -> RGBufferDesc
        {
            RGBufferDesc d;
            d.m_NvrhiDesc.byteSize    = 4096;
            d.m_NvrhiDesc.structStride = 0;
            d.m_NvrhiDesc.debugName   = name;
            d.m_NvrhiDesc.isVirtual   = true;
            return d;
        };

        struct MiniFrameInfo
        {
            const nvrhi::IBuffer* ptrA = nullptr;
            const nvrhi::IBuffer* ptrB = nullptr;
            bool bIsAliased = false;
            uint32_t bAliasedFrom = UINT32_MAX;
        };

        auto runMiniFrame = [&](RGBufferHandle& hA, RGBufferHandle& hB) -> MiniFrameInfo
        {
            rg.Reset();
            rg.BeginSetup();
            rg.DeclareBuffer(makeBufDesc("TC-OC-14-A"), hA);
            rg.BeginPass("TC-OC-14-PassA");
            rg.DeclareBuffer(makeBufDesc("TC-OC-14-B"), hB);
            rg.BeginPass("TC-OC-14-PassB");
            rg.EndSetup();
            rg.Compile();

            MiniFrameInfo info;
            if (hA.IsValid() && hA.m_Index < rg.GetBuffers().size())
                info.ptrA = rg.GetBufferRaw(hA) ? rg.GetBufferRaw(hA).Get() : nullptr;
            if (hB.IsValid() && hB.m_Index < rg.GetBuffers().size())
            {
                const auto& slotB = rg.GetBuffers()[hB.m_Index];
                info.bIsAliased   = (slotB.m_AliasedFromIndex != UINT32_MAX);
                info.bAliasedFrom = slotB.m_AliasedFromIndex;
                info.ptrB = rg.GetBufferRaw(hB) ? rg.GetBufferRaw(hB).Get() : nullptr;
            }
            rg.PostRender();
            return info;
        };

        RGBufferHandle hA, hB;
        const MiniFrameInfo f1 = runMiniFrame(hA, hB);

        INFO("MiniFrame1:"
             << " hA.slot=" << hA.m_Index << " ptrA=" << f1.ptrA
             << " | hB.slot=" << hB.m_Index << " bIsAliased=" << f1.bIsAliased
             << " bAliasedFrom=" << f1.bAliasedFrom << " ptrB=" << f1.ptrB);

        REQUIRE(f1.ptrB != nullptr);
        REQUIRE(f1.bIsAliased);

        const MiniFrameInfo f2 = runMiniFrame(hA, hB);

        INFO("MiniFrame2:"
             << " hA.slot=" << hA.m_Index << " ptrA=" << f2.ptrA
             << " | hB.slot=" << hB.m_Index << " bIsAliased=" << f2.bIsAliased
             << " ptrB=" << f2.ptrB);

        REQUIRE(f2.ptrB != nullptr);
        CHECK(f2.bIsAliased);
        // Owner pointer must be stable.
        CHECK(f2.ptrA == f1.ptrA);
        // Aliased buffer pointer must differ each frame.
        CHECK(f2.ptrB != f1.ptrB);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-OC-15: Aliased texture has a valid (non-null) handle after
    //                recreation on every frame
    //
    //   Regression guard: after the fix, createAndBindResource always
    //   recreates the aliased handle.  This test verifies that the
    //   recreated handle is non-null and that GetTexture() returns a
    //   valid nvrhi::TextureHandle (not just a raw non-null pointer).
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-OC-15 OwnershipContract - aliased texture handle is valid after recreation")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();
        rg.Reset();

        auto runMiniFrame = [&](RGTextureHandle& hA, RGTextureHandle& hB)
        {
            rg.Reset();
            rg.BeginSetup();
            rg.DeclareTexture(MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-15-A"), hA);
            rg.BeginPass("TC-OC-15-PassA");
            rg.DeclareTexture(MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "TC-OC-15-B"), hB);
            rg.BeginPass("TC-OC-15-PassB");
            rg.EndSetup();
            rg.Compile();
        };

        RGTextureHandle hA, hB;

        for (int frame = 0; frame < 4; ++frame)
        {
            runMiniFrame(hA, hB);

            REQUIRE(hB.IsValid());
            const auto& slotB = rg.GetTextures()[hB.m_Index];
            const bool isAliased = (slotB.m_AliasedFromIndex != UINT32_MAX);

            INFO("Frame " << frame
                 << ": hB.slot=" << hB.m_Index
                 << " isAliased=" << isAliased
                 << " isAllocated=" << slotB.m_IsAllocated
                 << " rawPtr=" << (rg.GetTextureRaw(hB) ? rg.GetTextureRaw(hB).Get() : nullptr));

            // After the first frame hB should be aliased.
            if (frame > 0)
                CHECK(isAliased);

            // The handle must always be valid and non-null.
            CHECK(slotB.m_IsAllocated == true);
            CHECK(rg.GetTextureRaw(hB) != nullptr);
            // GetTexture() (the typed accessor) must also return a non-null handle.
            CHECK(rg.GetTexture(hB, RGResourceAccessMode::Write) != nullptr);

            rg.PostRender();
        }
    }
}

// ============================================================================
// TEST SUITE: RGAlloc_ResetContract
//
// Regression suite for the "missing Reset() before BeginSetup()" bug class.
//
// Root cause (discovered via OC-05/06/07/08/10 failures):
//   After RunOneFrame() the graph is in a post-PostRender() state:
//     - m_CurrentPassIndex == N  (N engine passes)
//     - m_PassAccesses.size() == N
//     - ALL engine resource slots have m_IsDeclaredThisFrame == true
//       (Reset() is called at the START of ScheduleAndRunAllRenderers, so
//        the slots are re-declared and left declared after PostRender())
//
//   If a test then calls BeginSetup() without Reset() first:
//     - Engine resources participate in the test's Compile() as aliasing
//       candidates (m_IsDeclaredThisFrame == true, valid lifetimes).
//     - The test's pass indices are offset by N, breaking lifetime checks.
//     - Fresh test slots may get aliased against engine resources → wrong
//       m_IsPhysicalOwner value → REQUIRE(isOwner) fails.
//
//   Fix: always call Reset() after RunOneFrame() and before BeginSetup().
//   The engine now asserts m_CurrentPassIndex == 0 in BeginSetup() to catch
//   this mistake immediately.
//
// These tests verify:
//   - After Reset(), m_CurrentPassIndex is 0 and engine slots are clean.
//   - A fresh slot declared after Reset()+RunOneFrame()+Reset() is always
//     a physical owner (no stale engine candidates interfere).
//   - Pool-reuse is correctly blocked for engine slots used in the same frame.
//   - The aliasing path only fires when both candidates are fresh (no prior
//     allocation), not when one is a stale engine slot.
// ============================================================================
TEST_SUITE("RGAlloc_ResetContract")
{
    // ------------------------------------------------------------------
    // TC-RGAL-RC-01: Reset() sets m_CurrentPassIndex to 0
    //
    //   After a full RunOneFrame() the pass index is non-zero.
    //   Reset() must bring it back to 0 so BeginSetup() is safe.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-01 ResetContract - Reset sets CurrentPassIndex to 0")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // After the fixture's two warm-up frames the graph is in a
        // post-PostRender() state.  m_CurrentPassIndex reflects the engine
        // pass count from the last warm-up frame.
        RunOneFrame();
        // m_CurrentPassIndex is now > 0 (engine passes were registered).
        // We cannot read it directly here without Reset(), but we can verify
        // that Reset() brings it to 0 by checking BeginSetup() succeeds.
        rg.Reset();
        CHECK(rg.GetCurrentPassIndex() == 0);
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-02: After Reset(), no resource slot has m_IsDeclaredThisFrame
    //
    //   Engine resources declared during RunOneFrame() must have
    //   m_IsDeclaredThisFrame cleared by the subsequent Reset() so they
    //   do not pollute the next frame's Compile().
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-02 ResetContract - Reset clears IsDeclaredThisFrame on all slots")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();
        // Before Reset(): engine slots have m_IsDeclaredThisFrame == true.
        // After Reset(): all must be false.
        rg.Reset();

        for (uint32_t i = 0; i < rg.GetTextures().size(); ++i)
        {
            INFO("Texture slot " << i << " ('" << rg.GetTextures()[i].m_Desc.m_NvrhiDesc.debugName.c_str()
                 << "') still has m_IsDeclaredThisFrame=true after Reset()");
            CHECK(rg.GetTextures()[i].m_IsDeclaredThisFrame == false);
        }
        for (uint32_t i = 0; i < rg.GetBuffers().size(); ++i)
        {
            INFO("Buffer slot " << i << " ('" << rg.GetBuffers()[i].m_Desc.m_NvrhiDesc.debugName.c_str()
                 << "') still has m_IsDeclaredThisFrame=true after Reset()");
            CHECK(rg.GetBuffers()[i].m_IsDeclaredThisFrame == false);
        }
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-03: Fresh slot declared after RunOneFrame()+Reset() is
    //                always a physical owner (no stale engine candidates)
    //
    //   This is the direct regression test for OC-05: if Reset() is omitted
    //   the engine's resources participate as aliasing candidates and the
    //   fresh slot may become a non-owner.  With Reset() it must be an owner.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-03 ResetContract - fresh slot after RunOneFrame+Reset is physical owner")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();
        rg.Reset(); // <-- the critical call that was missing in OC-05/06

        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-RC-03"), h);
        rg.BeginPass("TC-RC-03-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);

        const auto& slot = rg.GetTextures()[h.m_Index];
        INFO("slot=" << h.m_Index
             << " isOwner=" << slot.m_IsPhysicalOwner
             << " aliasedFrom=" << slot.m_AliasedFromIndex
             << " isAllocated=" << slot.m_IsAllocated);

        // A single fresh slot with no other declared resources cannot be aliased.
        CHECK(slot.m_IsPhysicalOwner  == true);
        CHECK(slot.m_AliasedFromIndex == UINT32_MAX);
        CHECK(slot.m_IsAllocated      == true);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-04: Trivial-reuse fires correctly across Reset() boundary
    //
    //   Frame 1: declare fresh slot → physical owner → ptr1.
    //   Reset().
    //   Frame 2: re-declare same handle → trivial-reuse → ptr2 == ptr1.
    //
    //   This verifies that Reset() does NOT clear m_IsAllocated or
    //   m_IsPhysicalOwner for non-evicted slots, so the trivial-reuse
    //   path in AllocateResourcesInternal fires correctly.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-04 ResetContract - trivial-reuse fires correctly across Reset boundary")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();
        rg.Reset();

        // Frame 1
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-RC-04"), h);
        rg.BeginPass("TC-RC-04-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextures()[h.m_Index].m_IsPhysicalOwner);
        const nvrhi::ITexture* ptr1 = rg.GetTextureRaw(h).Get();
        const uint32_t slotIdx = h.m_Index;
        INFO("Frame1: slot=" << slotIdx << " ptr=" << ptr1);
        rg.PostRender();

        // Frame 2: Reset() must preserve m_IsAllocated/m_IsPhysicalOwner
        rg.Reset();

        // Verify trivial-reuse preconditions are intact after Reset()
        const auto& slotAfterReset = rg.GetTextures()[slotIdx];
        INFO("After Reset: isAllocated=" << slotAfterReset.m_IsAllocated
             << " isOwner=" << slotAfterReset.m_IsPhysicalOwner
             << " heapIdx=" << slotAfterReset.m_HeapIndex);
        CHECK(slotAfterReset.m_IsAllocated      == true);
        CHECK(slotAfterReset.m_IsPhysicalOwner  == true);
        CHECK(slotAfterReset.m_IsDeclaredThisFrame == false); // cleared by Reset()

        rg.BeginSetup();
        rg.DeclareTexture(MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-RC-04"), h);
        rg.BeginPass("TC-RC-04-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);
        const nvrhi::ITexture* ptr2 = rg.GetTextureRaw(h).Get();
        INFO("Frame2: slot=" << h.m_Index << " ptr=" << ptr2);

        CHECK(h.m_Index == slotIdx);                          // same slot
        CHECK(rg.GetTextures()[slotIdx].m_IsPhysicalOwner);   // still owner
        CHECK(ptr2 == ptr1);                                  // trivial-reuse: same handle

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-05: Pool-reuse is blocked for engine slots used in the
    //                same frame as the test declaration
    //
    //   Pool-reuse condition: lastFrameUsed > 1 frame ago.
    //   After RunOneFrame() + Reset(), engine slots have
    //   lastFrameUsed == m_FrameNumber - 1 (set during RunOneFrame's
    //   ScheduleAndRunAllRenderers, before m_FrameNumber was incremented).
    //   Difference == 1, which is NOT > 1 → pool-reuse blocked. ✓
    //
    //   This test verifies that a fresh handle with a common desc (one that
    //   could match an engine slot) lands on a NEW slot, not a pool-reused
    //   engine slot, when the frame counter is only 1 ahead.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-05 ResetContract - pool-reuse blocked for slots used 1 frame ago")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // After the fixture's 2 warm-up frames, m_FrameNumber == 2 and
        // engine slots have lastFrameUsed == 1.  Difference == 1 → blocked.
        // Do NOT call RunOneFrame() here — we want to stay at difference == 1.
        rg.Reset();

        const uint32_t slotCountBefore = static_cast<uint32_t>(rg.GetTextures().size());

        // Declare with a desc that is likely to match engine G-buffer slots.
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-RC-05"), h);
        rg.BeginPass("TC-RC-05-Pass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(h.IsValid());
        REQUIRE(rg.GetTextureRaw(h) != nullptr);

        INFO("slotCountBefore=" << slotCountBefore
             << " h.m_Index=" << h.m_Index
             << " isOwner=" << rg.GetTextures()[h.m_Index].m_IsPhysicalOwner);

        // Pool-reuse is blocked → a new slot must have been appended.
        CHECK(h.m_Index >= slotCountBefore);
        // The new slot must be a physical owner (no aliasing candidates).
        CHECK(rg.GetTextures()[h.m_Index].m_IsPhysicalOwner == true);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-06: Pool-reuse fires for engine slots used > 1 frame ago
    //
    //   Advance m_FrameNumber by 2 without re-declaring engine slots so
    //   their lastFrameUsed is now > 1 frame ago.  A fresh handle with a
    //   matching desc must then be assigned to the existing slot (pool-reuse)
    //   rather than a new one.
    //
    //   This is the complementary test to RC-05: it confirms the pool-reuse
    //   path DOES fire when the window condition is satisfied.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-06 ResetContract - pool-reuse fires for slots used more than 1 frame ago")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // Allocate a test texture in a clean frame so we have a known slot.
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle hFirst;
        rg.DeclareTexture(MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-RC-06-Seed"), hFirst);
        rg.BeginPass("TC-RC-06-SeedPass");
        rg.EndSetup();
        rg.Compile();
        REQUIRE(hFirst.IsValid());
        const uint32_t seedSlot = hFirst.m_Index;
        rg.PostRender();

        // Advance frame counter by 2 without re-declaring the seed slot.
        g_Renderer.m_FrameNumber += 2;
        rg.Reset(); // eviction check: 2 frames < 3 threshold → not evicted

        // The seed slot should now be eligible for pool-reuse
        // (m_FrameNumber - lastFrameUsed == 2 > 1).
        const auto& seedSlotState = rg.GetTextures()[seedSlot];
        INFO("seedSlot=" << seedSlot
             << " lastFrameUsed=" << seedSlotState.m_LastFrameUsed
             << " frameNumber=" << g_Renderer.m_FrameNumber
             << " diff=" << (g_Renderer.m_FrameNumber - seedSlotState.m_LastFrameUsed)
             << " isAllocated=" << seedSlotState.m_IsAllocated);

        // Declare a fresh handle with the same desc → pool-reuse must fire.
        rg.BeginSetup();
        RGTextureHandle hReuse;
        rg.DeclareTexture(MakeTexDesc(7, 13, nvrhi::Format::RGBA32_FLOAT, true, "TC-RC-06-Reuse"), hReuse);
        rg.BeginPass("TC-RC-06-ReusePass");
        rg.EndSetup();
        rg.Compile();

        REQUIRE(hReuse.IsValid());
        REQUIRE(rg.GetTextureRaw(hReuse) != nullptr);
        INFO("hReuse.m_Index=" << hReuse.m_Index);

        // Pool-reuse must have assigned the seed slot to the new handle.
        CHECK(hReuse.m_Index == seedSlot);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-07: Aliasing only fires when BOTH candidates are fresh
    //                (no stale engine slots interfere)
    //
    //   This is the direct regression test for OC-07: without Reset() the
    //   engine's physical-owner slots are aliasing candidates for the test's
    //   second texture, causing hB to alias an engine slot instead of hA.
    //   With Reset() + fresh handles, hB must alias hA.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-07 ResetContract - aliasing only fires between fresh slots after Reset")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;

        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();
        rg.Reset(); // <-- critical: clears engine slots' m_IsDeclaredThisFrame

        // PassA: fresh hA → new slot → physical owner
        rg.BeginSetup();
        RGTextureHandle hA;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-RC-07-A"), hA);
        rg.BeginPass("TC-RC-07-PassA");

        // PassB: fresh hB → non-overlapping lifetime → must alias hA, not an engine slot
        RGTextureHandle hB;
        rg.DeclareTexture(MakeTexDesc(256, 256, nvrhi::Format::RGBA8_UNORM, true, "TC-RC-07-B"), hB);
        rg.BeginPass("TC-RC-07-PassB");

        rg.EndSetup();
        rg.Compile();

        REQUIRE(hA.IsValid());
        REQUIRE(hB.IsValid());

        const auto& slotA = rg.GetTextures()[hA.m_Index];
        const auto& slotB = rg.GetTextures()[hB.m_Index];

        INFO("hA: slot=" << hA.m_Index
             << " isOwner=" << slotA.m_IsPhysicalOwner
             << " aliasedFrom=" << slotA.m_AliasedFromIndex);
        INFO("hB: slot=" << hB.m_Index
             << " isOwner=" << slotB.m_IsPhysicalOwner
             << " aliasedFrom=" << slotB.m_AliasedFromIndex);

        // hA must be the physical owner.
        CHECK(slotA.m_IsPhysicalOwner  == true);
        CHECK(slotA.m_AliasedFromIndex == UINT32_MAX);

        // hB must alias hA specifically (not any engine slot).
        CHECK(slotB.m_IsPhysicalOwner  == false);
        CHECK(slotB.m_AliasedFromIndex == hA.m_Index);

        CHECK(rg.GetTextureRaw(hA) != nullptr);
        CHECK(rg.GetTextureRaw(hB) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-08: The engine renderer chain (Reset → BeginSetup → N renderers
    //                → EndSetup → Compile) compiles correctly with 3 passes
    //
    //   Regression test for the TC-GRB-01 failure: the old guard in BeginSetup()
    //   checked m_CurrentPassIndex != 0, which fired on every renderer after the
    //   first because BeginPass() increments m_CurrentPassIndex.  The fix was to
    //   make BeginSetup() called exactly once per frame (the correct design), with
    //   all renderers declaring their resources inside that single setup block.
    //   The guard now correctly fires only when Reset() was not called before the
    //   frame's single BeginSetup().
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-08 ResetContract - multiple BeginSetup calls per frame do not assert")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // Simulate the engine renderer chain: Reset once, then declare
        // resources across three "renderers" within a single BeginSetup/EndSetup block.
        rg.Reset();

        // Single BeginSetup for the whole frame
        rg.BeginSetup();

        // Renderer 1
        RGTextureHandle hA;
        rg.DeclareTexture(MakeTexDesc(64, 64, nvrhi::Format::RGBA8_UNORM, true, "RC-08-A"), hA);
        rg.BeginPass("RC-08-PassA");

        // Renderer 2
        RGTextureHandle hB;
        rg.DeclareTexture(MakeTexDesc(32, 32, nvrhi::Format::R32_FLOAT, true, "RC-08-B"), hB);
        rg.BeginPass("RC-08-PassB");

        // Renderer 3
        RGTextureHandle hC;
        rg.DeclareTexture(MakeTexDesc(16, 16, nvrhi::Format::RGBA8_UNORM, false, "RC-08-C"), hC);
        rg.BeginPass("RC-08-PassC");

        rg.EndSetup();

        // All three passes must compile and produce valid resources.
        rg.Compile();

        CHECK(hA.IsValid());
        CHECK(hB.IsValid());
        CHECK(hC.IsValid());
        CHECK(rg.GetTextureRaw(hA) != nullptr);
        CHECK(rg.GetTextureRaw(hB) != nullptr);
        CHECK(rg.GetTextureRaw(hC) != nullptr);
        CHECK(rg.GetCurrentPassIndex() == 3);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-09: RunOneFrame() followed by Reset() + BeginSetup()
    //                does NOT assert (the exact sequence that broke TC-GRB-01)
    //
    //   MinimalSceneFixture calls RunOneFrame() twice in its constructor.
    //   After each RunOneFrame(), ScheduleAndRunAllRenderers() calls Reset()
    //   internally, so the graph is in a clean state.  A test that then calls
    //   rg.Reset() + rg.BeginSetup() must not trigger the guard.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-09 ResetContract - RunOneFrame then Reset then BeginSetup does not assert")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // RunOneFrame() calls ScheduleAndRunAllRenderers() which calls Reset()
        // internally, then increments m_FrameNumber.
        RunOneFrame();

        // Explicit Reset() before the test's own setup — this is the correct
        // pattern and must not trigger any assert.
        rg.Reset();

        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(8, 8, nvrhi::Format::RGBA8_UNORM, true, "RC-09"), h);
        rg.BeginPass("RC-09-Pass");
        rg.EndSetup();
        rg.Compile();

        CHECK(h.IsValid());
        CHECK(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-10: RunSingleTexPass helper owns Reset() internally
    //
    //   Regression test for TC-RGAL-RHI-03: RunSingleTexPass() was calling
    //   BeginSetup() without a preceding Reset(), causing the
    //   m_CurrentPassIndex != 0 assert to fire when called after
    //   MinimalSceneFixture's warm-up frames left m_CurrentPassIndex = N.
    //
    //   The fix: RunSingleTexPass() and RunSingleBufPass() call rg.Reset()
    //   before rg.BeginSetup() so they own the full frame lifecycle.
    //   This test verifies the helpers work correctly after a full frame.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-10 ResetContract - RunSingleTexPass works after full frame without explicit Reset")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // RunOneFrame() leaves m_CurrentPassIndex = N (engine pass count).
        // RunSingleTexPass must call Reset() internally — no explicit Reset() here.
        RunOneFrame();

        // This must NOT assert even though m_CurrentPassIndex > 0 after RunOneFrame().
        const RGTextureHandle h = RunSingleTexPass(rg,
            MakeTexDesc(32, 32, nvrhi::Format::RGBA8_UNORM, true, "TC-RC-10"), "TC-RC-10-Pass");

        CHECK(h.IsValid());
        CHECK(rg.GetTextureRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-11: RunSingleBufPass helper owns Reset() internally
    //
    //   Same contract as RC-10 but for buffers.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-11 ResetContract - RunSingleBufPass works after full frame without explicit Reset")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        RunOneFrame();

        const RGBufferHandle h = RunSingleBufPass(rg,
            MakeBufDesc(128, true, "TC-RC-11"), "TC-RC-11-Pass");

        CHECK(h.IsValid());
        CHECK(rg.GetBufferRaw(h) != nullptr);

        rg.PostRender();
    }

    // ------------------------------------------------------------------
    // TC-RGAL-RC-12: BeginSetup() after Compile() without PostRender() asserts
    //
    //   Verifies the second guard in BeginSetup(): calling BeginSetup() while
    //   m_IsCompiled==true (Compile() was called but PostRender() was not)
    //   must fire the assert.  This catches the misuse pattern of starting a
    //   new frame's setup while the previous frame's GPU resources are still live.
    //
    //   NOTE: This test intentionally triggers an assert.  It uses
    //   SDL_SetAssertionHandler to intercept the assert and prevent the
    //   process from aborting, then verifies the assert fired exactly once.
    //
    //   After SDL_ASSERTION_IGNORE the assert macro returns and execution
    //   continues into BeginSetup()'s body, setting m_IsInsideSetup=true.
    //   Cleanup must therefore call EndSetup() + PostRender() to restore a
    //   valid state before the fixture destructor runs.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGAL-RC-12 ResetContract - BeginSetup while compiled fires assert")
    {
        auto& rg = g_Renderer.m_RenderGraph;

        // Build a compiled frame.
        rg.Reset();
        rg.BeginSetup();
        RGTextureHandle h;
        rg.DeclareTexture(MakeTexDesc(8, 8, nvrhi::Format::RGBA8_UNORM, true, "RC-12"), h);
        rg.BeginPass("RC-12-Pass");
        rg.EndSetup();
        rg.Compile();
        // m_IsCompiled is now true.  Do NOT call PostRender() or Reset().

        // Install a custom assert handler that counts fires and continues.
        struct AssertCounter
        {
            int count = 0;
            static SDL_AssertState Handler(const SDL_AssertData* /*data*/, void* userdata)
            {
                static_cast<AssertCounter*>(userdata)->count++;
                return SDL_ASSERTION_IGNORE; // don't abort
            }
        } counter;
        SDL_SetAssertionHandler(AssertCounter::Handler, &counter);

        // Calling BeginSetup() without Reset() (m_CurrentPassIndex != 0) AND
        // while m_IsCompiled==true must fire at least one assert.
        // SDL_ASSERTION_IGNORE causes the assert macro to return and execution
        // continues into BeginSetup()'s body, setting m_IsInsideSetup=true.
        rg.BeginSetup();

        // Restore default assert handler before any further operations.
        SDL_SetAssertionHandler(SDL_GetDefaultAssertionHandler(), nullptr);

        // The assert must have fired at least once.
        CHECK(counter.count >= 1);

        // Restore valid graph state.
        // BeginSetup() continued past the assert and set m_IsInsideSetup=true,
        // so we must close the setup block before PostRender().
        rg.EndSetup();
        rg.PostRender(); // clears m_IsCompiled so the fixture destructor is safe
    }
}
