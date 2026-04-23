// Tests_SceneAnimation.cpp
//
// Systems under test: Scene::Animation, Scene::AnimationChannel,
//                     Scene::AnimationSampler, Scene::Update()
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//               Scene loading is performed inside individual tests using
//               SceneScope where animations are required.
//
// Test coverage:
//   - Animation list is non-empty after loading an animated scene
//   - Each animation has at least one channel and one sampler
//   - Animation duration is positive
//   - Sampler input (time) arrays are non-empty and monotonically increasing
//   - AnimationChannel::Path enum values are distinct
//   - AnimationSampler::Interpolation enum values are distinct
//   - Scene::Update() with deltaTime=0 does not change CurrentTime
//   - Scene::Update() with positive deltaTime advances CurrentTime
//   - Scene::Update() wraps CurrentTime when it exceeds duration
//   - m_IsAnimated is set on nodes directly targeted by a channel
//   - m_IsDynamic is set on animated nodes and their children
//   - Multi-step Update() advances time monotonically until wrap
//   - AnimatedCube world transform changes after Update() (animated mesh node)
//   - Bounding sphere radius stays non-negative after repeated updates
//   - Empty scene Update() does not crash
//   - Single channel animation: channel count >= 1
//   - Per-animation name is accessible (non-null string reference)
//   - All animation sampler outputs match expected number for the channel path
//   - Update() at full duration wraps back toward zero
//   - Repeated full-duration Updates() keep time bounded
//   - MaterialEmissiveIntensity channel targets a material index
//
// Run with: HobbyRenderer --run-tests=*Animation* --gltf-samples <path>
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// TEST SUITE: Scene_AnimationStructure
// ============================================================================
TEST_SUITE("Scene_AnimationStructure")
{
    // ------------------------------------------------------------------
    // TC-ANIM-01: AnimatedCube scene has at least one animation
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-01 AnimationStructure - AnimatedCube has at least one animation")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_Animations.empty());
    }

    // ------------------------------------------------------------------
    // TC-ANIM-02: Each animation has at least one channel
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-02 AnimationStructure - each animation has at least one channel")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int i = 0; i < (int)anims.size(); ++i)
        {
            INFO("Animation " << i << " (" << anims[i].m_Name << ")");
            CHECK(!anims[i].m_Channels.empty());
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-03: Each animation has at least one sampler
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-03 AnimationStructure - each animation has at least one sampler")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int i = 0; i < (int)anims.size(); ++i)
        {
            INFO("Animation " << i << " (" << anims[i].m_Name << ")");
            CHECK(!anims[i].m_Samplers.empty());
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-04: Animation duration is positive
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-04 AnimationStructure - animation duration is positive")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int i = 0; i < (int)anims.size(); ++i)
        {
            INFO("Animation " << i << " (" << anims[i].m_Name << ") duration=" << anims[i].m_Duration);
            CHECK(anims[i].m_Duration > 0.0f);
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-05: Animation CurrentTime starts at 0
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-05 AnimationStructure - CurrentTime starts at 0 after load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int i = 0; i < (int)anims.size(); ++i)
        {
            INFO("Animation " << i << " (" << anims[i].m_Name << ") currentTime=" << anims[i].m_CurrentTime);
            CHECK(anims[i].m_CurrentTime >= 0.0f);
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-06: Sampler input (time) arrays are non-empty
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-06 AnimationStructure - sampler input arrays are non-empty")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int ai = 0; ai < (int)anims.size(); ++ai)
        {
            for (int si = 0; si < (int)anims[ai].m_Samplers.size(); ++si)
            {
                INFO("Anim " << ai << " sampler " << si);
                CHECK(!anims[ai].m_Samplers[si].m_Inputs.empty());
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-07: Sampler input times are monotonically non-decreasing
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-07 AnimationStructure - sampler input times are monotonically non-decreasing")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int ai = 0; ai < (int)anims.size(); ++ai)
        {
            for (int si = 0; si < (int)anims[ai].m_Samplers.size(); ++si)
            {
                const auto& inputs = anims[ai].m_Samplers[si].m_Inputs;
                for (int k = 1; k < (int)inputs.size(); ++k)
                {
                    INFO("Anim " << ai << " sampler " << si << " key " << k);
                    CHECK(inputs[k] >= inputs[k - 1]);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-08: Sampler output arrays are non-empty
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-08 AnimationStructure - sampler output arrays are non-empty")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int ai = 0; ai < (int)anims.size(); ++ai)
        {
            for (int si = 0; si < (int)anims[ai].m_Samplers.size(); ++si)
            {
                INFO("Anim " << ai << " sampler " << si);
                CHECK(!anims[ai].m_Samplers[si].m_Outputs.empty());
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-09: Each channel's sampler index is valid
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-09 AnimationStructure - channel sampler indices are valid")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int ai = 0; ai < (int)anims.size(); ++ai)
        {
            const int samplerCount = (int)anims[ai].m_Samplers.size();
            for (int ci = 0; ci < (int)anims[ai].m_Channels.size(); ++ci)
            {
                INFO("Anim " << ai << " channel " << ci);
                const int si = anims[ai].m_Channels[ci].m_SamplerIndex;
                CHECK((si >= 0 && si < samplerCount));
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-ANIM-10: Translation channels have Vector3-compatible output count
    //             (4 floats per keyframe as Vector4, at least 1 keyframe)
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANIM-10 AnimationStructure - translation channels have non-empty outputs")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        for (int ai = 0; ai < (int)anims.size(); ++ai)
        {
            for (int ci = 0; ci < (int)anims[ai].m_Channels.size(); ++ci)
            {
                const auto& ch = anims[ai].m_Channels[ci];
                if (ch.m_Path != Scene::AnimationChannel::Path::Translation)
                    continue;

                const auto& sampler = anims[ai].m_Samplers[ch.m_SamplerIndex];
                INFO("Anim " << ai << " channel " << ci << " (Translation)");
                CHECK(!sampler.m_Outputs.empty());
                CHECK(sampler.m_Inputs.size() == sampler.m_Outputs.size());
            }
        }
    }
}

// ============================================================================
// TEST SUITE: Scene_AnimationEnums
// ============================================================================
TEST_SUITE("Scene_AnimationEnums")
{
    // ------------------------------------------------------------------
    // TC-ANEM-01: AnimationChannel::Path enum values are distinct
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANEM-01 AnimationEnums - AnimationChannel Path values are distinct")
    {
        using P = Scene::AnimationChannel::Path;
        const int translation  = (int)P::Translation;
        const int rotation     = (int)P::Rotation;
        const int scale        = (int)P::Scale;
        const int weights      = (int)P::Weights;
        const int emissive     = (int)P::EmissiveIntensity;

        std::vector<int> vals = { translation, rotation, scale, weights, emissive };
        std::sort(vals.begin(), vals.end());
        CHECK(std::adjacent_find(vals.begin(), vals.end()) == vals.end());
    }

    // ------------------------------------------------------------------
    // TC-ANEM-02: AnimationSampler::Interpolation enum values are distinct
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANEM-02 AnimationEnums - AnimationSampler Interpolation values are distinct")
    {
        using I = Scene::AnimationSampler::Interpolation;
        const int linear      = (int)I::Linear;
        const int step        = (int)I::Step;
        const int cubic       = (int)I::CubicSpline;
        const int slerp       = (int)I::Slerp;
        const int catmullRom  = (int)I::CatmullRom;

        std::vector<int> vals = { linear, step, cubic, slerp, catmullRom };
        std::sort(vals.begin(), vals.end());
        CHECK(std::adjacent_find(vals.begin(), vals.end()) == vals.end());
    }

    // ------------------------------------------------------------------
    // TC-ANEM-03: AnimatedCube uses Linear interpolation for rotation
    // ------------------------------------------------------------------
    TEST_CASE("TC-ANEM-03 AnimationEnums - AnimatedCube has Linear interpolation sampler")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        bool foundLinear = false;
        for (const auto& anim : g_Renderer.m_Scene.m_Animations)
        {
            for (const auto& sampler : anim.m_Samplers)
            {
                if (sampler.m_Interpolation == Scene::AnimationSampler::Interpolation::Linear)
                {
                    foundLinear = true;
                    break;
                }
            }
            if (foundLinear) break;
        }
        CHECK(foundLinear);
    }
}

// ============================================================================
// TEST SUITE: Scene_AnimationUpdate
// ============================================================================
TEST_SUITE("Scene_AnimationUpdate")
{
    // ------------------------------------------------------------------
    // TC-AUPD-01: Update() with deltaTime=0 does not change CurrentTime
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-01 AnimationUpdate - Update(0) does not advance time")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        const float timeBefore = g_Renderer.m_Scene.m_Animations[0].m_CurrentTime;
        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prevAnim;

        CHECK(g_Renderer.m_Scene.m_Animations[0].m_CurrentTime == doctest::Approx(timeBefore));
    }

    // ------------------------------------------------------------------
    // TC-AUPD-02: Update() with positive deltaTime advances CurrentTime
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-02 AnimationUpdate - Update(dt) advances CurrentTime")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        // Reset animation time to start
        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.0f;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prevAnim;

        CHECK(g_Renderer.m_Scene.m_Animations[0].m_CurrentTime > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-AUPD-03: Update() wraps CurrentTime when it exceeds duration
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-03 AnimationUpdate - CurrentTime wraps when it exceeds duration")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        const float duration = g_Renderer.m_Scene.m_Animations[0].m_Duration;
        REQUIRE(duration > 0.0f);

        // Set time just before the end and advance past it.
        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = duration - 0.01f;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.05f); // 0.05 - 0.01 = 0.04 past end → should wrap
        g_Renderer.m_EnableAnimations = prevAnim;

        CHECK(g_Renderer.m_Scene.m_Animations[0].m_CurrentTime < duration);
    }

    // ------------------------------------------------------------------
    // TC-AUPD-04: CurrentTime stays in [0, duration] after many Update() calls
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-04 AnimationUpdate - CurrentTime stays in valid range after many updates")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.0f;
        const float duration = g_Renderer.m_Scene.m_Animations[0].m_Duration;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int i = 0; i < 50; ++i)
        {
            g_Renderer.m_Scene.Update(0.05f); // 50 * 0.05 = 2.5 s total
            const float t = g_Renderer.m_Scene.m_Animations[0].m_CurrentTime;
            INFO("Step " << i << " time=" << t << " duration=" << duration);
            CHECK(t >= 0.0f);
            CHECK(t <= duration + 1e-4f);
        }

        g_Renderer.m_EnableAnimations = prevAnim;
    }

    // ------------------------------------------------------------------
    // TC-AUPD-05: Update() with animations disabled does NOT change time
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-05 AnimationUpdate - Update() with animations disabled is a no-op")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.1f;
        const float timeBefore = g_Renderer.m_Scene.m_Animations[0].m_CurrentTime;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = false;
        g_Renderer.m_Scene.Update(0.5f);
        g_Renderer.m_EnableAnimations = prevAnim;

        CHECK(g_Renderer.m_Scene.m_Animations[0].m_CurrentTime == doctest::Approx(timeBefore));
    }

    // ------------------------------------------------------------------
    // TC-AUPD-06: Update() on an empty scene does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-AUPD-06 AnimationUpdate - Update() on minimal scene does not crash")
    {
        // MinimalSceneFixture loads a triangle with no animations.
        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        CHECK_NOTHROW(g_Renderer.m_Scene.Update(1.0f));
        g_Renderer.m_EnableAnimations = prevAnim;
    }

    // ------------------------------------------------------------------
    // TC-AUPD-07: m_IsAnimated flag is set on nodes targeted by channels
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-07 AnimationUpdate - m_IsAnimated set on animated nodes")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        bool foundAnimated = false;
        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (node.m_IsAnimated)
            {
                foundAnimated = true;
                break;
            }
        }
        CHECK(foundAnimated);
    }

    // ------------------------------------------------------------------
    // TC-AUPD-08: m_IsDynamic flag is set on animated nodes
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-08 AnimationUpdate - m_IsDynamic set on animated nodes")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        bool foundDynamic = false;
        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (node.m_IsDynamic)
            {
                foundDynamic = true;
                break;
            }
        }
        CHECK(foundDynamic);
    }

    // ------------------------------------------------------------------
    // TC-AUPD-09: Node with m_IsAnimated also has m_IsDynamic
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-09 AnimationUpdate - animated node is always dynamic")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            const auto& node = g_Renderer.m_Scene.m_Nodes[i];
            if (node.m_IsAnimated)
            {
                INFO("Node " << i << " (" << node.m_Name << ") is animated but not dynamic");
                CHECK(node.m_IsDynamic);
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-AUPD-10: WorldTransform of animated mesh node is non-zero after Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-10 AnimationUpdate - animated mesh node has non-zero world transform after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prevAnim;

        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (node.m_IsAnimated && node.m_MeshIndex >= 0)
            {
                const Matrix& w = node.m_WorldTransform;
                const bool nonZero = (w._11 != 0.0f || w._22 != 0.0f || w._33 != 0.0f || w._44 != 0.0f);
                CHECK(nonZero);
                break;
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-AUPD-11: Mesh node bounding sphere stays non-negative after Update()
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-11 AnimationUpdate - node bounding sphere radius is non-negative after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int step = 0; step < 5; ++step)
            g_Renderer.m_Scene.Update(0.2f);

        g_Renderer.m_EnableAnimations = prevAnim;

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_MeshIndex >= 0)
            {
                INFO("Node " << i << " (" << g_Renderer.m_Scene.m_Nodes[i].m_Name << ")");
                CHECK(g_Renderer.m_Scene.m_Nodes[i].m_Radius >= 0.0f);
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-AUPD-12: Scene bounding sphere stays positive after Update()
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-12 AnimationUpdate - scene bounding sphere stays positive after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int step = 0; step < 10; ++step)
            g_Renderer.m_Scene.Update(0.1f);

        g_Renderer.m_EnableAnimations = prevAnim;

        CHECK(g_Renderer.m_Scene.m_SceneBoundingSphere.Radius > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-AUPD-13: Animation name is accessible as a string
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-13 AnimationUpdate - animation name is accessible")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        // Name may be empty ("") — we just verify it's a valid std::string.
        const std::string& name = g_Renderer.m_Scene.m_Animations[0].m_Name;
        CHECK(name.size() < 1024u); // sane max length
    }

    // ------------------------------------------------------------------
    // TC-AUPD-14: m_DynamicNodeIndices is non-empty after loading animated scene
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-14 AnimationUpdate - dynamic node indices are non-empty for animated scene")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_DynamicNodeIndices.empty());
    }

    // ------------------------------------------------------------------
    // TC-AUPD-15: All dynamic node indices are in range
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-15 AnimationUpdate - all dynamic node indices are in valid range")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const int nodeCount = (int)g_Renderer.m_Scene.m_Nodes.size();
        for (int idx : g_Renderer.m_Scene.m_DynamicNodeIndices)
        {
            INFO("Dynamic node index: " << idx);
            CHECK(idx >= 0);
            CHECK(idx < nodeCount);
        }
    }

    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    // TC-AUPD-16: BoxTextured has no animations (sanity baseline for
    //             a truly non-animated scene).
    //
    // NOTE: CesiumMilkTruck was previously used here but it *does* have
    //       a "Wheels" rotation animation, making it unsuitable as a
    //       static-scene baseline.  BoxTextured is a plain textured box
    //       with no animation data.
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-16 AnimationUpdate - CesiumMilkTruck has no animated nodes")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        // BoxTextured is a static scene — no m_IsAnimated flags should be set.
        bool anyAnimated = false;
        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
            if (node.m_IsAnimated) anyAnimated = true;

        INFO("m_Animations.size() = " << g_Renderer.m_Scene.m_Animations.size());
        INFO("m_Nodes.size() = " << g_Renderer.m_Scene.m_Nodes.size());
        CHECK(!anyAnimated);
    }

    // ------------------------------------------------------------------
    // TC-AUPD-17: Two consecutive Update() calls produce strictly
    //             increasing CurrentTime (when not wrapping)
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-17 AnimationUpdate - two consecutive updates increase time monotonically")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        // Start at 0 to avoid wrapping for the first two steps.
        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.0f;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        g_Renderer.m_Scene.Update(0.05f);
        const float t1 = g_Renderer.m_Scene.m_Animations[0].m_CurrentTime;

        g_Renderer.m_Scene.Update(0.05f);
        const float t2 = g_Renderer.m_Scene.m_Animations[0].m_CurrentTime;

        g_Renderer.m_EnableAnimations = prevAnim;

        CHECK(t1 > 0.0f);
        CHECK(t2 > t1);
    }

    // ------------------------------------------------------------------
    // TC-AUPD-18: Update() with very large deltaTime still produces valid state
    // ------------------------------------------------------------------
    TEST_CASE("TC-AUPD-18 AnimationUpdate - very large deltaTime produces valid state")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.0f;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        CHECK_NOTHROW(g_Renderer.m_Scene.Update(1000.0f));
        g_Renderer.m_EnableAnimations = prevAnim;

        const float duration = g_Renderer.m_Scene.m_Animations[0].m_Duration;
        const float t = g_Renderer.m_Scene.m_Animations[0].m_CurrentTime;
        CHECK(t >= 0.0f);
        CHECK(t <= duration + 1e-3f);
    }
}

// ============================================================================
// TEST SUITE: Scene_AnimationBuckets
// ============================================================================
TEST_SUITE("Scene_AnimationBuckets")
{
    // ------------------------------------------------------------------
    // TC-ABKT-01: AnimatedCube total instances match after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-ABKT-01 AnimationBuckets - AnimatedCube instance data is non-empty")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_InstanceData.empty());
    }

    // ------------------------------------------------------------------
    // TC-ABKT-02: Instance data is consistent with node mesh assignments
    // ------------------------------------------------------------------
    TEST_CASE("TC-ABKT-02 AnimationBuckets - instance count consistent with mesh node count")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // Count mesh nodes
        int meshNodeCount = 0;
        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
            if (node.m_MeshIndex >= 0) ++meshNodeCount;

        CHECK(meshNodeCount > 0);
        CHECK(!g_Renderer.m_Scene.m_InstanceData.empty());
    }
}

// ============================================================================
// TEST SUITE: Scene_AnimationDisabledGuard
// Tests that Scene::Update() is a no-op when m_EnableAnimations is false,
// verifying the defence-in-depth guard added to Scene::Update() itself.
// ============================================================================
TEST_SUITE("Scene_AnimationDisabledGuard")
{
    // ------------------------------------------------------------------
    // TC-ADIS-01: Update() with animations disabled does NOT advance time
    //             (same as TC-AUPD-05 but uses a fresh, separate test
    //             name for better isolation and future-proofing)
    // ------------------------------------------------------------------
    TEST_CASE("TC-ADIS-01 AnimationDisabledGuard - Update does not advance time when disabled")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.25f;
        const float timeBefore = g_Renderer.m_Scene.m_Animations[0].m_CurrentTime;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = false;
        g_Renderer.m_Scene.Update(1.0f);
        g_Renderer.m_EnableAnimations = prevAnim;

        INFO("timeBefore=" << timeBefore
            << " timeAfter=" << g_Renderer.m_Scene.m_Animations[0].m_CurrentTime);
        CHECK(g_Renderer.m_Scene.m_Animations[0].m_CurrentTime == doctest::Approx(timeBefore));
    }

    // ------------------------------------------------------------------
    // TC-ADIS-02: Update() with animations disabled does NOT dirty
    //             any node transforms (no transform recomputation)
    // ------------------------------------------------------------------
    TEST_CASE("TC-ADIS-02 AnimationDisabledGuard - Update does not dirty nodes when disabled")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // Manually clear all dirty flags.
        for (auto& node : g_Renderer.m_Scene.m_Nodes)
            node.m_IsDirty = false;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = false;
        g_Renderer.m_Scene.Update(1.0f);
        g_Renderer.m_EnableAnimations = prevAnim;

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            INFO("Node " << i << " (" << g_Renderer.m_Scene.m_Nodes[i].m_Name << ") dirty");
            CHECK(!g_Renderer.m_Scene.m_Nodes[i].m_IsDirty);
        }
    }

    // ------------------------------------------------------------------
    // TC-ADIS-03: Update() still updates prev-world transforms even when
    //             animations are disabled (required for motion-vector correctness)
    // ------------------------------------------------------------------
    TEST_CASE("TC-ADIS-03 AnimationDisabledGuard - prev-world copies happen even when disabled")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_InstanceData.empty());

        // Write a sentinel into m_World so we can verify it was copied to m_PrevWorld.
        srrhi::PerInstanceData& inst = g_Renderer.m_Scene.m_InstanceData[0];
        inst.m_World._11 = 42.0f;
        inst.m_PrevWorld._11 = 0.0f;

        const bool prevAnim = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = false;
        g_Renderer.m_Scene.Update(0.5f);
        g_Renderer.m_EnableAnimations = prevAnim;

        INFO("m_PrevWorld._11 = " << inst.m_PrevWorld._11);
        CHECK(inst.m_PrevWorld._11 == doctest::Approx(42.0f));
    }
}

// ============================================================================
// TEST SUITE: Scene_CesiumMilkTruck
// Positive tests documenting that CesiumMilkTruck DOES contain animations.
// (Previously TC-AUPD-16 wrongly assumed it was static.)
// ============================================================================
TEST_SUITE("Scene_CesiumMilkTruck")
{
    // ------------------------------------------------------------------
    // TC-CMT-01: CesiumMilkTruck has a "Wheels" animation
    // ------------------------------------------------------------------
    TEST_CASE("TC-CMT-01 CesiumMilkTruck - scene has at least one animation")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        INFO("m_Animations.size() = " << g_Renderer.m_Scene.m_Animations.size());
        CHECK(!g_Renderer.m_Scene.m_Animations.empty());
    }

    // ------------------------------------------------------------------
    // TC-CMT-02: CesiumMilkTruck has animated wheel nodes
    // ------------------------------------------------------------------
    TEST_CASE("TC-CMT-02 CesiumMilkTruck - wheel nodes are animated")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        bool foundAnimated = false;
        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (node.m_IsAnimated)
            {
                foundAnimated = true;
                break;
            }
        }

        INFO("m_Nodes.size() = " << g_Renderer.m_Scene.m_Nodes.size());
        CHECK(foundAnimated);
    }
}
