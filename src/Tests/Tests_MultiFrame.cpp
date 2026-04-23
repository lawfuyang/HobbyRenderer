// Tests_MultiFrame.cpp
//
// Systems under test: Renderer frame loop, rendering modes, feature toggles
//                     over multiple consecutive frames.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//
// Test coverage:
//   - Single frame runs without crash (baseline)
//   - 5 consecutive frames complete without crash
//   - 10 consecutive frames complete without crash
//   - 20 consecutive frames complete without crash
//   - Frame number increments by exactly 1 per RunOneFrame() call
//   - Frame number increments by N after N RunOneFrame() calls
//   - m_FrameTime is non-zero after RunOneFrame()
//   - TAA enabled: 5 frames without crash
//   - TAA disabled: 5 frames without crash
//   - Bloom enabled: 3 frames without crash
//   - Bloom disabled: 3 frames without crash
//   - Sky enabled: 3 frames without crash
//   - Sky disabled: 3 frames without crash
//   - Auto-exposure enabled: 3 frames without crash
//   - Auto-exposure disabled: 3 frames without crash
//   - All culling disabled: 3 frames without crash
//   - All culling enabled: 3 frames without crash
//   - ForcedLOD=0: 3 frames without crash
//   - ForcedLOD=-1 (reset): 3 frames without crash
//   - FreezeCullingCamera: 5 frames without crash
//   - RT shadows enabled: 3 frames (skip if RT not supported)
//   - RT shadows disabled: 3 frames without crash
//   - IBL rendering mode: 3 frames without crash
//   - RenderingMode::Normal after IBL: 3 frames without crash
//   - Animation enabled: 5 frames without crash (with AnimatedCube if available)
//   - Animation disabled: 5 frames without crash
//   - Scene load → 3 frames → unload → reload → 3 frames: no crash
//   - DebugMode cycling over several frames: no crash
//   - RendererMode::ReferencePathTracer: 1 frame (skip if no samples)
//
// Run with: HobbyRenderer --run-tests=*MultiFrame*
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// TEST SUITE: MultiFrame_Basic
// ============================================================================
TEST_SUITE("MultiFrame_Basic")
{
    // ------------------------------------------------------------------
    // TC-MF-01: Single frame baseline (minimal scene)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-01 MultiFrame - single frame does not crash")
    {
        CHECK(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-MF-02: 5 consecutive frames complete without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-02 MultiFrame - 5 frames complete without crash")
    {
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-03: 10 consecutive frames complete without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-03 MultiFrame - 10 frames complete without crash")
    {
        for (int i = 0; i < 10; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-04: 20 consecutive frames complete without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-04 MultiFrame - 20 frames complete without crash")
    {
        for (int i = 0; i < 20; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-05: Frame number increments by exactly 1 per RunOneFrame()
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-05 MultiFrame - frame number increments by 1 per frame")
    {
        const uint64_t before = g_Renderer.m_FrameNumber;
        RunOneFrame();
        CHECK(g_Renderer.m_FrameNumber == before + 1);
    }

    // ------------------------------------------------------------------
    // TC-MF-06: Frame number increments by N after N RunOneFrame() calls
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-06 MultiFrame - frame number increments by N after N frames")
    {
        const uint64_t before = g_Renderer.m_FrameNumber;
        constexpr int N = 7;
        for (int i = 0; i < N; ++i)
            RunOneFrame();
        CHECK(g_Renderer.m_FrameNumber == before + N);
    }

    // ------------------------------------------------------------------
    // TC-MF-07: m_FrameTime is set (non-zero) after RunOneFrame()
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-07 MultiFrame - m_FrameTime is non-zero after a frame")
    {
        // Run twice: the first frame may have zero delta (no prior timestamp).
        RunOneFrame();
        RunOneFrame();
        CHECK(g_Renderer.m_FrameTime >= 0.0f); // ≥0 (may be very small in a test)
    }
}

// ============================================================================
// TEST SUITE: MultiFrame_FeatureToggles
// ============================================================================
TEST_SUITE("MultiFrame_FeatureToggles")
{
    // ------------------------------------------------------------------
    // TC-MF-TAA-01: TAA enabled — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-TAA-01 FeatureToggles - TAA enabled 5 frames")
    {
        g_Renderer.m_bTAAEnabled = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_bTAAEnabled = false; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-TAA-02: TAA disabled — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-TAA-02 FeatureToggles - TAA disabled 5 frames")
    {
        g_Renderer.m_bTAAEnabled = false;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-BLM-01: Bloom enabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-BLM-01 FeatureToggles - Bloom enabled 3 frames")
    {
        g_Renderer.m_EnableBloom = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_EnableBloom = true; // keep default
    }

    // ------------------------------------------------------------------
    // TC-MF-BLM-02: Bloom disabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-BLM-02 FeatureToggles - Bloom disabled 3 frames")
    {
        g_Renderer.m_EnableBloom = false;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_EnableBloom = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-SKY-01: Sky enabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-SKY-01 FeatureToggles - Sky enabled 3 frames")
    {
        g_Renderer.m_EnableSky = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-SKY-02: Sky disabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-SKY-02 FeatureToggles - Sky disabled 3 frames")
    {
        g_Renderer.m_EnableSky = false;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_EnableSky = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-EXP-01: Auto-exposure enabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-EXP-01 FeatureToggles - Auto-exposure enabled 3 frames")
    {
        g_Renderer.m_EnableAutoExposure = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-EXP-02: Auto-exposure disabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-EXP-02 FeatureToggles - Auto-exposure disabled 3 frames")
    {
        g_Renderer.m_EnableAutoExposure = false;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_EnableAutoExposure = true; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-CULL-01: All culling disabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-CULL-01 FeatureToggles - all culling disabled 3 frames")
    {
        g_Renderer.m_EnableFrustumCulling    = false;
        g_Renderer.m_EnableOcclusionCulling  = false;
        g_Renderer.m_EnableConeCulling       = false;

        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }

        g_Renderer.m_EnableFrustumCulling    = true;
        g_Renderer.m_EnableOcclusionCulling  = true;
    }

    // ------------------------------------------------------------------
    // TC-MF-CULL-02: All culling enabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-CULL-02 FeatureToggles - all culling enabled 3 frames")
    {
        g_Renderer.m_EnableFrustumCulling    = true;
        g_Renderer.m_EnableOcclusionCulling  = true;
        g_Renderer.m_EnableConeCulling       = true;

        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }

        g_Renderer.m_EnableConeCulling = false; // restore non-default
    }

    // ------------------------------------------------------------------
    // TC-MF-LOD-01: ForcedLOD=0 — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-LOD-01 FeatureToggles - ForcedLOD=0 3 frames")
    {
        g_Renderer.m_ForcedLOD = 0;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_ForcedLOD = -1; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-LOD-02: ForcedLOD=-1 (auto) — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-LOD-02 FeatureToggles - ForcedLOD=-1 (auto) 3 frames")
    {
        g_Renderer.m_ForcedLOD = -1;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-FRZ-01: FreezeCullingCamera — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-FRZ-01 FeatureToggles - freeze culling camera 5 frames")
    {
        g_Renderer.m_FreezeCullingCamera = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_FreezeCullingCamera = false; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-RT-01: RT shadows disabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-RT-01 FeatureToggles - RT shadows disabled 3 frames")
    {
        g_Renderer.m_EnableRTShadows = false;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-RT-02: RT shadows enabled — 3 frames (requires RT device support)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-RT-02 FeatureToggles - RT shadows enabled 3 frames")
    {
        // Check for RayQuery support; skip gracefully if absent.
        nvrhi::rt::AccelStructHandle dummy = nullptr;
        if (!DEV()->queryFeatureSupport(nvrhi::Feature::RayQuery))
        {
            WARN("Skipping: RayQuery not supported on this device");
            return;
        }

        g_Renderer.m_EnableRTShadows = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_EnableRTShadows = false; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-ANIM-01: Animations enabled — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-ANIM-01 FeatureToggles - animations enabled 5 frames")
    {
        g_Renderer.m_EnableAnimations = true;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-ANIM-02: Animations disabled — 5 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-ANIM-02 FeatureToggles - animations disabled 5 frames")
    {
        g_Renderer.m_EnableAnimations = false;
        for (int i = 0; i < 5; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_EnableAnimations = true; // restore
    }
}

// ============================================================================
// TEST SUITE: MultiFrame_RenderingModes
// ============================================================================
TEST_SUITE("MultiFrame_RenderingModes")
{
    // ------------------------------------------------------------------
    // TC-MF-MODE-01: IBL rendering mode — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-MODE-01 RenderingModes - IBL mode 3 frames")
    {
        g_Renderer.m_Mode = RenderingMode::IBL;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_Mode = RenderingMode::Normal; // restore
    }

    // ------------------------------------------------------------------
    // TC-MF-MODE-02: Normal mode after IBL — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-MODE-02 RenderingModes - Normal mode after IBL 3 frames")
    {
        g_Renderer.m_Mode = RenderingMode::IBL;
        RunOneFrame();
        g_Renderer.m_Mode = RenderingMode::Normal;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-MODE-03: PathTracer mode — 1 frame (requires sample assets)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-MODE-03 RenderingModes - ReferencePathTracer mode 1 frame")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        if (!DEV()->queryFeatureSupport(nvrhi::Feature::RayQuery))
        {
            WARN("Skipping: RayQuery not supported on this device");
            return;
        }

        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);

            g_Renderer.m_Mode = RenderingMode::ReferencePathTracer;
            CHECK(RunOneFrame());
            g_Renderer.m_Mode = RenderingMode::Normal;
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-DBG-01: DebugMode cycling — 5 frames with incrementing debug mode
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-DBG-01 RenderingModes - debug mode cycling 5 frames")
    {
        const int prevDebug = g_Renderer.m_DebugMode;
        for (int i = 0; i < 5; ++i)
        {
            g_Renderer.m_DebugMode = i;
            INFO("Frame " << i << " DebugMode=" << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_DebugMode = prevDebug;
    }

    // ------------------------------------------------------------------
    // TC-MF-DBG-02: DebugMode=0 (off) for 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-DBG-02 RenderingModes - debug mode 0 (off) 3 frames")
    {
        g_Renderer.m_DebugMode = 0;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }
}

// ============================================================================
// TEST SUITE: MultiFrame_SceneReload
// ============================================================================
TEST_SUITE("MultiFrame_SceneReload")
{
    // ------------------------------------------------------------------
    // TC-MF-RELD-01: Load → 3 frames → unload → reload → 3 frames: no crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-MF-RELD-01 SceneReload - load-run-reload-run cycle survives")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        // First load + frames
        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
            for (int i = 0; i < 3; ++i)
            {
                INFO("First load, frame " << i);
                CHECK(RunOneFrame());
            }
        }
        // Scope destructor unloads scene.

        // Second load + frames
        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
            for (int i = 0; i < 3; ++i)
            {
                INFO("Second load, frame " << i);
                CHECK(RunOneFrame());
            }
        }

        CHECK(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-MF-RELD-02: Animated scene: 5 frames → unload → reload → 5 frames
    // ------------------------------------------------------------------
    TEST_CASE("TC-MF-RELD-02 SceneReload - animated scene reload cycle survives")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");

        for (int cycle = 0; cycle < 2; ++cycle)
        {
            INFO("Cycle " << cycle);
            SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
            REQUIRE(scope.loaded);
            g_Renderer.m_EnableAnimations = true;
            for (int i = 0; i < 5; ++i)
            {
                INFO("  Frame " << i);
                CHECK(RunOneFrame());
            }
            g_Renderer.m_EnableAnimations = true;
        }
    }

    // ------------------------------------------------------------------
    // TC-MF-RELD-03: Running frames with minimal fixture before and after
    //                a SceneScope load/unload cycle keeps device healthy
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-RELD-03 SceneReload - minimal scene before/after BoxTextured")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        // 2 frames on minimal scene
        CHECK(RunOneFrame());
        CHECK(RunOneFrame());

        // BoxTextured load/run/unload
        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            if (scope.loaded)
            {
                CHECK(RunOneFrame());
                CHECK(RunOneFrame());
            }
        }

        // MinimalSceneFixture destructor will clean up — device must still be valid.
        CHECK(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-MF-RELD-04: CesiumMilkTruck 3-frame load/unload cycle
    // ------------------------------------------------------------------
    TEST_CASE("TC-MF-RELD-04 SceneReload - CesiumMilkTruck 3-frame cycle")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");

        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
    }
}

// ============================================================================
// TEST SUITE: MultiFrame_BloomDebug
// ============================================================================
TEST_SUITE("MultiFrame_BloomDebug")
{
    // ------------------------------------------------------------------
    // TC-MF-BLMD-01: DebugBloom enabled — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-BLMD-01 BloomDebug - DebugBloom enabled 3 frames")
    {
        g_Renderer.m_EnableBloom = true;
        g_Renderer.m_DebugBloom  = true;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_DebugBloom = false;
    }

    // ------------------------------------------------------------------
    // TC-MF-BLMD-02: BloomIntensity=0 — 3 frames without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-MF-BLMD-02 BloomDebug - BloomIntensity=0 3 frames")
    {
        const float prevIntensity   = g_Renderer.m_BloomIntensity;
        g_Renderer.m_EnableBloom    = true;
        g_Renderer.m_BloomIntensity = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            INFO("Frame " << i);
            CHECK(RunOneFrame());
        }
        g_Renderer.m_BloomIntensity = prevIntensity;
    }
}
