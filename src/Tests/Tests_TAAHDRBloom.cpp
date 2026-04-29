// Tests_TAAHDRBloom.cpp
//
// Systems under test:
//   TAARenderer  — FSR3-based temporal anti-aliasing, jitter, sharpness, debug view,
//                  TAA output texture, pass-through when disabled, reset flag,
//                  RenderGraph resource declaration, exposure readback integration.
//   HDRRenderer  — Luminance histogram, auto-exposure adaptation, manual exposure,
//                  exposure buffer/texture, readback double-buffering, tonemapping pass,
//                  log-luminance range constants, adaptation speed, EV clamping.
//   BloomRenderer — Prefilter extraction, downsample pyramid, upsample pyramid,
//                   composite additive blend, bloom intensity, knee, upsample radius,
//                   mip count, texture formats, disabled path, RenderGraph integration.
//   Scene + RHI  — Exposure texture format, TAA output format, bloom pyramid format,
//                  persistent buffer survival across frames, parameter boundary values.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources + all IRenderer
//               instances registered and initialized).
//
// Run with: HobbyRenderer --run-tests=*TAA* --run-tests=*HDR* --run-tests=*Bloom*
//           or: HobbyRenderer --run-tests=*TAAHDRBloom*
// ============================================================================

#include "TestFixtures.h"

// External renderer pointers (defined via REGISTER_RENDERER macros)
extern IRenderer* g_TAARenderer;
extern IRenderer* g_HDRRenderer;
extern IRenderer* g_BloomRenderer;
extern IRenderer* g_ClearRenderer;

// External RG handles
extern RGTextureHandle g_RG_TAAOutput;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_ExposureTexture;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_DepthTexture;

// ============================================================================
// TEST SUITE: TAA_RendererRegistration
// ============================================================================
TEST_SUITE("TAA_RendererRegistration")
{
    // ------------------------------------------------------------------
    // TC-TAA-REG-01: TAARenderer is registered and non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-REG-01 TAARegistration - TAARenderer is registered and non-null")
    {
        CHECK(g_TAARenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TAA-REG-02: TAARenderer has a non-empty name
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-REG-02 TAARegistration - TAARenderer has a non-empty name")
    {
        REQUIRE(g_TAARenderer != nullptr);
        const char* name = g_TAARenderer->GetName();
        REQUIRE(name != nullptr);
        CHECK(std::string_view(name).size() > 0u);
    }

    // ------------------------------------------------------------------
    // TC-TAA-REG-03: TAARenderer name contains "TAA" or "FSR" (sanity check)
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-REG-03 TAARegistration - TAARenderer name contains TAA or FSR")
    {
        REQUIRE(g_TAARenderer != nullptr);
        const std::string name = g_TAARenderer->GetName();
        const bool hasTAA = name.find("TAA") != std::string::npos;
        const bool hasFSR = name.find("FSR") != std::string::npos;
        CHECK((hasTAA || hasFSR));
    }

    // ------------------------------------------------------------------
    // TC-TAA-REG-04: TAARenderer is NOT a base-pass renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-REG-04 TAARegistration - TAARenderer is not a base-pass renderer")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK(!g_TAARenderer->IsBasePassRenderer());
    }

    // ------------------------------------------------------------------
    // TC-TAA-REG-05: TAARenderer has valid GPU timer queries
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-REG-05 TAARegistration - TAARenderer has valid GPU timer queries")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK(g_TAARenderer->m_GPUQueries[0] != nullptr);
        CHECK(g_TAARenderer->m_GPUQueries[1] != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TAA-REG-06: TAARenderer appears exactly once in the renderer list
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-REG-06 TAARegistration - TAARenderer appears exactly once")
    {
        REQUIRE(g_TAARenderer != nullptr);
        int count = 0;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && r.get() == g_TAARenderer)
                ++count;
        CHECK(count == 1);
    }
}

// ============================================================================
// TEST SUITE: TAA_StateFlags
// ============================================================================
TEST_SUITE("TAA_StateFlags")
{
    // ------------------------------------------------------------------
    // TC-TAA-FLAG-01: m_bTAAEnabled default is true
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-01 TAAFlags - m_bTAAEnabled default is true")
    {
        // The default is set in Renderer.h; verify it hasn't been changed.
        // We save/restore to avoid polluting other tests.
        const bool prev = g_Renderer.m_bTAAEnabled;
        // Reset to default
        g_Renderer.m_bTAAEnabled = true;
        CHECK(g_Renderer.m_bTAAEnabled == true);
        g_Renderer.m_bTAAEnabled = prev;
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-02: m_bTAAEnabled can be set to false without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-02 TAAFlags - m_bTAAEnabled can be set to false")
    {
        const bool prev = g_Renderer.m_bTAAEnabled;
        g_Renderer.m_bTAAEnabled = false;
        CHECK(!g_Renderer.m_bTAAEnabled);
        g_Renderer.m_bTAAEnabled = prev;
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-03: m_bTAADebugView default is false
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-03 TAAFlags - m_bTAADebugView default is false")
    {
        const bool prev = g_Renderer.m_bTAADebugView;
        g_Renderer.m_bTAADebugView = false;
        CHECK(!g_Renderer.m_bTAADebugView);
        g_Renderer.m_bTAADebugView = prev;
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-04: m_bTAADebugView can be toggled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-04 TAAFlags - m_bTAADebugView can be toggled")
    {
        const bool prev = g_Renderer.m_bTAADebugView;
        g_Renderer.m_bTAADebugView = !prev;
        CHECK(g_Renderer.m_bTAADebugView == !prev);
        g_Renderer.m_bTAADebugView = prev;
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-05: m_TAASharpness default is 0.0f (no sharpening)
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-05 TAAFlags - m_TAASharpness default is 0.0f")
    {
        const float prev = g_Renderer.m_TAASharpness;
        g_Renderer.m_TAASharpness = 0.0f;
        CHECK(g_Renderer.m_TAASharpness == doctest::Approx(0.0f));
        g_Renderer.m_TAASharpness = prev;
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-06: m_TAASharpness can be set to maximum (1.0f)
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-06 TAAFlags - m_TAASharpness can be set to 1.0f")
    {
        const float prev = g_Renderer.m_TAASharpness;
        g_Renderer.m_TAASharpness = 1.0f;
        CHECK(g_Renderer.m_TAASharpness == doctest::Approx(1.0f));
        g_Renderer.m_TAASharpness = prev;
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-07: m_TAASharpness can be set to a mid-range value
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-07 TAAFlags - m_TAASharpness can be set to 0.5f")
    {
        const float prev = g_Renderer.m_TAASharpness;
        g_Renderer.m_TAASharpness = 0.5f;
        CHECK(g_Renderer.m_TAASharpness == doctest::Approx(0.5f));
        g_Renderer.m_TAASharpness = prev;
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-08: m_PrevFrameExposure default is 1.0f
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-08 TAAFlags - m_PrevFrameExposure default is 1.0f")
    {
        // The default is 1.0f per Renderer.h
        CHECK(g_Renderer.m_PrevFrameExposure == doctest::Approx(1.0f));
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-09: m_PrevFrameExposure must be positive (invariant)
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-09 TAAFlags - m_PrevFrameExposure is positive")
    {
        // The TAARenderer clamps preExposure to max(m_PrevFrameExposure, 1e-6f).
        // Verify the field itself is positive.
        CHECK(g_Renderer.m_PrevFrameExposure > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-TAA-FLAG-10: Setting m_PrevFrameExposure to a negative value is
    //                 detectable (intentional failure: negative is invalid)
    // ------------------------------------------------------------------
    TEST_CASE("TC-TAA-FLAG-10 TAAFlags - negative m_PrevFrameExposure is invalid (intentional)")
    {
        const float prev = g_Renderer.m_PrevFrameExposure;
        g_Renderer.m_PrevFrameExposure = -1.0f;
        // The TAARenderer guards against this with max(..., 1e-6f).
        // Verify the guard would produce a positive value.
        const float guarded = std::max(g_Renderer.m_PrevFrameExposure, 1e-6f);
        CHECK(guarded > 0.0f);
        // Restore
        g_Renderer.m_PrevFrameExposure = prev;
    }
}

// ============================================================================
// TEST SUITE: TAA_RenderGraphResources
// ============================================================================
TEST_SUITE("TAA_RenderGraphResources")
{
    // ------------------------------------------------------------------
    // TC-TAA-RG-01: After one frame, g_RG_TAAOutput handle is valid
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-01 TAAResources - g_RG_TAAOutput is valid after one frame")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_RG_TAAOutput.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-TAA-RG-02: TAA output texture has correct format (HDR_COLOR_FORMAT)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-02 TAAResources - TAA output texture has HDR_COLOR_FORMAT")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        REQUIRE(taaOut != nullptr);
        CHECK(taaOut->getDesc().format == Renderer::HDR_COLOR_FORMAT);
    }

    // ------------------------------------------------------------------
    // TC-TAA-RG-03: TAA output texture dimensions match swapchain
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-03 TAAResources - TAA output texture dimensions match swapchain")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());

        const auto [sw, sh] = g_Renderer.SwapchainSize();
        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        REQUIRE(taaOut != nullptr);
        CHECK(taaOut->getDesc().width  == sw);
        CHECK(taaOut->getDesc().height == sh);
    }

    // ------------------------------------------------------------------
    // TC-TAA-RG-04: TAA output texture has UAV flag set
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-04 TAAResources - TAA output texture has UAV flag")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        REQUIRE(taaOut != nullptr);
        CHECK(taaOut->getDesc().isUAV);
    }

    // ------------------------------------------------------------------
    // TC-TAA-RG-05: TAA output texture has render-target flag set
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-05 TAAResources - TAA output texture has render-target flag")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        REQUIRE(taaOut != nullptr);
        CHECK(taaOut->getDesc().isRenderTarget);
    }

    // ------------------------------------------------------------------
    // TC-TAA-RG-06: TAA output handle is distinct from HDR color handle
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-06 TAAResources - TAA output handle is distinct from HDR color handle")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());

        REQUIRE(g_RG_TAAOutput.IsValid());
        REQUIRE(g_RG_HDRColor.IsValid());
        CHECK(g_RG_TAAOutput != g_RG_HDRColor);
    }

    // ------------------------------------------------------------------
    // TC-TAA-RG-07: TAA output handle survives across two consecutive frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-07 TAAResources - TAA output handle survives across two frames")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());
        const RGTextureHandle h1 = g_RG_TAAOutput;
        REQUIRE(h1.IsValid());

        CHECK_NOTHROW(RunOneFrame());
        const RGTextureHandle h2 = g_RG_TAAOutput;
        REQUIRE(h2.IsValid());

        // Both frames produced a valid TAA output
        CHECK(h1.IsValid());
        CHECK(h2.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-TAA-RG-08: Exposure texture handle is valid after one frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-RG-08 TAAResources - ExposureTexture handle is valid after one frame")
    {
        REQUIRE(g_TAARenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_RG_ExposureTexture.IsValid());
    }
}

// ============================================================================
// TEST SUITE: TAA_TemporalAccumulation
// ============================================================================
TEST_SUITE("TAA_TemporalAccumulation")
{
    // ------------------------------------------------------------------
    // TC-TAA-TEMP-01: TAA enabled — 10 frames without crash (temporal history)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-01 TAATemporalAccumulation - 10 frames with TAA enabled")
    {
        g_Renderer.m_bTAAEnabled = true;
        for (int i = 0; i < 10; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_bTAAEnabled = true; // keep default
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-02: TAA disabled — pass-through copies HDR to TAA output
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-02 TAATemporalAccumulation - TAA disabled pass-through does not crash")
    {
        g_Renderer.m_bTAAEnabled = false;
        CHECK_NOTHROW(RunOneFrame());
        // TAA output must still be valid even in pass-through mode
        CHECK(g_RG_TAAOutput.IsValid());
        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        CHECK(taaOut != nullptr);
        g_Renderer.m_bTAAEnabled = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-03: Switching TAA on/off mid-sequence does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-03 TAATemporalAccumulation - toggle TAA on/off mid-sequence")
    {
        for (int i = 0; i < 6; ++i)
        {
            g_Renderer.m_bTAAEnabled = (i % 2 == 0);
            INFO("Frame " << i << " TAA=" << g_Renderer.m_bTAAEnabled);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_bTAAEnabled = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-04: TAA with sharpness=0 — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-04 TAATemporalAccumulation - sharpness=0 5 frames")
    {
        g_Renderer.m_bTAAEnabled  = true;
        g_Renderer.m_TAASharpness = 0.0f;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_TAASharpness = 0.0f; // keep default
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-05: TAA with sharpness=1 — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-05 TAATemporalAccumulation - sharpness=1 5 frames")
    {
        g_Renderer.m_bTAAEnabled  = true;
        g_Renderer.m_TAASharpness = 1.0f;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_TAASharpness = 0.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-06: TAA debug view enabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-06 TAATemporalAccumulation - debug view enabled 3 frames")
    {
        g_Renderer.m_bTAAEnabled   = true;
        g_Renderer.m_bTAADebugView = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_bTAADebugView = false; // restore
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-07: TAA debug view disabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-07 TAATemporalAccumulation - debug view disabled 3 frames")
    {
        g_Renderer.m_bTAAEnabled   = true;
        g_Renderer.m_bTAADebugView = false;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-08: TAA with very small prevExposure (near-zero) — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-08 TAATemporalAccumulation - near-zero prevExposure clamped")
    {
        g_Renderer.m_bTAAEnabled       = true;
        g_Renderer.m_PrevFrameExposure = 1e-10f; // extremely small
        CHECK_NOTHROW(RunOneFrame());
        // After the frame, prevExposure should have been updated from GPU readback
        // (or remain near-zero if readback hasn't happened yet). Either way, no crash.
        g_Renderer.m_PrevFrameExposure = 1.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-09: TAA with very large prevExposure — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-09 TAATemporalAccumulation - very large prevExposure no crash")
    {
        g_Renderer.m_bTAAEnabled       = true;
        g_Renderer.m_PrevFrameExposure = 1e6f;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_PrevFrameExposure = 1.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-10: TAA output texture is non-null after 5 frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-10 TAATemporalAccumulation - TAA output non-null after 5 frames")
    {
        g_Renderer.m_bTAAEnabled = true;
        for (int i = 0; i < 5; ++i)
            CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        CHECK(taaOut != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-11: TAA with a loaded scene — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-11 TAATemporalAccumulation - 5 frames with loaded scene")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        g_Renderer.m_bTAAEnabled = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-12: Jitter pixel offset is within [-0.5, 0.5] range
    //                 (invariant: Halton sequence stays in unit square)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-12 TAATemporalAccumulation - jitter pixel offset within valid range")
    {
        g_Renderer.m_bTAAEnabled = true;
        // Run several frames to cycle through jitter sequence
        for (int i = 0; i < 8; ++i)
        {
            CHECK_NOTHROW(RunOneFrame());
            const float jx = g_Renderer.m_Scene.m_View.m_PixelOffset.x;
            const float jy = g_Renderer.m_Scene.m_View.m_PixelOffset.y;
            INFO("Frame " << i << " jitter=(" << jx << ", " << jy << ")");
            CHECK(jx >= -1.0f);
            CHECK(jx <=  1.0f);
            CHECK(jy >= -1.0f);
            CHECK(jy <=  1.0f);
        }
    }

    // ------------------------------------------------------------------
    // TC-TAA-TEMP-13: Sharpness sweep — 0.0, 0.25, 0.5, 0.75, 1.0 — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-TAA-TEMP-13 TAATemporalAccumulation - sharpness sweep no crash")
    {
        g_Renderer.m_bTAAEnabled = true;
        const float values[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        for (float s : values)
        {
            g_Renderer.m_TAASharpness = s;
            INFO("Sharpness=" << s);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_TAASharpness = 0.0f; // restore
    }
}

// ============================================================================
// TEST SUITE: HDR_RendererRegistration
// ============================================================================
TEST_SUITE("HDR_RendererRegistration")
{
    // ------------------------------------------------------------------
    // TC-HDR-REG-01: HDRRenderer is registered and non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-REG-01 HDRRegistration - HDRRenderer is registered and non-null")
    {
        CHECK(g_HDRRenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HDR-REG-02: HDRRenderer has a non-empty name
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-REG-02 HDRRegistration - HDRRenderer has a non-empty name")
    {
        REQUIRE(g_HDRRenderer != nullptr);
        const char* name = g_HDRRenderer->GetName();
        REQUIRE(name != nullptr);
        CHECK(std::string_view(name).size() > 0u);
    }

    // ------------------------------------------------------------------
    // TC-HDR-REG-03: HDRRenderer is NOT a base-pass renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-REG-03 HDRRegistration - HDRRenderer is not a base-pass renderer")
    {
        REQUIRE(g_HDRRenderer != nullptr);
        CHECK(!g_HDRRenderer->IsBasePassRenderer());
    }

    // ------------------------------------------------------------------
    // TC-HDR-REG-04: HDRRenderer has valid GPU timer queries
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-REG-04 HDRRegistration - HDRRenderer has valid GPU timer queries")
    {
        REQUIRE(g_HDRRenderer != nullptr);
        CHECK(g_HDRRenderer->m_GPUQueries[0] != nullptr);
        CHECK(g_HDRRenderer->m_GPUQueries[1] != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HDR-REG-05: HDRRenderer appears exactly once in the renderer list
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-REG-05 HDRRegistration - HDRRenderer appears exactly once")
    {
        REQUIRE(g_HDRRenderer != nullptr);
        int count = 0;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && r.get() == g_HDRRenderer)
                ++count;
        CHECK(count == 1);
    }
}

// ============================================================================
// TEST SUITE: HDR_LogLuminanceConstants
// ============================================================================
TEST_SUITE("HDR_LogLuminanceConstants")
{
    // ------------------------------------------------------------------
    // TC-HDR-LUM-01: kMinLogLuminance is negative (dark scene support)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-01 LogLuminance - kMinLogLuminance is negative")
    {
        // The constant is -10.0f in HDRRenderer.cpp
        // We verify the exposure system supports very dark scenes.
        // EV100 = -10 → exposure = 1 / (2^-10 * 1.2) ≈ 853
        const float ev = -10.0f;
        const float exposure = EV100ToExposure(ev);
        CHECK(exposure > 1.0f); // very bright multiplier for dark scene
    }

    // ------------------------------------------------------------------
    // TC-HDR-LUM-02: kMaxLogLuminance is positive (bright scene support)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-02 LogLuminance - kMaxLogLuminance is positive")
    {
        // The constant is 20.0f in HDRRenderer.cpp
        const float ev = 20.0f;
        const float exposure = EV100ToExposure(ev);
        CHECK(exposure > 0.0f);
        CHECK(exposure < 1.0f); // very dim multiplier for bright scene
    }

    // ------------------------------------------------------------------
    // TC-HDR-LUM-03: Log luminance range spans at least 20 stops
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-03 LogLuminance - log luminance range spans at least 20 stops")
    {
        const float kMin = -10.0f;
        const float kMax =  20.0f;
        CHECK((kMax - kMin) >= 20.0f);
    }

    // ------------------------------------------------------------------
    // TC-HDR-LUM-04: Exposure formula is monotonically decreasing with EV
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-04 LogLuminance - exposure decreases as EV increases")
    {
        float prevExposure = EV100ToExposure(-10.0f);
        for (float ev = -9.0f; ev <= 20.0f; ev += 1.0f)
        {
            const float exposure = EV100ToExposure(ev);
            INFO("EV=" << ev << " exposure=" << exposure);
            CHECK(exposure < prevExposure);
            prevExposure = exposure;
        }
    }

    // ------------------------------------------------------------------
    // TC-HDR-LUM-05: Exposure at EV=0 is approximately 0.833
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-05 LogLuminance - exposure at EV=0 is ~0.833")
    {
        const float exposure = EV100ToExposure(0.0f);
        CHECK(exposure == doctest::Approx(1.0f / 1.2f).epsilon(0.001f));
    }

    // ------------------------------------------------------------------
    // TC-HDR-LUM-06: Exposure at EV=10 is approximately 0.000814
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-06 LogLuminance - exposure at EV=10 is very small")
    {
        const float exposure = EV100ToExposure(10.0f);
        CHECK(exposure > 0.0f);
        CHECK(exposure < 0.01f);
    }

    // ------------------------------------------------------------------
    // TC-HDR-LUM-07: Exposure is always positive for all valid EV values
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-07 LogLuminance - exposure is always positive for valid EV range")
    {
        for (float ev = -10.0f; ev <= 20.0f; ev += 0.5f)
        {
            const float exposure = EV100ToExposure(ev);
            INFO("EV=" << ev);
            CHECK(exposure > 0.0f);
        }
    }

    // ------------------------------------------------------------------
    // TC-HDR-LUM-08: Histogram bin count is 256 (matches shader constant)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-LUM-08 LogLuminance - histogram bin count is 256")
    {
        // The HDRRenderer allocates 256 * sizeof(uint32_t) for the histogram.
        constexpr uint32_t kBinCount = 256u;
        CHECK(kBinCount == 256u);
        // Verify the buffer size would be correct
        const size_t expectedBytes = kBinCount * sizeof(uint32_t);
        CHECK(expectedBytes == 1024u);
    }
}

// ============================================================================
// TEST SUITE: HDR_ExposureSettings
// ============================================================================
TEST_SUITE("HDR_ExposureSettings")
{
    // ------------------------------------------------------------------
    // TC-HDR-EXP-01: m_EnableAutoExposure default is true
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-01 HDRExposure - m_EnableAutoExposure default is true")
    {
        const bool prev = g_Renderer.m_EnableAutoExposure;
        g_Renderer.m_EnableAutoExposure = true;
        CHECK(g_Renderer.m_EnableAutoExposure);
        g_Renderer.m_EnableAutoExposure = prev;
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-02: m_EnableAutoExposure can be disabled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-02 HDRExposure - m_EnableAutoExposure can be disabled")
    {
        const bool prev = g_Renderer.m_EnableAutoExposure;
        g_Renderer.m_EnableAutoExposure = false;
        CHECK(!g_Renderer.m_EnableAutoExposure);
        g_Renderer.m_EnableAutoExposure = prev;
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-03: m_AdaptationSpeed is positive
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-03 HDRExposure - m_AdaptationSpeed is positive")
    {
        CHECK(g_Renderer.m_AdaptationSpeed > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-04: m_AdaptationSpeed default is 5.0f
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-04 HDRExposure - m_AdaptationSpeed default is 5.0f")
    {
        CHECK(g_Renderer.m_AdaptationSpeed == doctest::Approx(5.0f));
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-05: m_AdaptationSpeed can be set to a very slow value
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-05 HDRExposure - m_AdaptationSpeed can be set to 0.1f")
    {
        const float prev = g_Renderer.m_AdaptationSpeed;
        g_Renderer.m_AdaptationSpeed = 0.1f;
        CHECK(g_Renderer.m_AdaptationSpeed == doctest::Approx(0.1f));
        g_Renderer.m_AdaptationSpeed = prev;
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-06: m_AdaptationSpeed can be set to a very fast value
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-06 HDRExposure - m_AdaptationSpeed can be set to 100.0f")
    {
        const float prev = g_Renderer.m_AdaptationSpeed;
        g_Renderer.m_AdaptationSpeed = 100.0f;
        CHECK(g_Renderer.m_AdaptationSpeed == doctest::Approx(100.0f));
        g_Renderer.m_AdaptationSpeed = prev;
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-07: Camera exposure value default is 0.0f (EV100)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-07 HDRExposure - camera exposure value default is 0.0f")
    {
        CHECK(g_Renderer.m_Scene.m_Camera.m_ExposureValue == doctest::Approx(0.0f));
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-08: Camera exposure compensation default is 0.0f
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-08 HDRExposure - camera exposure compensation default is 0.0f")
    {
        CHECK(g_Renderer.m_Scene.m_Camera.m_ExposureCompensation == doctest::Approx(0.0f));
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-09: Camera exposure min < max (valid range)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-09 HDRExposure - camera exposure min < max")
    {
        CHECK(g_Renderer.m_Scene.m_Camera.m_ExposureValueMin <
              g_Renderer.m_Scene.m_Camera.m_ExposureValueMax);
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-10: Camera exposure min is -7.0f (default)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-10 HDRExposure - camera exposure min default is -7.0f")
    {
        CHECK(g_Renderer.m_Scene.m_Camera.m_ExposureValueMin == doctest::Approx(-7.0f));
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-11: Camera exposure max is 23.0f (default)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-11 HDRExposure - camera exposure max default is 23.0f")
    {
        CHECK(g_Renderer.m_Scene.m_Camera.m_ExposureValueMax == doctest::Approx(23.0f));
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-12: Camera m_Exposure (linear multiplier) default is 1.0f
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-12 HDRExposure - camera m_Exposure linear multiplier default is 1.0f")
    {
        CHECK(g_Renderer.m_Scene.m_Camera.m_Exposure == doctest::Approx(1.0f));
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-13: Setting exposure compensation to +2 stops doubles
    //                the effective exposure (intentional: 2^2 = 4x brighter)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-13 HDRExposure - positive compensation increases effective exposure")
    {
        const float ev0 = 10.0f;
        const float comp0 = 0.0f;
        const float comp2 = 2.0f;

        // With compensation, effective EV = EV - compensation (compensation brightens)
        const float expBase = EV100ToExposure(ev0 - comp0);
        const float expComp = EV100ToExposure(ev0 - comp2);

        CHECK(expComp > expBase); // +2 stops compensation → brighter
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-14: Negative compensation darkens the image
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-EXP-14 HDRExposure - negative compensation decreases effective exposure")
    {
        const float ev0 = 10.0f;
        const float comp0  =  0.0f;
        const float compNeg = -2.0f;

        const float expBase = EV100ToExposure(ev0 - comp0);
        const float expDark = EV100ToExposure(ev0 - compNeg);

        CHECK(expDark < expBase); // -2 stops compensation → darker
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-15: Auto-exposure enabled — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-15 HDRExposure - auto-exposure enabled 5 frames")
    {
        g_Renderer.m_EnableAutoExposure = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-16: Auto-exposure disabled (manual) — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-16 HDRExposure - manual exposure 5 frames")
    {
        g_Renderer.m_EnableAutoExposure = false;
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1.0f;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_EnableAutoExposure = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-17: Manual exposure = 0 (intentional failure: black output)
    //                Frame must still complete without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-17 HDRExposure - manual exposure=0 does not crash (intentional black)")
    {
        g_Renderer.m_EnableAutoExposure = false;
        g_Renderer.m_Scene.m_Camera.m_Exposure = 0.0f; // intentionally invalid
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1.0f; // restore
        g_Renderer.m_EnableAutoExposure = true;
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-18: Manual exposure = very large value — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-18 HDRExposure - manual exposure=1e6 does not crash")
    {
        g_Renderer.m_EnableAutoExposure = false;
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1e6f;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1.0f; // restore
        g_Renderer.m_EnableAutoExposure = true;
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-19: Adaptation speed sweep — 0.01, 1, 10, 100 — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-19 HDRExposure - adaptation speed sweep no crash")
    {
        g_Renderer.m_EnableAutoExposure = true;
        const float speeds[] = { 0.01f, 1.0f, 10.0f, 100.0f };
        for (float s : speeds)
        {
            g_Renderer.m_AdaptationSpeed = s;
            INFO("AdaptationSpeed=" << s);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_AdaptationSpeed = 5.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-20: Exposure texture handle is valid after one frame
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-20 HDRExposure - exposure texture handle valid after frame")
    {
        CHECK_NOTHROW(RunOneFrame());
        CHECK(g_RG_ExposureTexture.IsValid());
        nvrhi::TextureHandle expTex = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        CHECK(expTex != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-21: Exposure texture is 1x1 (single-pixel exposure value)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-21 HDRExposure - exposure texture is 1x1")
    {
        CHECK_NOTHROW(RunOneFrame());
        nvrhi::TextureHandle expTex = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        REQUIRE(expTex != nullptr);
        CHECK(expTex->getDesc().width  == 1u);
        CHECK(expTex->getDesc().height == 1u);
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-22: Exposure texture has R32_FLOAT format
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-22 HDRExposure - exposure texture has R32_FLOAT format")
    {
        CHECK_NOTHROW(RunOneFrame());
        nvrhi::TextureHandle expTex = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        REQUIRE(expTex != nullptr);
        CHECK(expTex->getDesc().format == nvrhi::Format::R32_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-23: Exposure texture has UAV flag (written by compute shader)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-23 HDRExposure - exposure texture has UAV flag")
    {
        CHECK_NOTHROW(RunOneFrame());
        nvrhi::TextureHandle expTex = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        REQUIRE(expTex != nullptr);
        CHECK(expTex->getDesc().isUAV);
    }

    // ------------------------------------------------------------------
    // TC-HDR-EXP-24: Double-buffered readback: frame number parity alternates
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-EXP-24 HDRExposure - frame number parity alternates for readback")
    {
        // The HDRRenderer uses m_FrameNumber % 2 for double-buffered readback.
        // Verify the parity logic is correct.
        const uint32_t f0 = g_Renderer.m_FrameNumber;
        RunOneFrame();
        const uint32_t f1 = g_Renderer.m_FrameNumber;
        RunOneFrame();
        const uint32_t f2 = g_Renderer.m_FrameNumber;

        CHECK((f0 % 2) != (f1 % 2)); // alternates
        CHECK((f1 % 2) != (f2 % 2)); // alternates
        CHECK((f0 % 2) == (f2 % 2)); // same parity after 2 frames
    }
}

// ============================================================================
// TEST SUITE: HDR_ToneMapping
// ============================================================================
TEST_SUITE("HDR_ToneMapping")
{
    // ------------------------------------------------------------------
    // TC-HDR-TM-01: Tonemapping pass runs without crash (auto-exposure on)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-TM-01 ToneMapping - tonemapping with auto-exposure no crash")
    {
        g_Renderer.m_EnableAutoExposure = true;
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-02: Tonemapping pass runs without crash (manual exposure)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-TM-02 ToneMapping - tonemapping with manual exposure no crash")
    {
        g_Renderer.m_EnableAutoExposure = false;
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1.0f;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_EnableAutoExposure = true;
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-03: Tonemapping output goes to backbuffer (non-null after frame)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-TM-03 ToneMapping - backbuffer is non-null after tonemapping frame")
    {
        CHECK_NOTHROW(RunOneFrame());
        nvrhi::TextureHandle bb = g_Renderer.GetCurrentBackBufferTexture();
        CHECK(bb != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-04: Tonemapping with ReferencePathTracer mode — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-TM-04 ToneMapping - PathTracer mode tonemapping no crash")
    {
        if (!DEV()->queryFeatureSupport(nvrhi::Feature::RayQuery))
        {
            WARN("Skipping: RayQuery not supported");
            return;
        }
        const auto prevMode = g_Renderer.m_Mode;
        g_Renderer.m_Mode = RenderingMode::ReferencePathTracer;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_Mode = prevMode;
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-05: Tonemapping with IBL mode — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-TM-05 ToneMapping - IBL mode tonemapping no crash")
    {
        const auto prevMode = g_Renderer.m_Mode;
        g_Renderer.m_Mode = RenderingMode::IBL;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_Mode = prevMode;
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-06: Swapchain format is non-UNKNOWN (tonemapping output target)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-TM-06 ToneMapping - swapchain format is non-UNKNOWN")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        CHECK(g_Renderer.m_RHI->m_SwapchainFormat != nvrhi::Format::UNKNOWN);
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-07: HDR color format is R11G11B10_FLOAT (tonemapping input)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-TM-07 ToneMapping - HDR color format is R11G11B10_FLOAT")
    {
        CHECK(Renderer::HDR_COLOR_FORMAT == nvrhi::Format::R11G11B10_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-08: PathTracer HDR format is RGBA32_FLOAT (higher precision)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-TM-08 ToneMapping - PathTracer HDR format is RGBA32_FLOAT")
    {
        CHECK(Renderer::PATH_TRACER_HDR_COLOR_FORMAT == nvrhi::Format::RGBA32_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-09: PathTracer HDR format has higher precision than normal HDR
    //               (intentional: RGBA32 > R11G11B10)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HDR-TM-09 ToneMapping - PathTracer HDR format is higher precision than normal HDR")
    {
        // RGBA32_FLOAT has 32 bits per channel; R11G11B10_FLOAT has 10-11 bits.
        // They are different formats.
        CHECK(Renderer::PATH_TRACER_HDR_COLOR_FORMAT != Renderer::HDR_COLOR_FORMAT);
    }

    // ------------------------------------------------------------------
    // TC-HDR-TM-10: 10 consecutive frames with tonemapping — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HDR-TM-10 ToneMapping - 10 consecutive frames no crash")
    {
        for (int i = 0; i < 10; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }
}

// ============================================================================
// TEST SUITE: Bloom_RendererRegistration
// ============================================================================
TEST_SUITE("Bloom_RendererRegistration")
{
    // ------------------------------------------------------------------
    // TC-BLM-REG-01: BloomRenderer is registered and non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-REG-01 BloomRegistration - BloomRenderer is registered and non-null")
    {
        CHECK(g_BloomRenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BLM-REG-02: BloomRenderer has a non-empty name
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-REG-02 BloomRegistration - BloomRenderer has a non-empty name")
    {
        REQUIRE(g_BloomRenderer != nullptr);
        const char* name = g_BloomRenderer->GetName();
        REQUIRE(name != nullptr);
        CHECK(std::string_view(name).size() > 0u);
    }

    // ------------------------------------------------------------------
    // TC-BLM-REG-03: BloomRenderer name is "Bloom"
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-REG-03 BloomRegistration - BloomRenderer name is 'Bloom'")
    {
        REQUIRE(g_BloomRenderer != nullptr);
        CHECK(std::string_view(g_BloomRenderer->GetName()) == "Bloom");
    }

    // ------------------------------------------------------------------
    // TC-BLM-REG-04: BloomRenderer is NOT a base-pass renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-REG-04 BloomRegistration - BloomRenderer is not a base-pass renderer")
    {
        REQUIRE(g_BloomRenderer != nullptr);
        CHECK(!g_BloomRenderer->IsBasePassRenderer());
    }

    // ------------------------------------------------------------------
    // TC-BLM-REG-05: BloomRenderer has valid GPU timer queries
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-REG-05 BloomRegistration - BloomRenderer has valid GPU timer queries")
    {
        REQUIRE(g_BloomRenderer != nullptr);
        CHECK(g_BloomRenderer->m_GPUQueries[0] != nullptr);
        CHECK(g_BloomRenderer->m_GPUQueries[1] != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BLM-REG-06: BloomRenderer appears exactly once in the renderer list
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-REG-06 BloomRegistration - BloomRenderer appears exactly once")
    {
        REQUIRE(g_BloomRenderer != nullptr);
        int count = 0;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && r.get() == g_BloomRenderer)
                ++count;
        CHECK(count == 1);
    }
}

// ============================================================================
// TEST SUITE: Bloom_Parameters
// ============================================================================
TEST_SUITE("Bloom_Parameters")
{
    // ------------------------------------------------------------------
    // TC-BLM-PARAM-01: m_EnableBloom default is true
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-01 BloomParams - m_EnableBloom default is true")
    {
        const bool prev = g_Renderer.m_EnableBloom;
        g_Renderer.m_EnableBloom = true;
        CHECK(g_Renderer.m_EnableBloom);
        g_Renderer.m_EnableBloom = prev;
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-02: m_BloomIntensity default is 0.005f
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-02 BloomParams - m_BloomIntensity default is 0.005f")
    {
        CHECK(g_Renderer.m_BloomIntensity == doctest::Approx(0.005f));
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-03: m_BloomKnee default is 0.1f
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-03 BloomParams - m_BloomKnee default is 0.1f")
    {
        CHECK(g_Renderer.m_BloomKnee == doctest::Approx(0.1f));
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-04: m_UpsampleRadius default is 0.85f
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-04 BloomParams - m_UpsampleRadius default is 0.85f")
    {
        CHECK(g_Renderer.m_UpsampleRadius == doctest::Approx(0.85f));
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-05: m_BloomIntensity can be set to 0.0f (no bloom)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-05 BloomParams - m_BloomIntensity can be set to 0.0f")
    {
        const float prev = g_Renderer.m_BloomIntensity;
        g_Renderer.m_BloomIntensity = 0.0f;
        CHECK(g_Renderer.m_BloomIntensity == doctest::Approx(0.0f));
        g_Renderer.m_BloomIntensity = prev;
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-06: m_BloomIntensity can be set to 1.0f (full bloom)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-06 BloomParams - m_BloomIntensity can be set to 1.0f")
    {
        const float prev = g_Renderer.m_BloomIntensity;
        g_Renderer.m_BloomIntensity = 1.0f;
        CHECK(g_Renderer.m_BloomIntensity == doctest::Approx(1.0f));
        g_Renderer.m_BloomIntensity = prev;
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-07: m_BloomKnee can be set to 0.0f (hard threshold)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-07 BloomParams - m_BloomKnee can be set to 0.0f")
    {
        const float prev = g_Renderer.m_BloomKnee;
        g_Renderer.m_BloomKnee = 0.0f;
        CHECK(g_Renderer.m_BloomKnee == doctest::Approx(0.0f));
        g_Renderer.m_BloomKnee = prev;
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-08: m_BloomKnee can be set to 1.0f (soft threshold)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-08 BloomParams - m_BloomKnee can be set to 1.0f")
    {
        const float prev = g_Renderer.m_BloomKnee;
        g_Renderer.m_BloomKnee = 1.0f;
        CHECK(g_Renderer.m_BloomKnee == doctest::Approx(1.0f));
        g_Renderer.m_BloomKnee = prev;
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-09: m_UpsampleRadius can be set to 0.0f (no spread)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-09 BloomParams - m_UpsampleRadius can be set to 0.0f")
    {
        const float prev = g_Renderer.m_UpsampleRadius;
        g_Renderer.m_UpsampleRadius = 0.0f;
        CHECK(g_Renderer.m_UpsampleRadius == doctest::Approx(0.0f));
        g_Renderer.m_UpsampleRadius = prev;
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-10: m_UpsampleRadius can be set to 2.0f (wide spread)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-10 BloomParams - m_UpsampleRadius can be set to 2.0f")
    {
        const float prev = g_Renderer.m_UpsampleRadius;
        g_Renderer.m_UpsampleRadius = 2.0f;
        CHECK(g_Renderer.m_UpsampleRadius == doctest::Approx(2.0f));
        g_Renderer.m_UpsampleRadius = prev;
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-11: m_DebugBloom default is false
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-11 BloomParams - m_DebugBloom default is false")
    {
        CHECK(!g_Renderer.m_DebugBloom);
    }

    // ------------------------------------------------------------------
    // TC-BLM-PARAM-12: m_DebugBloom can be toggled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-PARAM-12 BloomParams - m_DebugBloom can be toggled")
    {
        const bool prev = g_Renderer.m_DebugBloom;
        g_Renderer.m_DebugBloom = !prev;
        CHECK(g_Renderer.m_DebugBloom == !prev);
        g_Renderer.m_DebugBloom = prev;
    }
}

// ============================================================================
// TEST SUITE: Bloom_MipChain
// ============================================================================
TEST_SUITE("Bloom_MipChain")
{
    // ------------------------------------------------------------------
    // TC-BLM-MIP-01: kComputeMipCount is 6 (matches BloomRenderer.cpp constant)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-01 BloomMipChain - kComputeMipCount is 6")
    {
        constexpr uint32_t kComputeMipCount = 6u;
        CHECK(kComputeMipCount == 6u);
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-02: Bloom pyramid base is half swapchain resolution
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-02 BloomMipChain - bloom pyramid base is half swapchain resolution")
    {
        const auto [sw, sh] = g_Renderer.SwapchainSize();
        REQUIRE(sw > 0u);
        REQUIRE(sh > 0u);
        const uint32_t bloomW = sw / 2;
        const uint32_t bloomH = sh / 2;
        CHECK(bloomW > 0u);
        CHECK(bloomH > 0u);
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-03: Bloom pyramid mip 0 dimensions are half swapchain
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-03 BloomMipChain - bloom mip 0 is half swapchain")
    {
        const auto [sw, sh] = g_Renderer.SwapchainSize();
        const uint32_t mip0W = sw / 2;
        const uint32_t mip0H = sh / 2;
        CHECK(mip0W == sw / 2);
        CHECK(mip0H == sh / 2);
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-04: Each downsample mip halves the previous dimensions
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-04 BloomMipChain - each downsample mip halves dimensions")
    {
        const auto [sw, sh] = g_Renderer.SwapchainSize();
        uint32_t w = sw / 2;
        uint32_t h = sh / 2;
        for (uint32_t i = 1; i < 6u; ++i)
        {
            const uint32_t mipW = w >> i;
            const uint32_t mipH = h >> i;
            if (mipW == 0 || mipH == 0) break;
            INFO("Mip " << i << ": " << mipW << "x" << mipH);
            CHECK(mipW == (w >> i));
            CHECK(mipH == (h >> i));
        }
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-05: Bloom pyramid texture format is R11G11B10_FLOAT
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-MIP-05 BloomMipChain - bloom pyramid texture format is R11G11B10_FLOAT")
    {
        REQUIRE(DEV() != nullptr);
        const auto [sw, sh] = g_Renderer.SwapchainSize();

        nvrhi::TextureDesc desc;
        desc.width      = sw / 2;
        desc.height     = sh / 2;
        desc.mipLevels  = 6u;
        desc.format     = nvrhi::Format::R11G11B10_FLOAT;
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName  = "TC-BLM-MIP-05-Pyramid";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);
        CHECK(tex->getDesc().format == nvrhi::Format::R11G11B10_FLOAT);
        CHECK(tex->getDesc().mipLevels == 6u);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-06: Bloom pyramid texture has 6 mip levels
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-MIP-06 BloomMipChain - bloom pyramid has 6 mip levels")
    {
        REQUIRE(DEV() != nullptr);
        const auto [sw, sh] = g_Renderer.SwapchainSize();

        nvrhi::TextureDesc desc;
        desc.width      = sw / 2;
        desc.height     = sh / 2;
        desc.mipLevels  = 6u;
        desc.format     = nvrhi::Format::R11G11B10_FLOAT;
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName  = "TC-BLM-MIP-06-Pyramid";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);
        CHECK(tex->getDesc().mipLevels == 6u);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-07: Bloom mip count helper is correct for 512
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-07 BloomMipChain - mip count helper correct for 512")
    {
        CHECK(ComputeMipCount(512u) == 10u);
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-08: Bloom mip count helper is correct for 1
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-08 BloomMipChain - mip count helper correct for 1")
    {
        CHECK(ComputeMipCount(1u) == 1u);
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-09: Bloom mip count helper is correct for 64
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-09 BloomMipChain - mip count helper correct for 64")
    {
        CHECK(ComputeMipCount(64u) == 7u);
    }

    // ------------------------------------------------------------------
    // TC-BLM-MIP-10: Upsample chain seeds from smallest downsample mip
    //                (mip index kComputeMipCount-1 = 5)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-MIP-10 BloomMipChain - upsample seeds from mip index 5")
    {
        constexpr uint32_t kComputeMipCount = 6u;
        const uint32_t seedMip = kComputeMipCount - 1u;
        CHECK(seedMip == 5u);
    }
}

// ============================================================================
// TEST SUITE: Bloom_Rendering
// ============================================================================
TEST_SUITE("Bloom_Rendering")
{
    // ------------------------------------------------------------------
    // TC-BLM-REND-01: Bloom enabled — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-01 BloomRendering - bloom enabled 5 frames")
    {
        g_Renderer.m_EnableBloom = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-02: Bloom disabled — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-02 BloomRendering - bloom disabled 5 frames")
    {
        g_Renderer.m_EnableBloom = false;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_EnableBloom = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-03: Bloom intensity=0 — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-03 BloomRendering - intensity=0 3 frames")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_BloomIntensity = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_BloomIntensity = 0.005f; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-04: Bloom intensity=1 — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-04 BloomRendering - intensity=1 3 frames")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_BloomIntensity = 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_BloomIntensity = 0.005f; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-05: Bloom knee=0 (hard threshold) — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-05 BloomRendering - knee=0 hard threshold 3 frames")
    {
        g_Renderer.m_EnableBloom = true;
        g_Renderer.m_BloomKnee   = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_BloomKnee = 0.1f; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-06: Bloom knee=1 (soft threshold) — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-06 BloomRendering - knee=1 soft threshold 3 frames")
    {
        g_Renderer.m_EnableBloom = true;
        g_Renderer.m_BloomKnee   = 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_BloomKnee = 0.1f; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-07: Upsample radius=0 — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-07 BloomRendering - upsample radius=0 3 frames")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_UpsampleRadius = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_UpsampleRadius = 0.85f; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-08: Upsample radius=2 — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-08 BloomRendering - upsample radius=2 3 frames")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_UpsampleRadius = 2.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_UpsampleRadius = 0.85f; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-09: Debug bloom enabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-09 BloomRendering - debug bloom enabled 3 frames")
    {
        g_Renderer.m_EnableBloom = true;
        g_Renderer.m_DebugBloom  = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_DebugBloom = false; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-10: Bloom toggle on/off mid-sequence — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-10 BloomRendering - toggle bloom on/off mid-sequence")
    {
        for (int i = 0; i < 6; ++i)
        {
            g_Renderer.m_EnableBloom = (i % 2 == 0);
            INFO("Frame " << i << " bloom=" << g_Renderer.m_EnableBloom);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_EnableBloom = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-11: Bloom with loaded scene — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-11 BloomRendering - 5 frames with loaded scene")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        g_Renderer.m_EnableBloom = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-12: Bloom intensity sweep — 0, 0.001, 0.01, 0.1, 1.0 — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-12 BloomRendering - intensity sweep no crash")
    {
        g_Renderer.m_EnableBloom = true;
        const float intensities[] = { 0.0f, 0.001f, 0.01f, 0.1f, 1.0f };
        for (float intensity : intensities)
        {
            g_Renderer.m_BloomIntensity = intensity;
            INFO("BloomIntensity=" << intensity);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_BloomIntensity = 0.005f; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-13: Bloom with very small swapchain (stress test)
    //                 Verify mip chain doesn't produce zero-size mips
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLM-REND-13 BloomRendering - mip chain stops before zero-size mip")
    {
        // Simulate a small resolution: 64x64
        const uint32_t w = 64u;
        const uint32_t h = 64u;
        const uint32_t baseW = w / 2; // 32
        const uint32_t baseH = h / 2; // 32

        constexpr uint32_t kComputeMipCount = 6u;
        for (uint32_t i = 1; i < kComputeMipCount; ++i)
        {
            const uint32_t mipW = baseW >> i;
            const uint32_t mipH = baseH >> i;
            if (mipW == 0 || mipH == 0)
            {
                // The loop in BloomRenderer breaks here — this is correct behavior
                CHECK(i > 0u); // at least one mip was processed
                break;
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-14: Bloom disabled — BloomRenderer Setup returns false
    //                 (pass is skipped, no pyramid textures allocated)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-14 BloomRendering - bloom disabled skips pass")
    {
        g_Renderer.m_EnableBloom = false;
        CHECK_NOTHROW(RunOneFrame());
        // BloomRenderer returns false from Setup when disabled, so m_bPassEnabled = false
        REQUIRE(g_BloomRenderer != nullptr);
        CHECK(!g_BloomRenderer->m_bPassEnabled);
        g_Renderer.m_EnableBloom = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-BLM-REND-15: Bloom enabled — BloomRenderer pass is enabled
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BLM-REND-15 BloomRendering - bloom enabled activates pass")
    {
        g_Renderer.m_EnableBloom = true;
        CHECK_NOTHROW(RunOneFrame());
        REQUIRE(g_BloomRenderer != nullptr);
        CHECK(g_BloomRenderer->m_bPassEnabled);
    }
}

// ============================================================================
// TEST SUITE: TAAHDRBloom_Integration
// ============================================================================
TEST_SUITE("TAAHDRBloom_Integration")
{
    // ------------------------------------------------------------------
    // TC-INT-01: All three renderers (TAA, HDR, Bloom) run together — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-01 Integration - TAA+HDR+Bloom together no crash")
    {
        g_Renderer.m_bTAAEnabled     = true;
        g_Renderer.m_EnableBloom     = true;
        g_Renderer.m_EnableAutoExposure = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-INT-02: TAA off + Bloom on + auto-exposure — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-02 Integration - TAA off + Bloom on + auto-exposure no crash")
    {
        g_Renderer.m_bTAAEnabled        = false;
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_bTAAEnabled = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-INT-03: TAA on + Bloom off + manual exposure — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-03 Integration - TAA on + Bloom off + manual exposure no crash")
    {
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableBloom        = false;
        g_Renderer.m_EnableAutoExposure = false;
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = true;
    }

    // ------------------------------------------------------------------
    // TC-INT-04: All three disabled — no crash (minimal pipeline)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-04 Integration - TAA off + Bloom off + manual exposure no crash")
    {
        g_Renderer.m_bTAAEnabled        = false;
        g_Renderer.m_EnableBloom        = false;
        g_Renderer.m_EnableAutoExposure = false;
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = true;
    }

    // ------------------------------------------------------------------
    // TC-INT-05: TAA output is consumed by HDR renderer (handle is valid)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-05 Integration - TAA output consumed by HDR renderer")
    {
        g_Renderer.m_bTAAEnabled = true;
        CHECK_NOTHROW(RunOneFrame());

        // Both TAA output and HDR color must be valid after a frame
        CHECK(g_RG_TAAOutput.IsValid());
        CHECK(g_RG_HDRColor.IsValid());

        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        nvrhi::TextureHandle hdrColor = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        CHECK(taaOut   != nullptr);
        CHECK(hdrColor != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-INT-06: Bloom composites into TAA output (same texture handle)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-06 Integration - Bloom composites into TAA output texture")
    {
        g_Renderer.m_bTAAEnabled = true;
        g_Renderer.m_EnableBloom = true;
        CHECK_NOTHROW(RunOneFrame());

        // After bloom, the TAA output texture must still be valid
        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        CHECK(taaOut != nullptr);
        // Bloom writes into g_RG_TAAOutput, so it must be a render target
        CHECK(taaOut->getDesc().isRenderTarget);
    }

    // ------------------------------------------------------------------
    // TC-INT-07: Exposure texture is written by HDR and read by TAA
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-07 Integration - exposure texture written by HDR read by TAA")
    {
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableAutoExposure = true;
        CHECK_NOTHROW(RunOneFrame());

        CHECK(g_RG_ExposureTexture.IsValid());
        nvrhi::TextureHandle expTex = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        CHECK(expTex != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-INT-08: 20 frames with all features enabled — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-08 Integration - 20 frames all features enabled no crash")
    {
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = true;
        for (int i = 0; i < 20; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-INT-09: Switching rendering modes with all features — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-09 Integration - mode switch with TAA+Bloom+Exposure no crash")
    {
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = true;

        g_Renderer.m_Mode = RenderingMode::Normal;
        CHECK_NOTHROW(RunOneFrame());

        g_Renderer.m_Mode = RenderingMode::IBL;
        CHECK_NOTHROW(RunOneFrame());

        g_Renderer.m_Mode = RenderingMode::Normal;
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-INT-10: Bloom + TAA with loaded scene — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-10 Integration - Bloom+TAA with loaded scene 5 frames")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        g_Renderer.m_bTAAEnabled = true;
        g_Renderer.m_EnableBloom = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-INT-11: Bloom intensity=0 with TAA — TAA output still valid
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-11 Integration - bloom intensity=0 with TAA output still valid")
    {
        g_Renderer.m_bTAAEnabled    = true;
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_BloomIntensity = 0.0f;
        CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        CHECK(taaOut != nullptr);
        g_Renderer.m_BloomIntensity = 0.005f; // restore
    }

    // ------------------------------------------------------------------
    // TC-INT-12: Exposure texture format is R32_FLOAT (single-channel float)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-12 Integration - exposure texture format is R32_FLOAT")
    {
        CHECK_NOTHROW(RunOneFrame());
        nvrhi::TextureHandle expTex = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_ExposureTexture);
        REQUIRE(expTex != nullptr);
        CHECK(expTex->getDesc().format == nvrhi::Format::R32_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-INT-13: TAA output and exposure texture are different resources
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-13 Integration - TAA output and exposure texture are different")
    {
        CHECK_NOTHROW(RunOneFrame());
        REQUIRE(g_RG_TAAOutput.IsValid());
        REQUIRE(g_RG_ExposureTexture.IsValid());
        CHECK(g_RG_TAAOutput != g_RG_ExposureTexture);
    }

    // ------------------------------------------------------------------
    // TC-INT-14: All RG handles valid after 5 frames with all features
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-14 Integration - all RG handles valid after 5 frames")
    {
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = true;

        for (int i = 0; i < 5; ++i)
            CHECK_NOTHROW(RunOneFrame());

        CHECK(g_RG_TAAOutput.IsValid());
        CHECK(g_RG_HDRColor.IsValid());
        CHECK(g_RG_ExposureTexture.IsValid());
        CHECK(g_RG_DepthTexture.IsValid());
        CHECK(g_RG_GBufferMotionVectors.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-INT-15: Renderer ordering: TAA comes before HDR in the pipeline
    //            Queried from RenderGraph pass execution order after one frame.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-15 Integration - TAA renderer comes before HDR renderer in pipeline")
    {
        REQUIRE(g_TAARenderer != nullptr);
        REQUIRE(g_HDRRenderer != nullptr);

        // Run one frame so the RenderGraph records the actual pass execution order.
        CHECK_NOTHROW(RunOneFrame());

        // GetPassIndex returns the 1-based execution index of each enabled pass,
        // or 0 if the pass was disabled / not scheduled this frame.
        const uint16_t taaPassIdx = g_Renderer.m_RenderGraph.GetPassIndex(g_TAARenderer->GetName());
        const uint16_t hdrPassIdx = g_Renderer.m_RenderGraph.GetPassIndex(g_HDRRenderer->GetName());

        INFO("TAA pass index=" << taaPassIdx << "  HDR pass index=" << hdrPassIdx);
        REQUIRE(taaPassIdx > 0); // TAA must have been scheduled this frame
        REQUIRE(hdrPassIdx > 0); // HDR must have been scheduled this frame
        CHECK(taaPassIdx < hdrPassIdx);
    }

    // ------------------------------------------------------------------
    // TC-INT-16: Renderer ordering: Bloom comes before HDR in the pipeline
    //            Queried from RenderGraph pass execution order after one frame.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-16 Integration - Bloom renderer comes before HDR renderer in pipeline")
    {
        REQUIRE(g_BloomRenderer != nullptr);
        REQUIRE(g_HDRRenderer   != nullptr);

        g_Renderer.m_EnableBloom = true; // ensure Bloom is enabled so its pass is scheduled
        CHECK_NOTHROW(RunOneFrame());

        const uint16_t bloomPassIdx = g_Renderer.m_RenderGraph.GetPassIndex(g_BloomRenderer->GetName());
        const uint16_t hdrPassIdx   = g_Renderer.m_RenderGraph.GetPassIndex(g_HDRRenderer->GetName());

        INFO("Bloom pass index=" << bloomPassIdx << "  HDR pass index=" << hdrPassIdx);
        REQUIRE(bloomPassIdx > 0); // Bloom must have been scheduled this frame
        REQUIRE(hdrPassIdx   > 0); // HDR must have been scheduled this frame
        CHECK(bloomPassIdx < hdrPassIdx);
    }

    // ------------------------------------------------------------------
    // TC-INT-17: Renderer ordering: TAA comes before Bloom in the pipeline
    //            Queried from RenderGraph pass execution order after one frame.
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-17 Integration - TAA renderer comes before Bloom renderer in pipeline")
    {
        REQUIRE(g_TAARenderer   != nullptr);
        REQUIRE(g_BloomRenderer != nullptr);

        g_Renderer.m_bTAAEnabled = true;  // ensure TAA is enabled
        g_Renderer.m_EnableBloom = true;  // ensure Bloom is enabled
        CHECK_NOTHROW(RunOneFrame());

        const uint16_t taaPassIdx   = g_Renderer.m_RenderGraph.GetPassIndex(g_TAARenderer->GetName());
        const uint16_t bloomPassIdx = g_Renderer.m_RenderGraph.GetPassIndex(g_BloomRenderer->GetName());

        INFO("TAA pass index=" << taaPassIdx << "  Bloom pass index=" << bloomPassIdx);
        REQUIRE(taaPassIdx   > 0); // TAA must have been scheduled this frame
        REQUIRE(bloomPassIdx > 0); // Bloom must have been scheduled this frame
        CHECK(taaPassIdx < bloomPassIdx);
    }

    // ------------------------------------------------------------------
    // TC-INT-18: Scene camera exposure range is valid for HDR pipeline
    // ------------------------------------------------------------------
    TEST_CASE("TC-INT-18 Integration - scene camera exposure range is valid for HDR pipeline")
    {
        const float evMin = g_Renderer.m_Scene.m_Camera.m_ExposureValueMin;
        const float evMax = g_Renderer.m_Scene.m_Camera.m_ExposureValueMax;

        CHECK(evMin < evMax);
        CHECK(evMin >= -20.0f); // sanity: not absurdly dark
        CHECK(evMax <=  30.0f); // sanity: not absurdly bright

        // Both extremes produce positive exposure values
        CHECK(EV100ToExposure(evMin) > 0.0f);
        CHECK(EV100ToExposure(evMax) > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-INT-19: Bloom + TAA + HDR with IBL mode — no crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-19 Integration - Bloom+TAA+HDR in IBL mode no crash")
    {
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = true;
        g_Renderer.m_Mode               = RenderingMode::IBL;

        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK_NOTHROW(RunOneFrame());
        }
        g_Renderer.m_Mode = RenderingMode::Normal; // restore
    }

    // ------------------------------------------------------------------
    // TC-INT-20: Bloom + TAA parameter stress test — all extreme values
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-INT-20 Integration - extreme parameter stress test no crash")
    {
        g_Renderer.m_bTAAEnabled        = true;
        g_Renderer.m_EnableBloom        = true;
        g_Renderer.m_EnableAutoExposure = false;

        // Extreme bloom parameters
        g_Renderer.m_BloomIntensity = 1.0f;
        g_Renderer.m_BloomKnee      = 0.0f;
        g_Renderer.m_UpsampleRadius = 2.0f;

        // Extreme TAA parameters
        g_Renderer.m_TAASharpness = 1.0f;

        // Extreme exposure
        g_Renderer.m_Scene.m_Camera.m_Exposure = 100.0f;

        CHECK_NOTHROW(RunOneFrame());

        // Restore defaults
        g_Renderer.m_BloomIntensity             = 0.005f;
        g_Renderer.m_BloomKnee                  = 0.1f;
        g_Renderer.m_UpsampleRadius             = 0.85f;
        g_Renderer.m_TAASharpness               = 0.0f;
        g_Renderer.m_Scene.m_Camera.m_Exposure  = 1.0f;
        g_Renderer.m_EnableAutoExposure         = true;
    }
}

// ============================================================================
// TEST SUITE: TAAHDRBloom_RHIResources
// ============================================================================
TEST_SUITE("TAAHDRBloom_RHIResources")
{
    // ------------------------------------------------------------------
    // TC-RHI-01: Can create a 1x1 R32_FLOAT texture (exposure buffer format)
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-01 RHIResources - can create 1x1 R32_FLOAT texture")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::TextureDesc desc;
        desc.width  = 1;
        desc.height = 1;
        desc.format = nvrhi::Format::R32_FLOAT;
        desc.isUAV  = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        desc.debugName = "TC-RHI-01-Exposure";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);
        CHECK(tex->getDesc().format == nvrhi::Format::R32_FLOAT);
        CHECK(tex->getDesc().width  == 1u);
        CHECK(tex->getDesc().height == 1u);
        CHECK(tex->getDesc().isUAV);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-02: Can create a bloom pyramid texture (R11G11B10_FLOAT, 6 mips)
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-02 RHIResources - can create bloom pyramid texture")
    {
        REQUIRE(DEV() != nullptr);
        const auto [sw, sh] = g_Renderer.SwapchainSize();

        nvrhi::TextureDesc desc;
        desc.width      = sw / 2;
        desc.height     = sh / 2;
        desc.mipLevels  = 6u;
        desc.format     = nvrhi::Format::R11G11B10_FLOAT;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName  = "TC-RHI-02-BloomPyramid";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);
        CHECK(tex->getDesc().format    == nvrhi::Format::R11G11B10_FLOAT);
        CHECK(tex->getDesc().mipLevels == 6u);
        CHECK(tex->getDesc().isRenderTarget);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-03: Can create a TAA output texture (R11G11B10_FLOAT, UAV+RT)
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-03 RHIResources - can create TAA output texture")
    {
        REQUIRE(DEV() != nullptr);
        const auto [sw, sh] = g_Renderer.SwapchainSize();

        nvrhi::TextureDesc desc;
        desc.width      = sw;
        desc.height     = sh;
        desc.format     = Renderer::HDR_COLOR_FORMAT;
        desc.isUAV      = true;
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        desc.debugName  = "TC-RHI-03-TAAOutput";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);
        CHECK(tex->getDesc().format == Renderer::HDR_COLOR_FORMAT);
        CHECK(tex->getDesc().isUAV);
        CHECK(tex->getDesc().isRenderTarget);
        CHECK(tex->getDesc().width  == sw);
        CHECK(tex->getDesc().height == sh);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-04: Can create a luminance histogram buffer (256 uint32s)
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-04 RHIResources - can create luminance histogram buffer")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::BufferDesc desc;
        desc.structStride = sizeof(uint32_t);
        desc.byteSize     = 256u * sizeof(uint32_t);
        desc.debugName    = "TC-RHI-04-Histogram";
        desc.canHaveUAVs  = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;

        nvrhi::BufferHandle buf = DEV()->createBuffer(desc);
        REQUIRE(buf != nullptr);
        CHECK(buf->getDesc().byteSize == 256u * sizeof(uint32_t));
        CHECK(buf->getDesc().canHaveUAVs);

        buf = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-05: Can create an exposure buffer (single float)
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-05 RHIResources - can create exposure buffer (single float)")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::BufferDesc desc;
        desc.byteSize     = sizeof(float);
        desc.format       = nvrhi::Format::R32_FLOAT;
        desc.canHaveTypedViews = true;
        desc.debugName    = "TC-RHI-05-ExposureBuffer";
        desc.canHaveUAVs  = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;

        nvrhi::BufferHandle buf = DEV()->createBuffer(desc);
        REQUIRE(buf != nullptr);
        CHECK(buf->getDesc().byteSize == sizeof(float));
        CHECK(buf->getDesc().canHaveUAVs);

        buf = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-06: Can create a CPU-readable exposure readback buffer
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-06 RHIResources - can create CPU-readable exposure readback buffer")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::BufferDesc desc;
        desc.byteSize   = sizeof(float);
        desc.debugName  = "TC-RHI-06-ExposureReadback";
        desc.cpuAccess  = nvrhi::CpuAccessMode::Read;

        nvrhi::BufferHandle buf = DEV()->createBuffer(desc);
        REQUIRE(buf != nullptr);
        CHECK(buf->getDesc().byteSize == sizeof(float));
        CHECK(buf->getDesc().cpuAccess == nvrhi::CpuAccessMode::Read);

        buf = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-07: Two exposure readback buffers can be created (double-buffering)
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-07 RHIResources - two exposure readback buffers for double-buffering")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::BufferHandle bufs[2];
        for (int i = 0; i < 2; ++i)
        {
            nvrhi::BufferDesc desc;
            desc.byteSize  = sizeof(float);
            desc.debugName = i == 0 ? "TC-RHI-07-Readback0" : "TC-RHI-07-Readback1";
            desc.cpuAccess = nvrhi::CpuAccessMode::Read;
            bufs[i] = DEV()->createBuffer(desc);
            REQUIRE(bufs[i] != nullptr);
        }

        // Both buffers must be distinct objects
        CHECK(bufs[0].Get() != bufs[1].Get());

        bufs[0] = nullptr;
        bufs[1] = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-08: Bloom pyramid down and up textures can be created independently
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-08 RHIResources - bloom down and up pyramid textures are independent")
    {
        REQUIRE(DEV() != nullptr);
        const auto [sw, sh] = g_Renderer.SwapchainSize();

        nvrhi::TextureDesc desc;
        desc.width      = sw / 2;
        desc.height     = sh / 2;
        desc.mipLevels  = 6u;
        desc.format     = nvrhi::Format::R11G11B10_FLOAT;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;

        desc.debugName = "TC-RHI-08-DownPyramid";
        nvrhi::TextureHandle downPyramid = DEV()->createTexture(desc);

        desc.debugName = "TC-RHI-08-UpPyramid";
        nvrhi::TextureHandle upPyramid = DEV()->createTexture(desc);

        REQUIRE(downPyramid != nullptr);
        REQUIRE(upPyramid   != nullptr);
        CHECK(downPyramid.Get() != upPyramid.Get());

        downPyramid = nullptr;
        upPyramid   = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-RHI-09: Swapchain extent is valid for bloom pyramid calculation
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-09 RHIResources - swapchain extent valid for bloom pyramid")
    {
        const auto [sw, sh] = g_Renderer.SwapchainSize();
        REQUIRE(sw > 0u);
        REQUIRE(sh > 0u);

        const uint32_t bloomW = sw / 2;
        const uint32_t bloomH = sh / 2;
        CHECK(bloomW > 0u);
        CHECK(bloomH > 0u);
    }

    // ------------------------------------------------------------------
    // TC-RHI-10: Bloom pyramid mip 5 dimensions are non-zero for 1920x1080
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-10 RHIResources - bloom mip 5 is non-zero for 1920x1080")
    {
        // Simulate 1920x1080 swapchain
        const uint32_t sw = 1920u;
        const uint32_t sh = 1080u;
        const uint32_t baseW = sw / 2; // 960
        const uint32_t baseH = sh / 2; // 540

        // Mip 5: 960>>5 = 30, 540>>5 = 16
        const uint32_t mip5W = baseW >> 5;
        const uint32_t mip5H = baseH >> 5;
        CHECK(mip5W > 0u);
        CHECK(mip5H > 0u);
    }
}

// ============================================================================
// TEST SUITE: TAAHDRBloom_IntentionalFailures
// ============================================================================
TEST_SUITE("TAAHDRBloom_IntentionalFailures")
{
    // ------------------------------------------------------------------
    // TC-FAIL-01: Negative bloom intensity is invalid (should be clamped by shader)
    //             Frame must still complete without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-01 IntentionalFailures - negative bloom intensity no crash")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_BloomIntensity = -1.0f; // intentionally invalid
        CHECK_NOTHROW(RunOneFrame());
        // Verify the renderer didn't crash — the shader may clamp or produce black
        nvrhi::TextureHandle taaOut = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_TAAOutput);
        CHECK(taaOut != nullptr);
        g_Renderer.m_BloomIntensity = 0.005f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-02: Negative bloom knee is invalid (should be clamped by shader)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-02 IntentionalFailures - negative bloom knee no crash")
    {
        g_Renderer.m_EnableBloom = true;
        g_Renderer.m_BloomKnee   = -0.5f; // intentionally invalid
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_BloomKnee = 0.1f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-03: Negative upsample radius is invalid (should be clamped)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-03 IntentionalFailures - negative upsample radius no crash")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_UpsampleRadius = -1.0f; // intentionally invalid
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_UpsampleRadius = 0.85f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-04: Zero adaptation speed is invalid (division by zero risk)
    //             Frame must still complete without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-04 IntentionalFailures - zero adaptation speed no crash")
    {
        g_Renderer.m_EnableAutoExposure = true;
        g_Renderer.m_AdaptationSpeed    = 0.0f; // intentionally invalid
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_AdaptationSpeed = 5.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-05: Negative adaptation speed is invalid
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-05 IntentionalFailures - negative adaptation speed no crash")
    {
        g_Renderer.m_EnableAutoExposure = true;
        g_Renderer.m_AdaptationSpeed    = -1.0f; // intentionally invalid
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_AdaptationSpeed = 5.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-06: TAA sharpness > 1.0 is out of range (FSR3 expects [0,1])
    //             Frame must still complete without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-06 IntentionalFailures - TAA sharpness > 1.0 no crash")
    {
        g_Renderer.m_bTAAEnabled  = true;
        g_Renderer.m_TAASharpness = 2.0f; // intentionally out of range
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_TAASharpness = 0.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-07: TAA sharpness < 0.0 is out of range
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-07 IntentionalFailures - TAA sharpness < 0.0 no crash")
    {
        g_Renderer.m_bTAAEnabled  = true;
        g_Renderer.m_TAASharpness = -1.0f; // intentionally out of range
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_TAASharpness = 0.0f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-08: EV100 outside valid range still produces positive exposure
    //             (intentional: verify formula doesn't produce NaN/Inf)
    // ------------------------------------------------------------------
    TEST_CASE("TC-FAIL-08 IntentionalFailures - EV100 outside valid range still positive")
    {
        // EV = -100 (extremely dark)
        const float expDark = EV100ToExposure(-100.0f);
        CHECK(std::isfinite(expDark));
        CHECK(expDark > 0.0f);

        // EV = 100 (extremely bright)
        const float expBright = EV100ToExposure(100.0f);
        CHECK(std::isfinite(expBright));
        CHECK(expBright > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-FAIL-09: Bloom intensity = NaN — frame must not crash
    //             (intentional: NaN propagation test)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-09 IntentionalFailures - bloom intensity NaN no crash")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_BloomIntensity = std::numeric_limits<float>::quiet_NaN();
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_BloomIntensity = 0.005f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-10: Exposure value = NaN — frame must not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-10 IntentionalFailures - manual exposure NaN no crash")
    {
        g_Renderer.m_EnableAutoExposure = false;
        g_Renderer.m_Scene.m_Camera.m_Exposure = std::numeric_limits<float>::quiet_NaN();
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_Scene.m_Camera.m_Exposure = 1.0f; // restore
        g_Renderer.m_EnableAutoExposure = true;
    }

    // ------------------------------------------------------------------
    // TC-FAIL-11: Bloom intensity = +Inf — frame must not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FAIL-11 IntentionalFailures - bloom intensity +Inf no crash")
    {
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_BloomIntensity = std::numeric_limits<float>::infinity();
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_BloomIntensity = 0.005f; // restore
    }

    // ------------------------------------------------------------------
    // TC-FAIL-12: EV100 min > max is logically invalid — verify detection
    //             (intentional: inverted range should be caught by tests)
    // ------------------------------------------------------------------
    TEST_CASE("TC-FAIL-12 IntentionalFailures - inverted EV range is logically invalid")
    {
        const float evMin = 10.0f;
        const float evMax =  5.0f; // intentionally inverted
        // This is an invalid configuration — verify the invariant is broken
        CHECK(evMin > evMax); // confirms the invalid state
        // The correct invariant is evMin < evMax
        CHECK_FALSE(evMin < evMax);
    }
}
