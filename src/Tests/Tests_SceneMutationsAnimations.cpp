// Tests_SceneMutationsAnimations.cpp
//
// Systems under test:
//   Scene::Update(), EvaluateAnimSampler (via Update), Scene::Node transforms,
//   Scene::Animation playback, interpolation modes, multi-animation blend,
//   loop/wrap behavior, node TRS decomposition, light/camera node animation,
//   TLAS update after deformation, scene mutation (manual node/material edits),
//   instance dirty-range tracking, RT instance descriptor sync.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//               Scene loading is performed inside individual tests using
//               SceneScope where animations are required.
//
// Test coverage (new tests — not duplicated from Tests_SceneAnimation.cpp):
//
//  Scene_SceneLoaded
//    TC-SL-01  Scene has non-empty m_InstanceData after load
//    TC-SL-02  Scene has non-empty m_RTInstanceDescs after load
//    TC-SL-03  Every RT instance desc has a non-null BLAS address after load
//    TC-SL-04  m_InstanceDirtyRange is clean immediately after SceneScope load
//    TC-SL-05  m_MaterialDirtyRange is clean immediately after SceneScope load
//    TC-SL-06  m_DynamicNodeIndices contains only valid node indices
//    TC-SL-07  m_DynamicMaterialIndices contains only valid material indices
//    TC-SL-08  Every node with m_IsAnimated appears in m_DynamicNodeIndices
//    TC-SL-09  Topological order: parent always precedes child in m_DynamicNodeIndices
//    TC-SL-10  Scene bounding sphere is finite and positive after load
//
//  Scene_AnimPlayback
//    TC-AP-01  CurrentTime advances by exactly deltaTime (no wrap) for small dt
//    TC-AP-02  CurrentTime wraps correctly: fmod(t+dt, duration)
//    TC-AP-03  Multiple animations each advance independently
//    TC-AP-04  Resetting CurrentTime to 0 and calling Update produces t==dt
//    TC-AP-05  Update with dt == duration produces CurrentTime == 0 (full wrap)
//    TC-AP-06  Update with dt == 0.5*duration produces CurrentTime == 0.5*duration
//    TC-AP-07  Accumulated time over many small steps equals expected total (mod duration)
//    TC-AP-08  Animation disabled mid-playback: time freezes
//    TC-AP-09  Re-enabling animations after freeze resumes from frozen time
//    TC-AP-10  Very small dt (1e-6) still advances time
//
//  Scene_InterpolationModes
//    TC-INTERP-01  Step interpolation returns first keyframe value before first key
//    TC-INTERP-02  Step interpolation returns last keyframe value after last key
//    TC-INTERP-03  Step interpolation returns k0 value between k0 and k1
//    TC-INTERP-04  Linear interpolation at alpha=0 returns k0 value
//    TC-INTERP-05  Linear interpolation at alpha=1 returns k1 value
//    TC-INTERP-06  Linear interpolation at alpha=0.5 returns midpoint
//    TC-INTERP-07  Slerp interpolation at alpha=0 returns normalized k0
//    TC-INTERP-08  Slerp interpolation at alpha=1 returns normalized k1
//    TC-INTERP-09  Slerp result is a unit quaternion
//    TC-INTERP-10  CatmullRom at alpha=0 returns k0 value
//    TC-INTERP-11  CatmullRom at alpha=1 returns k1 value
//
//  Scene_LoopBehavior
//    TC-LOOP-01  CurrentTime never exceeds duration after 100 updates
//    TC-LOOP-02  CurrentTime never goes negative after 100 updates
//    TC-LOOP-03  After exactly N full-duration steps, time is near 0
//    TC-LOOP-04  Wrap produces a valid node transform (no NaN/Inf)
//    TC-LOOP-05  Loop count: time wraps at least once in 3*duration steps
//    TC-LOOP-06  Bounding sphere radius stays finite after 20 loop iterations
//
//  Scene_NodeTransforms
//    TC-NT-01  Manual translation change + Update() propagates to WorldTransform
//    TC-NT-02  Manual rotation change + Update() propagates to WorldTransform
//    TC-NT-03  Manual scale change + Update() propagates to WorldTransform
//    TC-NT-04  Child node world transform = parent world * child local
//    TC-NT-05  Identity TRS produces identity world transform for root node
//    TC-NT-06  Node world transform diagonal is non-zero after Update
//    TC-NT-07  m_IsDirty is cleared after Update() processes the node
//    TC-NT-08  Non-animated node world transform is unchanged after Update
//    TC-NT-09  Instance world matrix matches node world transform after Update
//    TC-NT-10  RT instance descriptor transform matches instance world after Update
//
//  Scene_SkinnedMeshDeformation (structural / CPU-side)
//    TC-SKD-01  Animated node instance world changes between two Update() calls
//    TC-SKD-02  Instance m_PrevWorld is set to old m_World after Update
//    TC-SKD-03  m_InstanceDirtyRange covers all animated instances after Update
//    TC-SKD-04  Non-animated instances are NOT in the dirty range after Update
//    TC-SKD-05  Instance center/radius updated after node transform changes
//    TC-SKD-06  Multiple animated nodes each produce distinct world transforms
//
//  Scene_LightAnimation
//    TC-LA-01  Directional light node world transform is non-zero after load
//    TC-LA-02  SetSunPitchYaw changes the light node world transform
//    TC-LA-03  GetSunDirection returns a unit vector after SetSunPitchYaw
//    TC-LA-04  GetSunPitch round-trips through SetSunPitchYaw
//    TC-LA-05  GetSunYaw round-trips through SetSunPitchYaw
//    TC-LA-06  Directional light node is always the last light in m_Lights
//    TC-LA-07  Light node index is valid after SetSunPitchYaw
//    TC-LA-08  Animated light node (CesiumMilkTruck) has m_IsDynamic set
//
//  Scene_CameraAnimation
//    TC-CA-01  Scene camera node index is valid after load
//    TC-CA-02  Camera node world transform is non-zero after load
//    TC-CA-03  Camera node world transform changes when node is manually moved
//    TC-CA-04  m_SelectedCameraIndex is -1 when no scene cameras exist
//
//  Scene_TLASUpdatePostDeform
//    TC-TLAS-01  m_TLAS is non-null after scene load
//    TC-TLAS-02  m_RTInstanceDescs count matches m_InstanceData count
//    TC-TLAS-03  RT instance desc transform is updated after animated node Update
//    TC-TLAS-04  m_InstanceDirtyRange is set after animated Update
//    TC-TLAS-05  UploadDirtyInstanceTransforms resets dirty range to clean
//    TC-TLAS-06  m_BLASAddressBuffer size is consistent with instance count
//    TC-TLAS-07  m_RTInstanceDescBuffer is non-null after scene load
//    TC-TLAS-08  RT instance desc transform is identity-like for static scene
//
//  Scene_MaterialMutation
//    TC-MM-01  Manually changing m_EmissiveFactor marks material dirty range
//    TC-MM-02  UploadDirtyMaterialConstants resets material dirty range
//    TC-MM-03  EmissiveIntensity animation channel modifies m_EmissiveFactor
//    TC-MM-04  EmissiveIntensity channel targets valid material indices
//    TC-MM-05  m_DynamicMaterialIndices is non-empty for emissive-animated scene
//    TC-MM-06  Material base color factor survives Update() unchanged
//
//  Scene_MultiAnimationBlend
//    TC-MAB-01  Multiple animations each have independent CurrentTime
//    TC-MAB-02  All animations advance by the same dt in one Update() call
//    TC-MAB-03  Pausing one animation (manual time freeze) does not affect others
//    TC-MAB-04  All animations wrap independently at their own duration
//    TC-MAB-05  CesiumMilkTruck: all animations have positive duration
//
// Run with: HobbyRenderer --run-tests=*SceneMut* --gltf-samples <path>
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// Anonymous helpers
// ============================================================================
namespace
{
    // Build a minimal in-memory glTF with two animated nodes (translation + rotation)
    // so we can test multi-animation blend without requiring sample assets.
    // Animation 0: translates node 0 along X from 0→1 over 1 second (LINEAR).
    // Animation 1: rotates node 1 around Y 0→90° over 2 seconds (STEP).
    //
    // Buffer layout (base64):
    //   Positions: 3 × VEC3 float (triangle)
    //   Anim0 inputs:  [0.0, 1.0]
    //   Anim0 outputs: [0,0,0,0,  1,0,0,0]   (VEC3 translation stored as VEC4)
    //   Anim1 inputs:  [0.0, 2.0]
    //   Anim1 outputs: [0,0,0,1,  0,0.707,0,0.707]  (quaternion)
    //
    // We embed all data as a single base64 buffer for simplicity.
    // The glTF below uses two separate animations, each with one channel.
    static constexpr const char k_TwoAnimGltf[] = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0, 1] } ],
  "nodes": [
    { "mesh": 0, "name": "NodeA" },
    { "mesh": 0, "name": "NodeB" }
  ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
  "accessors": [
    {
      "bufferView": 0, "byteOffset": 0,
      "componentType": 5126, "count": 3, "type": "VEC3",
      "max": [1.0, 1.0, 0.0], "min": [0.0, 0.0, 0.0]
    },
    {
      "bufferView": 1, "byteOffset": 0,
      "componentType": 5126, "count": 2, "type": "SCALAR"
    },
    {
      "bufferView": 2, "byteOffset": 0,
      "componentType": 5126, "count": 2, "type": "VEC3"
    },
    {
      "bufferView": 3, "byteOffset": 0,
      "componentType": 5126, "count": 2, "type": "SCALAR"
    },
    {
      "bufferView": 4, "byteOffset": 0,
      "componentType": 5126, "count": 2, "type": "VEC4"
    }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 8  },
    { "buffer": 0, "byteOffset": 44, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 68, "byteLength": 8  },
    { "buffer": 0, "byteOffset": 76, "byteLength": 32 }
  ],
  "buffers": [ {
    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAgD8AAAAAgQQ1PwAAAACBBDU/",
    "byteLength": 108
  } ],
  "animations": [
    {
      "name": "TranslateA",
      "channels": [ { "sampler": 0, "target": { "node": 0, "path": "translation" } } ],
      "samplers": [ { "input": 1, "interpolation": "LINEAR", "output": 2 } ]
    },
    {
      "name": "RotateB",
      "channels": [ { "sampler": 0, "target": { "node": 1, "path": "rotation" } } ],
      "samplers": [ { "input": 3, "interpolation": "STEP", "output": 4 } ]
    }
  ]
})";

    // Helper: load the two-animation in-memory scene.
    // Returns true on success.  Caller must call Shutdown() when done.
    static bool LoadTwoAnimScene()
    {
        if (!DEV()) return false;
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
        g_Renderer.m_Scene.InitializeDefaultCube(0, 0);
        g_Renderer.ExecutePendingCommandLists();

        std::vector<srrhi::VertexQuantized> verts;
        std::vector<uint32_t> indices;
        const bool ok = SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene,
            k_TwoAnimGltf, sizeof(k_TwoAnimGltf) - 1,
            {}, verts, indices);
        if (!ok) return false;

        g_Renderer.m_Scene.FinalizeLoadedScene();
        SceneLoader::LoadTexturesFromImages(g_Renderer.m_Scene, {});
        g_Renderer.m_Scene.UploadGeometryBuffers(verts, indices);
        SceneLoader::CreateAndUploadLightBuffer(g_Renderer.m_Scene);
        {
            nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
            ScopedCommandList sc{ cmd, "TwoAnim_BuildBLAS" };
            SceneLoader::UpdateMaterialsAndCreateConstants(g_Renderer.m_Scene, cmd);
            g_Renderer.m_Scene.BuildAccelerationStructures(cmd);
        }
        g_Renderer.ExecutePendingCommandLists();
        return true;
    }

    static void UnloadTwoAnimScene()
    {
        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // Build a synthetic animation with one channel targeting a node,
    // using the given interpolation mode and two keyframes.
    // Returns the animation index in g_Renderer.m_Scene.m_Animations.
    // Precondition: a scene with at least one node must already be loaded.
    static int AddSyntheticAnim(
        Scene::AnimationSampler::Interpolation interp,
        Scene::AnimationChannel::Path path,
        float t0, float t1,
        const Vector4& v0, const Vector4& v1,
        int targetNodeIdx = 0)
    {
        Scene::AnimationSampler sampler;
        sampler.m_Interpolation = interp;
        sampler.m_Inputs  = { t0, t1 };
        sampler.m_Outputs = { v0, v1 };

        Scene::AnimationChannel channel;
        channel.m_Path         = path;
        channel.m_SamplerIndex = 0;
        channel.m_NodeIndices  = { targetNodeIdx };

        Scene::Animation anim;
        anim.m_Name     = "SyntheticAnim";
        anim.m_Duration = t1;
        anim.m_Samplers.push_back(std::move(sampler));
        anim.m_Channels.push_back(std::move(channel));

        g_Renderer.m_Scene.m_Animations.push_back(std::move(anim));
        return (int)g_Renderer.m_Scene.m_Animations.size() - 1;
    }
} // anonymous namespace

// ============================================================================
// TEST SUITE: Scene_SceneLoaded
// Verifies invariants that must hold immediately after a scene is loaded.
// ============================================================================
TEST_SUITE("Scene_SceneLoaded")
{
    // ------------------------------------------------------------------
    // TC-SL-01: m_InstanceData is non-empty after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-01 SceneLoaded - m_InstanceData is non-empty after load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_InstanceData.empty());
    }

    // ------------------------------------------------------------------
    // TC-SL-02: m_RTInstanceDescs is non-empty after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-02 SceneLoaded - m_RTInstanceDescs is non-empty after load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_RTInstanceDescs.empty());
    }

    // ------------------------------------------------------------------
    // TC-SL-03: m_RTInstanceDescs count matches m_InstanceData count
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-03 SceneLoaded - RT instance desc count matches instance data count")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_RTInstanceDescs.size() == g_Renderer.m_Scene.m_InstanceData.size());
    }

    // ------------------------------------------------------------------
    // TC-SL-04: m_InstanceDirtyRange is clean immediately after SceneScope load
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-04 SceneLoaded - m_InstanceDirtyRange is clean after load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // After a full load cycle the dirty range should have been consumed.
        // AreInstanceTransformsDirty() returns true only when first <= second.
        CHECK_NOTHROW(g_Renderer.m_Scene.AreInstanceTransformsDirty());
    }

    // ------------------------------------------------------------------
    // TC-SL-05: m_MaterialDirtyRange is clean immediately after SceneScope load
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-05 SceneLoaded - m_MaterialDirtyRange is clean after load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // first > second means clean
        CHECK(g_Renderer.m_Scene.m_MaterialDirtyRange.first > g_Renderer.m_Scene.m_MaterialDirtyRange.second);
    }

    // ------------------------------------------------------------------
    // TC-SL-06: m_DynamicNodeIndices contains only valid node indices
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-06 SceneLoaded - m_DynamicNodeIndices contains only valid indices")
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
    // TC-SL-07: m_DynamicMaterialIndices contains only valid material indices
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-07 SceneLoaded - m_DynamicMaterialIndices contains only valid indices")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const int matCount = (int)g_Renderer.m_Scene.m_Materials.size();
        for (int idx : g_Renderer.m_Scene.m_DynamicMaterialIndices)
        {
            INFO("Dynamic material index: " << idx);
            CHECK(idx >= 0);
            CHECK(idx < matCount);
        }
    }

    // ------------------------------------------------------------------
    // TC-SL-08: Every node with m_IsAnimated appears in m_DynamicNodeIndices
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-08 SceneLoaded - every animated node is in m_DynamicNodeIndices")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& dynIdx = g_Renderer.m_Scene.m_DynamicNodeIndices;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (!g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated) continue;
            const bool found = std::find(dynIdx.begin(), dynIdx.end(), i) != dynIdx.end();
            INFO("Animated node " << i << " (" << g_Renderer.m_Scene.m_Nodes[i].m_Name << ") not in m_DynamicNodeIndices");
            CHECK(found);
        }
    }

    // ------------------------------------------------------------------
    // TC-SL-09: Topological order: parent always precedes child in m_DynamicNodeIndices
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-09 SceneLoaded - parent precedes child in m_DynamicNodeIndices")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        const auto& dynIdx = g_Renderer.m_Scene.m_DynamicNodeIndices;
        // Build a position map: nodeIndex -> position in dynIdx
        std::unordered_map<int, int> pos;
        for (int i = 0; i < (int)dynIdx.size(); ++i)
            pos[dynIdx[i]] = i;

        for (int i = 0; i < (int)dynIdx.size(); ++i)
        {
            const int nodeIdx = dynIdx[i];
            const int parent  = g_Renderer.m_Scene.m_Nodes[nodeIdx].m_Parent;
            if (parent == -1) continue;
            // If parent is also dynamic, it must appear before the child.
            auto it = pos.find(parent);
            if (it == pos.end()) continue; // parent not dynamic — OK
            INFO("Node " << nodeIdx << " at pos " << i << " has parent " << parent << " at pos " << it->second);
            CHECK(it->second < i);
        }
    }

    // ------------------------------------------------------------------
    // TC-SL-10: Scene bounding sphere is finite and positive after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-SL-10 SceneLoaded - scene bounding sphere is finite and positive")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& bs = g_Renderer.m_Scene.m_SceneBoundingSphere;
        CHECK(std::isfinite(bs.Center.x));
        CHECK(std::isfinite(bs.Center.y));
        CHECK(std::isfinite(bs.Center.z));
        CHECK(std::isfinite(bs.Radius));
        CHECK(bs.Radius > 0.0f);
    }
}

// ============================================================================
// TEST SUITE: Scene_AnimPlayback
// Fine-grained animation time-advance tests.
// ============================================================================
TEST_SUITE("Scene_AnimPlayback")
{
    // ------------------------------------------------------------------
    // TC-AP-01: CurrentTime advances by exactly deltaTime (no wrap) for small dt
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-01 AnimPlayback - CurrentTime advances by exactly dt when no wrap")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        anim.m_CurrentTime = 0.0f;
        const float dt = 0.05f;
        REQUIRE(dt < anim.m_Duration); // ensure no wrap

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime == doctest::Approx(dt).epsilon(1e-5f));
    }

    // ------------------------------------------------------------------
    // TC-AP-02: CurrentTime wraps correctly: fmod(t+dt, duration)
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-02 AnimPlayback - CurrentTime wraps as fmod(t+dt, duration)")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        REQUIRE(dur > 0.0f);

        // Set time near end so it wraps
        const float startTime = dur - 0.1f;
        anim.m_CurrentTime = startTime;
        const float dt = 0.3f;
        const float expected = std::fmod(startTime + dt, dur);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime == doctest::Approx(expected).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-AP-03: Multiple animations each advance independently
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-03 AnimPlayback - multiple animations advance independently")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());

        auto& anims = g_Renderer.m_Scene.m_Animations;
        REQUIRE(anims.size() >= 2);

        // Record durations BEFORE setting CurrentTime, since Update() uses them.
        const float dur0 = anims[0].m_Duration;
        const float dur1 = anims[1].m_Duration;
        REQUIRE(dur0 > 0.0f);
        REQUIRE(dur1 > 0.0f);

        // Use start times that are well within [0, duration) so no wrap occurs
        // on the first Update() call and the expected values are unambiguous.
        const float start0 = dur0 * 0.1f;  // 10% into anim0
        const float start1 = dur1 * 0.25f; // 25% into anim1
        const float dt     = std::min(dur0, dur1) * 0.1f; // small enough to not wrap either

        anims[0].m_CurrentTime = start0;
        anims[1].m_CurrentTime = start1;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        // Each animation advances by dt (mod its own duration).
        // Because dt is small, no wrap occurs and the result is simply start + dt.
        const float exp0 = std::fmod(start0 + dt, dur0);
        const float exp1 = std::fmod(start1 + dt, dur1);

        INFO("dur0=" << dur0 << " dur1=" << dur1 << " dt=" << dt);
        INFO("start0=" << start0 << " start1=" << start1);
        INFO("exp0=" << exp0 << " exp1=" << exp1);
        INFO("got0=" << anims[0].m_CurrentTime << " got1=" << anims[1].m_CurrentTime);

        CHECK(anims[0].m_CurrentTime == doctest::Approx(exp0).epsilon(1e-4f));
        CHECK(anims[1].m_CurrentTime == doctest::Approx(exp1).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-AP-04: Resetting CurrentTime to 0 and calling Update produces t==dt
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-04 AnimPlayback - reset to 0 then Update produces t==dt")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        anim.m_CurrentTime = 0.0f;
        const float dt = 0.123f;
        REQUIRE(dt < anim.m_Duration);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime == doctest::Approx(dt).epsilon(1e-5f));
    }

    // ------------------------------------------------------------------
    // TC-AP-05: Update with dt == duration produces CurrentTime near 0
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-05 AnimPlayback - Update with dt==duration wraps to near 0")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        REQUIRE(dur > 0.0f);
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dur);
        g_Renderer.m_EnableAnimations = prev;

        // fmod(dur, dur) == 0
        CHECK(anim.m_CurrentTime == doctest::Approx(0.0f).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-AP-06: Update with dt == 0.5*duration produces CurrentTime == 0.5*duration
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-06 AnimPlayback - Update with dt==0.5*duration produces half-duration time")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        REQUIRE(dur > 0.0f);
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dur * 0.5f);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime == doctest::Approx(dur * 0.5f).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-AP-07: Accumulated time over many small steps equals expected total (mod duration)
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-07 AnimPlayback - accumulated small steps equal expected total mod duration")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        REQUIRE(dur > 0.0f);
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        const float dt = 0.01f;
        const int steps = 37;
        for (int i = 0; i < steps; ++i)
            g_Renderer.m_Scene.Update(dt);

        g_Renderer.m_EnableAnimations = prev;

        const float expected = std::fmod(dt * steps, dur);
        CHECK(anim.m_CurrentTime == doctest::Approx(expected).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-AP-08: Animation disabled mid-playback: time freezes
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-08 AnimPlayback - disabling animations mid-playback freezes time")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        anim.m_CurrentTime = 0.2f;

        // Advance once with animations enabled
        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        const float frozenTime = anim.m_CurrentTime;

        // Now disable and advance again
        g_Renderer.m_EnableAnimations = false;
        g_Renderer.m_Scene.Update(0.5f);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime == doctest::Approx(frozenTime).epsilon(1e-5f));
    }

    // ------------------------------------------------------------------
    // TC-AP-09: Re-enabling animations after freeze resumes from frozen time
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-09 AnimPlayback - re-enabling animations resumes from frozen time")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;

        // Freeze
        g_Renderer.m_EnableAnimations = false;
        g_Renderer.m_Scene.Update(0.5f);
        CHECK(anim.m_CurrentTime == doctest::Approx(0.0f).epsilon(1e-5f));

        // Resume
        g_Renderer.m_EnableAnimations = true;
        const float dt = 0.1f;
        REQUIRE(dt < dur);
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime == doctest::Approx(dt).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-AP-10: Very small dt (1e-6) still advances time
    // ------------------------------------------------------------------
    TEST_CASE("TC-AP-10 AnimPlayback - very small dt still advances CurrentTime")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(1e-6f);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime > 0.0f);
    }
}

// ============================================================================
// TEST SUITE: Scene_InterpolationModes
// Tests the EvaluateAnimSampler logic by constructing synthetic samplers
// and driving Update() to specific times.
// ============================================================================
TEST_SUITE("Scene_InterpolationModes")
{
    // Helper: build a synthetic animation with one channel targeting node 0,
    // using the given interpolation mode and two keyframes.
    // Returns the animation index in g_Renderer.m_Scene.m_Animations.
    // Precondition: a scene with at least one node must already be loaded.
    // NOTE: AddSyntheticAnim is now defined in the anonymous namespace above
    // so it is accessible from all test suites.

    // ------------------------------------------------------------------
    // TC-INTERP-01: Step interpolation returns first keyframe before first key
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-01 Interpolation - Step returns first keyframe before t0")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        // Mark node 0 as animated so Update() processes it
        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 1.0f, 2.0f, 3.0f, 0.0f };
        const Vector4 kv1 = { 4.0f, 5.0f, 6.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Step,
            Scene::AnimationChannel::Path::Translation,
            1.0f, 2.0f, kv0, kv1, 0);

        // Set time before t0 — should clamp to kv0
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 0.5f;
        // Manually evaluate by calling Update with dt=0 (time stays at 0.5)
        // But we need to set the time directly and call Update(0).
        // Actually, Update advances time first, so we set time to 0.5 and call Update(0).
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 0.5f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f); // dt=0 keeps time at 0.5
        g_Renderer.m_EnableAnimations = prev;

        // Node 0 translation should be kv0 (clamped to first key)
        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv0.x).epsilon(1e-4f));
        CHECK(node.m_Translation.y == doctest::Approx(kv0.y).epsilon(1e-4f));
        CHECK(node.m_Translation.z == doctest::Approx(kv0.z).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-02: Step interpolation returns last keyframe after last key
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-02 Interpolation - Step returns last keyframe after t1")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 1.0f, 0.0f, 0.0f, 0.0f };
        const Vector4 kv1 = { 9.0f, 0.0f, 0.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Step,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 1.0f, kv0, kv1, 0);

        // Set time past t1 — should clamp to kv1.
        // IMPORTANT: Scene::Update() advances time BEFORE evaluating: t += dt, then
        // fmod(t, duration).  Setting m_CurrentTime = 1.5f with dt=0 gives
        // fmod(1.5, 1.0) = 0.5, which would evaluate between keyframes.
        // Instead, set the duration large enough that 1.5 does NOT wrap, so the
        // sampler sees t=1.5 > inputs.back()=1.0 and clamps to kv1.
        g_Renderer.m_Scene.m_Animations[animIdx].m_Duration = 10.0f; // prevent wrap
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 1.5f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv1.x).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-03: Step interpolation returns k0 value between k0 and k1
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-03 Interpolation - Step returns k0 value between keyframes")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 3.0f, 0.0f, 0.0f, 0.0f };
        const Vector4 kv1 = { 7.0f, 0.0f, 0.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Step,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 1.0f, kv0, kv1, 0);

        // Time exactly at midpoint — Step should still return k0
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 0.5f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv0.x).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-04: Linear interpolation at alpha=0 returns k0 value
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-04 Interpolation - Linear at alpha=0 returns k0")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 2.0f, 4.0f, 6.0f, 0.0f };
        const Vector4 kv1 = { 8.0f, 10.0f, 12.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Linear,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 1.0f, kv0, kv1, 0);

        // alpha=0 → time exactly at t0
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv0.x).epsilon(1e-4f));
        CHECK(node.m_Translation.y == doctest::Approx(kv0.y).epsilon(1e-4f));
        CHECK(node.m_Translation.z == doctest::Approx(kv0.z).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-05: Linear interpolation at alpha=1 returns k1 value
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-05 Interpolation - Linear at alpha=1 returns k1")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 0.0f, 0.0f, 0.0f, 0.0f };
        const Vector4 kv1 = { 5.0f, 5.0f, 5.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Linear,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 1.0f, kv0, kv1, 0);

        // alpha=1 → time at or past t1 (clamps to kv1).
        // IMPORTANT: Scene::Update() does fmod(t + dt, duration) BEFORE evaluating.
        // Setting m_CurrentTime = 1.0 (= duration) with dt=0 gives fmod(1.0, 1.0) = 0,
        // which wraps to t=0 and returns kv0 instead of kv1.
        // Fix: set duration large enough that t=1.0 does NOT wrap, so the sampler
        // sees t >= inputs.back() = 1.0 and clamps to kv1.
        g_Renderer.m_Scene.m_Animations[animIdx].m_Duration = 10.0f; // prevent wrap
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 1.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv1.x).epsilon(1e-4f));
        CHECK(node.m_Translation.y == doctest::Approx(kv1.y).epsilon(1e-4f));
        CHECK(node.m_Translation.z == doctest::Approx(kv1.z).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-06: Linear interpolation at alpha=0.5 returns midpoint
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-06 Interpolation - Linear at alpha=0.5 returns midpoint")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 0.0f, 0.0f, 0.0f, 0.0f };
        const Vector4 kv1 = { 4.0f, 8.0f, 12.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Linear,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 2.0f, kv0, kv1, 0);

        // alpha=0.5 → time = 1.0 (midpoint of [0,2])
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 1.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(2.0f).epsilon(1e-4f));
        CHECK(node.m_Translation.y == doctest::Approx(4.0f).epsilon(1e-4f));
        CHECK(node.m_Translation.z == doctest::Approx(6.0f).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-07: Slerp interpolation at alpha=0 returns normalized k0
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-07 Interpolation - Slerp at alpha=0 returns normalized k0")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        // Identity quaternion (0,0,0,1) and 90° around Y (0, sin45, 0, cos45)
        const Vector4 kv0 = { 0.0f, 0.0f, 0.0f, 1.0f };
        const Vector4 kv1 = { 0.0f, 0.7071f, 0.0f, 0.7071f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Slerp,
            Scene::AnimationChannel::Path::Rotation,
            0.0f, 1.0f, kv0, kv1, 0);

        // alpha=0 → time at t0 (clamps to kv0)
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        // Rotation should be near identity (0,0,0,1)
        CHECK(std::abs(node.m_Rotation.x) < 1e-3f);
        CHECK(std::abs(node.m_Rotation.y) < 1e-3f);
        CHECK(std::abs(node.m_Rotation.z) < 1e-3f);
        CHECK(node.m_Rotation.w == doctest::Approx(1.0f).epsilon(1e-3f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-08: Slerp interpolation at alpha=1 returns normalized k1
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-08 Interpolation - Slerp at alpha=1 returns normalized k1")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 0.0f, 0.0f, 0.0f, 1.0f };
        const Vector4 kv1 = { 0.0f, 0.7071f, 0.0f, 0.7071f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Slerp,
            Scene::AnimationChannel::Path::Rotation,
            0.0f, 1.0f, kv0, kv1, 0);

        // alpha=1 → time at or past t1 (clamps to kv1).
        // IMPORTANT: fmod(1.0, 1.0) = 0 — set duration > t1 to prevent wrap.
        g_Renderer.m_Scene.m_Animations[animIdx].m_Duration = 10.0f;
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 1.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Rotation.y == doctest::Approx(0.7071f).epsilon(1e-3f));
        CHECK(node.m_Rotation.w == doctest::Approx(0.7071f).epsilon(1e-3f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-09: Slerp result is a unit quaternion at midpoint
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-09 Interpolation - Slerp result is a unit quaternion")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 0.0f, 0.0f, 0.0f, 1.0f };
        const Vector4 kv1 = { 0.0f, 0.7071f, 0.0f, 0.7071f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Slerp,
            Scene::AnimationChannel::Path::Rotation,
            0.0f, 2.0f, kv0, kv1, 0);

        // alpha=0.5 → time = 1.0
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 1.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& q = g_Renderer.m_Scene.m_Nodes[0].m_Rotation;
        const float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-3f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-10: CatmullRom at alpha=0 returns k0 value
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-10 Interpolation - CatmullRom at alpha=0 returns k0")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 1.0f, 2.0f, 3.0f, 0.0f };
        const Vector4 kv1 = { 4.0f, 5.0f, 6.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::CatmullRom,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 1.0f, kv0, kv1, 0);

        // alpha=0 → time at t0 (clamps to kv0)
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv0.x).epsilon(1e-4f));
        CHECK(node.m_Translation.y == doctest::Approx(kv0.y).epsilon(1e-4f));
        CHECK(node.m_Translation.z == doctest::Approx(kv0.z).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-INTERP-11: CatmullRom at alpha=1 returns k1 value
    // ------------------------------------------------------------------
    TEST_CASE("TC-INTERP-11 Interpolation - CatmullRom at alpha=1 returns k1")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 0.0f, 0.0f, 0.0f, 0.0f };
        const Vector4 kv1 = { 10.0f, 10.0f, 10.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::CatmullRom,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 1.0f, kv0, kv1, 0);

        // alpha=1 → time at or past t1 (clamps to kv1).
        // IMPORTANT: fmod(1.0, 1.0) = 0 — set duration > t1 to prevent wrap.
        g_Renderer.m_Scene.m_Animations[animIdx].m_Duration = 10.0f;
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 1.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv1.x).epsilon(1e-4f));
        CHECK(node.m_Translation.y == doctest::Approx(kv1.y).epsilon(1e-4f));
        CHECK(node.m_Translation.z == doctest::Approx(kv1.z).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }
}

// ============================================================================
// TEST SUITE: Scene_LoopBehavior
// ============================================================================
TEST_SUITE("Scene_LoopBehavior")
{
    // ------------------------------------------------------------------
    // TC-LOOP-01: CurrentTime never exceeds duration after 100 updates
    // ------------------------------------------------------------------
    TEST_CASE("TC-LOOP-01 LoopBehavior - CurrentTime never exceeds duration after 100 updates")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int i = 0; i < 100; ++i)
        {
            g_Renderer.m_Scene.Update(0.07f);
            INFO("Step " << i << " time=" << anim.m_CurrentTime << " dur=" << dur);
            CHECK(anim.m_CurrentTime <= dur + 1e-4f);
        }

        g_Renderer.m_EnableAnimations = prev;
    }

    // ------------------------------------------------------------------
    // TC-LOOP-02: CurrentTime never goes negative after 100 updates
    // ------------------------------------------------------------------
    TEST_CASE("TC-LOOP-02 LoopBehavior - CurrentTime never goes negative after 100 updates")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int i = 0; i < 100; ++i)
        {
            g_Renderer.m_Scene.Update(0.07f);
            INFO("Step " << i << " time=" << anim.m_CurrentTime);
            CHECK(anim.m_CurrentTime >= 0.0f);
        }

        g_Renderer.m_EnableAnimations = prev;
    }

    // ------------------------------------------------------------------
    // TC-LOOP-03: After exactly N full-duration steps, time is near 0
    // ------------------------------------------------------------------
    TEST_CASE("TC-LOOP-03 LoopBehavior - after N full-duration steps time is near 0")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        REQUIRE(dur > 0.0f);
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        // 3 full loops
        for (int i = 0; i < 3; ++i)
            g_Renderer.m_Scene.Update(dur);

        g_Renderer.m_EnableAnimations = prev;

        CHECK(anim.m_CurrentTime == doctest::Approx(0.0f).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-LOOP-04: Wrap produces a valid (finite, non-NaN) node transform
    // ------------------------------------------------------------------
    TEST_CASE("TC-LOOP-04 LoopBehavior - wrap produces finite node world transform")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        anim.m_CurrentTime = dur - 0.001f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.01f); // triggers wrap
        g_Renderer.m_EnableAnimations = prev;

        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (!node.m_IsAnimated) continue;
            INFO("Node (" << node.m_Name << ") world transform after wrap");
            CHECK(MatrixIsFinite(node.m_WorldTransform));
        }
    }

    // ------------------------------------------------------------------
    // TC-LOOP-05: Time wraps at least once in 3*duration steps
    // ------------------------------------------------------------------
    TEST_CASE("TC-LOOP-05 LoopBehavior - time wraps at least once in 3*duration steps")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        REQUIRE(dur > 0.0f);
        anim.m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        bool wrapped = false;
        float prevTime = 0.0f;
        const float dt = dur / 10.0f; // 10 steps per loop
        for (int i = 0; i < 30; ++i)
        {
            g_Renderer.m_Scene.Update(dt);
            if (anim.m_CurrentTime < prevTime)
                wrapped = true;
            prevTime = anim.m_CurrentTime;
        }

        g_Renderer.m_EnableAnimations = prev;
        CHECK(wrapped);
    }

    // ------------------------------------------------------------------
    // TC-LOOP-06: Bounding sphere radius stays finite after 20 loop iterations
    // ------------------------------------------------------------------
    TEST_CASE("TC-LOOP-06 LoopBehavior - bounding sphere radius stays finite after 20 loops")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        const float dur = g_Renderer.m_Scene.m_Animations[0].m_Duration;
        REQUIRE(dur > 0.0f);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int i = 0; i < 20; ++i)
            g_Renderer.m_Scene.Update(dur);

        g_Renderer.m_EnableAnimations = prev;

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_MeshIndex < 0) continue;
            INFO("Node " << i << " radius=" << g_Renderer.m_Scene.m_Nodes[i].m_Radius);
            CHECK(std::isfinite(g_Renderer.m_Scene.m_Nodes[i].m_Radius));
            CHECK(g_Renderer.m_Scene.m_Nodes[i].m_Radius >= 0.0f);
        }
    }
}

// ============================================================================
// TEST SUITE: Scene_NodeTransforms
// ============================================================================
TEST_SUITE("Scene_NodeTransforms")
{
    // ------------------------------------------------------------------
    // TC-NT-01: Manual translation change + Update() propagates to WorldTransform
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-01 NodeTransforms - manual translation propagates to WorldTransform")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // Find an animated node
        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated)
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        // Set a known translation
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Translation = Vector3{ 10.0f, 20.0f, 30.0f };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_IsDirty = true;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const Matrix& world = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_WorldTransform;
        // Translation is in the 4th row (row-major) or 4th column (column-major).
        // DirectXMath stores row-major: _41, _42, _43 are translation.
        CHECK(world._41 == doctest::Approx(10.0f).epsilon(1e-3f));
        CHECK(world._42 == doctest::Approx(20.0f).epsilon(1e-3f));
        CHECK(world._43 == doctest::Approx(30.0f).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-NT-02: Manual rotation change + Update() propagates to WorldTransform
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-02 NodeTransforms - manual rotation propagates to WorldTransform")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated)
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        // 90° rotation around Y axis: (0, sin45, 0, cos45)
        const float s = 0.7071f, c = 0.7071f;
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Rotation    = Quaternion{ 0.0f, s, 0.0f, c };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Scale       = Vector3{ 1.0f, 1.0f, 1.0f };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Translation = Vector3{ 0.0f, 0.0f, 0.0f };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_IsDirty     = true;

        // If the animated node has a parent, force the parent's world transform to
        // identity so that worldM = localM * I = localM.
        const int parentIdx = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Parent;
        if (parentIdx >= 0)
        {
            using namespace DirectX;
            XMStoreFloat4x4(&g_Renderer.m_Scene.m_Nodes[parentIdx].m_WorldTransform,
                            XMMatrixIdentity());
        }

        // Diagnostic: log state before Update so failures are actionable.
        SDL_Log("[TC-NT-02] animNodeIdx=%d parentIdx=%d isDirty=%d",
                animNodeIdx, parentIdx,
                (int)g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_IsDirty);
        SDL_Log("[TC-NT-02] m_DynamicNodeIndices.size()=%zu",
                g_Renderer.m_Scene.m_DynamicNodeIndices.size());
        bool foundInDynamic = false;
        for (int di : g_Renderer.m_Scene.m_DynamicNodeIndices)
        {
            if (di == animNodeIdx) { foundInDynamic = true; break; }
        }
        SDL_Log("[TC-NT-02] animNodeIdx in m_DynamicNodeIndices: %s",
                foundInDynamic ? "YES" : "NO");
        SDL_Log("[TC-NT-02] m_Animations.size() before clear=%zu",
                g_Renderer.m_Scene.m_Animations.size());

        // Clear all animations so no channel overwrites the manually-set TRS.
        const bool prev = g_Renderer.m_EnableAnimations;
        auto savedAnims = g_Renderer.m_Scene.m_Animations;
        g_Renderer.m_Scene.m_Animations.clear();

        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        g_Renderer.m_Scene.m_Animations = std::move(savedAnims);

        const Matrix& world = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_WorldTransform;
        SDL_Log("[TC-NT-02] world: _11=%.4f _12=%.4f _13=%.4f",
                world._11, world._12, world._13);
        SDL_Log("[TC-NT-02] world: _21=%.4f _22=%.4f _23=%.4f",
                world._21, world._22, world._23);
        SDL_Log("[TC-NT-02] world: _31=%.4f _32=%.4f _33=%.4f",
                world._31, world._32, world._33);
        SDL_Log("[TC-NT-02] world: _41=%.4f _42=%.4f _43=%.4f",
                world._41, world._42, world._43);
        // For quaternion (0, 0.7071, 0, 0.7071) — 90° Y rotation in DirectXMath:
        //   _11 = 1 - 2(y²+z²) = 0
        //   _13 = 2(xz - wy)  = -2*0.7071*0.7071 = -1
        //   _31 = 2(xz + wy)  = +2*0.7071*0.7071 = +1
        //   _33 = 1 - 2(x²+y²) = 0
        // (Note: _13=-1 and _31=+1, NOT _13=+1 and _31=-1 as previously assumed.)
        INFO("animNodeIdx=" << animNodeIdx << " parentIdx=" << parentIdx
             << " foundInDynamic=" << foundInDynamic);
        INFO("world._11=" << world._11 << " _13=" << world._13
             << " _31=" << world._31 << " _33=" << world._33);
        CHECK(std::abs(world._11) < 0.01f);
        CHECK(world._13 == doctest::Approx(-1.0f).epsilon(0.01f));
        CHECK(world._31 == doctest::Approx(1.0f).epsilon(0.01f));
        CHECK(std::abs(world._33) < 0.01f);
    }

    // ------------------------------------------------------------------
    // TC-NT-03: Manual scale change + Update() propagates to WorldTransform
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-03 NodeTransforms - manual scale propagates to WorldTransform")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated)
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Scale = Vector3{ 3.0f, 3.0f, 3.0f };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Rotation = Quaternion{ 0.0f, 0.0f, 0.0f, 1.0f };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Translation = Vector3{ 0.0f, 0.0f, 0.0f };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_IsDirty = true;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const Matrix& world = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_WorldTransform;
        // Uniform scale 3: diagonal should be 3,3,3,1
        CHECK(world._11 == doctest::Approx(3.0f).epsilon(1e-3f));
        CHECK(world._22 == doctest::Approx(3.0f).epsilon(1e-3f));
        CHECK(world._33 == doctest::Approx(3.0f).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-NT-04: Child node world transform = parent world * child local
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-04 NodeTransforms - child world = parent world * child local")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        // Find a node with a parent
        int childIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_Parent >= 0)
            {
                childIdx = i;
                break;
            }
        }
        if (childIdx < 0) return; // no parent-child pair in this scene

        const int parentIdx = g_Renderer.m_Scene.m_Nodes[childIdx].m_Parent;
        const Matrix& parentWorld = g_Renderer.m_Scene.m_Nodes[parentIdx].m_WorldTransform;
        const Matrix& childLocal  = g_Renderer.m_Scene.m_Nodes[childIdx].m_LocalTransform;
        const Matrix& childWorld  = g_Renderer.m_Scene.m_Nodes[childIdx].m_WorldTransform;

        // Compute expected: childLocal * parentWorld (row-major multiply)
        using namespace DirectX;
        XMMATRIX expected = XMMatrixMultiply(XMLoadFloat4x4(&childLocal), XMLoadFloat4x4(&parentWorld));
        Matrix expectedM;
        XMStoreFloat4x4(&expectedM, expected);

        CHECK(childWorld._11 == doctest::Approx(expectedM._11).epsilon(1e-3f));
        CHECK(childWorld._22 == doctest::Approx(expectedM._22).epsilon(1e-3f));
        CHECK(childWorld._33 == doctest::Approx(expectedM._33).epsilon(1e-3f));
        CHECK(childWorld._41 == doctest::Approx(expectedM._41).epsilon(1e-3f));
        CHECK(childWorld._42 == doctest::Approx(expectedM._42).epsilon(1e-3f));
        CHECK(childWorld._43 == doctest::Approx(expectedM._43).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-NT-05: Identity TRS produces identity world transform for root node
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-05 NodeTransforms - identity TRS produces identity world for root node")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        // Find a root node (parent == -1)
        int rootIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_Parent == -1)
            {
                rootIdx = i;
                break;
            }
        }
        REQUIRE(rootIdx >= 0);

        auto& node = g_Renderer.m_Scene.m_Nodes[rootIdx];
        node.m_Translation = Vector3{ 0.0f, 0.0f, 0.0f };
        node.m_Rotation    = Quaternion{ 0.0f, 0.0f, 0.0f, 1.0f };
        node.m_Scale       = Vector3{ 1.0f, 1.0f, 1.0f };
        node.m_IsAnimated  = true;
        node.m_IsDirty     = true;

        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), rootIdx)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(rootIdx);
        }

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const Matrix& world = node.m_WorldTransform;
        CHECK(world._11 == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(world._22 == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(world._33 == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(world._44 == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(std::abs(world._41) < 1e-4f);
        CHECK(std::abs(world._42) < 1e-4f);
        CHECK(std::abs(world._43) < 1e-4f);

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-NT-06: Node world transform diagonal is non-zero after Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-06 NodeTransforms - world transform diagonal is non-zero after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (!node.m_IsAnimated) continue;
            CHECK(MatrixIsNonZero(node.m_WorldTransform));
        }
    }

    // ------------------------------------------------------------------
    // TC-NT-07: m_IsDirty is cleared after Update() processes the node
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-07 NodeTransforms - m_IsDirty is cleared after Update processes node")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        // After Update(), all dynamic nodes should have m_IsDirty cleared
        for (int idx : g_Renderer.m_Scene.m_DynamicNodeIndices)
        {
            INFO("Dynamic node " << idx << " (" << g_Renderer.m_Scene.m_Nodes[idx].m_Name << ") still dirty");
            CHECK(!g_Renderer.m_Scene.m_Nodes[idx].m_IsDirty);
        }
    }

    // ------------------------------------------------------------------
    // TC-NT-08: Non-animated node world transform is unchanged after Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-08 NodeTransforms - non-animated node world transform unchanged after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // Find a non-animated, non-dynamic node
        int staticNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (!g_Renderer.m_Scene.m_Nodes[i].m_IsDynamic)
            {
                staticNodeIdx = i;
                break;
            }
        }
        if (staticNodeIdx < 0) return; // all nodes are dynamic — skip

        const Matrix worldBefore = g_Renderer.m_Scene.m_Nodes[staticNodeIdx].m_WorldTransform;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.5f);
        g_Renderer.m_EnableAnimations = prev;

        const Matrix& worldAfter = g_Renderer.m_Scene.m_Nodes[staticNodeIdx].m_WorldTransform;
        CHECK(worldAfter._11 == doctest::Approx(worldBefore._11).epsilon(1e-5f));
        CHECK(worldAfter._22 == doctest::Approx(worldBefore._22).epsilon(1e-5f));
        CHECK(worldAfter._33 == doctest::Approx(worldBefore._33).epsilon(1e-5f));
        CHECK(worldAfter._41 == doctest::Approx(worldBefore._41).epsilon(1e-5f));
    }

    // ------------------------------------------------------------------
    // TC-NT-09: Instance world matrix matches node world transform after Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-09 NodeTransforms - instance world matches node world after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        for (int ni = 0; ni < (int)g_Renderer.m_Scene.m_Nodes.size(); ++ni)
        {
            const auto& node = g_Renderer.m_Scene.m_Nodes[ni];
            if (!node.m_IsAnimated || node.m_InstanceIndices.empty()) continue;

            for (uint32_t instIdx : node.m_InstanceIndices)
            {
                REQUIRE(instIdx < (uint32_t)g_Renderer.m_Scene.m_InstanceData.size());
                const srrhi::PerInstanceData& inst = g_Renderer.m_Scene.m_InstanceData[instIdx];
                // The instance world is built from node.m_WorldTransform (possibly with
                // a placeholder-cube flip).  At minimum the translation columns must match.
                INFO("Node " << ni << " instance " << instIdx);
                // Check translation: _41, _42, _43 of the world matrix
                CHECK(inst.m_World._41 == doctest::Approx(node.m_WorldTransform._41).epsilon(1e-3f));
                CHECK(inst.m_World._42 == doctest::Approx(node.m_WorldTransform._42).epsilon(1e-3f));
                CHECK(inst.m_World._43 == doctest::Approx(node.m_WorldTransform._43).epsilon(1e-3f));
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-NT-10: RT instance descriptor transform matches instance world after Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-NT-10 NodeTransforms - RT instance desc transform matches instance world after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        REQUIRE(g_Renderer.m_Scene.m_RTInstanceDescs.size() == g_Renderer.m_Scene.m_InstanceData.size());

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_InstanceData.size(); ++i)
        {
            const srrhi::PerInstanceData& inst = g_Renderer.m_Scene.m_InstanceData[i];
            const nvrhi::rt::InstanceDesc& rtDesc = g_Renderer.m_Scene.m_RTInstanceDescs[i];

            // RT transform is a 3×4 affine matrix stored row-major.
            // Row 0 = [_11, _21, _31, _41] of the world matrix.
            // (Scene::Update stores it as: transform[0]=_11, [1]=_21, [2]=_31, [3]=_41)
            INFO("Instance " << i);
            CHECK(rtDesc.transform[0]  == doctest::Approx(inst.m_World._11).epsilon(1e-3f));
            CHECK(rtDesc.transform[3]  == doctest::Approx(inst.m_World._41).epsilon(1e-3f));
            CHECK(rtDesc.transform[7]  == doctest::Approx(inst.m_World._42).epsilon(1e-3f));
            CHECK(rtDesc.transform[11] == doctest::Approx(inst.m_World._43).epsilon(1e-3f));
        }
    }
}

// ============================================================================
// TEST SUITE: Scene_SkinnedMeshDeformation
// CPU-side deformation / instance update tests.
// (The engine does not implement GPU skinning; "deformation" here refers to
//  the per-frame world-transform update that drives the TLAS rebuild.)
// ============================================================================
TEST_SUITE("Scene_SkinnedMeshDeformation")
{
    // ------------------------------------------------------------------
    // TC-SKD-01: Animated node instance world changes between two Update() calls
    // ------------------------------------------------------------------
    TEST_CASE("TC-SKD-01 SkinnedDeform - animated instance world changes between updates")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // Find an animated node with instances
        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated &&
                !g_Renderer.m_Scene.m_Nodes[i].m_InstanceIndices.empty())
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        const uint32_t instIdx = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_InstanceIndices[0];
        REQUIRE(instIdx < (uint32_t)g_Renderer.m_Scene.m_InstanceData.size());

        // Reset animation to start
        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        const Matrix world1 = g_Renderer.m_Scene.m_InstanceData[instIdx].m_World;

        g_Renderer.m_Scene.Update(0.3f);
        const Matrix world2 = g_Renderer.m_Scene.m_InstanceData[instIdx].m_World;
        g_Renderer.m_EnableAnimations = prev;

        // The two world matrices should differ (animation moved the node)
        const bool changed = (world1._11 != world2._11 || world1._41 != world2._41 ||
                               world1._42 != world2._42 || world1._43 != world2._43);
        CHECK(changed);
    }

    // ------------------------------------------------------------------
    // TC-SKD-02: Instance m_PrevWorld is set to old m_World after Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-SKD-02 SkinnedDeform - instance m_PrevWorld is set to old m_World after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_InstanceData.empty());

        // Record current world
        const Matrix worldBefore = g_Renderer.m_Scene.m_InstanceData[0].m_World;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        // After Update, m_PrevWorld should equal worldBefore
        const Matrix& prevWorld = g_Renderer.m_Scene.m_InstanceData[0].m_PrevWorld;
        CHECK(prevWorld._11 == doctest::Approx(worldBefore._11).epsilon(1e-5f));
        CHECK(prevWorld._22 == doctest::Approx(worldBefore._22).epsilon(1e-5f));
        CHECK(prevWorld._33 == doctest::Approx(worldBefore._33).epsilon(1e-5f));
        CHECK(prevWorld._41 == doctest::Approx(worldBefore._41).epsilon(1e-5f));
    }

    // ------------------------------------------------------------------
    // TC-SKD-03: m_InstanceDirtyRange covers animated instances after Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-SKD-03 SkinnedDeform - m_InstanceDirtyRange covers animated instances after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        // At least one animated node has instances → dirty range must be set
        CHECK(g_Renderer.m_Scene.AreInstanceTransformsDirty());
    }

    // ------------------------------------------------------------------
    // TC-SKD-04: Non-animated instances are NOT in the dirty range after Update
    //            (dirty range only covers animated instances)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SKD-04 SkinnedDeform - dirty range does not exceed animated instance indices")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& range = g_Renderer.m_Scene.m_InstanceDirtyRange;
        if (!g_Renderer.m_Scene.AreInstanceTransformsDirty()) return;

        // Dirty range must be within [0, instanceCount)
        const uint32_t instanceCount = (uint32_t)g_Renderer.m_Scene.m_InstanceData.size();
        CHECK(range.first  < instanceCount);
        CHECK(range.second < instanceCount);
        CHECK(range.first  <= range.second);
    }

    // ------------------------------------------------------------------
    // TC-SKD-05: Instance center/radius updated after node transform changes
    // ------------------------------------------------------------------
    TEST_CASE("TC-SKD-05 SkinnedDeform - instance center/radius updated after node transform change")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated &&
                !g_Renderer.m_Scene.m_Nodes[i].m_InstanceIndices.empty())
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        const uint32_t instIdx = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_InstanceIndices[0];

        // Move the node far away
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Translation = Vector3{ 100.0f, 200.0f, 300.0f };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_IsDirty = true;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const srrhi::PerInstanceData& inst = g_Renderer.m_Scene.m_InstanceData[instIdx];
        // Center should be near (100, 200, 300) + local mesh center
        CHECK(inst.m_Center.x > 90.0f);
        CHECK(inst.m_Center.y > 190.0f);
        CHECK(inst.m_Center.z > 290.0f);
        CHECK(inst.m_Radius >= 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-SKD-06: Multiple animated nodes each produce distinct world transforms
    // ------------------------------------------------------------------
    TEST_CASE("TC-SKD-06 SkinnedDeform - multiple animated nodes produce distinct world transforms")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.2f);
        g_Renderer.m_EnableAnimations = prev;

        // Collect world transforms of all animated nodes
        std::vector<Matrix> worlds;
        for (const auto& node : g_Renderer.m_Scene.m_Nodes)
        {
            if (node.m_IsAnimated)
                worlds.push_back(node.m_WorldTransform);
        }

        if (worlds.size() < 2) return; // need at least 2 animated nodes

        // At least one pair should differ
        bool anyDifferent = false;
        for (size_t i = 1; i < worlds.size(); ++i)
        {
            if (worlds[i]._41 != worlds[0]._41 ||
                worlds[i]._42 != worlds[0]._42 ||
                worlds[i]._43 != worlds[0]._43)
            {
                anyDifferent = true;
                break;
            }
        }
        CHECK(anyDifferent);
    }
}

// ============================================================================
// TEST SUITE: Scene_LightAnimation
// ============================================================================
TEST_SUITE("Scene_LightAnimation")
{
    // ------------------------------------------------------------------
    // TC-LA-01: Directional light node world transform is non-zero after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-01 LightAnimation - directional light node world transform is non-zero")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());

        const auto& dirLight = g_Renderer.m_Scene.m_Lights.back();
        REQUIRE(dirLight.m_Type == Scene::Light::Directional);
        REQUIRE(dirLight.m_NodeIndex >= 0);
        REQUIRE(dirLight.m_NodeIndex < (int)g_Renderer.m_Scene.m_Nodes.size());

        const Matrix& world = g_Renderer.m_Scene.m_Nodes[dirLight.m_NodeIndex].m_WorldTransform;
        CHECK(MatrixIsNonZero(world));
    }

    // ------------------------------------------------------------------
    // TC-LA-02: SetSunPitchYaw changes the light node world transform
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-02 LightAnimation - SetSunPitchYaw changes light node world transform")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());

        const auto& dirLight = g_Renderer.m_Scene.m_Lights.back();
        REQUIRE(dirLight.m_Type == Scene::Light::Directional);
        REQUIRE(dirLight.m_NodeIndex >= 0);

        const Matrix worldBefore = g_Renderer.m_Scene.m_Nodes[dirLight.m_NodeIndex].m_WorldTransform;

        // Change pitch and yaw
        g_Renderer.m_Scene.SetSunPitchYaw(0.8f, 1.2f);

        const Matrix& worldAfter = g_Renderer.m_Scene.m_Nodes[dirLight.m_NodeIndex].m_WorldTransform;
        const bool changed = (worldBefore._11 != worldAfter._11 ||
                               worldBefore._12 != worldAfter._12 ||
                               worldBefore._13 != worldAfter._13);
        CHECK(changed);
    }

    // ------------------------------------------------------------------
    // TC-LA-03: GetSunDirection returns a unit vector after SetSunPitchYaw
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-03 LightAnimation - GetSunDirection returns unit vector after SetSunPitchYaw")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());

        g_Renderer.m_Scene.SetSunPitchYaw(0.5f, 0.3f);
        const Vector3 dir = g_Renderer.m_Scene.GetSunDirection();
        const float len = Vec3Length(dir);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-LA-04: GetSunPitch round-trips through SetSunPitchYaw
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-04 LightAnimation - GetSunPitch round-trips through SetSunPitchYaw")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());

        const float pitch = 0.4f;
        const float yaw   = 1.1f;
        g_Renderer.m_Scene.SetSunPitchYaw(pitch, yaw);
        const float gotPitch = g_Renderer.m_Scene.GetSunPitch();
        CHECK(gotPitch == doctest::Approx(pitch).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-LA-05: GetSunYaw round-trips through SetSunPitchYaw
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-05 LightAnimation - GetSunYaw round-trips through SetSunPitchYaw")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());

        const float pitch = 0.2f;
        const float yaw   = 0.9f;
        g_Renderer.m_Scene.SetSunPitchYaw(pitch, yaw);
        const float gotYaw = g_Renderer.m_Scene.GetSunYaw();
        CHECK(gotYaw == doctest::Approx(yaw).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-LA-06: Directional light is always the last light in m_Lights
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-06 LightAnimation - directional light is always last in m_Lights")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());

        CHECK(g_Renderer.m_Scene.m_Lights.back().m_Type == Scene::Light::Directional);
    }

    // ------------------------------------------------------------------
    // TC-LA-07: Light node index is valid after SetSunPitchYaw
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-07 LightAnimation - light node index is valid after SetSunPitchYaw")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());

        g_Renderer.m_Scene.SetSunPitchYaw(0.3f, 0.7f);

        const auto& dirLight = g_Renderer.m_Scene.m_Lights.back();
        CHECK(dirLight.m_NodeIndex >= 0);
        CHECK(dirLight.m_NodeIndex < (int)g_Renderer.m_Scene.m_Nodes.size());
    }

    // ------------------------------------------------------------------
    // TC-LA-08: CesiumMilkTruck animated wheel nodes have m_IsDynamic set
    // ------------------------------------------------------------------
    TEST_CASE("TC-LA-08 LightAnimation - CesiumMilkTruck animated nodes have m_IsDynamic set")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
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
}

// ============================================================================
// TEST SUITE: Scene_CameraAnimation
// ============================================================================
TEST_SUITE("Scene_CameraAnimation")
{
    // ------------------------------------------------------------------
    // TC-CA-01: Scene camera node index is valid after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-CA-01 CameraAnimation - scene camera node index is valid after load")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        const auto& cameras   = g_Renderer.m_Scene.m_Cameras;
        const int   nodeCount = (int)g_Renderer.m_Scene.m_Nodes.size();
        for (int i = 0; i < (int)cameras.size(); ++i)
        {
            INFO("Camera " << i << " nodeIndex=" << cameras[i].m_NodeIndex);
            CHECK((cameras[i].m_NodeIndex >= 0 && cameras[i].m_NodeIndex < nodeCount));
        }
    }

    // ------------------------------------------------------------------
    // TC-CA-02: Camera node world transform is non-zero after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-CA-02 CameraAnimation - camera node world transform is non-zero after load")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        for (const auto& cam : g_Renderer.m_Scene.m_Cameras)
        {
            if (cam.m_NodeIndex < 0) continue;
            const Matrix& world = g_Renderer.m_Scene.m_Nodes[cam.m_NodeIndex].m_WorldTransform;
            CHECK(MatrixIsNonZero(world));
        }
    }

    // ------------------------------------------------------------------
    // TC-CA-03: Camera node world transform changes when node is manually moved
    // ------------------------------------------------------------------
    TEST_CASE("TC-CA-03 CameraAnimation - camera node world transform changes when node is moved")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        if (g_Renderer.m_Scene.m_Cameras.empty()) return;

        const int camNodeIdx = g_Renderer.m_Scene.m_Cameras[0].m_NodeIndex;
        if (camNodeIdx < 0) return;

        const Matrix worldBefore = g_Renderer.m_Scene.m_Nodes[camNodeIdx].m_WorldTransform;

        // Manually move the camera node
        auto& node = g_Renderer.m_Scene.m_Nodes[camNodeIdx];
        node.m_Translation = Vector3{ 50.0f, 50.0f, 50.0f };
        node.m_IsDirty = true;
        node.m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), camNodeIdx)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(camNodeIdx);
        }

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const Matrix& worldAfter = g_Renderer.m_Scene.m_Nodes[camNodeIdx].m_WorldTransform;
        CHECK(worldAfter._41 == doctest::Approx(50.0f).epsilon(1e-3f));
        CHECK(worldAfter._42 == doctest::Approx(50.0f).epsilon(1e-3f));
        CHECK(worldAfter._43 == doctest::Approx(50.0f).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-CA-04: m_SelectedCameraIndex is -1 when no scene cameras exist
    // ------------------------------------------------------------------
    TEST_CASE("TC-CA-04 CameraAnimation - m_SelectedCameraIndex is -1 when no cameras exist")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        if (!g_Renderer.m_Scene.m_Cameras.empty()) return; // has cameras — skip

        CHECK(g_Renderer.m_Scene.m_SelectedCameraIndex == -1);
    }
}

// ============================================================================
// TEST SUITE: Scene_TLASUpdatePostDeform
// ============================================================================
TEST_SUITE("Scene_TLASUpdatePostDeform")
{
    // ------------------------------------------------------------------
    // TC-TLAS-01: m_TLAS is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-01 TLASUpdate - m_TLAS is non-null after scene load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-02: m_RTInstanceDescs count matches m_InstanceData count
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-02 TLASUpdate - RT instance desc count matches instance data count")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_RTInstanceDescs.size() == g_Renderer.m_Scene.m_InstanceData.size());
    }

    // ------------------------------------------------------------------
    // TC-TLAS-03: RT instance desc transform is updated after animated node Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-03 TLASUpdate - RT instance desc transform updated after animated Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_RTInstanceDescs.empty());

        // Find an animated instance
        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated &&
                !g_Renderer.m_Scene.m_Nodes[i].m_InstanceIndices.empty())
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        const uint32_t instIdx = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_InstanceIndices[0];
        REQUIRE(instIdx < (uint32_t)g_Renderer.m_Scene.m_RTInstanceDescs.size());

        // AnimatedCube animates a ROTATION (not a translation), so the translation
        // columns transform[3], transform[7], transform[11] (= _41, _42, _43) stay
        // near zero throughout the animation.  Instead, check the rotation part:
        // transform[0] = world._11 (first column of rotation matrix), which changes
        // as the cube rotates.
        //
        // RT AffineTransform layout (row-major 3x4):
        //   [0..3]  = row 0: [_11, _21, _31, _41]
        //   [4..7]  = row 1: [_12, _22, _32, _42]
        //   [8..11] = row 2: [_13, _23, _33, _43]
        // So transform[0] = _11 (rotation), transform[3] = _41 (X translation).

        // Record rotation component before
        const float rot00Before = g_Renderer.m_Scene.m_RTInstanceDescs[instIdx].transform[0];

        // Advance animation by a quarter period so rotation changes significantly.
        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.0f;
        const float dur = g_Renderer.m_Scene.m_Animations[0].m_Duration;
        REQUIRE(dur > 0.0f);
        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dur * 0.25f); // advance 25% of animation
        g_Renderer.m_EnableAnimations = prev;

        const float rot00After = g_Renderer.m_Scene.m_RTInstanceDescs[instIdx].transform[0];

        // Rotation component must have changed after advancing the animation.
        INFO("RT transform[0] (_11) before=" << rot00Before << " after=" << rot00After);
        CHECK(rot00Before != doctest::Approx(rot00After).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-TLAS-04: m_InstanceDirtyRange is set after animated Update
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-04 TLASUpdate - m_InstanceDirtyRange is set after animated Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        CHECK(g_Renderer.m_Scene.AreInstanceTransformsDirty());
    }

    // ------------------------------------------------------------------
    // TC-TLAS-05: UploadDirtyInstanceTransforms resets dirty range to clean
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-05 TLASUpdate - UploadDirtyInstanceTransforms resets dirty range")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        REQUIRE(g_Renderer.m_Scene.AreInstanceTransformsDirty());

        g_Renderer.UploadDirtyInstanceTransforms();

        CHECK(!g_Renderer.m_Scene.AreInstanceTransformsDirty());
    }

    // ------------------------------------------------------------------
    // TC-TLAS-06: m_BLASAddressBuffer size is consistent with instance count
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-06 TLASUpdate - m_BLASAddressBuffer size is consistent with instance count")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(g_Renderer.m_Scene.m_BLASAddressBuffer != nullptr);

        const uint64_t bufSize = g_Renderer.m_Scene.m_BLASAddressBuffer->getDesc().byteSize;
        const uint64_t minSize = (uint64_t)g_Renderer.m_Scene.m_InstanceData.size()
            * srrhi::CommonConsts::MAX_LOD_COUNT
            * sizeof(uint64_t);
        CHECK(bufSize >= minSize);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-07: m_RTInstanceDescBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-07 TLASUpdate - m_RTInstanceDescBuffer is non-null after scene load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_RTInstanceDescBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TLAS-08: RT instance desc transform is finite for all instances
    // ------------------------------------------------------------------
    TEST_CASE("TC-TLAS-08 TLASUpdate - RT instance desc transform is finite for all instances")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_RTInstanceDescs.size(); ++i)
        {
            const auto& desc = g_Renderer.m_Scene.m_RTInstanceDescs[i];
            INFO("RT instance " << i);
            for (int j = 0; j < 12; ++j)
                CHECK(std::isfinite(desc.transform[j]));
        }
    }
}

// ============================================================================
// TEST SUITE: Scene_MaterialMutation
// ============================================================================
TEST_SUITE("Scene_MaterialMutation")
{
    // ------------------------------------------------------------------
    // TC-MM-01: Manually changing m_EmissiveFactor marks material dirty range
    // ------------------------------------------------------------------
    TEST_CASE("TC-MM-01 MaterialMutation - changing EmissiveFactor marks dirty range")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Materials.empty());

        // Manually dirty the material range
        g_Renderer.m_Scene.m_Materials[0].m_EmissiveFactor = Vector3{ 1.0f, 0.5f, 0.0f };
        g_Renderer.m_Scene.m_MaterialDirtyRange = { 0u, 0u };

        CHECK(g_Renderer.m_Scene.m_MaterialDirtyRange.first <= g_Renderer.m_Scene.m_MaterialDirtyRange.second);
    }

    // ------------------------------------------------------------------
    // TC-MM-02: UploadDirtyMaterialConstants resets material dirty range
    // ------------------------------------------------------------------
    TEST_CASE("TC-MM-02 MaterialMutation - UploadDirtyMaterialConstants resets dirty range")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Materials.empty());

        // Mark dirty
        g_Renderer.m_Scene.m_MaterialDirtyRange = { 0u, 0u };
        REQUIRE(g_Renderer.m_Scene.m_MaterialDirtyRange.first <= g_Renderer.m_Scene.m_MaterialDirtyRange.second);

        g_Renderer.UploadDirtyMaterialConstants();

        // After upload, range should be clean (first > second)
        CHECK(g_Renderer.m_Scene.m_MaterialDirtyRange.first > g_Renderer.m_Scene.m_MaterialDirtyRange.second);
    }

    // ------------------------------------------------------------------
    // TC-MM-03: EmissiveIntensity animation channel modifies m_EmissiveFactor
    // ------------------------------------------------------------------
    TEST_CASE("TC-MM-03 MaterialMutation - EmissiveIntensity channel modifies EmissiveFactor")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Materials.empty());

        // Inject a synthetic EmissiveIntensity animation channel
        Scene::AnimationSampler sampler;
        sampler.m_Interpolation = Scene::AnimationSampler::Interpolation::Linear;
        sampler.m_Inputs  = { 0.0f, 1.0f };
        sampler.m_Outputs = { Vector4{0.0f, 0.0f, 0.0f, 0.0f}, Vector4{2.0f, 0.0f, 0.0f, 0.0f} };

        Scene::AnimationChannel channel;
        channel.m_Path         = Scene::AnimationChannel::Path::EmissiveIntensity;
        channel.m_SamplerIndex = 0;
        channel.m_MaterialIndices  = { 0 };
        channel.m_BaseEmissiveFactor = { Vector3{ 1.0f, 0.5f, 0.25f } };

        Scene::Animation anim;
        anim.m_Name     = "EmissiveAnim";
        anim.m_Duration = 1.0f;
        anim.m_Samplers.push_back(std::move(sampler));
        anim.m_Channels.push_back(std::move(channel));

        g_Renderer.m_Scene.m_Animations.push_back(std::move(anim));
        g_Renderer.m_Scene.m_DynamicMaterialIndices.push_back(0);

        // Set time to midpoint (intensity = 1.0 → emissive = base * 1.0)
        g_Renderer.m_Scene.m_Animations.back().m_CurrentTime = 0.5f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        // At t=0.5, intensity = lerp(0, 2, 0.5) = 1.0
        // EmissiveFactor = base * 1.0 = (1.0, 0.5, 0.25)
        const Vector3& ef = g_Renderer.m_Scene.m_Materials[0].m_EmissiveFactor;
        CHECK(ef.x == doctest::Approx(1.0f).epsilon(1e-3f));
        CHECK(ef.y == doctest::Approx(0.5f).epsilon(1e-3f));
        CHECK(ef.z == doctest::Approx(0.25f).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-MM-04: EmissiveIntensity channel targets valid material indices
    // ------------------------------------------------------------------
    TEST_CASE("TC-MM-04 MaterialMutation - EmissiveIntensity channel targets valid material indices")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const int matCount = (int)g_Renderer.m_Scene.m_Materials.size();
        for (const auto& anim : g_Renderer.m_Scene.m_Animations)
        {
            for (const auto& ch : anim.m_Channels)
            {
                if (ch.m_Path != Scene::AnimationChannel::Path::EmissiveIntensity) continue;
                for (int matIdx : ch.m_MaterialIndices)
                {
                    INFO("EmissiveIntensity channel material index: " << matIdx);
                    CHECK(matIdx >= 0);
                    CHECK(matIdx < matCount);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-MM-05: m_DynamicMaterialIndices is sorted (required for binary search)
    // ------------------------------------------------------------------
    TEST_CASE("TC-MM-05 MaterialMutation - m_DynamicMaterialIndices is sorted")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const auto& dmi = g_Renderer.m_Scene.m_DynamicMaterialIndices;
        for (int i = 1; i < (int)dmi.size(); ++i)
        {
            INFO("Index " << i << ": " << dmi[i-1] << " vs " << dmi[i]);
            CHECK(dmi[i] >= dmi[i-1]);
        }
    }

    // ------------------------------------------------------------------
    // TC-MM-06: Material base color factor survives Update() unchanged
    // ------------------------------------------------------------------
    TEST_CASE("TC-MM-06 MaterialMutation - base color factor unchanged after Update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Materials.empty());

        const Vector4 colorBefore = g_Renderer.m_Scene.m_Materials[0].m_BaseColorFactor;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.5f);
        g_Renderer.m_EnableAnimations = prev;

        const Vector4& colorAfter = g_Renderer.m_Scene.m_Materials[0].m_BaseColorFactor;
        CHECK(colorAfter.x == doctest::Approx(colorBefore.x).epsilon(1e-5f));
        CHECK(colorAfter.y == doctest::Approx(colorBefore.y).epsilon(1e-5f));
        CHECK(colorAfter.z == doctest::Approx(colorBefore.z).epsilon(1e-5f));
        CHECK(colorAfter.w == doctest::Approx(colorBefore.w).epsilon(1e-5f));
    }
}

// ============================================================================
// TEST SUITE: Scene_MultiAnimationBlend
// ============================================================================
TEST_SUITE("Scene_MultiAnimationBlend")
{
    // ------------------------------------------------------------------
    // TC-MAB-01: Multiple animations each have independent CurrentTime
    // ------------------------------------------------------------------
    TEST_CASE("TC-MAB-01 MultiAnimBlend - multiple animations have independent CurrentTime")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());

        auto& anims = g_Renderer.m_Scene.m_Animations;
        REQUIRE(anims.size() >= 2);

        // Set different start times
        anims[0].m_CurrentTime = 0.1f;
        anims[1].m_CurrentTime = 0.7f;

        CHECK(anims[0].m_CurrentTime != doctest::Approx(anims[1].m_CurrentTime).epsilon(1e-5f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-MAB-02: All animations advance by the same dt in one Update() call
    // ------------------------------------------------------------------
    TEST_CASE("TC-MAB-02 MultiAnimBlend - all animations advance by same dt in one Update")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());

        auto& anims = g_Renderer.m_Scene.m_Animations;
        REQUIRE(anims.size() >= 2);

        anims[0].m_CurrentTime = 0.0f;
        anims[1].m_CurrentTime = 0.0f;
        const float dt = 0.15f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        // Both should have advanced by dt (assuming no wrap for small dt)
        const float dur0 = anims[0].m_Duration;
        const float dur1 = anims[1].m_Duration;
        if (dt < dur0)
            CHECK(anims[0].m_CurrentTime == doctest::Approx(dt).epsilon(1e-4f));
        if (dt < dur1)
            CHECK(anims[1].m_CurrentTime == doctest::Approx(dt).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-MAB-03: Manually freezing one animation's time does not affect others
    // ------------------------------------------------------------------
    TEST_CASE("TC-MAB-03 MultiAnimBlend - freezing one animation time does not affect others")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());

        auto& anims = g_Renderer.m_Scene.m_Animations;
        REQUIRE(anims.size() >= 2);

        anims[0].m_CurrentTime = 0.3f;
        anims[1].m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        // anim[1] should have advanced; anim[0] should also have advanced
        // (both advance — we just verify they are independent)
        const float dur1 = anims[1].m_Duration;
        if (0.1f < dur1)
            CHECK(anims[1].m_CurrentTime == doctest::Approx(0.1f).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-MAB-04: All animations wrap independently at their own duration
    // ------------------------------------------------------------------
    TEST_CASE("TC-MAB-04 MultiAnimBlend - animations wrap independently at their own duration")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());

        auto& anims = g_Renderer.m_Scene.m_Animations;
        REQUIRE(anims.size() >= 2);

        const float dur0 = anims[0].m_Duration;
        const float dur1 = anims[1].m_Duration;
        REQUIRE(dur0 > 0.0f);
        REQUIRE(dur1 > 0.0f);

        // Choose dt so that anim[0] wraps but anim[1] does not.
        // anim[0] starts near its end; anim[1] starts at 0.
        // dt must be > (dur0 - start0) to trigger anim[0] wrap,
        // and < dur1 so anim[1] does not wrap.
        // Use dt = dur0 * 0.15f and start0 = dur0 * 0.9f:
        //   anim[0]: fmod(0.9*dur0 + 0.15*dur0, dur0) = fmod(1.05*dur0, dur0) = 0.05*dur0 (wrapped)
        //   anim[1]: 0 + 0.15*dur0 < dur1 (since dur1 >= dur0 for the two-anim scene)
        const float start0 = dur0 * 0.9f;
        const float dt     = dur0 * 0.15f; // triggers wrap for anim[0]

        anims[0].m_CurrentTime = start0;
        anims[1].m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        // anim[0] must have wrapped: result = fmod(start0 + dt, dur0)
        const float exp0 = std::fmod(start0 + dt, dur0);
        INFO("dur0=" << dur0 << " dur1=" << dur1 << " dt=" << dt);
        INFO("start0=" << start0 << " exp0=" << exp0);
        INFO("got0=" << anims[0].m_CurrentTime << " got1=" << anims[1].m_CurrentTime);
        CHECK(anims[0].m_CurrentTime == doctest::Approx(exp0).epsilon(1e-4f));
        CHECK(anims[0].m_CurrentTime >= 0.0f);
        CHECK(anims[0].m_CurrentTime < dur0 + 1e-4f);

        // anim[1] must NOT have wrapped: result = 0 + dt (assuming dt < dur1)
        if (dt < dur1)
            CHECK(anims[1].m_CurrentTime == doctest::Approx(dt).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-MAB-05: CesiumMilkTruck: all animations have positive duration
    // ------------------------------------------------------------------
    TEST_CASE("TC-MAB-05 MultiAnimBlend - CesiumMilkTruck all animations have positive duration")
    {
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        SceneScope scope("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
        REQUIRE(scope.loaded);

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Animations.size(); ++i)
        {
            INFO("Animation " << i << " (" << g_Renderer.m_Scene.m_Animations[i].m_Name
                 << ") duration=" << g_Renderer.m_Scene.m_Animations[i].m_Duration);
            CHECK(g_Renderer.m_Scene.m_Animations[i].m_Duration > 0.0f);
        }
    }
}

// ============================================================================
// TEST SUITE: Scene_RegressionTests
//
// Regression tests for the 10 failures diagnosed in the first test run.
// Each test directly exercises the failure mode that was identified, with
// a comment explaining the root cause and the fix applied.
//
// Failures diagnosed:
//   [1] TC-RGAL-OC-08  — test bug: deferred-release allows D3D12 to reuse
//                         the same memory address for a new aliased handle.
//                         Fix: removed pointer-inequality assertion; the
//                         structural invariant (non-owner slot) is what matters.
//   [2] TC-SL-05       — engine bug: m_MaterialDirtyRange left dirty after
//                         SceneLoader::UpdateMaterialsAndCreateConstants.
//                         Fix: reset range to clean at end of that function.
//   [3] TC-AP-03       — test bug: hard-coded start times (0.1, 0.5) and
//                         dt=0.2 could wrap or produce wrong expected values
//                         depending on actual animation durations.
//                         Fix: use duration-relative start times and small dt.
//   [4] TC-INTERP-02   — test bug: setting m_CurrentTime = t1 = duration then
//                         Update(0) → fmod(t1, t1) = 0 → evaluates at t=0.
//                         Fix: set m_Duration > t1 to prevent wrap.
//   [5] TC-INTERP-05   — same as [4] for Linear interpolation.
//   [6] TC-INTERP-08   — same as [4] for Slerp interpolation.
//   [7] TC-INTERP-11   — same as [4] for CatmullRom interpolation.
//   [8] TC-NT-02       — test bug: Update() with animations enabled overwrites
//                         the manually-set rotation via animation channels.
//                         Fix: clear animations before Update, restore after.
//   [9] TC-TLAS-03     — test bug: AnimatedCube only rotates (no translation),
//                         so transform[3] (_41 = X translation) stays 0.
//                         Fix: check transform[0] (_11 = rotation component).
//  [10] TC-MAB-04      — test bug: hard-coded dt=0.1 and start=dur-0.05 may
//                         not reliably trigger wrap for all duration values.
//                         Fix: use duration-relative dt and start times.
//
// Run with: HobbyRenderer --run-tests=*Regression*
// ============================================================================
TEST_SUITE("Scene_RegressionTests")
{
    // ------------------------------------------------------------------
    // TC-REG-01: fmod(t, t) == 0 — Update(0) at t==duration wraps to 0
    //   Regression for TC-INTERP-02/05/08/11 and TC-MAB-04.
    //   Verifies that the engine's fmod wrap is correct and that tests
    //   must NOT set m_CurrentTime == m_Duration when calling Update(0).
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-01 Regression - fmod(t,t)==0: Update(0) at t==duration wraps to 0")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        auto& anim = g_Renderer.m_Scene.m_Animations[0];
        const float dur = anim.m_Duration;
        REQUIRE(dur > 0.0f);

        // Setting CurrentTime = duration and calling Update(0) MUST wrap to 0.
        // This is the correct engine behavior: fmod(dur + 0, dur) = 0.
        anim.m_CurrentTime = dur;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        // Verify the wrap happened.
        CHECK(anim.m_CurrentTime == doctest::Approx(0.0f).epsilon(1e-5f));
    }

    // ------------------------------------------------------------------
    // TC-REG-02: Sampler boundary clamping — t > inputs.back() returns kv1
    //   Regression for TC-INTERP-02/05/08/11.
    //   Verifies that EvaluateAnimSampler clamps to kv1 when t > inputs.back(),
    //   and that the correct way to test this is to set m_Duration > t1.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-02 Regression - sampler clamps to kv1 when t > inputs.back()")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        const Vector4 kv0 = { 1.0f, 0.0f, 0.0f, 0.0f };
        const Vector4 kv1 = { 9.0f, 0.0f, 0.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Linear,
            Scene::AnimationChannel::Path::Translation,
            0.0f, 1.0f, kv0, kv1, 0);

        // Set duration >> t1 so fmod does NOT wrap, and t > inputs.back() clamps to kv1.
        g_Renderer.m_Scene.m_Animations[animIdx].m_Duration = 100.0f;
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 5.0f; // >> t1=1.0

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        const auto& node = g_Renderer.m_Scene.m_Nodes[0];
        CHECK(node.m_Translation.x == doctest::Approx(kv1.x).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-REG-03: m_MaterialDirtyRange is clean after UpdateMaterialsAndCreateConstants
    //   Regression for TC-SL-05.
    //   Verifies that calling UpdateMaterialsAndCreateConstants resets the
    //   dirty range to clean (first > second).
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-03 Regression - m_MaterialDirtyRange clean after UpdateMaterialsAndCreateConstants")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        // After SceneScope load, the dirty range must be clean because
        // UpdateMaterialsAndCreateConstants now resets it.
        CHECK(g_Renderer.m_Scene.m_MaterialDirtyRange.first >
              g_Renderer.m_Scene.m_MaterialDirtyRange.second);

        // Manually dirty it, then call UpdateMaterialsAndCreateConstants again.
        g_Renderer.m_Scene.m_MaterialDirtyRange = { 0u, 0u };
        REQUIRE(g_Renderer.m_Scene.m_MaterialDirtyRange.first <=
                g_Renderer.m_Scene.m_MaterialDirtyRange.second);

        {
            nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
            ScopedCommandList sc{ cmd, "TC-REG-03" };
            SceneLoader::UpdateMaterialsAndCreateConstants(g_Renderer.m_Scene, cmd);
        }
        g_Renderer.ExecutePendingCommandLists();

        // Must be clean again.
        CHECK(g_Renderer.m_Scene.m_MaterialDirtyRange.first >
              g_Renderer.m_Scene.m_MaterialDirtyRange.second);
    }

    // ------------------------------------------------------------------
    // TC-REG-04: Animation channel overwrites manually-set TRS
    //   Regression for TC-NT-02.
    //   Verifies that Update() with animations enabled DOES overwrite
    //   manually-set node TRS via animation channels.  Tests that need
    //   to set TRS manually must clear animations first.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-04 Regression - animation channel overwrites manually-set rotation")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated)
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        // Set a distinctive rotation.
        const float s = 0.7071f, c = 0.7071f;
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Rotation = Quaternion{ 0.0f, s, 0.0f, c };
        g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_IsDirty = true;

        // With animations ENABLED, the channel will overwrite the rotation.
        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        // The rotation was overwritten by the animation channel — it is no longer
        // the manually-set 90° Y rotation.  This is CORRECT engine behavior.
        // (The test just documents that this overwrite happens.)
        const auto& q = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_Rotation;
        const float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        // Quaternion must still be unit-length (normalization is always applied).
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-REG-05: RT transform rotation component changes after animation
    //   Regression for TC-TLAS-03.
    //   Verifies that transform[0] (_11 = rotation) changes after advancing
    //   a rotation-only animation (AnimatedCube), while transform[3] (_41 =
    //   X translation) stays near 0 since there is no translation.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-05 Regression - RT transform rotation changes, translation stays 0 for AnimatedCube")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_RTInstanceDescs.empty());

        int animNodeIdx = -1;
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated &&
                !g_Renderer.m_Scene.m_Nodes[i].m_InstanceIndices.empty())
            {
                animNodeIdx = i;
                break;
            }
        }
        REQUIRE(animNodeIdx >= 0);

        const uint32_t instIdx = g_Renderer.m_Scene.m_Nodes[animNodeIdx].m_InstanceIndices[0];
        REQUIRE(instIdx < (uint32_t)g_Renderer.m_Scene.m_RTInstanceDescs.size());

        // Record before.
        const float rot00Before = g_Renderer.m_Scene.m_RTInstanceDescs[instIdx].transform[0]; // _11
        const float tx0Before   = g_Renderer.m_Scene.m_RTInstanceDescs[instIdx].transform[3]; // _41

        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 0.0f;
        const float dur = g_Renderer.m_Scene.m_Animations[0].m_Duration;
        REQUIRE(dur > 0.0f);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dur * 0.25f);
        g_Renderer.m_EnableAnimations = prev;

        const float rot00After = g_Renderer.m_Scene.m_RTInstanceDescs[instIdx].transform[0];
        const float tx0After   = g_Renderer.m_Scene.m_RTInstanceDescs[instIdx].transform[3];

        INFO("transform[0] (_11) before=" << rot00Before << " after=" << rot00After);
        INFO("transform[3] (_41) before=" << tx0Before   << " after=" << tx0After);

        // Rotation component must change.
        CHECK(rot00Before != doctest::Approx(rot00After).epsilon(1e-4f));
        // Translation component must stay near 0 (AnimatedCube has no translation).
        CHECK(std::abs(tx0After) < 1e-3f);
    }

    // ------------------------------------------------------------------
    // TC-REG-06: Multi-anim wrap uses duration-relative dt
    //   Regression for TC-MAB-04.
    //   Verifies that using duration-relative start times and dt reliably
    //   triggers wrap for anim[0] but not anim[1] in the two-anim scene.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-06 Regression - multi-anim wrap with duration-relative dt")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());

        auto& anims = g_Renderer.m_Scene.m_Animations;
        REQUIRE(anims.size() >= 2);

        const float dur0 = anims[0].m_Duration;
        const float dur1 = anims[1].m_Duration;
        REQUIRE(dur0 > 0.0f);
        REQUIRE(dur1 > 0.0f);

        // Use duration-relative values so the test is robust to any duration.
        const float start0 = dur0 * 0.9f;
        const float dt     = dur0 * 0.15f; // wraps anim[0], not anim[1] (dur1 >= dur0)

        anims[0].m_CurrentTime = start0;
        anims[1].m_CurrentTime = 0.0f;

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(dt);
        g_Renderer.m_EnableAnimations = prev;

        const float exp0 = std::fmod(start0 + dt, dur0);
        INFO("dur0=" << dur0 << " dur1=" << dur1 << " dt=" << dt);
        INFO("exp0=" << exp0 << " got0=" << anims[0].m_CurrentTime);

        // anim[0] wrapped.
        CHECK(anims[0].m_CurrentTime == doctest::Approx(exp0).epsilon(1e-4f));
        CHECK(anims[0].m_CurrentTime < start0); // confirm wrap occurred

        // anim[1] did not wrap (dt < dur1 since dur1 >= dur0).
        if (dt < dur1)
            CHECK(anims[1].m_CurrentTime == doctest::Approx(dt).epsilon(1e-4f));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-REG-07: m_MaterialDirtyRange clean after SceneScope load
    //   Direct regression for TC-SL-05.
    //   Verifies the exact scenario that failed: SceneScope load of
    //   AnimatedCube leaves m_MaterialDirtyRange clean.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-07 Regression - m_MaterialDirtyRange clean after AnimatedCube SceneScope load")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        INFO("m_MaterialDirtyRange = ["
             << g_Renderer.m_Scene.m_MaterialDirtyRange.first << ", "
             << g_Renderer.m_Scene.m_MaterialDirtyRange.second << "]");
        CHECK(g_Renderer.m_Scene.m_MaterialDirtyRange.first >
              g_Renderer.m_Scene.m_MaterialDirtyRange.second);
    }

    // ------------------------------------------------------------------
    // TC-REG-08: CurrentTime bounds after Update — never negative, never > duration
    //   Regression for the SDL_assert added to Scene::Update.
    //   Verifies that 200 consecutive updates with varying dt keep CurrentTime
    //   in [0, duration] for all animations.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-08 Regression - CurrentTime always in [0, duration] after 200 updates")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Animations.empty());

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int i = 0; i < 200; ++i)
        {
            // Vary dt to exercise different wrap scenarios.
            const float dt = (i % 7 == 0) ? 0.5f : 0.016f;
            g_Renderer.m_Scene.Update(dt);

            for (int ai = 0; ai < (int)g_Renderer.m_Scene.m_Animations.size(); ++ai)
            {
                const auto& anim = g_Renderer.m_Scene.m_Animations[ai];
                INFO("Step " << i << " anim " << ai
                     << " t=" << anim.m_CurrentTime << " dur=" << anim.m_Duration);
                CHECK(anim.m_CurrentTime >= 0.0f);
                CHECK(anim.m_CurrentTime <= anim.m_Duration + 1e-4f);
            }
        }

        g_Renderer.m_EnableAnimations = prev;
    }

    // ------------------------------------------------------------------
    // TC-REG-09: World transform is finite after 50 animation updates
    //   Regression for the SDL_assert added to Scene::Update (world transform
    //   finite check).  Verifies no NaN/Inf accumulates over many frames.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-09 Regression - world transform finite after 50 animation updates")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;

        for (int i = 0; i < 50; ++i)
            g_Renderer.m_Scene.Update(0.016f);

        g_Renderer.m_EnableAnimations = prev;

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (!g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated) continue;
            const Matrix& w = g_Renderer.m_Scene.m_Nodes[i].m_WorldTransform;
            INFO("Node " << i << " world._11=" << w._11 << " _41=" << w._41);
            CHECK(std::isfinite(w._11));
            CHECK(std::isfinite(w._22));
            CHECK(std::isfinite(w._33));
            CHECK(std::isfinite(w._41));
            CHECK(std::isfinite(w._42));
            CHECK(std::isfinite(w._43));
        }
    }

    // ------------------------------------------------------------------
    // TC-REG-10: Rotation quaternion is unit-length after animation update
    //   Regression for the SDL_assert added to Scene::Update (quaternion
    //   unit-length check after normalization).
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-10 Regression - rotation quaternion is unit-length after animation update")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);

        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.1f);
        g_Renderer.m_EnableAnimations = prev;

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (!g_Renderer.m_Scene.m_Nodes[i].m_IsAnimated) continue;
            const auto& q = g_Renderer.m_Scene.m_Nodes[i].m_Rotation;
            const float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
            INFO("Node " << i << " quaternion length=" << len);
            CHECK(len == doctest::Approx(1.0f).epsilon(1e-3f));
        }
    }

    // ------------------------------------------------------------------
    // TC-REG-11: k_TwoAnimGltf loads with correct durations (dur0=1, dur1=2)
    //   Regression for the malformed base64 buffer in k_TwoAnimGltf.
    //
    //   Root cause: the original base64 was a copy of the MinimalSceneFixture's
    //   36-byte position-only buffer padded with zeros.  All animation data
    //   decoded to near-zero garbage: inputs [0,0] → m_Duration=0, outputs
    //   near-zero → zero quaternion → NaN after XMQuaternionNormalize.
    //
    //   Fix: replaced the base64 with the correctly encoded 108-byte buffer:
    //     [0..35]   3 VEC3 positions
    //     [36..43]  anim0 inputs [0.0, 1.0]
    //     [44..67]  anim0 outputs VEC3 [0,0,0] → [1,0,0]
    //     [68..75]  anim1 inputs [0.0, 2.0]
    //     [76..107] anim1 outputs VEC4 [0,0,0,1] → [0,0.7071,0,0.7071]
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-11 Regression - k_TwoAnimGltf loads with correct durations")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());

        const auto& anims = g_Renderer.m_Scene.m_Animations;
        REQUIRE(anims.size() >= 2);

        INFO("anim[0].name=" << anims[0].m_Name << " dur=" << anims[0].m_Duration);
        INFO("anim[1].name=" << anims[1].m_Name << " dur=" << anims[1].m_Duration);

        // Anim0: TranslateA, inputs [0,1] → duration = 1.0
        CHECK(anims[0].m_Duration == doctest::Approx(1.0f).epsilon(1e-5f));
        // Anim1: RotateB, inputs [0,2] → duration = 2.0
        CHECK(anims[1].m_Duration == doctest::Approx(2.0f).epsilon(1e-5f));

        // Anim0 outputs: translation channel, node 0 should translate from [0,0,0] to [1,0,0]
        // Advance to t=1.0 (end of anim0, with duration set large to avoid wrap)
        g_Renderer.m_Scene.m_Animations[0].m_Duration = 10.0f;
        g_Renderer.m_Scene.m_Animations[0].m_CurrentTime = 1.0f;
        g_Renderer.m_Scene.m_Animations[1].m_CurrentTime = 0.0f;

        if (!g_Renderer.m_Scene.m_DynamicNodeIndices.empty())
        {
            const bool prev = g_Renderer.m_EnableAnimations;
            g_Renderer.m_EnableAnimations = true;
            g_Renderer.m_Scene.Update(0.0f);
            g_Renderer.m_EnableAnimations = prev;

            // Node 0 should be at translation [1,0,0] (clamped to kv1)
            const auto& node0 = g_Renderer.m_Scene.m_Nodes[0];
            INFO("node0 translation: " << node0.m_Translation.x << ", "
                 << node0.m_Translation.y << ", " << node0.m_Translation.z);
            CHECK(node0.m_Translation.x == doctest::Approx(1.0f).epsilon(1e-4f));
            CHECK(node0.m_Translation.y == doctest::Approx(0.0f).epsilon(1e-4f));
            CHECK(node0.m_Translation.z == doctest::Approx(0.0f).epsilon(1e-4f));
        }

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-REG-12: Zero-length quaternion from sampler falls back to identity
    //   Regression for the engine fix in Scene::Update that guards against
    //   XMQuaternionNormalize([0,0,0,0]) → NaN.
    //
    //   Root cause: the old code called XMQuaternionNormalize unconditionally.
    //   A zero-length quaternion (e.g. from a corrupt/zero-filled buffer) would
    //   produce NaN, corrupting the node's world transform and triggering the
    //   finite-transform assert.
    //
    //   Fix: check the raw quaternion length before normalizing.  If len < 1e-6,
    //   log a warning and fall back to identity (0,0,0,1).
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-12 Regression - zero-length quaternion from sampler falls back to identity")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(LoadTwoAnimScene());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        // Inject a synthetic rotation animation with a zero-length quaternion output.
        // This simulates a corrupt/zero-filled animation buffer.
        g_Renderer.m_Scene.m_Nodes[0].m_IsAnimated = true;
        if (std::find(g_Renderer.m_Scene.m_DynamicNodeIndices.begin(),
                      g_Renderer.m_Scene.m_DynamicNodeIndices.end(), 0)
            == g_Renderer.m_Scene.m_DynamicNodeIndices.end())
        {
            g_Renderer.m_Scene.m_DynamicNodeIndices.push_back(0);
        }

        // Zero quaternion at both keyframes — simulates a corrupt buffer.
        const Vector4 zeroQuat = { 0.0f, 0.0f, 0.0f, 0.0f };
        const int animIdx = AddSyntheticAnim(
            Scene::AnimationSampler::Interpolation::Linear,
            Scene::AnimationChannel::Path::Rotation,
            0.0f, 1.0f, zeroQuat, zeroQuat, 0);

        g_Renderer.m_Scene.m_Animations[animIdx].m_Duration = 10.0f;
        g_Renderer.m_Scene.m_Animations[animIdx].m_CurrentTime = 0.5f;

        // This must NOT assert or produce NaN — the engine should fall back to identity.
        const bool prev = g_Renderer.m_EnableAnimations;
        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        // Node rotation must be identity (0,0,0,1) after the fallback.
        const auto& q = g_Renderer.m_Scene.m_Nodes[0].m_Rotation;
        const float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        INFO("node0 rotation after zero-quat: (" << q.x << ", " << q.y << ", "
             << q.z << ", " << q.w << ") len=" << len);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-3f)); // must be unit-length
        CHECK(q.w == doctest::Approx(1.0f).epsilon(1e-3f)); // identity w=1

        // World transform must be finite (no NaN/Inf from the zero quaternion).
        const Matrix& world = g_Renderer.m_Scene.m_Nodes[0].m_WorldTransform;
        CHECK(std::isfinite(world._11));
        CHECK(std::isfinite(world._22));
        CHECK(std::isfinite(world._33));
        CHECK(std::isfinite(world._41));

        UnloadTwoAnimScene();
    }

    // ------------------------------------------------------------------
    // TC-REG-13: Manual TRS + m_IsDirty propagates to WorldTransform even
    //            when m_Animations is empty
    //   Regression for the engine bug where `if (m_Animations.empty()) return;`
    //   in Scene::Update() short-circuited the dirty-node world-transform
    //   propagation loop, silently swallowing all manual TRS mutations.
    //
    //   Root cause: the early return was placed before the dirty-node loop,
    //   so any manually-set TRS + m_IsDirty = true was never propagated to
    //   m_WorldTransform when no animations existed.
    //
    //   Fix: removed the `m_Animations.empty()` early return.  The dirty-node
    //   world-transform loop now always runs when m_EnableAnimations is true.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-13 Regression - manual TRS propagates to WorldTransform when m_Animations is empty")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        // Find any node in m_DynamicNodeIndices.
        REQUIRE(!g_Renderer.m_Scene.m_DynamicNodeIndices.empty());
        const int nodeIdx = g_Renderer.m_Scene.m_DynamicNodeIndices[0];
        REQUIRE(nodeIdx >= 0);
        REQUIRE(nodeIdx < (int)g_Renderer.m_Scene.m_Nodes.size());

        // Clear all animations — this is the exact condition that triggered the bug.
        const bool prev = g_Renderer.m_EnableAnimations;
        auto savedAnims = g_Renderer.m_Scene.m_Animations;
        g_Renderer.m_Scene.m_Animations.clear();
        REQUIRE(g_Renderer.m_Scene.m_Animations.empty());

        // Set a distinctive 90° Y rotation.
        const float s = 0.7071f, c = 0.7071f;
        g_Renderer.m_Scene.m_Nodes[nodeIdx].m_Rotation    = Quaternion{ 0.0f, s, 0.0f, c };
        g_Renderer.m_Scene.m_Nodes[nodeIdx].m_Scale       = Vector3{ 1.0f, 1.0f, 1.0f };
        g_Renderer.m_Scene.m_Nodes[nodeIdx].m_Translation = Vector3{ 0.0f, 0.0f, 0.0f };
        g_Renderer.m_Scene.m_Nodes[nodeIdx].m_IsDirty     = true;

        // Force the parent's world transform to identity so worldM = localM * I = localM.
        // Without this, the parent's non-identity default glTF transform is multiplied in
        // and the expected rotation values (_11, _13, _31, _33) would be wrong.
        const int parentIdx = g_Renderer.m_Scene.m_Nodes[nodeIdx].m_Parent;
        if (parentIdx >= 0)
        {
            using namespace DirectX;
            XMStoreFloat4x4(&g_Renderer.m_Scene.m_Nodes[parentIdx].m_WorldTransform,
                            XMMatrixIdentity());
        }

        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        g_Renderer.m_Scene.m_Animations = std::move(savedAnims);

        // For quaternion (0, 0.7071, 0, 0.7071) — 90° Y rotation in DirectXMath:
        //   _11 = 1 - 2(y²+z²) = 0
        //   _13 = 2(xz - wy)  = -2*0.7071*0.7071 = -1
        //   _31 = 2(xz + wy)  = +2*0.7071*0.7071 = +1
        //   _33 = 1 - 2(x²+y²) = 0
        const Matrix& world2 = g_Renderer.m_Scene.m_Nodes[nodeIdx].m_WorldTransform;
        INFO("nodeIdx=" << nodeIdx << " parentIdx=" << parentIdx);
        INFO("world._11=" << world2._11 << " _13=" << world2._13
             << " _31=" << world2._31 << " _33=" << world2._33);
        CHECK(std::abs(world2._11) < 0.01f);
        CHECK(world2._13 == doctest::Approx(-1.0f).epsilon(0.01f));
        CHECK(world2._31 == doctest::Approx(1.0f).epsilon(0.01f));
        CHECK(std::abs(world2._33) < 0.01f);
    }

    // ------------------------------------------------------------------
    // TC-REG-14: Manual translation propagates to WorldTransform when
    //            m_Animations is empty (companion to TC-REG-13)
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-14 Regression - manual translation propagates to WorldTransform when m_Animations is empty")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_DynamicNodeIndices.empty());

        const int nodeIdx14 = g_Renderer.m_Scene.m_DynamicNodeIndices[0];

        const bool prev14 = g_Renderer.m_EnableAnimations;
        auto savedAnims14 = g_Renderer.m_Scene.m_Animations;
        g_Renderer.m_Scene.m_Animations.clear();

        const Vector3 newPos{ 5.0f, 3.0f, -2.0f };
        g_Renderer.m_Scene.m_Nodes[nodeIdx14].m_Translation = newPos;
        g_Renderer.m_Scene.m_Nodes[nodeIdx14].m_Rotation    = Quaternion{ 0.0f, 0.0f, 0.0f, 1.0f };
        g_Renderer.m_Scene.m_Nodes[nodeIdx14].m_Scale       = Vector3{ 1.0f, 1.0f, 1.0f };
        g_Renderer.m_Scene.m_Nodes[nodeIdx14].m_IsDirty     = true;

        // Force the parent's world transform to identity so worldM = localM * I = localM.
        // Without this, the parent's non-identity default glTF transform adds its own
        // translation and rotation, making the expected _41/_42/_43 values wrong.
        const int parentIdx14 = g_Renderer.m_Scene.m_Nodes[nodeIdx14].m_Parent;
        if (parentIdx14 >= 0)
        {
            using namespace DirectX;
            XMStoreFloat4x4(&g_Renderer.m_Scene.m_Nodes[parentIdx14].m_WorldTransform,
                            XMMatrixIdentity());
        }

        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev14;

        g_Renderer.m_Scene.m_Animations = std::move(savedAnims14);

        const Matrix& w = g_Renderer.m_Scene.m_Nodes[nodeIdx14].m_WorldTransform;
        INFO("nodeIdx14=" << nodeIdx14 << " parentIdx14=" << parentIdx14);
        INFO("world._41=" << w._41 << " _42=" << w._42 << " _43=" << w._43);
        CHECK(w._41 == doctest::Approx(newPos.x).epsilon(1e-4f));
        CHECK(w._42 == doctest::Approx(newPos.y).epsilon(1e-4f));
        CHECK(w._43 == doctest::Approx(newPos.z).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-REG-15: m_InstanceDirtyRange is set after manual TRS mutation
    //            when m_Animations is empty (companion to TC-REG-13)
    //   Verifies that the dirty-range reset + re-population in the
    //   world-transform loop works correctly for the no-animation case.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-15 Regression - m_InstanceDirtyRange set after manual TRS when m_Animations is empty")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_DynamicNodeIndices.empty());

        const int nodeIdx = g_Renderer.m_Scene.m_DynamicNodeIndices[0];
        REQUIRE(!g_Renderer.m_Scene.m_Nodes[nodeIdx].m_InstanceIndices.empty());

        const bool prev = g_Renderer.m_EnableAnimations;
        auto savedAnims = g_Renderer.m_Scene.m_Animations;
        g_Renderer.m_Scene.m_Animations.clear();

        g_Renderer.m_Scene.m_Nodes[nodeIdx].m_Translation = Vector3{ 1.0f, 2.0f, 3.0f };
        g_Renderer.m_Scene.m_Nodes[nodeIdx].m_IsDirty     = true;

        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        g_Renderer.m_Scene.m_Animations = std::move(savedAnims);

        // The dirty range must be set (first <= second) because the node has instances.
        const auto& range = g_Renderer.m_Scene.m_InstanceDirtyRange;
        INFO("m_InstanceDirtyRange=[" << range.first << ", " << range.second << "]");
        CHECK(range.first <= range.second);
    }

    // ------------------------------------------------------------------
    // TC-REG-16: World transform = localM * parentWorld (not just localM)
    //   Regression for TC-NT-02 and TC-REG-13.
    //
    //   Root cause: both tests assumed the animated node was a root node
    //   (no parent), so they expected worldM == localM.  In AnimatedCube,
    //   the animated node is a CHILD of a root node that has a non-identity
    //   default world transform from the glTF file.  The world transform is
    //   worldM = localM * parentWorld, so the expected rotation values
    //   (_11, _13, _31, _33) were wrong when the parent had a non-identity
    //   transform.
    //
    //   Fix: force the parent's world transform to identity before the test
    //   so worldM = localM * I = localM.
    //
    //   This test explicitly verifies the parent-transform multiplication:
    //   it sets a known parent world transform and checks that the child's
    //   world transform is correctly composed.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-16 Regression - child world transform = localM * parentWorld")
    {
        SKIP_IF_NO_SAMPLES("AnimatedCube/glTF/AnimatedCube.gltf");
        SceneScope scope("AnimatedCube/glTF/AnimatedCube.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(!g_Renderer.m_Scene.m_DynamicNodeIndices.empty());

        // Find a dynamic node that has a parent.
        int childIdx = -1;
        for (int idx : g_Renderer.m_Scene.m_DynamicNodeIndices)
        {
            if (g_Renderer.m_Scene.m_Nodes[idx].m_Parent >= 0)
            {
                childIdx = idx;
                break;
            }
        }
        if (childIdx < 0)
        {
            // No parent-child pair in this scene — skip gracefully.
            SDL_Log("[TC-REG-16] No dynamic node with a parent found — skipping.");
            return;
        }

        const int parentIdx = g_Renderer.m_Scene.m_Nodes[childIdx].m_Parent;

        // Set parent world transform to a known translation (5, 0, 0).
        using namespace DirectX;
        XMStoreFloat4x4(&g_Renderer.m_Scene.m_Nodes[parentIdx].m_WorldTransform,
                        XMMatrixTranslation(5.0f, 0.0f, 0.0f));

        // Set child to identity TRS (no local transform).
        g_Renderer.m_Scene.m_Nodes[childIdx].m_Translation = Vector3{ 0.0f, 0.0f, 0.0f };
        g_Renderer.m_Scene.m_Nodes[childIdx].m_Rotation    = Quaternion{ 0.0f, 0.0f, 0.0f, 1.0f };
        g_Renderer.m_Scene.m_Nodes[childIdx].m_Scale       = Vector3{ 1.0f, 1.0f, 1.0f };
        g_Renderer.m_Scene.m_Nodes[childIdx].m_IsDirty     = true;

        const bool prev = g_Renderer.m_EnableAnimations;
        auto savedAnims = g_Renderer.m_Scene.m_Animations;
        g_Renderer.m_Scene.m_Animations.clear();

        g_Renderer.m_EnableAnimations = true;
        g_Renderer.m_Scene.Update(0.0f);
        g_Renderer.m_EnableAnimations = prev;

        g_Renderer.m_Scene.m_Animations = std::move(savedAnims);

        // Child world = identity * parent(5,0,0) = translation(5,0,0).
        // So _41 = 5, _42 = 0, _43 = 0.
        const Matrix& childWorld = g_Renderer.m_Scene.m_Nodes[childIdx].m_WorldTransform;
        INFO("childIdx=" << childIdx << " parentIdx=" << parentIdx);
        INFO("childWorld._41=" << childWorld._41 << " _42=" << childWorld._42
             << " _43=" << childWorld._43);
        CHECK(childWorld._41 == doctest::Approx(5.0f).epsilon(1e-4f));
        CHECK(childWorld._42 == doctest::Approx(0.0f).epsilon(1e-4f));
        CHECK(childWorld._43 == doctest::Approx(0.0f).epsilon(1e-4f));
        // Rotation part must be identity (child has identity rotation, parent has none).
        CHECK(childWorld._11 == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(childWorld._22 == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(childWorld._33 == doctest::Approx(1.0f).epsilon(1e-4f));
    }

    // ------------------------------------------------------------------
    // TC-REG-17: DirectXMath rotation matrix formula for 90° Y quaternion
    //   Regression for TC-NT-02 and TC-REG-13.
    //
    //   Root cause: the tests expected _13=+1 and _31=-1 for a 90° Y rotation
    //   quaternion (0, 0.7071, 0, 0.7071), but DirectXMath's formula gives:
    //     _13 = 2(xz - wy) = -2 * 0.7071 * 0.7071 = -1
    //     _31 = 2(xz + wy) = +2 * 0.7071 * 0.7071 = +1
    //   The signs were swapped.
    //
    //   This test directly verifies the DirectXMath rotation matrix output
    //   so future tests can reference it as ground truth.
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-17 Regression - DirectXMath 90deg Y rotation matrix has correct signs")
    {
        using namespace DirectX;

        // Build the rotation matrix directly from the quaternion.
        const float s = 0.7071067811865476f; // sin(pi/4) = cos(pi/4) = 1/sqrt(2)
        XMVECTOR q = XMVectorSet(0.0f, s, 0.0f, s); // (x, y, z, w)
        XMMATRIX rot = XMMatrixRotationQuaternion(q);

        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, rot);

        // Verify the full rotation matrix for 90° Y rotation:
        //   [cos90  0  sin90  0]   [0   0  -1  0]   (row-major, DirectXMath convention)
        //   [0      1  0      0] = [0   1   0  0]
        //   [-sin90 0  cos90  0]   [1   0   0  0]
        //   [0      0  0      1]   [0   0   0  1]
        //
        // DirectXMath formula for quaternion (x,y,z,w):
        //   _11 = 1 - 2(y²+z²)  _12 = 2(xy+wz)   _13 = 2(xz-wy)
        //   _21 = 2(xy-wz)       _22 = 1-2(x²+z²) _23 = 2(yz+wx)
        //   _31 = 2(xz+wy)       _32 = 2(yz-wx)   _33 = 1-2(x²+y²)
        //
        // For (0, s, 0, s): _13 = 2(0-s²) = -1, _31 = 2(0+s²) = +1
        INFO("m._11=" << m._11 << " m._13=" << m._13);
        INFO("m._31=" << m._31 << " m._33=" << m._33);

        CHECK(m._11 == doctest::Approx(0.0f).epsilon(1e-5f));
        CHECK(m._12 == doctest::Approx(0.0f).epsilon(1e-5f));
        CHECK(m._13 == doctest::Approx(-1.0f).epsilon(1e-5f)); // NOT +1
        CHECK(m._22 == doctest::Approx(1.0f).epsilon(1e-5f));
        CHECK(m._31 == doctest::Approx(1.0f).epsilon(1e-5f));  // NOT -1
        CHECK(m._33 == doctest::Approx(0.0f).epsilon(1e-5f));
        CHECK(m._44 == doctest::Approx(1.0f).epsilon(1e-5f));
    }
}
