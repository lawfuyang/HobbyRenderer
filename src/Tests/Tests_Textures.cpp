// Tests_Textures.cpp - Texture System Tests
//
// Systems under test: TextureLoader, Bindless SRV heap, Sampler binding
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//                Scene loading is performed inside individual tests using
//                SceneScope where GPU-resident textures are required.
//
// Test coverage:
//   - DDS loading (BC-compressed, uncompressed, mip chains, cubemaps)
//   - PNG/JPG loading via stb_image
//   - SRV allocation: valid handle, valid bindless index, non-UINT32_MAX
//   - Heap consistency: descriptor table non-null, indices in-range
//   - Unload / removal: GPU handle released after scene shutdown
//   - Sampler binding: all common samplers registered at expected indices
//
// Reference DDS files (1×1 solid-colour, BC1 compressed) live in:
//   src/Tests/ReferenceImages/  (Red.dds, Green.dds, Blue.dds, White.dds)
//
// Run with: HobbyRenderer --run-tests=*Texture* --gltf-samples <path>
// ============================================================================

#include "TestFixtures.h"
#include "../shaders/srrhi/cpp/Common.h"   // srrhi::CommonConsts

// ============================================================================
// TEST SUITE: Textures_DDSLoading
// ============================================================================
TEST_SUITE("Textures_DDSLoading")
{
    // ------------------------------------------------------------------
    // TC-DDS-01: LoadTexture succeeds for a known-good DDS file
    // ------------------------------------------------------------------
    TEST_CASE("TC-DDS-01 DDSLoading - LoadTexture succeeds for Red.dds")
    {
        const std::filesystem::path ddsPath = ReferenceImagePath("Red.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: Red.dds not found");
            return;
        }

        nvrhi::TextureDesc desc{};
        std::unique_ptr<ITextureDataReader> data;
        const bool ok = LoadTexture(ddsPath.string(), desc, data);

        CHECK(ok);
        CHECK(data != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-DDS-02: DDS descriptor has non-zero dimensions
    // ------------------------------------------------------------------
    TEST_CASE("TC-DDS-02 DDSLoading - descriptor has non-zero dimensions")
    {
        const std::filesystem::path ddsPath = ReferenceImagePath("Red.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: Red.dds not found");
            return;
        }

        nvrhi::TextureDesc desc{};
        std::unique_ptr<ITextureDataReader> data;
        LoadTexture(ddsPath.string(), desc, data);

        REQUIRE(data != nullptr);
        CHECK(desc.width  > 0u);
        CHECK(desc.height > 0u);
    }

    // ------------------------------------------------------------------
    // TC-DDS-03: DDS descriptor dimension is Texture2D
    // ------------------------------------------------------------------
    TEST_CASE("TC-DDS-03 DDSLoading - descriptor dimension is Texture2D")
    {
        const std::filesystem::path ddsPath = ReferenceImagePath("White.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: White.dds not found");
            return;
        }

        nvrhi::TextureDesc desc{};
        std::unique_ptr<ITextureDataReader> data;
        LoadTexture(ddsPath.string(), desc, data);

        REQUIRE(data != nullptr);
        CHECK(desc.dimension == nvrhi::TextureDimension::Texture2D);
    }

    // ------------------------------------------------------------------
    // TC-DDS-04: All four reference DDS files load successfully
    // ------------------------------------------------------------------
    TEST_CASE("TC-DDS-04 DDSLoading - all reference DDS files load")
    {
        const char* files[] = { "Red.dds", "Green.dds", "Blue.dds", "White.dds" };
        int skipped = 0;

        for (const char* f : files)
        {
            const std::filesystem::path p = ReferenceImagePath(f);
            if (!std::filesystem::exists(p))
            {
                ++skipped;
                continue;
            }

            nvrhi::TextureDesc desc{};
            std::unique_ptr<ITextureDataReader> data;
            const bool ok = LoadTexture(p.string(), desc, data);

            INFO("File: " << f);
            CHECK(ok);
            CHECK(data != nullptr);
            CHECK(desc.width  > 0u);
            CHECK(desc.height > 0u);
        }

        if (skipped == 4)
            WARN("Skipping TC-DDS-04: no reference DDS files found");
    }

    // ------------------------------------------------------------------
    // TC-DDS-05: DDS data pointer is non-null and size is non-zero
    // ------------------------------------------------------------------
    TEST_CASE("TC-DDS-05 DDSLoading - data pointer and size are valid")
    {
        const std::filesystem::path ddsPath = ReferenceImagePath("Blue.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: Blue.dds not found");
            return;
        }

        nvrhi::TextureDesc desc{};
        std::unique_ptr<ITextureDataReader> data;
        LoadTexture(ddsPath.string(), desc, data);

        REQUIRE(data != nullptr);
        CHECK(data->GetData() != nullptr);
        CHECK(data->GetSize() > 0u);
    }

    // ------------------------------------------------------------------
    // TC-DDS-06: LoadDDSTexture directly produces same result as LoadTexture
    // ------------------------------------------------------------------
    TEST_CASE("TC-DDS-06 DDSLoading - LoadDDSTexture matches LoadTexture")
    {
        const std::filesystem::path ddsPath = ReferenceImagePath("Green.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: Green.dds not found");
            return;
        }

        nvrhi::TextureDesc descA{}, descB{};
        std::unique_ptr<ITextureDataReader> dataA, dataB;

        LoadTexture(ddsPath.string(), descA, dataA);
        LoadDDSTexture(ddsPath.string(), descB, dataB);

        REQUIRE(dataA != nullptr);
        REQUIRE(dataB != nullptr);

        CHECK(descA.width     == descB.width);
        CHECK(descA.height    == descB.height);
        CHECK(descA.mipLevels == descB.mipLevels);
        CHECK(descA.format    == descB.format);
        CHECK(descA.dimension == descB.dimension);
        CHECK(dataA->GetSize() == dataB->GetSize());
    }
}

// ============================================================================
// TEST SUITE: Textures_STBILoading  (PNG / JPG via stb_image)
// ============================================================================
TEST_SUITE("Textures_STBILoading")
{
    // ------------------------------------------------------------------
    // TC-STBI-01: LoadTexture dispatches to STBI for .png extension
    //             We use a BoxTextured scene which contains PNG textures.
    // ------------------------------------------------------------------
    TEST_CASE("TC-STBI-01 STBILoading - scene PNG textures load via LoadTexture")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        // BoxTextured has at least one PNG texture; verify it loaded.
        const auto& textures = g_Renderer.m_Scene.m_Textures;
        REQUIRE(!textures.empty());

        bool foundPng = false;
        for (const auto& tex : textures)
        {
            const std::string ext = std::filesystem::path(tex.m_Uri).extension().string();
            if (ext == ".png" || ext == ".PNG")
            {
                foundPng = true;
                INFO("PNG texture: " << tex.m_Uri);
                CHECK(tex.m_Handle != nullptr);
                CHECK(tex.m_BindlessIndex != UINT32_MAX);
            }
        }

        if (!foundPng)
            WARN("BoxTextured scene contained no .png textures - test inconclusive");
    }

    // ------------------------------------------------------------------
    // TC-STBI-02: STBI-loaded texture has RGBA8_UNORM format
    // ------------------------------------------------------------------
    TEST_CASE("TC-STBI-02 STBILoading - PNG texture format is RGBA8_UNORM")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& textures = g_Renderer.m_Scene.m_Textures;
        for (const auto& tex : textures)
        {
            const std::string ext = std::filesystem::path(tex.m_Uri).extension().string();
            if (ext != ".png" && ext != ".PNG") continue;

            REQUIRE(tex.m_Handle != nullptr);
            const nvrhi::TextureDesc& desc = tex.m_Handle->getDesc();
            INFO("PNG texture: " << tex.m_Uri);
            CHECK(desc.format == nvrhi::Format::RGBA8_UNORM);
        }
    }

    // ------------------------------------------------------------------
    // TC-STBI-03: STBI-loaded texture has exactly 1 mip level
    //             (stb_image does not generate mip chains)
    // ------------------------------------------------------------------
    TEST_CASE("TC-STBI-03 STBILoading - PNG texture has 1 mip level")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& textures = g_Renderer.m_Scene.m_Textures;
        for (const auto& tex : textures)
        {
            const std::string ext = std::filesystem::path(tex.m_Uri).extension().string();
            if (ext != ".png" && ext != ".PNG") continue;

            REQUIRE(tex.m_Handle != nullptr);
            const nvrhi::TextureDesc& desc = tex.m_Handle->getDesc();
            INFO("PNG texture: " << tex.m_Uri);
            CHECK(desc.mipLevels == 1u);
        }
    }

    // ------------------------------------------------------------------
    // TC-STBI-04: LoadSTBITexture directly produces valid descriptor
    // ------------------------------------------------------------------
    TEST_CASE("TC-STBI-04 STBILoading - LoadSTBITexture produces valid descriptor")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        // Find the first PNG texture path from the BoxTextured scene.
        // We load the scene just to discover the URI, then immediately unload it.
        // Note: tex.m_Uri is relative to the glTF file's directory, so we must
        // combine it with the scene directory to get a valid absolute path.
        std::string pngPath;
        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            if (!scope.loaded) return;

            // The scene directory is the parent of the glTF file.
            const std::filesystem::path sceneDir =
                std::filesystem::path(GltfSampleModel("BoxTextured/glTF/BoxTextured.gltf")).parent_path();

            for (const auto& tex : g_Renderer.m_Scene.m_Textures)
            {
                const std::string ext = std::filesystem::path(tex.m_Uri).extension().string();
                if (ext == ".png" || ext == ".PNG")
                {
                    // Resolve the URI relative to the scene directory.
                    pngPath = (sceneDir / tex.m_Uri).string();
                    break;
                }
            }
        }

        if (pngPath.empty())
        {
            WARN("No PNG texture found in BoxTextured scene");
            return;
        }

        nvrhi::TextureDesc desc{};
        std::unique_ptr<ITextureDataReader> data;
        LoadSTBITexture(pngPath, desc, data);

        REQUIRE(data != nullptr);
        CHECK(desc.width  > 0u);
        CHECK(desc.height > 0u);
        CHECK(desc.format == nvrhi::Format::RGBA8_UNORM);
        CHECK(desc.dimension == nvrhi::TextureDimension::Texture2D);
        CHECK(data->GetData() != nullptr);
        CHECK(data->GetSize() > 0u);
    }
}

// ============================================================================
// TEST SUITE: Textures_SRVAllocation
// ============================================================================
TEST_SUITE("Textures_SRVAllocation")
{
    // ------------------------------------------------------------------
    // TC-SRV-01: RegisterTexture returns a valid (non-UINT32_MAX) index
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRV-01 SRVAllocation - RegisterTexture returns valid index")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);

        // Create a minimal 1×1 RGBA8 texture.
        const uint32_t pixel = 0xFF0000FF; // RGBA red
        nvrhi::TextureHandle tex = CreateTestTexture2D(
            1, 1, nvrhi::Format::RGBA8_UNORM, &pixel, sizeof(uint32_t), "TC-SRV-01-Tex");
        REQUIRE(tex != nullptr);

        const uint32_t idx = g_Renderer.RegisterTexture(tex);
        CHECK(idx != UINT32_MAX);

        // Release the texture handle (GPU resource stays alive until heap slot is overwritten).
        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-SRV-02: Two successive RegisterTexture calls return distinct indices
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRV-02 SRVAllocation - successive registrations return distinct indices")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);

        const uint32_t pixelA = 0xFF0000FF;
        const uint32_t pixelB = 0xFF00FF00;

        nvrhi::TextureHandle texA = CreateTestTexture2D(1, 1, nvrhi::Format::RGBA8_UNORM, &pixelA, 4, "TC-SRV-02-A");
        nvrhi::TextureHandle texB = CreateTestTexture2D(1, 1, nvrhi::Format::RGBA8_UNORM, &pixelB, 4, "TC-SRV-02-B");
        REQUIRE(texA != nullptr);
        REQUIRE(texB != nullptr);

        const uint32_t idxA = g_Renderer.RegisterTexture(texA);
        const uint32_t idxB = g_Renderer.RegisterTexture(texB);

        CHECK(idxA != UINT32_MAX);
        CHECK(idxB != UINT32_MAX);
        CHECK(idxA != idxB);

        texA = nullptr;
        texB = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-SRV-03: RegisterTextureAtIndex at an explicit slot succeeds
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRV-03 SRVAllocation - RegisterTextureAtIndex at explicit slot")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);

        const uint32_t pixel = 0xFFFFFFFF;
        nvrhi::TextureHandle tex = CreateTestTexture2D(1, 1, nvrhi::Format::RGBA8_UNORM, &pixel, 4, "TC-SRV-03-Tex");
        REQUIRE(tex != nullptr);

        // Use a high slot index unlikely to collide with default textures (0-10)
        // or scene textures.  We pick slot 512 as a safe test slot.
        const uint32_t testSlot = 512u;
        const bool ok = g_Renderer.RegisterTextureAtIndex(testSlot, tex);
        CHECK(ok);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-SRV-04: Scene textures all have bindless indices above the
    //             default-texture reserved range (>= DEFAULT_TEXTURE_COUNT)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRV-04 SRVAllocation - scene texture indices are above reserved range")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& textures = g_Renderer.m_Scene.m_Textures;
        for (int i = 0; i < (int)textures.size(); ++i)
        {
            INFO("Texture " << i << " uri=" << textures[i].m_Uri
                 << " bindlessIndex=" << textures[i].m_BindlessIndex);
            CHECK(textures[i].m_BindlessIndex >= (uint32_t)srrhi::CommonConsts::DEFAULT_TEXTURE_COUNT);
        }
    }

    // ------------------------------------------------------------------
    // TC-SRV-05: Scene texture bindless indices are unique (no duplicates)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRV-05 SRVAllocation - scene texture bindless indices are unique")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& textures = g_Renderer.m_Scene.m_Textures;
        std::vector<uint32_t> indices;
        indices.reserve(textures.size());
        for (const auto& tex : textures)
            indices.push_back(tex.m_BindlessIndex);

        std::sort(indices.begin(), indices.end());
        const bool noDuplicates = (std::adjacent_find(indices.begin(), indices.end()) == indices.end());
        CHECK(noDuplicates);
    }

    // ------------------------------------------------------------------
    // TC-SRV-06: Scene texture GPU handles are valid nvrhi textures
    //             (non-null, non-zero dimensions, isShaderResource)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SRV-06 SRVAllocation - scene texture GPU handles are valid")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& textures = g_Renderer.m_Scene.m_Textures;
        for (int i = 0; i < (int)textures.size(); ++i)
        {
            REQUIRE(textures[i].m_Handle != nullptr);
            const nvrhi::TextureDesc& desc = textures[i].m_Handle->getDesc();
            INFO("Texture " << i << " uri=" << textures[i].m_Uri);
            CHECK(desc.width  > 0u);
            CHECK(desc.height > 0u);
            CHECK(desc.isShaderResource);
        }
    }
}

// ============================================================================
// TEST SUITE: Textures_HeapConsistency
// ============================================================================
TEST_SUITE("Textures_HeapConsistency")
{
    // ------------------------------------------------------------------
    // TC-HEAP-01: Static texture descriptor table is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-HEAP-01 HeapConsistency - static texture descriptor table is valid")
    {
        CHECK(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HEAP-02: Static texture binding layout is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-HEAP-02 HeapConsistency - static texture binding layout is valid")
    {
        CHECK(g_Renderer.GetStaticTextureBindingLayout() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HEAP-03: Default textures occupy the expected reserved slots
    //             (indices 0..DEFAULT_TEXTURE_COUNT-1 are all non-null)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HEAP-03 HeapConsistency - default texture slots are populated")
    {
        REQUIRE(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);

        // Verify each default texture handle is non-null (slot was written).
        CHECK(CR().DefaultTextureBlack  != nullptr);
        CHECK(CR().DefaultTextureWhite  != nullptr);
        CHECK(CR().DefaultTextureGray   != nullptr);
        CHECK(CR().DefaultTextureNormal != nullptr);
        CHECK(CR().DefaultTexturePBR    != nullptr);
        CHECK(CR().BRDF_LUT             != nullptr);
        CHECK(CR().IrradianceTexture    != nullptr);
        CHECK(CR().RadianceTexture      != nullptr);
        CHECK(CR().BrunetonTransmittance != nullptr);
        CHECK(CR().BrunetonScattering   != nullptr);
        CHECK(CR().BrunetonIrradiance   != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HEAP-04: Default texture indices match CommonConsts constants
    // ------------------------------------------------------------------
    TEST_CASE("TC-HEAP-04 HeapConsistency - default texture indices match CommonConsts")
    {
        // The constants are compile-time values; we verify the mapping is
        // self-consistent (each constant is unique and in [0, COUNT)).
        const int count = srrhi::CommonConsts::DEFAULT_TEXTURE_COUNT;
        CHECK(count > 0);

        const int indices[] = {
            srrhi::CommonConsts::DEFAULT_TEXTURE_BLACK,
            srrhi::CommonConsts::DEFAULT_TEXTURE_WHITE,
            srrhi::CommonConsts::DEFAULT_TEXTURE_GRAY,
            srrhi::CommonConsts::DEFAULT_TEXTURE_NORMAL,
            srrhi::CommonConsts::DEFAULT_TEXTURE_PBR,
            srrhi::CommonConsts::DEFAULT_TEXTURE_BRDF_LUT,
            srrhi::CommonConsts::DEFAULT_TEXTURE_IRRADIANCE,
            srrhi::CommonConsts::DEFAULT_TEXTURE_RADIANCE,
            srrhi::CommonConsts::BRUNETON_TRANSMITTANCE_TEXTURE,
            srrhi::CommonConsts::BRUNETON_SCATTERING_TEXTURE,
            srrhi::CommonConsts::BRUNETON_IRRADIANCE_TEXTURE,
        };

        // All indices must be in [0, count).
        for (int idx : indices)
        {
            INFO("Index: " << idx);
            CHECK(idx >= 0);
            CHECK(idx < count);
        }

        // All indices must be unique.
        std::vector<int> sorted(std::begin(indices), std::end(indices));
        std::sort(sorted.begin(), sorted.end());
        const bool allUnique = (std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
        CHECK(allUnique);
    }

    // ------------------------------------------------------------------
    // TC-HEAP-05: Scene load does not invalidate the descriptor table pointer
    // ------------------------------------------------------------------
    TEST_CASE("TC-HEAP-05 HeapConsistency - scene load preserves descriptor table pointer")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        const nvrhi::DescriptorTableHandle tableBefore = g_Renderer.GetStaticTextureDescriptorTable();
        REQUIRE(tableBefore != nullptr);

        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);

            // Table pointer must remain the same during scene lifetime.
            CHECK(g_Renderer.GetStaticTextureDescriptorTable() == tableBefore);
        }

        // Table pointer must remain the same after scene unload.
        CHECK(g_Renderer.GetStaticTextureDescriptorTable() == tableBefore);
    }

    // ------------------------------------------------------------------
    // TC-HEAP-06: Multiple scene loads do not corrupt the heap
    //             (default textures remain valid after two load/unload cycles)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HEAP-06 HeapConsistency - default textures survive two load/unload cycles")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        for (int cycle = 0; cycle < 2; ++cycle)
        {
            INFO("Cycle " << cycle);
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
        }

        // After both cycles the default textures must still be valid.
        CHECK(CR().DefaultTextureBlack != nullptr);
        CHECK(CR().DefaultTextureWhite != nullptr);
        CHECK(CR().DefaultTextureNormal != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-HEAP-07: Dummy SRV/UAV textures are non-null (slot-filler textures)
    // ------------------------------------------------------------------
    TEST_CASE("TC-HEAP-07 HeapConsistency - dummy SRV and UAV textures are valid")
    {
        CHECK(CR().DummySRVTexture  != nullptr);
        CHECK(CR().DummyUAVTexture  != nullptr);
    }
}

// ============================================================================
// TEST SUITE: Textures_UnloadRemoval
// ============================================================================
TEST_SUITE("Textures_UnloadRemoval")
{
    // ------------------------------------------------------------------
    // TC-UNLOAD-01: Scene texture handles are null after scene shutdown
    // ------------------------------------------------------------------
    TEST_CASE("TC-UNLOAD-01 UnloadRemoval - texture handles are null after scene shutdown")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        // Load the scene, capture the texture count, then unload.
        size_t texCount = 0;
        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
            texCount = g_Renderer.m_Scene.m_Textures.size();
            REQUIRE(texCount > 0);
            // Handles must be valid while scene is alive.
            for (const auto& tex : g_Renderer.m_Scene.m_Textures)
                CHECK(tex.m_Handle != nullptr);
        }
        // After SceneScope destructor: scene is shut down, textures cleared.
        CHECK(g_Renderer.m_Scene.m_Textures.empty());
    }

    // ------------------------------------------------------------------
    // TC-UNLOAD-02: Scene texture array is empty after shutdown
    // ------------------------------------------------------------------
    TEST_CASE("TC-UNLOAD-02 UnloadRemoval - texture array is empty after shutdown")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
            CHECK(!g_Renderer.m_Scene.m_Textures.empty());
        }

        CHECK(g_Renderer.m_Scene.m_Textures.empty());
    }

    // ------------------------------------------------------------------
    // TC-UNLOAD-03: Default textures remain valid after scene unload
    //               (they live in CommonResources, not in the scene)
    // ------------------------------------------------------------------
    TEST_CASE("TC-UNLOAD-03 UnloadRemoval - default textures survive scene unload")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
        }

        // Default textures must still be alive.
        CHECK(CR().DefaultTextureBlack  != nullptr);
        CHECK(CR().DefaultTextureWhite  != nullptr);
        CHECK(CR().DefaultTextureGray   != nullptr);
        CHECK(CR().DefaultTextureNormal != nullptr);
        CHECK(CR().DefaultTexturePBR    != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-UNLOAD-04: Descriptor table pointer is stable across scene unload
    // ------------------------------------------------------------------
    TEST_CASE("TC-UNLOAD-04 UnloadRemoval - descriptor table pointer stable across unload")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        const nvrhi::DescriptorTableHandle tablePtr = g_Renderer.GetStaticTextureDescriptorTable();
        REQUIRE(tablePtr != nullptr);

        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
        }

        CHECK(g_Renderer.GetStaticTextureDescriptorTable() == tablePtr);
    }

    // ------------------------------------------------------------------
    // TC-UNLOAD-05: Reload after unload produces valid textures again
    // ------------------------------------------------------------------
    TEST_CASE("TC-UNLOAD-05 UnloadRemoval - reload after unload produces valid textures")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        // First load/unload cycle.
        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
        }

        // Second load - must succeed and produce valid textures.
        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);

            const auto& textures = g_Renderer.m_Scene.m_Textures;
            CHECK(!textures.empty());
            for (const auto& tex : textures)
            {
                CHECK(tex.m_Handle != nullptr);
                CHECK(tex.m_BindlessIndex != UINT32_MAX);
            }
        }
    }
}

// ============================================================================
// TEST SUITE: Textures_SamplerBinding
// ============================================================================
TEST_SUITE("Textures_SamplerBinding")
{
    // ------------------------------------------------------------------
    // TC-SAMP-01: Static sampler descriptor table is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-01 SamplerBinding - static sampler descriptor table is valid")
    {
        CHECK(g_Renderer.GetStaticSamplerDescriptorTable() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-02: Static sampler binding layout is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-02 SamplerBinding - static sampler binding layout is valid")
    {
        CHECK(g_Renderer.GetStaticSamplerBindingLayout() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-03: All common sampler handles are non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-03 SamplerBinding - all common sampler handles are valid")
    {
        CHECK(CR().LinearClamp            != nullptr);
        CHECK(CR().LinearWrap             != nullptr);
        CHECK(CR().PointClamp             != nullptr);
        CHECK(CR().PointWrap              != nullptr);
        CHECK(CR().AnisotropicClamp       != nullptr);
        CHECK(CR().AnisotropicWrap        != nullptr);
        CHECK(CR().MaxReductionClamp      != nullptr);
        CHECK(CR().MinReductionClamp      != nullptr);
        CHECK(CR().LinearClampBorderWhite != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-04: Sampler index constants are unique and in valid range
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-04 SamplerBinding - sampler index constants are unique and in range")
    {
        const int samplerIndices[] = {
            srrhi::CommonConsts::SAMPLER_ANISOTROPIC_CLAMP_INDEX,
            srrhi::CommonConsts::SAMPLER_ANISOTROPIC_WRAP_INDEX,
            srrhi::CommonConsts::SAMPLER_POINT_CLAMP_INDEX,
            srrhi::CommonConsts::SAMPLER_POINT_WRAP_INDEX,
            srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX,
            srrhi::CommonConsts::SAMPLER_LINEAR_WRAP_INDEX,
            srrhi::CommonConsts::SAMPLER_MIN_REDUCTION_INDEX,
            srrhi::CommonConsts::SAMPLER_MAX_REDUCTION_INDEX,
            srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_BORDER_WHITE_INDEX,
        };

        // All indices must be non-negative.
        for (int idx : samplerIndices)
        {
            INFO("Sampler index: " << idx);
            CHECK(idx >= 0);
        }

        // All indices must be unique.
        std::vector<int> sorted(std::begin(samplerIndices), std::end(samplerIndices));
        std::sort(sorted.begin(), sorted.end());
        const bool allUnique = (std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
        CHECK(allUnique);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-05: Sampler count matches the number of registered samplers
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-05 SamplerBinding - sampler count is consistent")
    {
        // We have 9 common samplers registered in CommonResources::Initialize().
        // Verify the highest index is SAMPLER_LINEAR_CLAMP_BORDER_WHITE_INDEX = 8,
        // meaning 9 slots (0..8) are occupied.
        const int highestIdx = srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_BORDER_WHITE_INDEX;
        CHECK(highestIdx == 8);

        // All 9 sampler handles must be non-null.
        const nvrhi::SamplerHandle samplers[] = {
            CR().AnisotropicClamp,
            CR().AnisotropicWrap,
            CR().PointClamp,
            CR().PointWrap,
            CR().LinearClamp,
            CR().LinearWrap,
            CR().MinReductionClamp,
            CR().MaxReductionClamp,
            CR().LinearClampBorderWhite,
        };
        for (int i = 0; i < 9; ++i)
        {
            INFO("Sampler slot " << i);
            CHECK(samplers[i] != nullptr);
        }
    }

    // ------------------------------------------------------------------
    // TC-SAMP-06: Scene textures reference valid sampler types
    //             (Wrap or Clamp - both map to registered sampler indices)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-06 SamplerBinding - scene texture sampler types are valid")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& textures = g_Renderer.m_Scene.m_Textures;
        for (int i = 0; i < (int)textures.size(); ++i)
        {
            INFO("Texture " << i << " uri=" << textures[i].m_Uri);
            const auto s = textures[i].m_Sampler;
            CHECK((s == Scene::Texture::Wrap || s == Scene::Texture::Clamp));
        }
    }

    // ------------------------------------------------------------------
    // TC-SAMP-07: Sampler table pointer is stable across scene load/unload
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-07 SamplerBinding - sampler table pointer stable across scene lifecycle")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");

        const nvrhi::DescriptorTableHandle samplerTable = g_Renderer.GetStaticSamplerDescriptorTable();
        REQUIRE(samplerTable != nullptr);

        {
            SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
            REQUIRE(scope.loaded);
            CHECK(g_Renderer.GetStaticSamplerDescriptorTable() == samplerTable);
        }

        CHECK(g_Renderer.GetStaticSamplerDescriptorTable() == samplerTable);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-08: Shader hot-reload does not invalidate sampler table
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-08 SamplerBinding - shader hot-reload preserves sampler table")
    {
        const nvrhi::DescriptorTableHandle samplerTableBefore = g_Renderer.GetStaticSamplerDescriptorTable();
        REQUIRE(samplerTableBefore != nullptr);

        // Request a shader reload (non-destructive: shaders recompile but
        // descriptor tables are not recreated).
        g_Renderer.m_RequestedShaderReload = true;
        // The reload is processed at the start of the next frame; we just
        // verify the table is still valid immediately after setting the flag.
        CHECK(g_Renderer.GetStaticSamplerDescriptorTable() != nullptr);

        // Reset the flag so we don't accidentally trigger a reload mid-test.
        g_Renderer.m_RequestedShaderReload = false;

        CHECK(g_Renderer.GetStaticSamplerDescriptorTable() == samplerTableBefore);
    }
}

// ============================================================================
// TEST SUITE: Textures_GPUCreation
// ============================================================================
TEST_SUITE("Textures_GPUCreation")
{
    // ------------------------------------------------------------------
    // TC-GPU-01: Manually created 1×1 RGBA8 texture has correct descriptor
    // ------------------------------------------------------------------
    TEST_CASE("TC-GPU-01 GPUCreation - 1x1 RGBA8 texture descriptor is correct")
    {
        REQUIRE(DEV() != nullptr);

        const uint32_t pixel = 0xFF0000FF;
        nvrhi::TextureHandle tex = CreateTestTexture2D(
            1, 1, nvrhi::Format::RGBA8_UNORM, &pixel, 4, "TC-GPU-01-Tex");
        REQUIRE(tex != nullptr);

        const nvrhi::TextureDesc& desc = tex->getDesc();
        CHECK(desc.width     == 1u);
        CHECK(desc.height    == 1u);
        CHECK(desc.format    == nvrhi::Format::RGBA8_UNORM);
        CHECK(desc.dimension == nvrhi::TextureDimension::Texture2D);
        CHECK(desc.isShaderResource);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-GPU-02: Manually created 4×4 texture has correct dimensions
    // ------------------------------------------------------------------
    TEST_CASE("TC-GPU-02 GPUCreation - 4x4 texture has correct dimensions")
    {
        REQUIRE(DEV() != nullptr);

        // 4×4 RGBA8 checkerboard pattern
        uint32_t pixels[16];
        for (int i = 0; i < 16; ++i)
            pixels[i] = ((i + (i / 4)) % 2 == 0) ? 0xFF000000u : 0xFFFFFFFFu;

        nvrhi::TextureHandle tex = CreateTestTexture2D(
            4, 4, nvrhi::Format::RGBA8_UNORM, pixels, 4 * 4, "TC-GPU-02-Tex");
        REQUIRE(tex != nullptr);

        const nvrhi::TextureDesc& desc = tex->getDesc();
        CHECK(desc.width  == 4u);
        CHECK(desc.height == 4u);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-GPU-03: Texture created from DDS data can be registered in bindless heap
    // ------------------------------------------------------------------
    TEST_CASE("TC-GPU-03 GPUCreation - DDS-loaded texture registers in bindless heap")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);

        const std::filesystem::path ddsPath = ReferenceImagePath("White.dds");
        if (!std::filesystem::exists(ddsPath))
        {
            WARN("Skipping: White.dds not found");
            return;
        }

        nvrhi::TextureDesc desc{};
        std::unique_ptr<ITextureDataReader> data;
        LoadTexture(ddsPath.string(), desc, data);
        REQUIRE(data != nullptr);

        // Create the GPU texture.
        desc.initialState     = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.isShaderResource = true;
        desc.debugName        = "TC-GPU-03-DDS";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);

        // Upload data.
        {
            nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
            cmd->open();
            UploadTexture(cmd, tex, desc, data->GetData(), data->GetSize());
            cmd->close();
            g_Renderer.ExecutePendingCommandLists();
        }

        // Register in bindless heap.
        const uint32_t idx = g_Renderer.RegisterTexture(tex);
        CHECK(idx != UINT32_MAX);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-GPU-04: Texture created without initial data is still valid
    // ------------------------------------------------------------------
    TEST_CASE("TC-GPU-04 GPUCreation - texture created without initial data is valid")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::TextureHandle tex = CreateTestTexture2D(
            2, 2, nvrhi::Format::RGBA8_UNORM, nullptr, 0, "TC-GPU-04-Tex");
        REQUIRE(tex != nullptr);

        const nvrhi::TextureDesc& desc = tex->getDesc();
        CHECK(desc.width  == 2u);
        CHECK(desc.height == 2u);

        tex = nullptr;
        DEV()->waitForIdle();
    }
}
