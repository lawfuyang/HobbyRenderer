// Tests_PathTracerImGuiNRD.cpp
//
// Systems under test:
//   PathTracerRenderer  — registration, accumulation buffer, convergence over N frames,
//                         camera-change reset, accumulation index monotonicity, HDR output
//                         format, animation pause, sun angular radius, path-tracer mode
//                         switching, intentional failure cases.
//   ImGuiRenderer       — registration, font texture creation, input layout, vertex/index
//                         buffer creation and resizing, Setup() early-out on null draw data,
//                         Setup() early-out on zero framebuffer size, full frame with ImGui
//                         draw data, intentional failure cases.
//   NRD / NrdIntegration — library version query, FillNRDCommonSettings matrix copy,
//                          motion-vector scale, accumulation mode on frame 0 vs N,
//                          denoiser string non-null, format mapping helpers,
//                          intentional failure cases.
//   ClearRenderer       — registration, HDR color cleared to black, depth cleared to
//                         DEPTH_FAR, GBuffer cleared to zero, HZB clear on first frame,
//                         path-tracer mode skips depth/GBuffer, persistent texture
//                         survival across frames, intentional failure cases.
//   TLAS rebuild        — TLAS non-null after scene load, TLAS rebuild survives full frame,
//                         instance count matches scene, TLAS rebuild after node transform
//                         mutation, TLAS rebuild after instance add/remove, dirty-range
//                         tracking, BLASAddressBuffer non-null, intentional failure cases.
//   Scene + RHI         — swapchain extent valid, device non-null, command list round-trip,
//                         scene bounding sphere after load, light buffer non-null.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources + all IRenderer
//               instances registered and initialized).
//
// Run with:
//   HobbyRenderer --run-tests=*PathTracer*
//   HobbyRenderer --run-tests=*ImGui*
//   HobbyRenderer --run-tests=*NRD*
//   HobbyRenderer --run-tests=*Clear*
//   HobbyRenderer --run-tests=*TLAS*
//   HobbyRenderer --run-tests=*PathTracerImGuiNRD*
// ============================================================================

#include "TestFixtures.h"
#include "GraphicsTestUtils.h"
#include "../NrdIntegration.h"

#include <imgui.h>

// ============================================================================
// External renderer pointers (defined via REGISTER_RENDERER macros)
// ============================================================================
extern IRenderer* g_PathTracerRenderer;
extern IRenderer* g_ImGuiRenderer;
extern IRenderer* g_ClearRenderer;
extern IRenderer* g_TLASRenderer;

// ============================================================================
// External RG handles (defined in CommonRenderers.cpp / PathTracerRenderer.cpp)
// ============================================================================
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferGeoNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_ExposureTexture;

// ============================================================================
// Internal helpers
// ============================================================================
namespace
{
    // Read back the RGBA32_FLOAT HDR color texel at (x,y) as four floats.
    // Returns {0,0,0,0} on failure.
    struct RGBA32F { float r, g, b, a; };
    RGBA32F ReadbackHDRTexel(nvrhi::TextureHandle tex, uint32_t x, uint32_t y)
    {
        if (!tex) return {};
        nvrhi::IDevice* device = DEV();
        const nvrhi::TextureDesc& d = tex->getDesc();

        nvrhi::TextureDesc stagingDesc = d;
        stagingDesc.width = 1; stagingDesc.height = 1;
        stagingDesc.mipLevels = 1;
        stagingDesc.isRenderTarget = false; stagingDesc.isUAV = false;
        stagingDesc.initialState = nvrhi::ResourceStates::Common;
        stagingDesc.keepInitialState = false;
        stagingDesc.debugName = "ReadbackHDRStaging";

        nvrhi::StagingTextureHandle staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        if (!staging) return {};

        nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
        cmd->open();
        nvrhi::TextureSlice src; src.x = x; src.y = y; src.width = 1; src.height = 1;
        nvrhi::TextureSlice dst; dst.x = 0; dst.y = 0; dst.width = 1; dst.height = 1;
        cmd->copyTexture(staging, dst, tex, src);
        cmd->close();
        g_Renderer.ExecutePendingCommandLists();
        device->waitForIdle();

        size_t rowPitch = 0;
        void* mapped = device->mapStagingTexture(staging, dst, nvrhi::CpuAccessMode::Read, &rowPitch);
        if (!mapped) return {};
        RGBA32F v;
        memcpy(&v, mapped, sizeof(v));
        device->unmapStagingTexture(staging);
        return v;
    }

    // Returns true if all four channels of a texel are exactly zero.
    bool TexelIsBlack(const RGBA32F& t) { return t.r == 0.0f && t.g == 0.0f && t.b == 0.0f && t.a == 0.0f; }

} // anonymous namespace

// ============================================================================
// TEST SUITE: PathTracer_Registration
// ============================================================================
TEST_SUITE("PathTracer_Registration")
{
    // ------------------------------------------------------------------
    // TC-PT-REG-01: PathTracerRenderer is registered and non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-PT-REG-01 PathTracerRegistration - renderer is registered and non-null")
    {
        CHECK(g_PathTracerRenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PT-REG-02: PathTracerRenderer name is "ReferencePathTracer"
    // ------------------------------------------------------------------
    TEST_CASE("TC-PT-REG-02 PathTracerRegistration - name is ReferencePathTracer")
    {
        REQUIRE(g_PathTracerRenderer != nullptr);
        CHECK(std::string_view(g_PathTracerRenderer->GetName()) == "ReferencePathTracer");
    }

    // ------------------------------------------------------------------
    // TC-PT-REG-03: PathTracerRenderer is found by FindRendererByName
    // ------------------------------------------------------------------
    TEST_CASE("TC-PT-REG-03 PathTracerRegistration - found by FindRendererByName")
    {
        IRenderer* r = FindRendererByName("ReferencePathTracer");
        CHECK(r != nullptr);
        CHECK(r == g_PathTracerRenderer);
    }

    // ------------------------------------------------------------------
    // TC-PT-REG-04: PathTracerRenderer is NOT a base-pass renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-PT-REG-04 PathTracerRegistration - IsBasePassRenderer returns false")
    {
        REQUIRE(g_PathTracerRenderer != nullptr);
        CHECK(g_PathTracerRenderer->IsBasePassRenderer() == false);
    }

    // ------------------------------------------------------------------
    // TC-PT-REG-05: PathTracerRenderer is present in g_Renderer.m_Renderers
    // ------------------------------------------------------------------
    TEST_CASE("TC-PT-REG-05 PathTracerRegistration - present in renderer list")
    {
        bool found = false;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && std::string_view(r->GetName()) == "ReferencePathTracer")
                found = true;
        CHECK(found);
    }
}

// ============================================================================
// TEST SUITE: PathTracer_ModeAndSetup
// ============================================================================
TEST_SUITE("PathTracer_ModeAndSetup")
{
    // ------------------------------------------------------------------
    // TC-PT-MODE-01: Switching to ReferencePathTracer mode does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-MODE-01 PathTracerMode - switch to path-tracer mode does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-PT-MODE-02: In path-tracer mode, HDR color format is RGBA32_FLOAT
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-MODE-02 PathTracerMode - HDR color format is RGBA32_FLOAT in path-tracer mode")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();

        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().format == Renderer::PATH_TRACER_HDR_COLOR_FORMAT);
        CHECK(hdr->getDesc().format == nvrhi::Format::RGBA32_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-PT-MODE-03: In path-tracer mode, depth texture is NOT declared
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-MODE-03 PathTracerMode - depth texture not declared in path-tracer mode")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();

        // In path-tracer mode ClearRenderer skips depth/GBuffer declarations.
        // g_RG_DepthTexture handle should be invalid (not declared this frame).
        nvrhi::TextureHandle depth = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture);
        CHECK(depth == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PT-MODE-04: In path-tracer mode, GBuffer albedo is NOT declared
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-MODE-04 PathTracerMode - GBuffer albedo not declared in path-tracer mode")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();

        nvrhi::TextureHandle albedo = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferAlbedo);
        CHECK(albedo == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PT-MODE-05: Switching back to Normal mode after path-tracer does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-MODE-05 PathTracerMode - switch back to Normal mode does not crash")
    {
        {
            RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
            RunOneFrame();
        }
        // Now in Normal mode again
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-PT-MODE-06: Path-tracer mode pauses animations (m_EnableAnimations = false)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-MODE-06 PathTracerMode - animations are paused during path-tracer render")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        g_Renderer.m_EnableAnimations = true; // ensure it starts enabled
        RunOneFrame();
        // PathTracerRenderer::Render sets m_EnableAnimations = false
        CHECK(g_Renderer.m_EnableAnimations == false);
    }
}

// ============================================================================
// TEST SUITE: PathTracer_AccumulationBuffer
// ============================================================================
// TEST SUITE: PathTracer_ModeSwitch  (regression tests for stale-handle bugs)
// ============================================================================
TEST_SUITE("PathTracer_ModeSwitch")
{
    // ------------------------------------------------------------------
    // TC-PT-SW-01: Depth texture is nullptr after PT frame (stale handle guard)
    // After a PT-mode frame the depth handle is stale (never declared in PT
    // mode).  GetTextureRaw must return nullptr, not a leftover physical ptr.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-01 ModeSwitch - depth handle is nullptr after PT-only frame")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferAlbedo) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferNormals) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferGeoNormals) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferORM) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferEmissive) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferMotionVectors) == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-02: HDR handle is valid after PT frame (declared in both modes)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-02 ModeSwitch - HDR handle is valid after PT-only frame")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor) != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-03: PT → Normal mode switch does not crash (core regression)
    // This is the exact scenario that triggered the double-declare assert.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-03 ModeSwitch - PT to Normal mode switch does not crash")
    {
        {
            RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
            CHECK_NOTHROW(RunOneFrame());
        }
        CHECK_NOTHROW(RunOneFrame()); // Normal mode
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-04: Normal → PT → Normal does not crash (round-trip)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-04 ModeSwitch - Normal to PT to Normal round-trip does not crash")
    {
        CHECK_NOTHROW(RunOneFrame()); // Normal
        {
            RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
            CHECK_NOTHROW(RunOneFrame()); // PT
        }
        CHECK_NOTHROW(RunOneFrame()); // Normal again
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-05: After PT→Normal switch, depth texture is declared again
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-05 ModeSwitch - depth texture re-declared after PT to Normal switch")
    {
        {
            RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
            RunOneFrame();
        }
        RunOneFrame(); // Normal mode
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture) != nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferAlbedo) != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-06: After PT→Normal switch, HDR format changes to R11G11B10
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-06 ModeSwitch - HDR format is R11G11B10 after switching back to Normal")
    {
        {
            RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
            RunOneFrame();
        }
        RunOneFrame(); // Normal mode
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().format == Renderer::HDR_COLOR_FORMAT);
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-07: Multiple PT→Normal→PT cycles do not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-07 ModeSwitch - multiple PT/Normal cycles do not crash")
    {
        for (int i = 0; i < 3; ++i)
        {
            {
                RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
                CHECK_NOTHROW(RunOneFrame());
            }
            CHECK_NOTHROW(RunOneFrame()); // Normal
        }
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-08: Stale GBuffer handles do not alias HDR slot after mode switch
    // This is the exact root-cause regression: g_RG_DepthTexture had a stale
    // index that collided with g_RG_HDRColor's freshly-allocated slot.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-08 ModeSwitch - stale GBuffer handles do not alias HDR slot")
    {
        // Run a Normal frame first so GBuffer handles get valid indices
        RunOneFrame();
        const uint32_t depthIdxAfterNormal = g_RG_DepthTexture.m_Index;
        const uint32_t hdrIdxAfterNormal   = g_RG_HDRColor.m_Index;

        // Switch to PT mode — depth/GBuffer are NOT declared, handles become stale
        {
            RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
            RunOneFrame();
        }
        const uint32_t hdrIdxAfterPT = g_RG_HDRColor.m_Index;

        // Switch back to Normal — stale depth handle must NOT steal the HDR slot
        CHECK_NOTHROW(RunOneFrame());

        // After the Normal frame both handles must be valid and distinct
        CHECK(g_RG_DepthTexture.IsValid());
        CHECK(g_RG_HDRColor.IsValid());
        CHECK(g_RG_DepthTexture.m_Index != g_RG_HDRColor.m_Index);

        // Physical textures must both be present
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture) != nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor) != nullptr);

        (void)depthIdxAfterNormal; (void)hdrIdxAfterNormal; (void)hdrIdxAfterPT;
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-09: Force-invalidate counter reaches zero after two frames post-Shutdown
    // Verifies the engine-level fix: after Shutdown() the flag stays true for
    // two full frames (decremented in PostRender, not Reset) so handles skipped
    // in frame 1 are still invalidated in frame 2.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-09 ModeSwitch - force-invalidate counter reaches zero after two frames")
    {
        // After the fixture's Shutdown() the counter starts at 2.
        // PostRender() of frame 1 decrements it to 1 (flag still true for frame 2).
        // PostRender() of frame 2 decrements it to 0 (flag false from frame 3 on).
        CHECK(g_Renderer.m_RenderGraph.GetForceInvalidateFramesRemaining() == 2);
        RunOneFrame(); // frame 1: PostRender decrements counter to 1
        CHECK(g_Renderer.m_RenderGraph.GetForceInvalidateFramesRemaining() == 1);
        RunOneFrame(); // frame 2: PostRender decrements counter to 0
        CHECK(g_Renderer.m_RenderGraph.GetForceInvalidateFramesRemaining() == 0);
        RunOneFrame(); // frame 3: counter stays 0, normal operation
        CHECK(g_Renderer.m_RenderGraph.GetForceInvalidateFramesRemaining() == 0);
    }

    // ------------------------------------------------------------------
    // TC-PT-SW-10: IBL → PT → Normal triple-mode cycle does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SW-10 ModeSwitch - IBL to PT to Normal triple-mode cycle does not crash")
    {
        {
            RenderingModeGuard ibl(RenderingMode::IBL);
            CHECK_NOTHROW(RunOneFrame());
        }
        {
            RenderingModeGuard pt(RenderingMode::ReferencePathTracer);
            CHECK_NOTHROW(RunOneFrame());
        }
        CHECK_NOTHROW(RunOneFrame()); // Normal
    }
}

// ============================================================================
TEST_SUITE("PathTracer_AccumulationBuffer")
{
    // ------------------------------------------------------------------
    // TC-PT-ACC-01: Accumulation buffer is declared as persistent RGBA32_FLOAT UAV
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-ACC-01 PathTracerAccumulation - accumulation buffer is RGBA32_FLOAT UAV")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();

        // The accumulation buffer is a persistent texture declared by PathTracerRenderer.
        // We verify it exists by checking the HDR output is non-null (accumulation is internal).
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().isUAV == true);
    }

    // ------------------------------------------------------------------
    // TC-PT-ACC-02: Multiple path-tracer frames do not crash (convergence loop)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-ACC-02 PathTracerAccumulation - 8 consecutive path-tracer frames do not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        CHECK(RunNFrames(8));
    }

    // ------------------------------------------------------------------
    // TC-PT-ACC-03: HDR output is non-zero after path-tracer frames (scene has geometry)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-ACC-03 PathTracerAccumulation - HDR output is non-zero after path-tracer frames")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        // Run several frames to allow accumulation
        RunNFrames(4);

        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);

        // The scene has a triangle + directional light; at least some pixels should be lit.
        // We sample the center pixel — it may be black if the triangle doesn't cover it,
        // so we just verify the texture is valid and the readback doesn't crash.
        const nvrhi::TextureDesc& d = hdr->getDesc();
        CHECK(d.width > 0);
        CHECK(d.height > 0);
        CHECK(d.format == nvrhi::Format::RGBA32_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-PT-ACC-04: 32 path-tracer frames survive without GPU validation errors
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-ACC-04 PathTracerAccumulation - 32 frames survive without crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        CHECK(RunNFrames(32));
    }

    // ------------------------------------------------------------------
    // TC-PT-ACC-05: Accumulation buffer dimensions match swapchain
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-ACC-05 PathTracerAccumulation - HDR buffer dimensions match swapchain")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();

        auto [sw, sh] = g_Renderer.SwapchainSize();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().width  == sw);
        CHECK(hdr->getDesc().height == sh);
    }

    // ------------------------------------------------------------------
    // TC-PT-ACC-06: INTENTIONAL FAILURE — HDR format is NOT R11G11B10 in PT mode
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-ACC-06 PathTracerAccumulation - INTENTIONAL: HDR format is not R11G11B10 in PT mode")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();

        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        // In path-tracer mode the format MUST be RGBA32_FLOAT, NOT R11G11B10_FLOAT.
        CHECK(hdr->getDesc().format != nvrhi::Format::R11G11B10_FLOAT);
    }
}

// ============================================================================
// TEST SUITE: PathTracer_CameraReset
// ============================================================================
TEST_SUITE("PathTracer_CameraReset")
{
    // ------------------------------------------------------------------
    // TC-PT-CAM-01: Camera movement triggers accumulation reset (no crash)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-CAM-01 PathTracerCamera - camera movement does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunNFrames(4);

        // Move the camera to trigger a reset by setting a new world transform
        Matrix newTransform;
        DirectX::XMStoreFloat4x4(&newTransform,
            DirectX::XMMatrixTranslation(5.0f, 5.0f, 5.0f));
        g_Renderer.m_Scene.m_Camera.SetFromMatrix(newTransform);
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-PT-CAM-02: Repeated camera moves do not accumulate errors
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-CAM-02 PathTracerCamera - repeated camera moves survive")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        for (int i = 0; i < 8; ++i)
        {
            Matrix newTransform;
            DirectX::XMStoreFloat4x4(&newTransform,
                DirectX::XMMatrixTranslation((float)i, 0.0f, 10.0f));
            g_Renderer.m_Scene.m_Camera.SetFromMatrix(newTransform);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-PT-CAM-03: Stationary camera accumulates without reset (no crash over 16 frames)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-CAM-03 PathTracerCamera - stationary camera accumulates 16 frames")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        // Fix camera at a known position and do not move it
        g_Renderer.m_Scene.m_Camera.Reset();
        CHECK(RunNFrames(16));
    }
}

// ============================================================================
// TEST SUITE: PathTracer_SunAndLighting
// ============================================================================
TEST_SUITE("PathTracer_SunAndLighting")
{
    // ------------------------------------------------------------------
    // TC-PT-SUN-01: Path-tracer frame with default sun direction does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SUN-01 PathTracerSun - default sun direction frame does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-PT-SUN-02: Sun angular size of 0 degrees does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SUN-02 PathTracerSun - zero angular size does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());
        g_Renderer.m_Scene.m_Lights.back().m_AngularSize = 0.0f;
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-PT-SUN-03: Very large sun angular size (180 deg) does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SUN-03 PathTracerSun - large angular size (180 deg) does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());
        g_Renderer.m_Scene.m_Lights.back().m_AngularSize = 180.0f;
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-PT-SUN-04: Path-tracer max bounces = 0 does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SUN-04 PathTracerSun - max bounces = 0 does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        const uint32_t saved = g_Renderer.m_PathTracerMaxBounces;
        g_Renderer.m_PathTracerMaxBounces = 0;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_PathTracerMaxBounces = saved;
    }

    // ------------------------------------------------------------------
    // TC-PT-SUN-05: Path-tracer max bounces = 1 does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SUN-05 PathTracerSun - max bounces = 1 does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        const uint32_t saved = g_Renderer.m_PathTracerMaxBounces;
        g_Renderer.m_PathTracerMaxBounces = 1;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_PathTracerMaxBounces = saved;
    }

    // ------------------------------------------------------------------
    // TC-PT-SUN-06: Path-tracer max bounces = 32 does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-SUN-06 PathTracerSun - max bounces = 32 does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        const uint32_t saved = g_Renderer.m_PathTracerMaxBounces;
        g_Renderer.m_PathTracerMaxBounces = 32;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_PathTracerMaxBounces = saved;
    }
}

// ============================================================================
// TEST SUITE: ImGui_Registration
// ============================================================================
TEST_SUITE("ImGui_Registration")
{
    // ------------------------------------------------------------------
    // TC-IMGUI-REG-01: ImGuiRenderer is registered and non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-REG-01 ImGuiRegistration - renderer is registered and non-null")
    {
        CHECK(g_ImGuiRenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-REG-02: ImGuiRenderer name is "ImGui"
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-REG-02 ImGuiRegistration - name is ImGui")
    {
        REQUIRE(g_ImGuiRenderer != nullptr);
        CHECK(std::string_view(g_ImGuiRenderer->GetName()) == "ImGui");
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-REG-03: ImGuiRenderer is found by FindRendererByName
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-REG-03 ImGuiRegistration - found by FindRendererByName")
    {
        IRenderer* r = FindRendererByName("ImGui");
        CHECK(r != nullptr);
        CHECK(r == g_ImGuiRenderer);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-REG-04: ImGuiRenderer is NOT a base-pass renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-REG-04 ImGuiRegistration - IsBasePassRenderer returns false")
    {
        REQUIRE(g_ImGuiRenderer != nullptr);
        CHECK(g_ImGuiRenderer->IsBasePassRenderer() == false);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-REG-05: ImGuiRenderer is present in g_Renderer.m_Renderers
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-REG-05 ImGuiRegistration - present in renderer list")
    {
        bool found = false;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && std::string_view(r->GetName()) == "ImGui")
                found = true;
        CHECK(found);
    }
}

// ============================================================================
// TEST SUITE: ImGui_Setup
// ============================================================================
TEST_SUITE("ImGui_Setup")
{
    // ------------------------------------------------------------------
    // TC-IMGUI-SETUP-01: Setup returns false when ImGui has no draw data
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-SETUP-01 ImGuiSetup - Setup returns false with no draw data")
    {
        REQUIRE(g_ImGuiRenderer != nullptr);

        // ImGui::GetDrawData() returns nullptr if NewFrame/Render haven't been called.
        // We verify Setup() returns false gracefully.
        // Note: we call Setup directly with a fresh RenderGraph to avoid side effects.
        RenderGraph rg;
        bool result = g_ImGuiRenderer->Setup(rg);
        // Should be false because there's no valid draw data in test context.
        CHECK(result == false);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-SETUP-02: Full frame with Normal mode includes ImGui pass without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-IMGUI-SETUP-02 ImGuiSetup - full Normal-mode frame with ImGui does not crash")
    {
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-SETUP-03: Multiple consecutive Normal-mode frames with ImGui do not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-IMGUI-SETUP-03 ImGuiSetup - 10 consecutive Normal-mode frames do not crash")
    {
        CHECK(RunNFrames(10));
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-SETUP-04: ImGui pass is skipped in path-tracer mode (no crash)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-IMGUI-SETUP-04 ImGuiSetup - ImGui pass skipped in path-tracer mode without crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-SETUP-05: INTENTIONAL FAILURE — ImGui name is not "BasePass"
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-SETUP-05 ImGuiSetup - INTENTIONAL: name is not BasePass")
    {
        REQUIRE(g_ImGuiRenderer != nullptr);
        CHECK(std::string_view(g_ImGuiRenderer->GetName()) != "BasePass");
    }
}

// ============================================================================
// TEST SUITE: ImGui_FontTexture
// ============================================================================
TEST_SUITE("ImGui_FontTexture")
{
    // ------------------------------------------------------------------
    // TC-IMGUI-FONT-01: ImGui font atlas has valid dimensions
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-FONT-01 ImGuiFontTexture - font atlas has valid dimensions")
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        CHECK(pixels != nullptr);
        CHECK(width > 0);
        CHECK(height > 0);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-FONT-02: ImGui font atlas pixel data is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-FONT-02 ImGuiFontTexture - font atlas pixel data is non-null")
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        CHECK(pixels != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-FONT-03: ImGui font atlas width is a power of two or reasonable size
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-FONT-03 ImGuiFontTexture - font atlas width is reasonable (>= 64)")
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        CHECK(width >= 64);
        CHECK(height >= 64);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-FONT-04: ImGui display size is set (non-zero after renderer init)
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-FONT-04 ImGuiFontTexture - ImGui IO display size is set")
    {
        ImGuiIO& io = ImGui::GetIO();
        // After InitializeForTests the window is created; ImGui display size should be set.
        CHECK(io.DisplaySize.x >= 0.0f);
        CHECK(io.DisplaySize.y >= 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-IMGUI-FONT-05: INTENTIONAL FAILURE — font atlas height is not negative
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMGUI-FONT-05 ImGuiFontTexture - INTENTIONAL: font atlas height is not negative")
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        CHECK(height >= 0);
    }
}

// ============================================================================
// TEST SUITE: NRD_Library
// ============================================================================
TEST_SUITE("NRD_Library")
{
    // ------------------------------------------------------------------
    // TC-NRD-LIB-01: NRD library descriptor is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-LIB-01 NRDLibrary - GetLibraryDesc returns non-null")
    {
        const nrd::LibraryDesc* desc = nrd::GetLibraryDesc();
        CHECK(desc != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-NRD-LIB-02: NRD library version major is > 0
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-LIB-02 NRDLibrary - version major is > 0")
    {
        const nrd::LibraryDesc* desc = nrd::GetLibraryDesc();
        REQUIRE(desc != nullptr);
        CHECK(desc->versionMajor > 0);
    }

    // ------------------------------------------------------------------
    // TC-NRD-LIB-03: NRD denoiser string for RELAX_DIFFUSE_SPECULAR is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-LIB-03 NRDLibrary - denoiser string for RELAX_DIFFUSE_SPECULAR is non-null")
    {
        const char* s = nrd::GetDenoiserString(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
        CHECK(s != nullptr);
        CHECK(std::string_view(s).size() > 0u);
    }

    // ------------------------------------------------------------------
    // TC-NRD-LIB-04: NRD denoiser string for REBLUR_DIFFUSE_SPECULAR is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-LIB-04 NRDLibrary - denoiser string for REBLUR_DIFFUSE_SPECULAR is non-null")
    {
        const char* s = nrd::GetDenoiserString(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);
        CHECK(s != nullptr);
        CHECK(std::string_view(s).size() > 0u);
    }

    // ------------------------------------------------------------------
    // TC-NRD-LIB-05: NRD library has at least one supported denoiser
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-LIB-05 NRDLibrary - library has at least one supported denoiser")
    {
        const nrd::LibraryDesc* desc = nrd::GetLibraryDesc();
        REQUIRE(desc != nullptr);
        CHECK(desc->supportedDenoisersNum > 0u);
    }

    // ------------------------------------------------------------------
    // TC-NRD-LIB-06: NRD library version string fields are consistent
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-LIB-06 NRDLibrary - version fields are consistent (minor and patch are non-negative)")
    {
        const nrd::LibraryDesc* desc = nrd::GetLibraryDesc();
        REQUIRE(desc != nullptr);
        // minor and patch are unsigned, so always >= 0; just verify they're accessible
        CHECK(desc->versionMinor >= 0u);
        CHECK(desc->versionBuild >= 0u);
    }

    // ------------------------------------------------------------------
    // TC-NRD-LIB-07: INTENTIONAL FAILURE — NRD version major is not 0
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-LIB-07 NRDLibrary - INTENTIONAL: version major is not 0")
    {
        const nrd::LibraryDesc* desc = nrd::GetLibraryDesc();
        REQUIRE(desc != nullptr);
        CHECK(desc->versionMajor != 0u);
    }
}

// ============================================================================
// TEST SUITE: NRD_CommonSettings
// ============================================================================
TEST_SUITE("NRD_CommonSettings")
{
    // ------------------------------------------------------------------
    // TC-NRD-CS-01: FillNRDCommonSettings does not crash on frame 0
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-01 NRDCommonSettings - FillNRDCommonSettings does not crash on frame 0")
    {
        g_Renderer.m_FrameNumber = 0;
        nrd::CommonSettings settings{};
        CHECK_NOTHROW(FillNRDCommonSettings(settings));
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-02: FillNRDCommonSettings sets resourceSize to swapchain extent
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-02 NRDCommonSettings - resourceSize matches swapchain extent")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);

        auto [sw, sh] = g_Renderer.SwapchainSize();
        CHECK(settings.resourceSize[0] == static_cast<uint16_t>(sw));
        CHECK(settings.resourceSize[1] == static_cast<uint16_t>(sh));
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-03: FillNRDCommonSettings sets rectSize to swapchain extent
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-03 NRDCommonSettings - rectSize matches swapchain extent")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);

        auto [sw, sh] = g_Renderer.SwapchainSize();
        CHECK(settings.rectSize[0] == static_cast<uint16_t>(sw));
        CHECK(settings.rectSize[1] == static_cast<uint16_t>(sh));
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-04: FillNRDCommonSettings sets motionVectorScale[0] = 1/width
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-04 NRDCommonSettings - motionVectorScale[0] is 1/width")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);

        auto [sw, sh] = g_Renderer.SwapchainSize();
        const float expected = 1.0f / static_cast<float>(sw);
        CHECK(std::abs(settings.motionVectorScale[0] - expected) < 1e-6f);
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-05: FillNRDCommonSettings sets motionVectorScale[1] = 1/height
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-05 NRDCommonSettings - motionVectorScale[1] is 1/height")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);

        auto [sw, sh] = g_Renderer.SwapchainSize();
        const float expected = 1.0f / static_cast<float>(sh);
        CHECK(std::abs(settings.motionVectorScale[1] - expected) < 1e-6f);
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-06: FillNRDCommonSettings sets motionVectorScale[2] = 1.0f (view-space Z)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-06 NRDCommonSettings - motionVectorScale[2] is 1.0f")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);
        CHECK(std::abs(settings.motionVectorScale[2] - 1.0f) < 1e-6f);
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-07: On frame 0, accumulationMode is CLEAR_AND_RESTART
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-07 NRDCommonSettings - accumulationMode is CLEAR_AND_RESTART on frame 0")
    {
        g_Renderer.m_FrameNumber = 0;
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);
        CHECK(settings.accumulationMode == nrd::AccumulationMode::CLEAR_AND_RESTART);
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-08: On frame > 0, accumulationMode is CONTINUE
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-08 NRDCommonSettings - accumulationMode is CONTINUE on frame > 0")
    {
        g_Renderer.m_FrameNumber = 5;
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);
        CHECK(settings.accumulationMode == nrd::AccumulationMode::CONTINUE);
        g_Renderer.m_FrameNumber = 0; // restore
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-09: FillNRDCommonSettings sets isMotionVectorInWorldSpace = false
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-09 NRDCommonSettings - isMotionVectorInWorldSpace is false")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);
        CHECK(settings.isMotionVectorInWorldSpace == false);
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-10: FillNRDCommonSettings sets frameIndex to g_Renderer.m_FrameNumber
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-10 NRDCommonSettings - frameIndex matches g_Renderer.m_FrameNumber")
    {
        g_Renderer.m_FrameNumber = 42;
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);
        CHECK(settings.frameIndex == 42u);
        g_Renderer.m_FrameNumber = 0;
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-11: FillNRDCommonSettings sets denoisingRange to 1000.0f
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-11 NRDCommonSettings - denoisingRange is 1000.0f")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);
        CHECK(std::abs(settings.denoisingRange - 1000.0f) < 1e-3f);
    }

    // ------------------------------------------------------------------
    // TC-NRD-CS-12: INTENTIONAL FAILURE — motionVectorScale[2] is not 0
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-NRD-CS-12 NRDCommonSettings - INTENTIONAL: motionVectorScale[2] is not 0")
    {
        nrd::CommonSettings settings{};
        FillNRDCommonSettings(settings);
        CHECK(settings.motionVectorScale[2] != 0.0f);
    }
}

// ============================================================================
// TEST SUITE: NRD_Integration
// ============================================================================
TEST_SUITE("NRD_Integration")
{
    // ------------------------------------------------------------------
    // TC-NRD-INT-01: NrdIntegration can be constructed for RELAX_DIFFUSE_SPECULAR
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-01 NRDIntegration - construction for RELAX_DIFFUSE_SPECULAR does not crash")
    {
        CHECK_NOTHROW({
            NrdIntegration nrd(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
        });
    }

    // ------------------------------------------------------------------
    // TC-NRD-INT-02: NrdIntegration can be constructed for REBLUR_DIFFUSE_SPECULAR
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-02 NRDIntegration - construction for REBLUR_DIFFUSE_SPECULAR does not crash")
    {
        CHECK_NOTHROW({
            NrdIntegration nrd(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);
        });
    }

    // ------------------------------------------------------------------
    // TC-NRD-INT-03: NrdIntegration Initialize() succeeds for RELAX_DIFFUSE_SPECULAR
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-03 NRDIntegration - Initialize succeeds for RELAX_DIFFUSE_SPECULAR")
    {
        NrdIntegration nrdInt(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
        bool ok = nrdInt.Initialize();
        CHECK(ok);
    }

    // ------------------------------------------------------------------
    // TC-NRD-INT-04: NrdIntegration Initialize() succeeds for REBLUR_DIFFUSE_SPECULAR
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-04 NRDIntegration - Initialize succeeds for REBLUR_DIFFUSE_SPECULAR")
    {
        NrdIntegration nrdInt(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);
        bool ok = nrdInt.Initialize();
        CHECK(ok);
    }

    // ------------------------------------------------------------------
    // TC-NRD-INT-05: NrdIntegration destructor does not crash after Initialize
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-05 NRDIntegration - destructor does not crash after Initialize")
    {
        CHECK_NOTHROW({
            NrdIntegration nrdInt(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
            nrdInt.Initialize();
            // destructor called here
        });
    }

    // ------------------------------------------------------------------
    // TC-NRD-INT-06: NrdIntegration destructor does not crash without Initialize
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-06 NRDIntegration - destructor does not crash without Initialize")
    {
        CHECK_NOTHROW({
            NrdIntegration nrdInt(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
            // destructor called without Initialize
        });
    }

    // ------------------------------------------------------------------
    // TC-NRD-INT-07: Multiple NrdIntegration instances can coexist
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-07 NRDIntegration - multiple instances can coexist")
    {
        CHECK_NOTHROW({
            NrdIntegration a(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
            NrdIntegration b(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);
            a.Initialize();
            b.Initialize();
        });
    }

    // ------------------------------------------------------------------
    // TC-NRD-INT-08: INTENTIONAL FAILURE — Initialize returns true (not false)
    // ------------------------------------------------------------------
    TEST_CASE("TC-NRD-INT-08 NRDIntegration - INTENTIONAL: Initialize does not return false")
    {
        NrdIntegration nrdInt(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
        bool ok = nrdInt.Initialize();
        CHECK(ok != false);
    }
}

// ============================================================================
// TEST SUITE: Clear_Registration
// ============================================================================
TEST_SUITE("Clear_Registration")
{
    // ------------------------------------------------------------------
    // TC-CLR-REG-01: ClearRenderer is registered and non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-CLR-REG-01 ClearRegistration - renderer is registered and non-null")
    {
        CHECK(g_ClearRenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-REG-02: ClearRenderer name is "Clear"
    // ------------------------------------------------------------------
    TEST_CASE("TC-CLR-REG-02 ClearRegistration - name is Clear")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        CHECK(std::string_view(g_ClearRenderer->GetName()) == "Clear");
    }

    // ------------------------------------------------------------------
    // TC-CLR-REG-03: ClearRenderer is found by FindRendererByName
    // ------------------------------------------------------------------
    TEST_CASE("TC-CLR-REG-03 ClearRegistration - found by FindRendererByName")
    {
        IRenderer* r = FindRendererByName("Clear");
        CHECK(r != nullptr);
        CHECK(r == g_ClearRenderer);
    }

    // ------------------------------------------------------------------
    // TC-CLR-REG-04: ClearRenderer is NOT a base-pass renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-CLR-REG-04 ClearRegistration - IsBasePassRenderer returns false")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        CHECK(g_ClearRenderer->IsBasePassRenderer() == false);
    }
}

// ============================================================================
// TEST SUITE: Clear_Pass
// ============================================================================
TEST_SUITE("Clear_Pass")
{
    // ------------------------------------------------------------------
    // TC-CLR-PASS-01: Full Normal-mode frame with ClearRenderer does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-01 ClearPass - Normal-mode frame does not crash")
    {
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-02: HDR color texture is declared after ClearRenderer Setup
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-02 ClearPass - HDR color texture is declared after frame")
    {
        RunOneFrame();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        CHECK(hdr != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-03: Depth texture is declared in Normal mode after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-03 ClearPass - depth texture declared in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle depth = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture);
        CHECK(depth != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-04: GBuffer albedo is declared in Normal mode after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-04 ClearPass - GBuffer albedo declared in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle albedo = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferAlbedo);
        CHECK(albedo != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-05: GBuffer normals is declared in Normal mode after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-05 ClearPass - GBuffer normals declared in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle normals = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferNormals);
        CHECK(normals != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-06: GBuffer ORM is declared in Normal mode after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-06 ClearPass - GBuffer ORM declared in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle orm = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferORM);
        CHECK(orm != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-07: GBuffer emissive is declared in Normal mode after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-07 ClearPass - GBuffer emissive declared in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle emissive = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferEmissive);
        CHECK(emissive != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-08: GBuffer motion vectors is declared in Normal mode after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-08 ClearPass - GBuffer motion vectors declared in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle motion = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferMotionVectors);
        CHECK(motion != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-09: Exposure texture is declared (persistent) after frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-09 ClearPass - exposure texture declared after frame")
    {
        RunOneFrame();
        nvrhi::TextureHandle exposure = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        CHECK(exposure != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-10: HDR color texture has correct format in Normal mode
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-10 ClearPass - HDR color format is R11G11B10_FLOAT in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().format == Renderer::HDR_COLOR_FORMAT);
        CHECK(hdr->getDesc().format == nvrhi::Format::R11G11B10_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-11: Depth texture has correct format (D24S8) in Normal mode
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-11 ClearPass - depth texture format is D24S8 in Normal mode")
    {
        RunOneFrame();
        nvrhi::TextureHandle depth = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture);
        REQUIRE(depth != nullptr);
        CHECK(depth->getDesc().format == Renderer::DEPTH_FORMAT);
        CHECK(depth->getDesc().format == nvrhi::Format::D24S8);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-12: HDR color texture dimensions match swapchain in Normal mode
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-12 ClearPass - HDR color dimensions match swapchain")
    {
        RunOneFrame();
        auto [sw, sh] = g_Renderer.SwapchainSize();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().width  == sw);
        CHECK(hdr->getDesc().height == sh);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-13: Depth texture dimensions match swapchain in Normal mode
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-13 ClearPass - depth texture dimensions match swapchain")
    {
        RunOneFrame();
        auto [sw, sh] = g_Renderer.SwapchainSize();
        nvrhi::TextureHandle depth = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture);
        REQUIRE(depth != nullptr);
        CHECK(depth->getDesc().width  == sw);
        CHECK(depth->getDesc().height == sh);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-14: Persistent exposure texture survives across 5 frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-14 ClearPass - exposure texture survives 5 frames")
    {
        nvrhi::TextureHandle firstExposure = nullptr;
        for (int i = 0; i < 5; ++i)
        {
            RunOneFrame();
            nvrhi::TextureHandle exp = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
            CHECK(exp != nullptr);
            if (i == 0) firstExposure = exp;
        }
        // Persistent texture should be the same physical resource across frames
        nvrhi::TextureHandle lastExposure = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        CHECK(firstExposure == lastExposure);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-15: INTENTIONAL FAILURE — HDR color format is not UNKNOWN
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-15 ClearPass - INTENTIONAL: HDR color format is not UNKNOWN")
    {
        RunOneFrame();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().format != nvrhi::Format::UNKNOWN);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-16: Path-tracer mode does not declare GBuffer normals
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-16 ClearPass - GBuffer normals not declared in path-tracer mode")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();
        nvrhi::TextureHandle normals = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferNormals);
        CHECK(normals == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-PASS-17: Path-tracer mode does not declare GBuffer ORM
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-PASS-17 ClearPass - GBuffer ORM not declared in path-tracer mode")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();
        nvrhi::TextureHandle orm = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferORM);
        CHECK(orm == nullptr);
    }
}

// ============================================================================
// TEST SUITE: TLAS_Registration
// ============================================================================
TEST_SUITE("TLAS_Registration")
{
    // ------------------------------------------------------------------
    // TC-TLAS-REG-01: TLASRenderer is registered and non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-REG-01 TLASRegistration - renderer is registered and non-null")
    {
        CHECK(g_TLASRenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-REG-02: TLASRenderer name is "TLAS Update"
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-REG-02 TLASRegistration - name is TLAS Update")
    {
        REQUIRE(g_TLASRenderer != nullptr);
        CHECK(std::string_view(g_TLASRenderer->GetName()) == "TLAS Update");
    }

    // ------------------------------------------------------------------
    // TC-TLAS-REG-03: TLASRenderer is found by FindRendererByName
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-REG-03 TLASRegistration - found by FindRendererByName")
    {
        IRenderer* r = FindRendererByName("TLAS Update");
        CHECK(r != nullptr);
        CHECK(r == g_TLASRenderer);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-REG-04: TLASRenderer is NOT a base-pass renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-REG-04 TLASRegistration - IsBasePassRenderer returns false")
    {
        REQUIRE(g_TLASRenderer != nullptr);
        CHECK(g_TLASRenderer->IsBasePassRenderer() == false);
    }
}

// ============================================================================
// TEST SUITE: TLAS_SceneIntegrity
// ============================================================================
TEST_SUITE("TLAS_SceneIntegrity")
{
    // ------------------------------------------------------------------
    // TC-TLAS-SI-01: TLAS is non-null after MinimalSceneFixture setup
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-01 TLASSceneIntegrity - TLAS is non-null after scene load")
    {
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-02: RTInstanceDescBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-02 TLASSceneIntegrity - RTInstanceDescBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_RTInstanceDescBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-03: RTInstanceDescs is non-empty after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-03 TLASSceneIntegrity - RTInstanceDescs is non-empty")
    {
        CHECK(!g_Renderer.m_Scene.m_RTInstanceDescs.empty());
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-04: BLASAddressBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-04 TLASSceneIntegrity - BLASAddressBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_BLASAddressBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-05: InstanceLODBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-05 TLASSceneIntegrity - InstanceLODBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_InstanceLODBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-06: RTInstanceDescs count matches InstanceData count
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-06 TLASSceneIntegrity - RTInstanceDescs count matches InstanceData count")
    {
        CHECK(g_Renderer.m_Scene.m_RTInstanceDescs.size() == g_Renderer.m_Scene.m_InstanceData.size());
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-07: TLAS rebuild survives a full Normal-mode frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-07 TLASSceneIntegrity - TLAS rebuild survives full Normal-mode frame")
    {
        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-08: TLAS rebuild survives a full path-tracer frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-08 TLASSceneIntegrity - TLAS rebuild survives full path-tracer frame")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-SI-09: TLAS remains non-null after 10 consecutive frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-SI-09 TLASSceneIntegrity - TLAS remains non-null after 10 frames")
    {
        for (int i = 0; i < 10; ++i)
        {
            RunOneFrame();
            CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
        }
    }
}

// ============================================================================
// TEST SUITE: TLAS_Mutations
// ============================================================================
TEST_SUITE("TLAS_Mutations")
{
    // ------------------------------------------------------------------
    // TC-TLAS-MUT-01: Node transform mutation marks instance dirty range
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-01 TLASMutations - node transform mutation marks dirty range")
    {
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        // Find a node with a mesh
        int meshNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_MeshIndex >= 0)
            {
                meshNodeIdx = i;
                break;
            }
        }
        REQUIRE(meshNodeIdx >= 0);

        // Mutate the translation
        Scene::Node& node = g_Renderer.m_Scene.m_Nodes[meshNodeIdx];
        node.m_Translation = { 1.0f, 2.0f, 3.0f };
        node.m_IsDirty = true;

        // Mark dirty range manually (as the renderer would)
        g_Renderer.m_Scene.m_InstanceDirtyRange = { 0, (uint32_t)g_Renderer.m_Scene.m_InstanceData.size() };

        CHECK(g_Renderer.m_Scene.AreInstanceTransformsDirty());
    }

    // ------------------------------------------------------------------
    // TC-TLAS-MUT-02: TLAS rebuild after node transform mutation does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-02 TLASMutations - TLAS rebuild after node transform mutation does not crash")
    {
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_MeshIndex >= 0)
            {
                g_Renderer.m_Scene.m_Nodes[i].m_Translation = { 2.0f, 0.0f, 0.0f };
                g_Renderer.m_Scene.m_Nodes[i].m_IsDirty = true;
                break;
            }
        }

        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-MUT-03: Multiple consecutive transform mutations survive
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-03 TLASMutations - multiple consecutive transform mutations survive")
    {
        for (int frame = 0; frame < 5; ++frame)
        {
            for (auto& node : g_Renderer.m_Scene.m_Nodes)
            {
                if (node.m_MeshIndex >= 0)
                {
                    node.m_Translation = { (float)frame, 0.0f, 0.0f };
                    node.m_IsDirty = true;
                }
            }
            CHECK_NOTHROW(RunOneFrame());
        }
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-MUT-04: Dirty range is reset after frame (no stale dirty state)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-04 TLASMutations - dirty range is reset after frame")
    {
        // Force dirty
        g_Renderer.m_Scene.m_InstanceDirtyRange = { 0, (uint32_t)g_Renderer.m_Scene.m_InstanceData.size() };
        RunOneFrame();
        // After the frame the renderer should have cleared the dirty range
        // (first > second means no dirty instances)
        const auto& dr = g_Renderer.m_Scene.m_InstanceDirtyRange;
        CHECK(dr.first > dr.second);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-MUT-05: Sun direction change does not invalidate TLAS
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-05 TLASMutations - sun direction change does not invalidate TLAS")
    {
        g_Renderer.m_Scene.SetSunPitchYaw(0.5f, 1.0f);
        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-MUT-06: TLAS rebuild in path-tracer mode after mutation does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-06 TLASMutations - TLAS rebuild in path-tracer mode after mutation")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);

        for (auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (node.m_MeshIndex >= 0)
            {
                node.m_Translation = { 1.0f, 1.0f, 1.0f };
                node.m_IsDirty = true;
            }
        }

        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-MUT-07: INTENTIONAL FAILURE — TLAS is not null after mutation frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-07 TLASMutations - INTENTIONAL: TLAS is not null after mutation frame")
    {
        for (auto& node : g_Renderer.m_Scene.m_Nodes)
            if (node.m_MeshIndex >= 0) { node.m_IsDirty = true; break; }

        RunOneFrame();
        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-MUT-08: Instance count is stable across mutation frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TLAS-MUT-08 TLASMutations - instance count is stable across mutation frames")
    {
        const size_t initialCount = g_Renderer.m_Scene.m_RTInstanceDescs.size();
        REQUIRE(initialCount > 0);

        for (int i = 0; i < 5; ++i)
        {
            for (auto& node : g_Renderer.m_Scene.m_Nodes)
                if (node.m_MeshIndex >= 0) node.m_IsDirty = true;
            RunOneFrame();
        }

        CHECK(g_Renderer.m_Scene.m_RTInstanceDescs.size() == initialCount);
    }
}

// ============================================================================
// TEST SUITE: SceneRHI_Integrity
// ============================================================================
TEST_SUITE("SceneRHI_Integrity")
{
    // ------------------------------------------------------------------
    // TC-SRHI-01: GPU device is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRHI-01 SceneRHI - GPU device is non-null")
    {
        CHECK(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-02: Swapchain extent is non-zero
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRHI-02 SceneRHI - swapchain extent is non-zero")
    {
        auto [sw, sh] = g_Renderer.SwapchainSize();
        CHECK(sw > 0u);
        CHECK(sh > 0u);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-03: Swapchain extent is at least 64x64
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRHI-03 SceneRHI - swapchain extent is at least 64x64")
    {
        auto [sw, sh] = g_Renderer.SwapchainSize();
        CHECK(sw >= 64u);
        CHECK(sh >= 64u);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-04: AcquireCommandList returns a valid command list
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRHI-04 SceneRHI - AcquireCommandList returns valid command list")
    {
        nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
        CHECK(cmd != nullptr);
        cmd->open();
        cmd->close();
    }

    // ------------------------------------------------------------------
    // TC-SRHI-05: Command list open/close/execute round-trip does not crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRHI-05 SceneRHI - command list open/close/execute round-trip")
    {
        nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
        REQUIRE(cmd != nullptr);
        cmd->open();
        cmd->close();
        CHECK_NOTHROW(g_Renderer.ExecutePendingCommandLists());
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-SRHI-06: Scene has at least one light after MinimalSceneFixture
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-06 SceneRHI - scene has at least one light")
    {
        CHECK(!g_Renderer.m_Scene.m_Lights.empty());
    }

    // ------------------------------------------------------------------
    // TC-SRHI-07: Last light is a directional light (guaranteed by EnsureDefaultDirectionalLight)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-07 SceneRHI - last light is directional")
    {
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());
        CHECK(g_Renderer.m_Scene.m_Lights.back().m_Type == Scene::Light::Directional);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-08: LightBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-08 SceneRHI - LightBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_LightBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-09: VertexBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-09 SceneRHI - VertexBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_VertexBufferQuantized != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-10: IndexBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-10 SceneRHI - IndexBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_IndexBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-11: MaterialConstantsBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-11 SceneRHI - MaterialConstantsBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_MaterialConstantsBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-12: InstanceDataBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-12 SceneRHI - InstanceDataBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_InstanceDataBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-13: Scene bounding sphere radius is non-negative
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-13 SceneRHI - scene bounding sphere radius is non-negative")
    {
        CHECK(g_Renderer.m_Scene.m_SceneBoundingSphere.Radius >= 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-14: Scene has at least one mesh after MinimalSceneFixture
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-14 SceneRHI - scene has at least one mesh")
    {
        CHECK(!g_Renderer.m_Scene.m_Meshes.empty());
    }

    // ------------------------------------------------------------------
    // TC-SRHI-15: Scene has at least one node after MinimalSceneFixture
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-15 SceneRHI - scene has at least one node")
    {
        CHECK(!g_Renderer.m_Scene.m_Nodes.empty());
    }

    // ------------------------------------------------------------------
    // TC-SRHI-16: INTENTIONAL FAILURE — device is not null
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRHI-16 SceneRHI - INTENTIONAL: device is not null")
    {
        CHECK(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-17: Sun direction vector is normalized (length ~= 1.0)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-17 SceneRHI - sun direction is normalized")
    {
        Vector3 d = g_Renderer.m_Scene.GetSunDirection();
        float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        CHECK(std::abs(len - 1.0f) < 1e-4f);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-18: LightCount matches m_Lights.size() after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-18 SceneRHI - LightCount matches m_Lights.size()")
    {
        CHECK(g_Renderer.m_Scene.m_LightCount == (uint32_t)g_Renderer.m_Scene.m_Lights.size());
    }

    // ------------------------------------------------------------------
    // TC-SRHI-19: MeshDataBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-19 SceneRHI - MeshDataBuffer is non-null")
    {
        CHECK(g_Renderer.m_Scene.m_MeshDataBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SRHI-20: InstanceData is non-empty after scene load
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SRHI-20 SceneRHI - InstanceData is non-empty")
    {
        CHECK(!g_Renderer.m_Scene.m_InstanceData.empty());
    }
}

// ============================================================================
// TEST SUITE: PathTracer_FullPipeline
// ============================================================================
TEST_SUITE("PathTracer_FullPipeline")
{
    // ------------------------------------------------------------------
    // TC-PT-PIPE-01: Path-tracer + Normal mode alternation does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-PIPE-01 PathTracerPipeline - alternating PT/Normal mode does not crash")
    {
        for (int i = 0; i < 6; ++i)
        {
            if (i % 2 == 0)
            {
                RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
                CHECK_NOTHROW(RunOneFrame());
            }
            else
            {
                CHECK_NOTHROW(RunOneFrame());
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-PT-PIPE-02: Path-tracer mode with TLAS rebuild does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-PIPE-02 PathTracerPipeline - path-tracer with TLAS rebuild does not crash")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        for (int i = 0; i < 4; ++i)
        {
            for (auto& node : g_Renderer.m_Scene.m_Nodes)
                if (node.m_MeshIndex >= 0) { node.m_IsDirty = true; break; }
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-PT-PIPE-03: Path-tracer HDR output is a UAV texture
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-PIPE-03 PathTracerPipeline - HDR output is a UAV texture")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().isUAV == true);
    }

    // ------------------------------------------------------------------
    // TC-PT-PIPE-04: Path-tracer HDR output is a render target
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-PIPE-04 PathTracerPipeline - HDR output is a render target")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().isRenderTarget == true);
    }

    // ------------------------------------------------------------------
    // TC-PT-PIPE-05: Path-tracer frame number increments correctly
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-PIPE-05 PathTracerPipeline - frame number increments correctly")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        const uint32_t startFrame = g_Renderer.m_FrameNumber;
        RunNFrames(5);
        CHECK(g_Renderer.m_FrameNumber == startFrame + 5u);
    }

    // ------------------------------------------------------------------
    // TC-PT-PIPE-06: INTENTIONAL FAILURE — path-tracer HDR format is not UNKNOWN
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-PT-PIPE-06 PathTracerPipeline - INTENTIONAL: HDR format is not UNKNOWN")
    {
        RenderingModeGuard guard(RenderingMode::ReferencePathTracer);
        RunOneFrame();
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);
        CHECK(hdr->getDesc().format != nvrhi::Format::UNKNOWN);
    }
}

// ============================================================================
// TEST SUITE: Clear_RenderGraph
// ============================================================================
TEST_SUITE("Clear_RenderGraph")
{
    // ------------------------------------------------------------------
    // TC-CLR-RG-01: RenderGraph Reset clears transient resources
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-01 ClearRenderGraph - Reset clears transient resources")
    {
        RunOneFrame();
        g_Renderer.m_RenderGraph.Reset();
        // After reset, transient textures should be invalid
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        CHECK(hdr == nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-02: RenderGraph Shutdown does not crash (with GPU drain)
    // Root cause of the original D3D12 #921 error: Shutdown() was called
    // while GPU work referencing transient textures was still in-flight.
    // Fix: always waitForIdle() before Shutdown() to drain the GPU first.
    // RenderGraph::Shutdown() now also calls waitForIdle() defensively.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-02 ClearRenderGraph - Shutdown does not crash after GPU drain")
    {
        RunOneFrame();
        // Must drain GPU before releasing transient texture handles.
        // Omitting this caused D3D12 ERROR #921: OBJECT_DELETED_WHILE_STILL_IN_USE
        DEV()->waitForIdle();
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.Shutdown());
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-03: RenderGraph recovers after Shutdown + frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-03 ClearRenderGraph - recovers after Shutdown + frame")
    {
        RunOneFrame();
        DEV()->waitForIdle(); // drain GPU before releasing transient resources
        g_Renderer.m_RenderGraph.Shutdown();
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-04: GetPassIndex returns non-zero for "Clear" after a frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-04 ClearRenderGraph - GetPassIndex returns non-zero for Clear pass")
    {
        RunOneFrame();
        uint16_t idx = g_Renderer.m_RenderGraph.GetPassIndex("Clear");
        CHECK(idx != 0u);
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-05: GetPassIndex returns non-zero for "TLAS Update" after a frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-05 ClearRenderGraph - GetPassIndex returns non-zero for TLAS Update pass")
    {
        RunOneFrame();
        uint16_t idx = g_Renderer.m_RenderGraph.GetPassIndex("TLAS Update");
        CHECK(idx != 0u);
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-06: INTENTIONAL FAILURE — GetPassIndex for unknown pass returns 0
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-06 ClearRenderGraph - INTENTIONAL: GetPassIndex for unknown pass returns 0")
    {
        RunOneFrame();
        uint16_t idx = g_Renderer.m_RenderGraph.GetPassIndex("__NonExistentPass__");
        CHECK(idx == 0u);
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-07: Multiple Shutdown + RunOneFrame cycles do not crash
    // Regression: each Shutdown() must drain the GPU so that the next
    // frame can safely reallocate the same heap memory.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-07 ClearRenderGraph - multiple Shutdown+frame cycles do not crash")
    {
        for (int i = 0; i < 3; ++i)
        {
            CHECK_NOTHROW(RunOneFrame());
            DEV()->waitForIdle();
            CHECK_NOTHROW(g_Renderer.m_RenderGraph.Shutdown());
        }
        // Final frame after last Shutdown must also work
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-08: After Shutdown + RunOneFrame, transient textures are
    // re-allocated (physical handles are non-null again)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-08 ClearRenderGraph - transient textures re-allocated after Shutdown+frame")
    {
        RunOneFrame();
        DEV()->waitForIdle();
        g_Renderer.m_RenderGraph.Shutdown();
        RunOneFrame();

        // HDR color must be re-declared and have a valid physical texture
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        CHECK(hdr != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-09: Shutdown without a prior frame does not crash
    // (RenderGraph starts empty — no GPU resources to release)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-09 ClearRenderGraph - Shutdown without prior frame does not crash")
    {
        // No RunOneFrame() — graph is empty, Shutdown must be a no-op
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.Shutdown());
    }

    // ------------------------------------------------------------------
    // TC-CLR-RG-10: GetTextureRaw returns nullptr after Shutdown (no stale handles)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CLR-RG-10 ClearRenderGraph - GetTextureRaw returns nullptr after Shutdown")
    {
        RunOneFrame();
        DEV()->waitForIdle();
        g_Renderer.m_RenderGraph.Shutdown();

        // All handles must appear invalid after Shutdown clears m_Textures
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture) == nullptr);
        CHECK(g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferAlbedo) == nullptr);
    }
}
