#include "TestFixtures.h"

#include "../AsyncMeshQueue.h"
#include "../AsyncTextureQueue.h"
#include "../PendingInstanceUpdate.h"

TEST_SUITE("AsyncStreaming_TextureQueue")
{
    TEST_CASE("TC-ASTQ-01 AsyncTextureQueue - invalid path yields cancelled command")
    {
        AsyncTextureQueue q;
        q.Start("TC-ASTQ-01");

        std::mutex m;
        std::condition_variable cv;
        bool bDone = false;
        bool bCancelled = false;
        bool bHasData = false;

        q.EnqueueLoad("Z:/this/path/does/not/exist.dds", 123u, Scene::Texture::Wrap,
            [&](TextureUpdateCommand cmd)
            {
                std::lock_guard<std::mutex> lk(m);
                bCancelled = cmd.m_bCancelled;
                bHasData = (cmd.m_Data != nullptr);
                bDone = true;
                cv.notify_one();
            });

        q.Flush();
        q.Stop("TC-ASTQ-01");

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(1), [&]() { return bDone; });
        }

        CHECK(bDone);
        CHECK(bCancelled);
        CHECK_FALSE(bHasData);
    }

    TEST_CASE("TC-ASTQ-02 AsyncTextureQueue - CancelLoad before Start deterministically cancels")
    {
        const std::filesystem::path ddsPath = ReferenceImagePath("Red.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: Red.dds not found");
            return;
        }

        AsyncTextureQueue q;

        std::mutex m;
        std::condition_variable cv;
        bool bDone = false;
        bool bCancelled = false;

        const PendingLoadID id = q.EnqueueLoad(ddsPath.string(), 5u, Scene::Texture::Wrap,
            [&](TextureUpdateCommand cmd)
            {
                std::lock_guard<std::mutex> lk(m);
                bCancelled = cmd.m_bCancelled;
                bDone = true;
                cv.notify_one();
            });

        q.CancelLoad(id);
        q.Start("TC-ASTQ-02");
        q.Flush();
        q.Stop("TC-ASTQ-02");

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(1), [&]() { return bDone; });
        }

        CHECK(bDone);
        CHECK(bCancelled);
    }
}

TEST_SUITE("AsyncStreaming_SceneTextureFlow")
{
    TEST_CASE("TC-ASTF-01 SceneLoader - reserves stable bindless slots before texture upload")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
        g_Renderer.m_Scene.InitializeDefaultCube(0, 0);
        g_Renderer.ExecutePendingCommandLists();

        Scene& scene = g_Renderer.m_Scene;
        scene.m_Textures.clear();
        scene.m_Textures.resize(3);
        scene.m_Textures[0].m_Uri = "";
        scene.m_Textures[1].m_Uri = "";
        scene.m_Textures[2].m_Uri = "";

        SceneLoader::LoadTexturesFromImages(scene, {});

        CHECK(scene.m_Textures[0].m_BindlessIndex != UINT32_MAX);
        CHECK(scene.m_Textures[1].m_BindlessIndex != UINT32_MAX);
        CHECK(scene.m_Textures[2].m_BindlessIndex != UINT32_MAX);

        CHECK(scene.m_Textures[0].m_BindlessIndex != scene.m_Textures[1].m_BindlessIndex);
        CHECK(scene.m_Textures[1].m_BindlessIndex != scene.m_Textures[2].m_BindlessIndex);
        CHECK(scene.m_Textures[0].m_BindlessIndex != scene.m_Textures[2].m_BindlessIndex);

        DEV()->waitForIdle();
        scene.Shutdown();
    }

    TEST_CASE("TC-ASTF-02 Scene::ApplyPendingUpdates - keeps reserved bindless slot on texture upload")
    {
        const std::filesystem::path ddsPath = ReferenceImagePath("Green.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: Green.dds not found");
            return;
        }

        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
        g_Renderer.m_Scene.InitializeDefaultCube(0, 0);
        g_Renderer.ExecutePendingCommandLists();

        Scene& scene = g_Renderer.m_Scene;
        scene.m_Textures.clear();
        scene.m_Textures.resize(1);
        scene.m_Textures[0].m_Uri = "";

        SceneLoader::LoadTexturesFromImages(scene, {});
        REQUIRE(scene.m_Textures[0].m_BindlessIndex != UINT32_MAX);
        const uint32_t reservedIndex = scene.m_Textures[0].m_BindlessIndex;

        TextureUpdateCommand cmd;
        cmd.m_LoadID = 1;
        cmd.m_TextureIndex = 0;
        const bool ok = LoadTexture(ddsPath.string(), cmd.m_Desc, cmd.m_Data);
        REQUIRE(ok);
        REQUIRE(cmd.m_Data != nullptr);

        {
            std::lock_guard<std::mutex> lk(scene.m_PendingTextureMutex);
            scene.m_PendingTextureUpdates.push_back(std::move(cmd));
        }

        scene.ApplyPendingUpdates();
        g_Renderer.ExecutePendingCommandLists();

        CHECK(scene.m_Textures[0].m_Handle != nullptr);
        CHECK(scene.m_Textures[0].m_BindlessIndex == reservedIndex);

        DEV()->waitForIdle();
        scene.Shutdown();
    }
}

TEST_SUITE("AsyncStreaming_MeshFlow")
{
    TEST_CASE("TC-ASMF-01 ApplyPendingUpdates - multi-primitive single-node patch uses primitive mapping")
    {
        REQUIRE(DEV() != nullptr);
        DEV()->waitForIdle();
        g_Renderer.m_Scene.Shutdown();
        g_Renderer.m_Scene.InitializeDefaultCube(64, 64);
        g_Renderer.ExecutePendingCommandLists();

        Scene& scene = g_Renderer.m_Scene;
        REQUIRE(!scene.m_Meshes.empty());
        REQUIRE(!scene.m_Meshes[0].m_Primitives.empty());
        const Scene::Primitive& defaultCubePrim = scene.m_Meshes[0].m_Primitives[0];

        // Two materials to force bucket split (opaque first, masked later).
        Scene::Material opaqueMat;
        opaqueMat.m_AlphaMode = srrhi::CommonConsts::ALPHA_MODE_OPAQUE;
        Scene::Material maskedMat;
        maskedMat.m_AlphaMode = srrhi::CommonConsts::ALPHA_MODE_MASK;
        scene.m_Materials.push_back(opaqueMat); // index 0
        scene.m_Materials.push_back(maskedMat); // index 1

        // Mesh with two primitives on the same node.
        // Primitive order: 0=masked, 1=opaque (bucket order will differ).
        Scene::Mesh mesh;
        {
            Scene::Primitive p0;
            p0.m_VertexOffset = defaultCubePrim.m_VertexOffset;
            p0.m_VertexCount = defaultCubePrim.m_VertexCount;
            p0.m_MeshDataIndex = 0;
            p0.m_MaterialIndex = 1;
            mesh.m_Primitives.push_back(p0);

            Scene::Primitive p1;
            p1.m_VertexOffset = defaultCubePrim.m_VertexOffset;
            p1.m_VertexCount = defaultCubePrim.m_VertexCount;
            p1.m_MeshDataIndex = 0;
            p1.m_MaterialIndex = 0;
            mesh.m_Primitives.push_back(p1);

            mesh.m_Center = Vector3{ 0.0f, 0.0f, 0.0f };
            mesh.m_Radius = 0.866f;
            mesh.m_bBoundsValid = false;
        }
        scene.m_Meshes.push_back(std::move(mesh));
        const int testMeshIdx = (int)scene.m_Meshes.size() - 1;

        // Node referencing this mesh.
        {
            Scene::Node node;
            node.m_MeshIndex = testMeshIdx;
            DirectX::XMStoreFloat4x4(&node.m_WorldTransform, DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&node.m_LocalTransform, DirectX::XMMatrixIdentity());
            scene.m_Nodes.push_back(std::move(node));
        }
        const int nodeIdx = (int)scene.m_Nodes.size() - 1;

        scene.EnsureDefaultDirectionalLight();
        scene.FinalizeLoadedScene();

        REQUIRE(scene.m_Nodes[nodeIdx].m_PrimitiveToInstanceIndex.size() >= 2);
        const uint32_t instForPrim0 = scene.m_Nodes[nodeIdx].m_PrimitiveToInstanceIndex[0];
        const uint32_t instForPrim1 = scene.m_Nodes[nodeIdx].m_PrimitiveToInstanceIndex[1];
        REQUIRE(instForPrim0 != UINT32_MAX);
        REQUIRE(instForPrim1 != UINT32_MAX);
        REQUIRE(instForPrim0 != instForPrim1);

        // Sanity: mapping follows primitive material indices.
        CHECK(scene.m_InstanceData[instForPrim0].m_MaterialIndex == 1u);
        CHECK(scene.m_InstanceData[instForPrim1].m_MaterialIndex == 0u);

        MeshUpdateCommand cmd;
        cmd.m_LoadID = 42;
        cmd.m_AffectedPrimitives = { { testMeshIdx, 1 } }; // update primitive 1 only

        auto makeVert = [](float x, float y, float z)
        {
            srrhi::VertexQuantized v{};
            v.m_Pos = { x, y, z };
            return v;
        };
        cmd.m_Vertices = {
            makeVert(0.0f, 0.0f, 0.0f),
            makeVert(1.0f, 0.0f, 0.0f),
            makeVert(0.0f, 1.0f, 0.0f)
        };
        cmd.m_Indices = { 0, 1, 2 };
        cmd.m_MeshData.m_LODCount = 1;
        cmd.m_MeshData.m_IndexOffsets[0] = 0;
        cmd.m_MeshData.m_IndexCounts[0] = 3;
        cmd.m_MeshData.m_MeshletOffsets[0] = 0;
        cmd.m_MeshData.m_MeshletCounts[0] = 0;

        Sphere s;
        Sphere::CreateFromPoints(s, cmd.m_Vertices.size(), &cmd.m_Vertices[0].m_Pos, sizeof(srrhi::VertexQuantized));
        cmd.m_LocalSphereCenter = Vector3(s.Center.x, s.Center.y, s.Center.z);
        cmd.m_LocalSphereRadius = s.Radius;

        {
            std::lock_guard<std::mutex> lk(scene.m_PendingMeshMutex);
            scene.m_PendingMeshUpdates.push_back(std::move(cmd));
        }

        scene.ApplyPendingUpdates();
        g_Renderer.ExecutePendingCommandLists();

        // Primitive 1 instance should be updated to real mesh data index;
        // primitive 0 should remain placeholder mesh data index.
        CHECK(scene.m_InstanceData[instForPrim1].m_MeshDataIndex != 0u);
        CHECK(scene.m_InstanceData[instForPrim0].m_MeshDataIndex == 0u);

        DEV()->waitForIdle();
        scene.Shutdown();
    }

    TEST_CASE("TC-ASMF-02 AsyncMeshQueue - invalid mmap input returns empty command without cancellation")
    {
        AsyncMeshQueue q;
        q.Start("TC-ASMF-02");

        std::mutex m;
        std::condition_variable cv;
        bool bDone = false;
        bool bCancelled = true;
        size_t vertCount = 1;
        std::pair<int, int> affected = { -1, -1 };

        PendingAsyncMeshInfo info;
        info.gltfPath = "";
        info.binFilePath = "Z:/no/such/file.bin";
        info.binDataOffset = 0;
        info.sceneMeshIdx = 7;
        info.scenePrimIdx = 3;
        info.posAccessor.present = true;
        info.posAccessor.count = 1;
        info.posAccessor.byteOffset = 0;
        info.posAccessor.byteStride = 12;
        info.posAccessor.componentType = 6; // float
        info.posAccessor.numComponents = 3;

        q.EnqueueLoad(info, [&](MeshUpdateCommand cmd)
        {
            std::lock_guard<std::mutex> lk(m);
            bCancelled = cmd.m_bCancelled;
            vertCount = cmd.m_Vertices.size();
            if (!cmd.m_AffectedPrimitives.empty())
                affected = cmd.m_AffectedPrimitives[0];
            bDone = true;
            cv.notify_one();
        });

        q.Flush();
        q.Stop("TC-ASMF-02");

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(1), [&]() { return bDone; });
        }

        CHECK(bDone);
        CHECK_FALSE(bCancelled);
        CHECK(vertCount == 0u);
        CHECK(affected.first == 7);
        CHECK(affected.second == 3);
    }
}
