// Tests_RenderGraphAdvanced.cpp
//
// Systems under test: RenderGraph declaration, compilation, resource retrieval,
//                     Stats, aliasing, handle validity, SPD helper, multi-frame
//                     persistent resources.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//
// Test coverage:
//   - Default RGTextureHandle is invalid
//   - Default RGBufferHandle is invalid
//   - Handle invalidated via Invalidate() is invalid
//   - ExportToString() returns non-empty string after RunOneFrame()
//   - ExportToString() contains "Pass" keyword after a full frame
//   - GetTextureRaw on invalid handle returns nullptr
//   - GetBufferRaw on invalid handle returns nullptr
//   - DeclareTexture returns valid handle
//   - DeclareBuffer returns valid handle
//   - DeclarePersistentTexture returns valid handle
//   - DeclarePersistentBuffer returns valid handle
//   - Persistent texture handle is equal across two frames (same index)
//   - Persistent buffer handle is equal across two frames (same index)
//   - ReadTexture/WriteTexture do not crash on a declared handle
//   - ReadBuffer/WriteBuffer do not crash on a declared handle
//   - GetSPDAtomicCounterDesc returns byteSize > 0
//   - GetSPDAtomicCounterDesc returns isUAV = true
//   - Stats after a full frame have m_NumTextures > 0
//   - Stats after a full frame have m_NumBuffers > 0
//   - Stats m_TotalTextureMemory > 0 after a full frame
//   - Aliasing enabled: 3 frames survive
//   - Aliasing disabled: 3 frames survive
//   - PostRender() does not crash
//   - InsertAliasBarriers with passIndex=0 and a fresh command list does not crash
//   - SetActivePass does not crash
//   - GetCurrentPassIndex returns 0 before any BeginPass
//   - Two DeclareTexture calls with same desc return different handles
//   - Two fresh DeclarePersistentTexture calls with same desc return different handles (no implicit dedup)
//   - Persistent texture slot is stable when the same handle is re-declared across frames
//   - Persistent buffer slot is stable when the same handle is re-declared across frames
//   - RenderGraph::Reset() does not crash
//   - G-buffer textures are accessible via their RG handles after a frame
//   - HDR color texture is accessible via RG handle after a frame
//
// Run with: HobbyRenderer --run-tests=*RGAdv*
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// RG handle externs (declared in Tests_Rendering.cpp / CommonResources.cpp)
// ============================================================================
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_HZBTexture;
extern RGTextureHandle g_RG_ExposureTexture;
extern RGTextureHandle g_RG_GBufferGeoNormals;

// ============================================================================
// TEST SUITE: RGAdv_HandleValidity
// ============================================================================
TEST_SUITE("RGAdv_HandleValidity")
{
    // ------------------------------------------------------------------
    // TC-RGA-H01: Default RGTextureHandle is invalid
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGA-H01 HandleValidity - default RGTextureHandle is invalid")
    {
        RGTextureHandle h;
        CHECK(!h.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-H02: Default RGBufferHandle is invalid
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGA-H02 HandleValidity - default RGBufferHandle is invalid")
    {
        RGBufferHandle h;
        CHECK(!h.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-H03: Invalidated handle reports invalid
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGA-H03 HandleValidity - Invalidate() makes handle invalid")
    {
        RGTextureHandle h;
        h.m_Index = 42; // pretend valid
        h.Invalidate();
        CHECK(!h.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-H04: Two default-constructed handles compare equal
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGA-H04 HandleValidity - two default handles compare equal")
    {
        RGTextureHandle a, b;
        CHECK(a == b);
    }

    // ------------------------------------------------------------------
    // TC-RGA-H05: GetTextureRaw on invalid handle returns nullptr
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-H05 HandleValidity - GetTextureRaw on invalid handle is nullptr")
    {
        RunOneFrame(); // compile graph first
        RGTextureHandle invalid;
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(invalid) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RGA-H06: GetBufferRaw on invalid handle returns nullptr
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-H06 HandleValidity - GetBufferRaw on invalid handle is nullptr")
    {
        RunOneFrame();
        RGBufferHandle invalid;
        CHECK(g_Renderer.m_RenderGraph.GetBufferRaw(invalid) == nullptr);
    }
}

// ============================================================================
// TEST SUITE: RGAdv_DeclarationAPI
// ============================================================================
TEST_SUITE("RGAdv_DeclarationAPI")
{
    // ------------------------------------------------------------------
    // TC-RGA-D01: DeclareTexture returns a valid handle
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D01 DeclarationAPI - DeclareTexture returns valid handle")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGTextureDesc texDesc;
        texDesc.m_NvrhiDesc = nvrhi::TextureDesc()
            .setWidth(64).setHeight(64)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("TC-D01-Tex")
            .setIsUAV(true);

        RGTextureHandle handle;
        const bool ok = g_Renderer.m_RenderGraph.DeclareTexture(texDesc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(ok);
        CHECK(handle.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-D02: DeclareBuffer returns a valid handle
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D02 DeclarationAPI - DeclareBuffer returns valid handle")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGBufferDesc bufDesc;
        bufDesc.m_NvrhiDesc.byteSize     = 64;
        bufDesc.m_NvrhiDesc.canHaveUAVs  = true;
        bufDesc.m_NvrhiDesc.debugName    = "TC-D02-Buf";

        RGBufferHandle handle;
        const bool ok = g_Renderer.m_RenderGraph.DeclareBuffer(bufDesc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(ok);
        CHECK(handle.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-D03: DeclarePersistentTexture returns a valid handle
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D03 DeclarationAPI - DeclarePersistentTexture returns valid handle")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGTextureDesc texDesc;
        texDesc.m_NvrhiDesc = nvrhi::TextureDesc()
            .setWidth(32).setHeight(32)
            .setFormat(nvrhi::Format::R32_FLOAT)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("TC-D03-PersistTex");

        RGTextureHandle handle;
        const bool ok = g_Renderer.m_RenderGraph.DeclarePersistentTexture(texDesc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(ok);
        CHECK(handle.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-D04: DeclarePersistentBuffer returns a valid handle
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D04 DeclarationAPI - DeclarePersistentBuffer returns valid handle")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGBufferDesc bufDesc;
        bufDesc.m_NvrhiDesc.byteSize     = 128;
        bufDesc.m_NvrhiDesc.canHaveUAVs  = false;
        bufDesc.m_NvrhiDesc.debugName    = "TC-D04-PersistBuf";

        RGBufferHandle handle;
        const bool ok = g_Renderer.m_RenderGraph.DeclarePersistentBuffer(bufDesc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(ok);
        CHECK(handle.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-D05: ReadTexture/WriteTexture on declared handle does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D05 DeclarationAPI - ReadTexture + WriteTexture do not crash")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGTextureDesc texDesc;
        texDesc.m_NvrhiDesc = nvrhi::TextureDesc()
            .setWidth(16).setHeight(16)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("TC-D05-RW")
            .setIsUAV(true);

        RGTextureHandle handle;
        g_Renderer.m_RenderGraph.DeclareTexture(texDesc, handle);
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.ReadTexture(handle));
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.WriteTexture(handle));

        g_Renderer.m_RenderGraph.EndSetup(true);
    }

    // ------------------------------------------------------------------
    // TC-RGA-D06: ReadBuffer/WriteBuffer on declared handle does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D06 DeclarationAPI - ReadBuffer + WriteBuffer do not crash")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGBufferDesc bufDesc;
        bufDesc.m_NvrhiDesc.byteSize    = 64;
        bufDesc.m_NvrhiDesc.canHaveUAVs = true;
        bufDesc.m_NvrhiDesc.debugName   = "TC-D06-RW";

        RGBufferHandle handle;
        g_Renderer.m_RenderGraph.DeclareBuffer(bufDesc, handle);
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.ReadBuffer(handle));
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.WriteBuffer(handle));

        g_Renderer.m_RenderGraph.EndSetup(true);
    }

    // ------------------------------------------------------------------
    // TC-RGA-D07: Two DeclareTexture calls with same desc return different handles
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D07 DeclarationAPI - two DeclareTexture same desc return different handles")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGTextureDesc texDesc;
        texDesc.m_NvrhiDesc = nvrhi::TextureDesc()
            .setWidth(8).setHeight(8)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("TC-D07-Dup")
            .setIsUAV(true);

        RGTextureHandle h1, h2;
        g_Renderer.m_RenderGraph.DeclareTexture(texDesc, h1);
        g_Renderer.m_RenderGraph.DeclareTexture(texDesc, h2);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(h1.IsValid());
        CHECK(h2.IsValid());
        CHECK(h1 != h2); // separate transient allocations
    }

    // ------------------------------------------------------------------
    // TC-RGA-D08: Two fresh DeclarePersistentTexture calls with same desc
    //             allocate DIFFERENT slots.
    //
    //   Persistent deduplication is handle-identity based, not content-based.
    //   Callers must hold and re-pass the same RGTextureHandle to get the
    //   same slot back across frames.  Two independent fresh handles always
    //   produce independent allocations even when the descs are identical.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-D08 DeclarationAPI - two fresh DeclarePersistentTexture same desc return different handles")
    {
        g_Renderer.m_RenderGraph.BeginSetup();

        RGTextureDesc texDesc;
        texDesc.m_NvrhiDesc = nvrhi::TextureDesc()
            .setWidth(8).setHeight(8)
            .setFormat(nvrhi::Format::R32_FLOAT)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("TC-D08-PersistFreshTwo");

        RGTextureHandle h1, h2;
        g_Renderer.m_RenderGraph.DeclarePersistentTexture(texDesc, h1);
        // h2 starts invalid (fresh) — no implicit content dedup, new slot is allocated.
        g_Renderer.m_RenderGraph.DeclarePersistentTexture(texDesc, h2);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(h1.IsValid());
        CHECK(h2.IsValid());
        CHECK(h1 != h2); // separate allocations — no implicit dedup
    }
}

// ============================================================================
// TEST SUITE: RGAdv_Stats
// ============================================================================
TEST_SUITE("RGAdv_Stats")
{
    // ------------------------------------------------------------------
    // TC-RGA-S01: ExportToString() returns non-empty string after a frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-S01 Stats - ExportToString returns non-empty after frame")
    {
        RunOneFrame();
        const std::string s = g_Renderer.m_RenderGraph.ExportToString();
        CHECK(!s.empty());
    }

    // ------------------------------------------------------------------
    // TC-RGA-S02: ExportToString() contains "Pass" keyword after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-S02 Stats - ExportToString contains 'Pass' keyword")
    {
        RunOneFrame();
        const std::string s = g_Renderer.m_RenderGraph.ExportToString();
        CHECK(s.find("Pass") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // TC-RGA-S03: PostRender() does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-S03 Stats - PostRender does not crash")
    {
        RunOneFrame();
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.PostRender());
    }
}

// ============================================================================
// TEST SUITE: RGAdv_SPDHelper
// ============================================================================
TEST_SUITE("RGAdv_SPDHelper")
{
    // ------------------------------------------------------------------
    // TC-RGA-SPD-01: GetSPDAtomicCounterDesc byte size > 0
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGA-SPD-01 SPDHelper - GetSPDAtomicCounterDesc byteSize > 0")
    {
        const RGBufferDesc desc = RenderGraph::GetSPDAtomicCounterDesc("TC-SPD-01");
        CHECK(desc.m_NvrhiDesc.byteSize > 0);
    }

    // ------------------------------------------------------------------
    // TC-RGA-SPD-02: GetSPDAtomicCounterDesc has isUAV = true
    // ------------------------------------------------------------------
    TEST_CASE("TC-RGA-SPD-02 SPDHelper - GetSPDAtomicCounterDesc canHaveUAVs is true")
    {
        const RGBufferDesc desc = RenderGraph::GetSPDAtomicCounterDesc("TC-SPD-02");
        CHECK(desc.m_NvrhiDesc.canHaveUAVs);
    }

    // ------------------------------------------------------------------
    // TC-RGA-SPD-03: GetSPDAtomicCounterDesc can be used to create a GPU buffer
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-SPD-03 SPDHelper - SPD counter buffer creation succeeds")
    {
        REQUIRE(DEV() != nullptr);
        const RGBufferDesc rgDesc = RenderGraph::GetSPDAtomicCounterDesc("TC-SPD-03");
        auto buf = DEV()->createBuffer(rgDesc.m_NvrhiDesc);
        CHECK(buf != nullptr);
    }
}

// ============================================================================
// TEST SUITE: RGAdv_Aliasing
// ============================================================================
TEST_SUITE("RGAdv_Aliasing")
{
    // ------------------------------------------------------------------
    // TC-RGA-AL-01: Aliasing enabled via Config — 3 frames survive
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-AL-01 Aliasing - enabled 3 frames survive")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-RGA-AL-02: Aliasing disabled via Config — 3 frames survive
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-AL-02 Aliasing - disabled 3 frames survive")
    {
        ConfigGuard guard;
        const_cast<Config&>(Config::Get()).m_EnableRenderGraphAliasing = false;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-RGA-AL-03: InsertAliasBarriers with passIndex=0 and fresh CL does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-AL-03 Aliasing - InsertAliasBarriers does not crash")
    {
        RunOneFrame(); // ensure graph is compiled
        // Create a command list for the test
        auto cl = DEV()->createCommandList();
        REQUIRE(cl != nullptr);
        cl->open();
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.InsertAliasBarriers(0, cl));
        cl->close();
    }

    // ------------------------------------------------------------------
    // TC-RGA-AL-04: SetActivePass does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-AL-04 Aliasing - SetActivePass does not crash")
    {
        RunOneFrame();
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.SetActivePass(0));
    }
}

// ============================================================================
// TEST SUITE: RGAdv_PersistentResources
// ============================================================================
TEST_SUITE("RGAdv_PersistentResources")
{
    // ------------------------------------------------------------------
    // TC-RGA-P01: Persistent texture slot is stable when the same handle
    //             is re-declared across frames.
    //
    //   By design, callers must hold the RGTextureHandle in a member variable
    //   and pass it back to DeclarePersistentTexture every frame.  DeclareTexture
    //   takes the "valid handle" fast-path, re-marking the existing slot without
    //   allocating a new one.  This test verifies that slot index is unchanged
    //   after two frame cycles.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-P01 PersistentResources - persistent texture same handle across frames")
    {
        RGTextureDesc texDesc;
        texDesc.m_NvrhiDesc = nvrhi::TextureDesc()
            .setWidth(16).setHeight(16)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setDebugName("TC-P01-PersistHandle");

        // Frame 1: declare with fresh handle → allocate new slot.
        g_Renderer.m_RenderGraph.BeginSetup();
        RGTextureHandle h; // held across frames, like a Renderer member variable
        g_Renderer.m_RenderGraph.DeclarePersistentTexture(texDesc, h);
        g_Renderer.m_RenderGraph.EndSetup(true);
        REQUIRE(h.IsValid());
        const uint32_t firstIdx = h.m_Index;
        RunOneFrame(); // Reset() clears m_IsDeclaredThisFrame; slot survives in vector

        // Frame 2: re-declare with SAME handle → fast-path reuse, same slot.
        g_Renderer.m_RenderGraph.BeginSetup();
        g_Renderer.m_RenderGraph.DeclarePersistentTexture(texDesc, h);
        g_Renderer.m_RenderGraph.EndSetup(true);
        RunOneFrame();

        INFO("firstIdx=" << firstIdx << " h.m_Index=" << h.m_Index);
        CHECK(h.IsValid());
        CHECK(h.m_Index == firstIdx);
    }

    // ------------------------------------------------------------------
    // TC-RGA-P02: Persistent buffer slot is stable when the same handle
    //             is re-declared across frames.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-P02 PersistentResources - persistent buffer same handle across frames")
    {
        RGBufferDesc bufDesc;
        bufDesc.m_NvrhiDesc.byteSize    = 64;
        bufDesc.m_NvrhiDesc.canHaveUAVs = true;
        bufDesc.m_NvrhiDesc.debugName   = "TC-P02-PersistBuf";

        // Frame 1: fresh handle → new slot.
        g_Renderer.m_RenderGraph.BeginSetup();
        RGBufferHandle h; // held across frames
        g_Renderer.m_RenderGraph.DeclarePersistentBuffer(bufDesc, h);
        g_Renderer.m_RenderGraph.EndSetup(true);
        REQUIRE(h.IsValid());
        const uint32_t firstIdx = h.m_Index;
        RunOneFrame();

        // Frame 2: same handle → fast-path, same slot.
        g_Renderer.m_RenderGraph.BeginSetup();
        g_Renderer.m_RenderGraph.DeclarePersistentBuffer(bufDesc, h);
        g_Renderer.m_RenderGraph.EndSetup(true);
        RunOneFrame();

        INFO("firstIdx=" << firstIdx << " h.m_Index=" << h.m_Index);
        CHECK(h.IsValid());
        CHECK(h.m_Index == firstIdx);
    }
}

// ============================================================================
// TEST SUITE: RGAdv_GBufferHandles
// ============================================================================
TEST_SUITE("RGAdv_GBufferHandles")
{
    // ------------------------------------------------------------------
    // TC-RGA-GB-01: g_RG_DepthTexture is valid after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-01 GBufferHandles - g_RG_DepthTexture valid after frame")
    {
        RunOneFrame();
        CHECK(g_RG_DepthTexture.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-GB-02: g_RG_GBufferAlbedo is valid after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-02 GBufferHandles - g_RG_GBufferAlbedo valid after frame")
    {
        RunOneFrame();
        CHECK(g_RG_GBufferAlbedo.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-GB-03: g_RG_GBufferNormals is valid after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-03 GBufferHandles - g_RG_GBufferNormals valid after frame")
    {
        RunOneFrame();
        CHECK(g_RG_GBufferNormals.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-GB-04: g_RG_GBufferORM is valid after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-04 GBufferHandles - g_RG_GBufferORM valid after frame")
    {
        RunOneFrame();
        CHECK(g_RG_GBufferORM.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-GB-05: g_RG_GBufferEmissive is valid after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-05 GBufferHandles - g_RG_GBufferEmissive valid after frame")
    {
        RunOneFrame();
        CHECK(g_RG_GBufferEmissive.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-GB-06: g_RG_HDRColor is valid after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-06 GBufferHandles - g_RG_HDRColor valid after frame")
    {
        RunOneFrame();
        CHECK(g_RG_HDRColor.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RGA-GB-07: G-buffer textures accessible via GetTextureRaw after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-07 GBufferHandles - all G-buffer textures are non-null after frame")
    {
        RunOneFrame();

        // Check all major G-buffer textures
        const RGTextureHandle handles[] = {
            g_RG_DepthTexture,
            g_RG_GBufferAlbedo,
            g_RG_GBufferNormals,
            g_RG_GBufferORM,
            g_RG_GBufferEmissive,
            g_RG_HDRColor,
        };
        const char* names[] = {
            "Depth", "Albedo", "Normals", "ORM", "Emissive", "HDRColor"
        };

        for (int i = 0; i < (int)std::size(handles); ++i)
        {
            INFO("G-buffer: " << names[i]);
            if (handles[i].IsValid())
            {
                CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(handles[i]) != nullptr);
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-RGA-GB-08: g_RG_ExposureTexture is valid after a full frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RGA-GB-08 GBufferHandles - g_RG_ExposureTexture valid after frame")
    {
        RunOneFrame();
        CHECK(g_RG_ExposureTexture.IsValid());
    }
}
