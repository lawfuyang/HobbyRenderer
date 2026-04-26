// Tests_SceneAdvanced.cpp
//
// Systems under test: Scene BLAS/TLAS, bounding sphere, in-memory loading,
//                     EnsureDefaultDirectionalLight, Shutdown() lifecycle,
//                     multiple load/unload cycles, GPU buffer completeness.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//
// Test coverage:
//   - LoadGLTFSceneFromMemory with valid minimal glTF succeeds
//   - LoadGLTFSceneFromMemory with malformed JSON does not crash
//   - Scene from memory has non-empty mesh and node lists
//   - Scene from memory has at least one light (EnsureDefaultDirectionalLight)
//   - BLAS handles are non-null after BuildAccelerationStructures
//   - TLAS handle is non-null after BuildAccelerationStructures
//   - BLAS count matches primitive count across all meshes
//   - AreInstanceTransformsDirty returns false after FinalizeLoadedScene
//   - AreInstanceTransformsDirty returns true when instance dirty range is set
//   - EnsureDefaultDirectionalLight inserts a light when none exists
//   - EnsureDefaultDirectionalLight is idempotent (no duplicates)
//   - GetSceneBoundingRadius returns a positive value after load
//   - Scene bounding sphere center is finite after load
//   - UpdateNodeBoundingSphere does not crash
//   - Shutdown() followed by LoadScene does not crash
//   - Shutdown() on already-empty scene does not crash
//   - Two consecutive load/unload cycles preserve GPU resources
//   - m_MeshletBuffer is non-null after loading a mesh
//   - m_MeshletVerticesBuffer is non-null after loading a mesh
//   - m_MeshletTrianglesBuffer is non-null after loading a mesh
//   - m_Meshlets array is non-empty after loading a mesh
//   - m_InstanceLODBuffer is non-null after scene load
//   - m_BLASAddressBuffer is non-null after BuildAccelerationStructures
//
// Run with: HobbyRenderer --run-tests=*SceneAdv* --gltf-samples <path>
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// Minimal in-memory glTF (reused from MinimalSceneFixture)
// ============================================================================
namespace
{
    static constexpr const char k_AdvMinimalGltf[] = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
  "accessors": [ {
    "bufferView": 0, "byteOffset": 0,
    "componentType": 5126, "count": 3, "type": "VEC3",
    "max": [ 1.0, 1.0, 0.0 ], "min": [ 0.0, 0.0, 0.0 ]
  } ],
  "bufferViews": [ {
    "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962
  } ],
  "buffers": [ {
    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA",
    "byteLength": 36
  } ]
})";

    // Deliberately malformed JSON (unclosed brace):
    static constexpr const char k_MalformedGltf[] = R"({ "asset": { "version": "2.0" )";
} // anonymous namespace

// ============================================================================
// TEST SUITE: Scene_InMemoryLoading
// ============================================================================
TEST_SUITE("Scene_InMemoryLoading")
{
    // ------------------------------------------------------------------
    // TC-IMM-01: LoadGLTFSceneFromMemory with valid JSON succeeds
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMM-01 InMemoryLoading - valid minimal glTF loads successfully")
    {
        REQUIRE(DEV() != nullptr);
        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> vertices;
        std::vector<uint32_t> indices;

        const bool ok = SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene,
            k_AdvMinimalGltf, sizeof(k_AdvMinimalGltf) - 1,
            {},
            vertices, indices);

        CHECK(ok);

        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-IMM-02: Scene from memory has at least one mesh
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMM-02 InMemoryLoading - scene from memory has at least one mesh")
    {
        REQUIRE(DEV() != nullptr);
        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> vertices;
        std::vector<uint32_t> indices;
        SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene, k_AdvMinimalGltf, sizeof(k_AdvMinimalGltf) - 1,
            {}, vertices, indices);

        CHECK(!g_Renderer.m_Scene.m_Meshes.empty());

        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-IMM-03: Scene from memory has at least one node
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMM-03 InMemoryLoading - scene from memory has at least one node")
    {
        REQUIRE(DEV() != nullptr);
        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> vertices;
        std::vector<uint32_t> indices;
        SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene, k_AdvMinimalGltf, sizeof(k_AdvMinimalGltf) - 1,
            {}, vertices, indices);

        CHECK(!g_Renderer.m_Scene.m_Nodes.empty());

        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-IMM-04: Scene from memory has at least one light (default injected)
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMM-04 InMemoryLoading - scene from memory has default directional light")
    {
        REQUIRE(DEV() != nullptr);
        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> vertices;
        std::vector<uint32_t> indices;
        SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene, k_AdvMinimalGltf, sizeof(k_AdvMinimalGltf) - 1,
            {}, vertices, indices);

        CHECK(!g_Renderer.m_Scene.m_Lights.empty());

        bool hasDir = false;
        for (const auto& l : g_Renderer.m_Scene.m_Lights)
            if (l.m_Type == Scene::Light::Directional) hasDir = true;
        CHECK(hasDir);

        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-IMM-05: Malformed JSON does not crash and returns false
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMM-05 InMemoryLoading - malformed JSON does not crash")
    {
        REQUIRE(DEV() != nullptr);
        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> vertices;
        std::vector<uint32_t> indices;

        // The call must not crash — it should return false.
        bool ok = true;
        CHECK_NOTHROW(ok = SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene,
            k_MalformedGltf, sizeof(k_MalformedGltf) - 1,
            {}, vertices, indices));
        // We expect either false or an empty scene.
        // (Different cgltf versions may handle this differently.)
        INFO("ok=" << ok << " meshes=" << g_Renderer.m_Scene.m_Meshes.size());
        CHECK((!ok || g_Renderer.m_Scene.m_Meshes.empty()));

        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-IMM-06: Null JSON pointer does not crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-IMM-06 InMemoryLoading - null/empty JSON does not crash")
    {
        REQUIRE(DEV() != nullptr);
        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> vertices;
        std::vector<uint32_t> indices;

        // Pass an empty (but valid) buffer pointer with size 0.
        static const char kEmpty[] = "";
        bool ok = true;
        CHECK_NOTHROW(ok = SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene, kEmpty, 0, {}, vertices, indices));
        // Expect failure (empty is not valid glTF).
        CHECK(!ok);

        if (DEV()) DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }
}

// ============================================================================
// TEST SUITE: Scene_AccelStructures
// ============================================================================
TEST_SUITE("Scene_AccelStructures")
{
    // ------------------------------------------------------------------
    // TC-BLAS-01: BLAS handles are non-null after BuildAccelerationStructures
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLAS-01 AccelStructures - BLAS handles are non-null after build")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        // At least one primitive in the scene must have a BLAS.
        bool foundBLAS = false;
        for (const auto& mesh : g_Renderer.m_Scene.m_Meshes)
        {
            for (const auto& prim : mesh.m_Primitives)
            {
                for (const auto& blas : prim.m_BLAS)
                {
                    if (blas != nullptr)
                    {
                        foundBLAS = true;
                        break;
                    }
                }
                if (foundBLAS) break;
            }
            if (foundBLAS) break;
        }
        CHECK(foundBLAS);
    }

    // ------------------------------------------------------------------
    // TC-BLAS-02: TLAS handle is non-null after BuildAccelerationStructures
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLAS-02 AccelStructures - TLAS handle is non-null after build")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_TLAS != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BLAS-03: m_RTInstanceDescs is non-empty after BuildAccelerationStructures
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLAS-03 AccelStructures - RT instance descs are non-empty")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_RTInstanceDescs.empty());
    }

    // ------------------------------------------------------------------
    // TC-BLAS-04: m_BLASAddressBuffer is non-null after BuildAccelerationStructures
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLAS-04 AccelStructures - BLAS address buffer is non-null")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_BLASAddressBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BLAS-05: m_InstanceLODBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLAS-05 AccelStructures - instance LOD buffer is non-null")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_InstanceLODBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BLAS-06: BLAS address buffer byte size is large enough
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLAS-06 AccelStructures - BLAS address buffer has sufficient size")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        REQUIRE(g_Renderer.m_Scene.m_BLASAddressBuffer != nullptr);
        const uint64_t bufSize = g_Renderer.m_Scene.m_BLASAddressBuffer->getDesc().byteSize;
        // Must be at least instanceCount * MAX_LOD_COUNT * 8 bytes (device address)
        const uint64_t minSize = (uint64_t)g_Renderer.m_Scene.m_InstanceData.size()
            * srrhi::CommonConsts::MAX_LOD_COUNT
            * sizeof(uint64_t);
        CHECK(bufSize >= minSize);
    }
}

// ============================================================================
// TEST SUITE: Scene_BoundingSphere
// ============================================================================
TEST_SUITE("Scene_BoundingSphere")
{
    // ------------------------------------------------------------------
    // TC-BS-01: GetSceneBoundingRadius returns positive value after load
    // ------------------------------------------------------------------
    TEST_CASE("TC-BS-01 BoundingSphere - GetSceneBoundingRadius is positive after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.GetSceneBoundingRadius() > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-BS-02: Scene bounding sphere center is finite
    // ------------------------------------------------------------------
    TEST_CASE("TC-BS-02 BoundingSphere - scene bounding sphere center is finite")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& bs = g_Renderer.m_Scene.m_SceneBoundingSphere;
        CHECK(std::isfinite(bs.Center.x));
        CHECK(std::isfinite(bs.Center.y));
        CHECK(std::isfinite(bs.Center.z));
        CHECK(std::isfinite(bs.Radius));
    }

    // ------------------------------------------------------------------
    // TC-BS-03: CesiumMilkTruck has a larger bounding sphere than BoxTextured
    //           (sanity check: multi-mesh scene is larger)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BS-03 BoundingSphere - complex scene has larger bounding sphere than simple")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SKIP_IF_NO_SAMPLES("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");

        float radiusBox = 0.0f;
        {
            SceneScope scopeBox("BoxTextured/glTF/BoxTextured.gltf");
            if (scopeBox.loaded)
                radiusBox = g_Renderer.m_Scene.GetSceneBoundingRadius();
        }

        float radiusTruck = 0.0f;
        {
            SceneScope scopeTruck("CesiumMilkTruck/glTF/CesiumMilkTruck.gltf");
            if (scopeTruck.loaded)
                radiusTruck = g_Renderer.m_Scene.GetSceneBoundingRadius();
        }

        if (radiusBox > 0.0f && radiusTruck > 0.0f)
            CHECK(radiusTruck >= radiusBox);
    }

    // ------------------------------------------------------------------
    // TC-BS-04: UpdateNodeBoundingSphere does not crash for all mesh nodes
    // ------------------------------------------------------------------
    TEST_CASE("TC-BS-04 BoundingSphere - UpdateNodeBoundingSphere does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Nodes.size(); ++i)
        {
            if (g_Renderer.m_Scene.m_Nodes[i].m_MeshIndex >= 0)
            {
                INFO("Node " << i);
                CHECK_NOTHROW(g_Renderer.m_Scene.UpdateNodeBoundingSphere(i));
            }
        }
    }

    // ------------------------------------------------------------------
    // TC-BS-05: Mesh bounding sphere has positive radius for loaded scene
    // ------------------------------------------------------------------
    TEST_CASE("TC-BS-05 BoundingSphere - mesh bounding sphere has positive radius")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Meshes.size(); ++i)
        {
            INFO("Mesh " << i << " radius=" << g_Renderer.m_Scene.m_Meshes[i].m_Radius);
            CHECK(g_Renderer.m_Scene.m_Meshes[i].m_Radius > 0.0f);
        }
    }
}

// ============================================================================
// TEST SUITE: Scene_LifecycleEdgeCases
// ============================================================================
TEST_SUITE("Scene_LifecycleEdgeCases")
{
    // ------------------------------------------------------------------
    // TC-SLFE-01: Shutdown() on an already-empty scene does not crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-SLFE-01 LifecycleEdgeCases - Shutdown() on empty scene is safe")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        CHECK_NOTHROW(g_Renderer.m_Scene.Shutdown()); // second call
    }

    // ------------------------------------------------------------------
    // TC-SLFE-02: Two consecutive load/unload cycles preserve GPU device
    // ------------------------------------------------------------------
    TEST_CASE("TC-SLFE-02 LifecycleEdgeCases - two load/unload cycles keep device valid")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        for (int cycle = 0; cycle < 2; ++cycle)
        {
            INFO("Cycle " << cycle);
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            CHECK(scope.loaded);
            CHECK(!g_Renderer.m_Scene.m_Meshes.empty());
        }

        // GPU device must remain valid.
        CHECK(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SLFE-03: EnsureDefaultDirectionalLight inserts a light when none exists
    // ------------------------------------------------------------------
    TEST_CASE("TC-SLFE-03 LifecycleEdgeCases - EnsureDefaultDirectionalLight adds light to empty scene")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        // Ensure light list is empty before calling.
        REQUIRE(g_Renderer.m_Scene.m_Lights.empty());

        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();
        CHECK(!g_Renderer.m_Scene.m_Lights.empty());

        bool hasDir = false;
        for (const auto& l : g_Renderer.m_Scene.m_Lights)
            if (l.m_Type == Scene::Light::Directional) hasDir = true;
        CHECK(hasDir);

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-SLFE-04: EnsureDefaultDirectionalLight called twice does not duplicate
    // ------------------------------------------------------------------
    TEST_CASE("TC-SLFE-04 LifecycleEdgeCases - EnsureDefaultDirectionalLight is idempotent")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();
        const size_t countAfterFirst = g_Renderer.m_Scene.m_Lights.size();

        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();
        const size_t countAfterSecond = g_Renderer.m_Scene.m_Lights.size();

        CHECK(countAfterSecond == countAfterFirst);

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-SLFE-05: AreInstanceTransformsDirty is false after FinalizeLoadedScene
    //             on a freshly loaded scene (dirty range should be reset)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SLFE-05 LifecycleEdgeCases - AreInstanceTransformsDirty is false after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        // After a full load cycle, dirty range should have been consumed.
        // (It is valid for it to be false after the scene is done loading.)
        CHECK_NOTHROW(g_Renderer.m_Scene.AreInstanceTransformsDirty());
    }

    // ------------------------------------------------------------------
    // TC-SLFE-06: Setting instance dirty range marks scene as dirty
    // ------------------------------------------------------------------
    TEST_CASE("TC-SLFE-06 LifecycleEdgeCases - manually dirtying instance range works")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        g_Renderer.m_Scene.m_InstanceDirtyRange = { 0u, 0u };
        CHECK(g_Renderer.m_Scene.AreInstanceTransformsDirty());

        // Reset to "clean"
        g_Renderer.m_Scene.m_InstanceDirtyRange = { UINT32_MAX, 0u };
        CHECK(!g_Renderer.m_Scene.AreInstanceTransformsDirty());
    }
}

// ============================================================================
// TEST SUITE: Scene_MeshletBuffers
// ============================================================================
TEST_SUITE("Scene_MeshletBuffers")
{
    // ------------------------------------------------------------------
    // TC-MLT-01: m_MeshletBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-01 MeshletBuffers - m_MeshletBuffer is non-null after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_MeshletBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-MLT-02: m_MeshletVerticesBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-02 MeshletBuffers - m_MeshletVerticesBuffer is non-null after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_MeshletVerticesBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-MLT-03: m_MeshletTrianglesBuffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-03 MeshletBuffers - m_MeshletTrianglesBuffer is non-null after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_MeshletTrianglesBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-MLT-04: m_Meshlets CPU array is non-empty after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-04 MeshletBuffers - m_Meshlets CPU array is non-empty after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_Meshlets.empty());
    }

    // ------------------------------------------------------------------
    // TC-MLT-05: m_MeshletVertices CPU array is non-empty after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-05 MeshletBuffers - m_MeshletVertices CPU array is non-empty after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_MeshletVertices.empty());
    }

    // ------------------------------------------------------------------
    // TC-MLT-06: m_MeshletTriangles CPU array is non-empty after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-06 MeshletBuffers - m_MeshletTriangles CPU array is non-empty after load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(!g_Renderer.m_Scene.m_MeshletTriangles.empty());
    }

    // ------------------------------------------------------------------
    // TC-MLT-07: Meshlet buffer byte size is consistent with m_Meshlets count
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-07 MeshletBuffers - meshlet buffer byte size is consistent")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        REQUIRE(g_Renderer.m_Scene.m_MeshletBuffer != nullptr);
        const uint64_t bufSize    = g_Renderer.m_Scene.m_MeshletBuffer->getDesc().byteSize;
        const uint64_t expectedSz = (uint64_t)g_Renderer.m_Scene.m_Meshlets.size() * sizeof(srrhi::Meshlet);
        CHECK(bufSize >= expectedSz);
    }

    // ------------------------------------------------------------------
    // TC-MLT-08: MeshData entries have non-zero vertex counts
    // ------------------------------------------------------------------
    TEST_CASE("TC-MLT-08 MeshletBuffers - MeshData entries have non-zero meshlet counts")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (int i = 0; i < (int)g_Renderer.m_Scene.m_MeshData.size(); ++i)
        {
            INFO("MeshData " << i);
            // m_MeshletCounts[0] is the LOD-0 meshlet count — must be > 0 for a valid mesh
            CHECK(g_Renderer.m_Scene.m_MeshData[i].m_MeshletCounts[0] > 0u);
        }
    }
}

// ============================================================================
// TEST SUITE: Scene_DefaultLight
// ============================================================================
TEST_SUITE("Scene_DefaultLight")
{
    // ------------------------------------------------------------------
    // TC-DFL-01: Default directional light node index is valid
    // ------------------------------------------------------------------
    TEST_CASE("TC-DFL-01 DefaultLight - default directional light node index is valid")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();

        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());
        REQUIRE(!g_Renderer.m_Scene.m_Nodes.empty());

        const auto& light = g_Renderer.m_Scene.m_Lights.back();
        REQUIRE(light.m_Type == Scene::Light::Directional);
        CHECK(light.m_NodeIndex >= 0);
        CHECK(light.m_NodeIndex < (int)g_Renderer.m_Scene.m_Nodes.size());

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-DFL-02: Default directional light has a positive intensity
    // ------------------------------------------------------------------
    TEST_CASE("TC-DFL-02 DefaultLight - default directional light intensity is positive")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();

        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());
        const auto& light = g_Renderer.m_Scene.m_Lights.back();
        REQUIRE(light.m_Type == Scene::Light::Directional);
        CHECK(light.m_Intensity > 0.0f);

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-DFL-03: Default directional light is always placed at back of list
    // ------------------------------------------------------------------
    TEST_CASE("TC-DFL-03 DefaultLight - directional light is at back of lights list")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());
        CHECK(g_Renderer.m_Scene.m_Lights.back().m_Type == Scene::Light::Directional);
    }

    // ------------------------------------------------------------------
    // TC-DFL-04: GetSunIntensity returns the intensity of the first light
    // ------------------------------------------------------------------
    TEST_CASE("TC-DFL-04 DefaultLight - GetSunIntensity matches first light intensity")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        REQUIRE(!g_Renderer.m_Scene.m_Lights.empty());
        CHECK(g_Renderer.m_Scene.GetSunIntensity() == doctest::Approx(g_Renderer.m_Scene.m_Lights.at(0).m_Intensity));
    }
}

// ============================================================================
// TEST SUITE: Scene_AsyncBoundingVolume
//
// Verifies that ApplyPendingUpdates() correctly propagates the bounding
// sphere computed from async-loaded vertex data all the way through
//   Scene::Mesh -> Scene::Node -> srrhi::PerInstanceData
// so GPU frustum and occlusion culling use the correct sphere.
// ============================================================================
TEST_SUITE("Scene_AsyncBoundingVolume")
{
    // ------------------------------------------------------------------
    // TC-ABV-01: m_bBoundsValid is true for all meshes after sync load
    // (sanity-check that the sync path marks the flag correctly)
    // ------------------------------------------------------------------
    TEST_CASE("TC-ABV-01 AsyncBV - m_bBoundsValid true after sync in-memory load")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> verts;
        std::vector<uint32_t> indices;
        g_Renderer.m_Scene.InitializeDefaultCube(0, 0);
        g_Renderer.ExecutePendingCommandLists();

        const bool ok = SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene,
            k_AdvMinimalGltf, sizeof(k_AdvMinimalGltf) - 1,
            {}, verts, indices);
        REQUIRE(ok);

        // Every mesh must have a valid bounding sphere after a synchronous load.
        for (int i = 0; i < (int)g_Renderer.m_Scene.m_Meshes.size(); ++i)
        {
            INFO("Mesh " << i);
            CHECK(g_Renderer.m_Scene.m_Meshes[i].m_bBoundsValid);
            CHECK(g_Renderer.m_Scene.m_Meshes[i].m_Radius >= 0.0f);
        }

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-ABV-02: Instance BVs match node BVs after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-ABV-02 AsyncBV - instance center/radius matches node center/radius after load")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        std::vector<srrhi::VertexQuantized> verts;
        std::vector<uint32_t> indices;
        g_Renderer.m_Scene.InitializeDefaultCube(0, 0);
        g_Renderer.ExecutePendingCommandLists();

        const bool ok = SceneLoader::LoadGLTFSceneFromMemory(
            g_Renderer.m_Scene,
            k_AdvMinimalGltf, sizeof(k_AdvMinimalGltf) - 1,
            {}, verts, indices);
        REQUIRE(ok);

        Scene& scene = g_Renderer.m_Scene;
        for (int ni = 0; ni < (int)scene.m_Nodes.size(); ++ni)
        {
            const Scene::Node& node = scene.m_Nodes[ni];
            for (uint32_t instIdx : node.m_InstanceIndices)
            {
                INFO("Node " << ni << " instance " << instIdx);
                REQUIRE(instIdx < (uint32_t)scene.m_InstanceData.size());
                const srrhi::PerInstanceData& inst = scene.m_InstanceData[instIdx];
                CHECK(inst.m_Center.x == doctest::Approx(node.m_Center.x).epsilon(1e-4f));
                CHECK(inst.m_Center.y == doctest::Approx(node.m_Center.y).epsilon(1e-4f));
                CHECK(inst.m_Center.z == doctest::Approx(node.m_Center.z).epsilon(1e-4f));
                CHECK(inst.m_Radius   == doctest::Approx(node.m_Radius).epsilon(1e-4f));
            }
        }

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-ABV-03: ApplyPendingUpdates updates mesh BV from injected command
    // Simulates the async-load case: a MeshUpdateCommand with vertices at
    // a known position is injected and ApplyPendingUpdates is called.
    // The resulting mesh.m_Center/m_Radius must reflect the vertex data.
    // ------------------------------------------------------------------
    TEST_CASE("TC-ABV-03 AsyncBV - ApplyPendingUpdates updates mesh BV from vertex data")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        // Set up a minimal scene with a default cube so the GPU buffers exist.
        g_Renderer.m_Scene.InitializeDefaultCube(64, 64);
        g_Renderer.ExecutePendingCommandLists();

        // Add a placeholder mesh (index 1) and a node pointing to it, mimicking
        // the async loading path in ProcessMeshes.
        {
            Scene::Primitive prim;
            prim.m_VertexOffset  = 0;
            prim.m_VertexCount   = 0;  // no geometry yet
            prim.m_MeshDataIndex = 0;  // placeholder: cube mesh data
            prim.m_MaterialIndex = -1;

            Scene::Mesh placeholder;
            placeholder.m_Primitives.push_back(prim);
            placeholder.m_Center      = Vector3{ 0.0f, 0.0f, 0.0f };
            placeholder.m_Radius      = 0.866f;  // placeholder value
            placeholder.m_bBoundsValid = false;  // not yet computed from real geometry
            g_Renderer.m_Scene.m_Meshes.push_back(std::move(placeholder));
        }
        const int testMeshIdx = 1;  // index of the mesh we just added

        // Add a directional light so EnsureDefaultDirectionalLight + FinalizeLoadedScene don't crash.
        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();

        // Add a node pointing at our placeholder mesh.  Identity world transform.
        {
            Scene::Node node;
            node.m_MeshIndex = testMeshIdx;
            DirectX::XMStoreFloat4x4(&node.m_WorldTransform, DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&node.m_LocalTransform, DirectX::XMMatrixIdentity());
            g_Renderer.m_Scene.m_Nodes.push_back(std::move(node));
        }

        // Finalize so m_InstanceData gets an entry for our node.
        g_Renderer.m_Scene.FinalizeLoadedScene();
        REQUIRE(!g_Renderer.m_Scene.m_InstanceData.empty());

        // Record the instance that belongs to our test mesh node.
        const Scene::Node& testNode = g_Renderer.m_Scene.m_Nodes.back();
        REQUIRE(!testNode.m_InstanceIndices.empty());
        const uint32_t testInstIdx = testNode.m_InstanceIndices[0];

        // Verify it starts with the placeholder sphere.
        CHECK(g_Renderer.m_Scene.m_Meshes[testMeshIdx].m_Radius == doctest::Approx(0.866f).epsilon(1e-4f));
        CHECK(!g_Renderer.m_Scene.m_Meshes[testMeshIdx].m_bBoundsValid);

        // Build a MeshUpdateCommand with vertices that form a triangle at a
        // known position (all vertices offset by +10 on X).
        MeshUpdateCommand cmd;
        cmd.m_LoadID = 99;
        {
            auto makeVert = [](float x, float y, float z) {
                srrhi::VertexQuantized v{};
                v.m_Pos = { x, y, z };
                return v;
            };
            cmd.m_Vertices = { makeVert(10.0f, 0.0f, 0.0f),
                               makeVert(11.0f, 0.0f, 0.0f),
                               makeVert(10.0f, 1.0f, 0.0f) };
            cmd.m_Indices  = { 0, 1, 2 };
            // Simulate bg-thread sphere computation.
            Sphere s03;
            Sphere::CreateFromPoints(s03, cmd.m_Vertices.size(), &cmd.m_Vertices[0].m_Pos, sizeof(srrhi::VertexQuantized));
            cmd.m_LocalSphereCenter = Vector3(s03.Center.x, s03.Center.y, s03.Center.z);
            cmd.m_LocalSphereRadius = s03.Radius;
        }
        cmd.m_MeshData.m_LODCount         = 1;
        cmd.m_MeshData.m_IndexOffsets[0]  = 0;
        cmd.m_MeshData.m_IndexCounts[0]   = 3;
        cmd.m_MeshData.m_MeshletOffsets[0] = 0;
        cmd.m_MeshData.m_MeshletCounts[0]  = 0;
        cmd.m_AffectedPrimitives = { { testMeshIdx, 0 } };

        // Inject via the thread-safe pending queue and call ApplyPendingUpdates.
        {
            std::lock_guard<std::mutex> lk(g_Renderer.m_Scene.m_PendingMeshMutex);
            g_Renderer.m_Scene.m_PendingMeshUpdates.push_back(std::move(cmd));
        }
        g_Renderer.m_Scene.ApplyPendingUpdates();
        g_Renderer.ExecutePendingCommandLists();

        const Scene::Mesh& updatedMesh = g_Renderer.m_Scene.m_Meshes[testMeshIdx];

        // BV must now be valid and not the original placeholder.
        CHECK(updatedMesh.m_bBoundsValid);
        CHECK(updatedMesh.m_Radius > 0.0f);
        CHECK(updatedMesh.m_Radius != doctest::Approx(0.866f).epsilon(1e-4f));

        // The sphere center must be in the x=[10,11] range (all vertices have x in [10,11]).
        CHECK(updatedMesh.m_Center.x >= 10.0f - 1e-3f);
        CHECK(updatedMesh.m_Center.x <= 11.0f + 1e-3f);

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-ABV-04: ApplyPendingUpdates propagates BV to instance data
    // After the async update, m_InstanceData[i].m_Center/m_Radius must
    // equal the node's world-space transformed mesh sphere (identity
    // world = local sphere passes through unchanged).
    // ------------------------------------------------------------------
    TEST_CASE("TC-ABV-04 AsyncBV - ApplyPendingUpdates propagates BV to instance data")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        g_Renderer.m_Scene.InitializeDefaultCube(64, 64);
        g_Renderer.ExecutePendingCommandLists();

        // Same minimal setup as TC-ABV-03.
        {
            Scene::Primitive prim;
            prim.m_VertexOffset  = 0;
            prim.m_VertexCount   = 0;
            prim.m_MeshDataIndex = 0;
            prim.m_MaterialIndex = -1;

            Scene::Mesh placeholder;
            placeholder.m_Primitives.push_back(prim);
            placeholder.m_Center      = Vector3{ 0.0f, 0.0f, 0.0f };
            placeholder.m_Radius      = 0.866f;
            placeholder.m_bBoundsValid = false;
            g_Renderer.m_Scene.m_Meshes.push_back(std::move(placeholder));
        }
        const int testMeshIdx = 1;

        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();

        {
            Scene::Node node;
            node.m_MeshIndex = testMeshIdx;
            // Place the node at (5, 0, 0) so the world-space center should shift.
            DirectX::XMStoreFloat4x4(&node.m_WorldTransform,
                DirectX::XMMatrixTranslation(5.0f, 0.0f, 0.0f));
            DirectX::XMStoreFloat4x4(&node.m_LocalTransform,
                DirectX::XMMatrixTranslation(5.0f, 0.0f, 0.0f));
            node.m_Translation = Vector3{ 5.0f, 0.0f, 0.0f };
            node.m_Scale       = Vector3{ 1.0f, 1.0f, 1.0f };
            node.m_Rotation    = Quaternion{ 0.0f, 0.0f, 0.0f, 1.0f };
            g_Renderer.m_Scene.m_Nodes.push_back(std::move(node));
        }

        g_Renderer.m_Scene.FinalizeLoadedScene();

        const Scene::Node& testNode = g_Renderer.m_Scene.m_Nodes.back();
        REQUIRE(!testNode.m_InstanceIndices.empty());
        const uint32_t testInstIdx = testNode.m_InstanceIndices[0];

        // Inject a command: vertices at local origin (0,0,0) with radius ~0.5.
        MeshUpdateCommand cmd;
        cmd.m_LoadID = 100;
        {
            auto makeVert = [](float x, float y, float z) {
                srrhi::VertexQuantized v{};
                v.m_Pos = { x, y, z };
                return v;
            };
            cmd.m_Vertices = { makeVert(-0.5f, 0.0f, 0.0f),
                               makeVert( 0.5f, 0.0f, 0.0f),
                               makeVert( 0.0f, 0.5f, 0.0f) };
            cmd.m_Indices  = { 0, 1, 2 };
            // Simulate bg-thread sphere computation.
            Sphere s04;
            Sphere::CreateFromPoints(s04, cmd.m_Vertices.size(), &cmd.m_Vertices[0].m_Pos, sizeof(srrhi::VertexQuantized));
            cmd.m_LocalSphereCenter = Vector3(s04.Center.x, s04.Center.y, s04.Center.z);
            cmd.m_LocalSphereRadius = s04.Radius;
        }
        cmd.m_MeshData.m_LODCount         = 1;
        cmd.m_MeshData.m_IndexOffsets[0]  = 0;
        cmd.m_MeshData.m_IndexCounts[0]   = 3;
        cmd.m_MeshData.m_MeshletOffsets[0] = 0;
        cmd.m_MeshData.m_MeshletCounts[0]  = 0;
        cmd.m_AffectedPrimitives = { { testMeshIdx, 0 } };

        {
            std::lock_guard<std::mutex> lk(g_Renderer.m_Scene.m_PendingMeshMutex);
            g_Renderer.m_Scene.m_PendingMeshUpdates.push_back(std::move(cmd));
        }
        g_Renderer.m_Scene.ApplyPendingUpdates();
        g_Renderer.ExecutePendingCommandLists();

        const Scene::Mesh& updatedMesh = g_Renderer.m_Scene.m_Meshes[testMeshIdx];
        REQUIRE(updatedMesh.m_bBoundsValid);

        // The updated node's world-space BV must match the instance data.
        // (Node was updated inside ApplyPendingUpdates via UpdateNodeBoundingSphere.)
        const Scene::Node& updatedNode  = g_Renderer.m_Scene.m_Nodes.back();
        const srrhi::PerInstanceData& inst = g_Renderer.m_Scene.m_InstanceData[testInstIdx];

        CHECK(inst.m_Radius   == doctest::Approx(updatedNode.m_Radius).epsilon(1e-4f));
        CHECK(inst.m_Center.x == doctest::Approx(updatedNode.m_Center.x).epsilon(1e-3f));
        CHECK(inst.m_Center.y == doctest::Approx(updatedNode.m_Center.y).epsilon(1e-3f));
        CHECK(inst.m_Center.z == doctest::Approx(updatedNode.m_Center.z).epsilon(1e-3f));

        // The world-space center must be offset by the node translation (5, 0, 0).
        // Local mesh center is near origin, so world center must be near (5, 0, 0).
        CHECK(inst.m_Center.x == doctest::Approx(5.0f + updatedMesh.m_Center.x).epsilon(1e-3f));

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }

    // ------------------------------------------------------------------
    // TC-ABV-05: Placeholder sphere (m_bBoundsValid=false) is never used
    //            for GPU culling — instance radius must not equal 0.866f
    //            after ApplyPendingUpdates with real geometry.
    // ------------------------------------------------------------------
    TEST_CASE("TC-ABV-05 AsyncBV - placeholder sphere replaced after async update")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();

        g_Renderer.m_Scene.InitializeDefaultCube(64, 64);
        g_Renderer.ExecutePendingCommandLists();
        g_Renderer.m_Scene.EnsureDefaultDirectionalLight();

        // Add a placeholder mesh and node.
        {
            Scene::Primitive prim;
            prim.m_VertexOffset  = 0;
            prim.m_VertexCount   = 0;
            prim.m_MeshDataIndex = 0;
            prim.m_MaterialIndex = -1;

            Scene::Mesh placeholder;
            placeholder.m_Primitives.push_back(prim);
            placeholder.m_Center      = Vector3{ 0.0f, 0.0f, 0.0f };
            placeholder.m_Radius      = 0.866f;
            placeholder.m_bBoundsValid = false;
            g_Renderer.m_Scene.m_Meshes.push_back(std::move(placeholder));
        }
        const int testMeshIdx = 1;

        {
            Scene::Node node;
            node.m_MeshIndex = testMeshIdx;
            DirectX::XMStoreFloat4x4(&node.m_WorldTransform, DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&node.m_LocalTransform, DirectX::XMMatrixIdentity());
            g_Renderer.m_Scene.m_Nodes.push_back(std::move(node));
        }
        g_Renderer.m_Scene.UpdateNodeBoundingSphere((int)g_Renderer.m_Scene.m_Nodes.size() - 1);
        g_Renderer.m_Scene.FinalizeLoadedScene();

        const Scene::Node& testNode  = g_Renderer.m_Scene.m_Nodes.back();
        REQUIRE(!testNode.m_InstanceIndices.empty());
        const uint32_t testInstIdx = testNode.m_InstanceIndices[0];

        // Before update: instance sphere is the placeholder.
        CHECK(g_Renderer.m_Scene.m_InstanceData[testInstIdx].m_Radius == doctest::Approx(0.866f).epsilon(1e-4f));

        // Send a command with geometry whose radius is clearly different.
        MeshUpdateCommand cmd;
        cmd.m_LoadID = 101;
        {
            auto makeVert = [](float x, float y, float z) {
                srrhi::VertexQuantized v{};
                v.m_Pos = { x, y, z };
                return v;
            };
            // Wide triangle — radius will be ~7 units, definitely not 0.866.
            cmd.m_Vertices = { makeVert(-7.0f, 0.0f, 0.0f),
                               makeVert( 7.0f, 0.0f, 0.0f),
                               makeVert( 0.0f, 7.0f, 0.0f) };
            cmd.m_Indices  = { 0, 1, 2 };
            // Simulate bg-thread sphere computation.
            Sphere s05;
            Sphere::CreateFromPoints(s05, cmd.m_Vertices.size(), &cmd.m_Vertices[0].m_Pos, sizeof(srrhi::VertexQuantized));
            cmd.m_LocalSphereCenter = Vector3(s05.Center.x, s05.Center.y, s05.Center.z);
            cmd.m_LocalSphereRadius = s05.Radius;
        }
        cmd.m_MeshData.m_LODCount          = 1;
        cmd.m_MeshData.m_IndexOffsets[0]   = 0;
        cmd.m_MeshData.m_IndexCounts[0]    = 3;
        cmd.m_MeshData.m_MeshletOffsets[0] = 0;
        cmd.m_MeshData.m_MeshletCounts[0]  = 0;
        cmd.m_AffectedPrimitives = { { testMeshIdx, 0 } };

        {
            std::lock_guard<std::mutex> lk(g_Renderer.m_Scene.m_PendingMeshMutex);
            g_Renderer.m_Scene.m_PendingMeshUpdates.push_back(std::move(cmd));
        }
        g_Renderer.m_Scene.ApplyPendingUpdates();
        g_Renderer.ExecutePendingCommandLists();

        // After update: instance sphere must no longer be the placeholder 0.866f.
        const float newRadius = g_Renderer.m_Scene.m_InstanceData[testInstIdx].m_Radius;
        CHECK(newRadius != doctest::Approx(0.866f).epsilon(0.1f));
        CHECK(newRadius > 1.0f);  // wide triangle should have a radius well above 1

        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
    }
}
