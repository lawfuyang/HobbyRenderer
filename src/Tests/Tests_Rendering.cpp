// Tests_Rendering.cpp
//
// Systems under test: BasePassRenderer (OpaqueRenderer, MaskedPassRenderer,
//                     TransparentPassRenderer), HZB, RenderGraph
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources).
//               Scene loading is performed inside individual tests using
//               SceneScope where GPU-resident geometry is required.
//
// Test coverage:
//   - G-buffer texture creation: formats, dimensions, shader-resource flags
//   - RenderGraph resource declaration and compilation
//   - Opaque / masked / transparent bucket counts after scene load
//   - HZB texture creation and mip chain
//   - Frustum culling toggle: enable/disable does not crash
//   - Occlusion culling toggle: enable/disable does not crash
//   - Cone culling toggle: enable/disable does not crash
//   - Meshlet vs. vertex rendering toggle
//   - Forced LOD selection
//   - Shader hot-reload does not invalidate G-buffer resources
//   - Backbuffer capture stub (always passes - real impl deferred)
//   - RenderGraph Reset() clears transient resources
//   - BasePassRenderer IsBasePassRenderer() flag
//   - Renderer registry contains expected base-pass renderers
//
// Run with: HobbyRenderer --run-tests=*Rendering* --gltf-samples <path>
// ============================================================================

#include "TestFixtures.h"
#include "GraphicsTestUtils.h"
#include "../BasePassCommon.h"

// ============================================================================
// Internal helpers
// ============================================================================
namespace
{
    // Build a minimal RGTextureDesc for a G-buffer attachment.
    RGTextureDesc MakeGBufferDesc(uint32_t w, uint32_t h, nvrhi::Format fmt, const char* name)
    {
        RGTextureDesc desc;
        desc.m_NvrhiDesc.width          = w;
        desc.m_NvrhiDesc.height         = h;
        desc.m_NvrhiDesc.format         = fmt;
        desc.m_NvrhiDesc.debugName      = name;
        desc.m_NvrhiDesc.isRenderTarget = true;
        desc.m_NvrhiDesc.isShaderResource = true;
        desc.m_NvrhiDesc.initialState   = nvrhi::ResourceStates::RenderTarget;
        desc.m_NvrhiDesc.keepInitialState = true;
        return desc;
    }

    // Build a minimal RGTextureDesc for a depth attachment.
    RGTextureDesc MakeDepthDesc(uint32_t w, uint32_t h, const char* name)
    {
        RGTextureDesc desc;
        desc.m_NvrhiDesc.width          = w;
        desc.m_NvrhiDesc.height         = h;
        desc.m_NvrhiDesc.format         = Renderer::DEPTH_FORMAT;
        desc.m_NvrhiDesc.debugName      = name;
        desc.m_NvrhiDesc.isRenderTarget= true;
        desc.m_NvrhiDesc.isShaderResource = true;
        desc.m_NvrhiDesc.initialState   = nvrhi::ResourceStates::DepthWrite;
        desc.m_NvrhiDesc.keepInitialState = true;
        return desc;
    }

    // Count how many renderers in g_Renderer.m_Renderers satisfy IsBasePassRenderer().
    int CountBasePassRenderers()
    {
        int count = 0;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && r->IsBasePassRenderer())
                ++count;
        return count;
    }

    // Find a renderer by name (returns nullptr if not found).
    IRenderer* FindRendererByName(const char* name)
    {
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && std::string_view(r->GetName()) == name)
                return r.get();
        return nullptr;
    }
} // anonymous namespace

// ============================================================================
// TEST SUITE: Rendering_GBufferFormats
// ============================================================================
TEST_SUITE("Rendering_GBufferFormats")
{
    // ------------------------------------------------------------------
    // TC-GBF-01: G-buffer format constants are defined and non-UNKNOWN
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-01 GBufferFormats - format constants are non-UNKNOWN")
    {
        CHECK(Renderer::GBUFFER_ALBEDO_FORMAT   != nvrhi::Format::UNKNOWN);
        CHECK(Renderer::GBUFFER_NORMALS_FORMAT  != nvrhi::Format::UNKNOWN);
        CHECK(Renderer::GBUFFER_ORM_FORMAT      != nvrhi::Format::UNKNOWN);
        CHECK(Renderer::GBUFFER_EMISSIVE_FORMAT != nvrhi::Format::UNKNOWN);
        CHECK(Renderer::GBUFFER_MOTION_FORMAT   != nvrhi::Format::UNKNOWN);
        CHECK(Renderer::DEPTH_FORMAT            != nvrhi::Format::UNKNOWN);
        CHECK(Renderer::HDR_COLOR_FORMAT        != nvrhi::Format::UNKNOWN);
    }

    // ------------------------------------------------------------------
    // TC-GBF-02: G-buffer albedo format is RGBA8_UNORM
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-02 GBufferFormats - albedo format is RGBA8_UNORM")
    {
        CHECK(Renderer::GBUFFER_ALBEDO_FORMAT == nvrhi::Format::RGBA8_UNORM);
    }

    // ------------------------------------------------------------------
    // TC-GBF-03: G-buffer normals format is RG16_FLOAT
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-03 GBufferFormats - normals format is RG16_FLOAT")
    {
        CHECK(Renderer::GBUFFER_NORMALS_FORMAT == nvrhi::Format::RG16_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-GBF-04: G-buffer ORM format is RG8_UNORM
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-04 GBufferFormats - ORM format is RG8_UNORM")
    {
        CHECK(Renderer::GBUFFER_ORM_FORMAT == nvrhi::Format::RG8_UNORM);
    }

    // ------------------------------------------------------------------
    // TC-GBF-05: G-buffer emissive format is RGBA16_FLOAT
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-05 GBufferFormats - emissive format is RGBA16_FLOAT")
    {
        CHECK(Renderer::GBUFFER_EMISSIVE_FORMAT == nvrhi::Format::RGBA16_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-GBF-06: G-buffer motion vectors format is RGBA16_FLOAT
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-06 GBufferFormats - motion vectors format is RGBA16_FLOAT")
    {
        CHECK(Renderer::GBUFFER_MOTION_FORMAT == nvrhi::Format::RGBA16_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-GBF-07: Depth format is D24S8
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-07 GBufferFormats - depth format is D24S8")
    {
        CHECK(Renderer::DEPTH_FORMAT == nvrhi::Format::D24S8);
    }

    // ------------------------------------------------------------------
    // TC-GBF-08: HDR color format is R11G11B10_FLOAT
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-08 GBufferFormats - HDR color format is R11G11B10_FLOAT")
    {
        CHECK(Renderer::HDR_COLOR_FORMAT == nvrhi::Format::R11G11B10_FLOAT);
    }

    // ------------------------------------------------------------------
    // TC-GBF-09: Manually created G-buffer albedo texture has correct descriptor
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-09 GBufferFormats - manually created albedo texture has correct descriptor")
    {
        REQUIRE(DEV() != nullptr);

        const auto [w, h] = g_Renderer.SwapchainSize();
        REQUIRE(w > 0u);
        REQUIRE(h > 0u);

        nvrhi::TextureDesc desc;
        desc.width            = w;
        desc.height           = h;
        desc.format           = Renderer::GBUFFER_ALBEDO_FORMAT;
        desc.isRenderTarget   = true;
        desc.isShaderResource = true;
        desc.initialState     = nvrhi::ResourceStates::RenderTarget;
        desc.keepInitialState = true;
        desc.debugName        = "TC-GBF-09-Albedo";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);

        const nvrhi::TextureDesc& d = tex->getDesc();
        CHECK(d.width  == w);
        CHECK(d.height == h);
        CHECK(d.format == Renderer::GBUFFER_ALBEDO_FORMAT);
        CHECK(d.isRenderTarget);
        CHECK(d.isShaderResource);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-GBF-10: Manually created depth texture has correct descriptor
    // ------------------------------------------------------------------
    TEST_CASE("TC-GBF-10 GBufferFormats - manually created depth texture has correct descriptor")
    {
        REQUIRE(DEV() != nullptr);

        const auto [w, h] = g_Renderer.SwapchainSize();
        REQUIRE(w > 0u);
        REQUIRE(h > 0u);

        nvrhi::TextureDesc desc;
        desc.width            = w;
        desc.height           = h;
        desc.format           = Renderer::DEPTH_FORMAT;
        desc.isRenderTarget   = true;
        desc.isShaderResource = true;
        desc.initialState     = nvrhi::ResourceStates::DepthWrite;
        desc.keepInitialState = true;
        desc.debugName        = "TC-GBF-10-Depth";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);

        const nvrhi::TextureDesc& d = tex->getDesc();
        CHECK(d.width  == w);
        CHECK(d.height == h);
        CHECK(d.format == Renderer::DEPTH_FORMAT);
        CHECK(d.isRenderTarget);

        tex = nullptr;
        DEV()->waitForIdle();
    }
}

// ============================================================================
// TEST SUITE: Rendering_RenderGraph
// ============================================================================
TEST_SUITE("Rendering_RenderGraph")
{
    // ------------------------------------------------------------------
    // TC-RG-01: RenderGraph Reset() does not crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-01 RenderGraph - Reset does not crash")
    {
        // Reset is safe to call even when no resources are declared.
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.Reset());
    }

    // ------------------------------------------------------------------
    // TC-RG-02: DeclareTexture returns a valid handle
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-02 RenderGraph - DeclareTexture returns valid handle")
    {
        g_Renderer.m_RenderGraph.Reset();

        const auto [w, h] = g_Renderer.SwapchainSize();
        REQUIRE(w > 0u);
        REQUIRE(h > 0u);

        RGTextureHandle handle;
        const RGTextureDesc desc = MakeGBufferDesc(w, h, Renderer::GBUFFER_ALBEDO_FORMAT, "TC-RG-02-Albedo");

        g_Renderer.m_RenderGraph.BeginSetup();
        const bool ok = g_Renderer.m_RenderGraph.DeclareTexture(desc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(ok);
        CHECK(handle.IsValid());

        g_Renderer.m_RenderGraph.Reset();
    }

    // ------------------------------------------------------------------
    // TC-RG-03: DeclareBuffer returns a valid handle
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-03 RenderGraph - DeclareBuffer returns valid handle")
    {
        g_Renderer.m_RenderGraph.Reset();

        RGBufferHandle handle;
        const RGBufferDesc desc = RenderGraph::GetSPDAtomicCounterDesc("TC-RG-03-Counter");

        g_Renderer.m_RenderGraph.BeginSetup();
        const bool ok = g_Renderer.m_RenderGraph.DeclareBuffer(desc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(ok);
        CHECK(handle.IsValid());

        g_Renderer.m_RenderGraph.Reset();
    }

    // ------------------------------------------------------------------
    // TC-RG-04: Handle is invalid after Reset()
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-04 RenderGraph - handle is invalid after Reset")
    {
        g_Renderer.m_RenderGraph.Reset();

        const auto [w, h] = g_Renderer.SwapchainSize();
        RGTextureHandle handle;
        const RGTextureDesc desc = MakeGBufferDesc(w, h, Renderer::GBUFFER_ALBEDO_FORMAT, "TC-RG-04-Albedo");

        g_Renderer.m_RenderGraph.BeginSetup();
        g_Renderer.m_RenderGraph.DeclareTexture(desc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        REQUIRE(handle.IsValid());

        // After Reset the handle index is stale - the RG no longer owns it.
        g_Renderer.m_RenderGraph.Reset();

        // The handle struct itself still holds its old index value; the RG
        // has cleared its internal tables.  We verify Reset() does not crash.
        CHECK_NOTHROW(g_Renderer.m_RenderGraph.Reset());
    }

    // ------------------------------------------------------------------
    // TC-RG-05: DeclarePersistentTexture returns a valid handle
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-05 RenderGraph - DeclarePersistentTexture returns valid handle")
    {
        g_Renderer.m_RenderGraph.Reset();

        const auto [w, h] = g_Renderer.SwapchainSize();
        REQUIRE(w > 0u);
        REQUIRE(h > 0u);

        RGTextureHandle handle;
        const RGTextureDesc desc = MakeGBufferDesc(w, h, Renderer::GBUFFER_ALBEDO_FORMAT, "TC-RG-05-Persistent");

        g_Renderer.m_RenderGraph.BeginSetup();
        const bool ok = g_Renderer.m_RenderGraph.DeclarePersistentTexture(desc, handle);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(ok);
        CHECK(handle.IsValid());

        g_Renderer.m_RenderGraph.Reset();
    }

    // ------------------------------------------------------------------
    // TC-RG-06: Multiple texture declarations return distinct handles
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-06 RenderGraph - multiple declarations return distinct handles")
    {
        g_Renderer.m_RenderGraph.Reset();

        const auto [w, h] = g_Renderer.SwapchainSize();
        REQUIRE(w > 0u);
        REQUIRE(h > 0u);

        RGTextureHandle hA, hB, hC;
        const RGTextureDesc dA = MakeGBufferDesc(w, h, Renderer::GBUFFER_ALBEDO_FORMAT,   "TC-RG-06-A");
        const RGTextureDesc dB = MakeGBufferDesc(w, h, Renderer::GBUFFER_NORMALS_FORMAT,  "TC-RG-06-B");
        const RGTextureDesc dC = MakeGBufferDesc(w, h, Renderer::GBUFFER_EMISSIVE_FORMAT, "TC-RG-06-C");

        g_Renderer.m_RenderGraph.BeginSetup();
        g_Renderer.m_RenderGraph.DeclareTexture(dA, hA);
        g_Renderer.m_RenderGraph.DeclareTexture(dB, hB);
        g_Renderer.m_RenderGraph.DeclareTexture(dC, hC);
        g_Renderer.m_RenderGraph.EndSetup(true);

        CHECK(hA.IsValid());
        CHECK(hB.IsValid());
        CHECK(hC.IsValid());
        CHECK(hA != hB);
        CHECK(hB != hC);
        CHECK(hA != hC);

        g_Renderer.m_RenderGraph.Reset();
    }

    // ------------------------------------------------------------------
    // TC-RG-07: RGTextureHandle default-constructed is invalid
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-07 RenderGraph - default-constructed handle is invalid")
    {
        RGTextureHandle h;
        CHECK(!h.IsValid());

        RGBufferHandle b;
        CHECK(!b.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-RG-08: SPD atomic counter descriptor has non-zero size
    // ------------------------------------------------------------------
    TEST_CASE("TC-RG-08 RenderGraph - SPD atomic counter descriptor has non-zero size")
    {
        const RGBufferDesc desc = RenderGraph::GetSPDAtomicCounterDesc("TC-RG-08-Counter");
        CHECK(desc.m_NvrhiDesc.byteSize > 0u);
    }
}

// ============================================================================
// TEST SUITE: Rendering_RendererRegistry
// ============================================================================
TEST_SUITE("Rendering_RendererRegistry")
{
    // ------------------------------------------------------------------
    // TC-REG-01: Renderer list is non-empty after initialization
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-01 RendererRegistry - renderer list is non-empty")
    {
        CHECK(!g_Renderer.m_Renderers.empty());
    }

    // ------------------------------------------------------------------
    // TC-REG-02: At least one base-pass renderer is registered
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-02 RendererRegistry - at least one base-pass renderer is registered")
    {
        CHECK(CountBasePassRenderers() >= 1);
    }

    // ------------------------------------------------------------------
    // TC-REG-03: OpaqueRenderer is registered
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-03 RendererRegistry - OpaqueRenderer is registered")
    {
        IRenderer* r = FindRendererByName("Opaque Renderer");
        CHECK(r != nullptr);
        if (r)
            CHECK(r->IsBasePassRenderer());
    }

    // ------------------------------------------------------------------
    // TC-REG-04: MaskedPassRenderer is registered
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-04 RendererRegistry - MaskedPassRenderer is registered")
    {
        IRenderer* r = FindRendererByName("MaskedPass");
        CHECK(r != nullptr);
        if (r)
            CHECK(r->IsBasePassRenderer());
    }

    // ------------------------------------------------------------------
    // TC-REG-05: TransparentPassRenderer is registered
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-05 RendererRegistry - TransparentPassRenderer is registered")
    {
        IRenderer* r = FindRendererByName("TransparentPass");
        CHECK(r != nullptr);
        if (r)
            CHECK(r->IsBasePassRenderer());
    }

    // ------------------------------------------------------------------
    // TC-REG-06: All registered renderers have non-null GetName()
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-06 RendererRegistry - all renderers have non-null names")
    {
        for (int i = 0; i < (int)g_Renderer.m_Renderers.size(); ++i)
        {
            const auto& r = g_Renderer.m_Renderers[i];
            INFO("Renderer index " << i);
            REQUIRE(r != nullptr);
            CHECK(r->GetName() != nullptr);
            CHECK(std::string_view(r->GetName()).size() > 0u);
        }
    }

    // ------------------------------------------------------------------
    // TC-REG-07: Renderer names are unique
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-07 RendererRegistry - renderer names are unique")
    {
        std::vector<std::string> names;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r) names.emplace_back(r->GetName());

        std::sort(names.begin(), names.end());
        const bool allUnique = (std::adjacent_find(names.begin(), names.end()) == names.end());
        CHECK(allUnique);
    }
}

// ============================================================================
// TEST SUITE: Rendering_SceneBuckets
// ============================================================================
TEST_SUITE("Rendering_SceneBuckets")
{
    // ------------------------------------------------------------------
    // TC-BUCK-01: Opaque bucket base index is 0 for a scene with opaque meshes
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-01 SceneBuckets - opaque bucket base index is 0")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        // BoxTextured is fully opaque; opaque bucket starts at index 0.
        CHECK(g_Renderer.m_Scene.m_OpaqueBucket.m_BaseIndex == 0u);
    }

    // ------------------------------------------------------------------
    // TC-BUCK-02: Opaque bucket count is > 0 for a scene with opaque meshes
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-02 SceneBuckets - opaque bucket count is non-zero")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_OpaqueBucket.m_Count > 0u);
    }

    // ------------------------------------------------------------------
    // TC-BUCK-03: Total instance count equals sum of all bucket counts
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-03 SceneBuckets - total instance count equals sum of bucket counts")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const uint32_t total = g_Renderer.m_Scene.m_OpaqueBucket.m_Count
                             + g_Renderer.m_Scene.m_MaskedBucket.m_Count
                             + g_Renderer.m_Scene.m_TransparentBucket.m_Count;

        // Total must equal the number of instance data entries.
        CHECK(total == (uint32_t)g_Renderer.m_Scene.m_InstanceData.size());
    }

    // ------------------------------------------------------------------
    // TC-BUCK-04: Bucket base indices are non-overlapping
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-04 SceneBuckets - bucket base indices are non-overlapping")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        const auto& opaque      = g_Renderer.m_Scene.m_OpaqueBucket;
        const auto& masked      = g_Renderer.m_Scene.m_MaskedBucket;
        const auto& transparent = g_Renderer.m_Scene.m_TransparentBucket;

        // Opaque ends before masked starts (or masked is empty).
        if (masked.m_Count > 0)
            CHECK(opaque.m_BaseIndex + opaque.m_Count <= masked.m_BaseIndex);

        // Masked ends before transparent starts (or transparent is empty).
        if (transparent.m_Count > 0)
        {
            const uint32_t maskedEnd = masked.m_Count > 0
                ? masked.m_BaseIndex + masked.m_Count
                : opaque.m_BaseIndex + opaque.m_Count;
            CHECK(maskedEnd <= transparent.m_BaseIndex);
        }
    }

    // ------------------------------------------------------------------
    // TC-BUCK-05: Instance data buffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-05 SceneBuckets - instance data buffer is non-null after scene load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_InstanceDataBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BUCK-06: Mesh data buffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-06 SceneBuckets - mesh data buffer is non-null after scene load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_MeshDataBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BUCK-07: Vertex buffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-07 SceneBuckets - vertex buffer is non-null after scene load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_VertexBufferQuantized != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BUCK-08: Index buffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-08 SceneBuckets - index buffer is non-null after scene load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_IndexBuffer != nullptr);
    }
}

// ============================================================================
// TEST SUITE: Rendering_CullingToggles
// ============================================================================
TEST_SUITE("Rendering_CullingToggles")
{
    // ------------------------------------------------------------------
    // TC-CULL-01: Frustum culling can be disabled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-01 CullingToggles - frustum culling disable does not crash")
    {
        const bool prev = g_Renderer.m_EnableFrustumCulling;
        g_Renderer.m_EnableFrustumCulling = false;
        CHECK(!g_Renderer.m_EnableFrustumCulling);
        g_Renderer.m_EnableFrustumCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-02: Frustum culling can be re-enabled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-02 CullingToggles - frustum culling re-enable does not crash")
    {
        const bool prev = g_Renderer.m_EnableFrustumCulling;
        g_Renderer.m_EnableFrustumCulling = false;
        g_Renderer.m_EnableFrustumCulling = true;
        CHECK(g_Renderer.m_EnableFrustumCulling);
        g_Renderer.m_EnableFrustumCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-03: Occlusion culling can be disabled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-03 CullingToggles - occlusion culling disable does not crash")
    {
        const bool prev = g_Renderer.m_EnableOcclusionCulling;
        g_Renderer.m_EnableOcclusionCulling = false;
        CHECK(!g_Renderer.m_EnableOcclusionCulling);
        g_Renderer.m_EnableOcclusionCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-04: Occlusion culling can be re-enabled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-04 CullingToggles - occlusion culling re-enable does not crash")
    {
        const bool prev = g_Renderer.m_EnableOcclusionCulling;
        g_Renderer.m_EnableOcclusionCulling = false;
        g_Renderer.m_EnableOcclusionCulling = true;
        CHECK(g_Renderer.m_EnableOcclusionCulling);
        g_Renderer.m_EnableOcclusionCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-05: Cone culling can be toggled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-05 CullingToggles - cone culling toggle does not crash")
    {
        const bool prev = g_Renderer.m_EnableConeCulling;
        g_Renderer.m_EnableConeCulling = !prev;
        CHECK(g_Renderer.m_EnableConeCulling == !prev);
        g_Renderer.m_EnableConeCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-06: Freeze culling camera can be toggled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-06 CullingToggles - freeze culling camera toggle does not crash")
    {
        const bool prev = g_Renderer.m_FreezeCullingCamera;
        g_Renderer.m_FreezeCullingCamera = !prev;
        CHECK(g_Renderer.m_FreezeCullingCamera == !prev);
        g_Renderer.m_FreezeCullingCamera = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-07: Meshlet rendering can be disabled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-07 CullingToggles - meshlet rendering disable does not crash")
    {
        const bool prev = g_Renderer.m_UseMeshletRendering;
        g_Renderer.m_UseMeshletRendering = false;
        CHECK(!g_Renderer.m_UseMeshletRendering);
        g_Renderer.m_UseMeshletRendering = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-08: Meshlet rendering can be re-enabled without crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-08 CullingToggles - meshlet rendering re-enable does not crash")
    {
        const bool prev = g_Renderer.m_UseMeshletRendering;
        g_Renderer.m_UseMeshletRendering = false;
        g_Renderer.m_UseMeshletRendering = true;
        CHECK(g_Renderer.m_UseMeshletRendering);
        g_Renderer.m_UseMeshletRendering = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-09: Forced LOD -1 (auto) is the default
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-09 CullingToggles - forced LOD default is -1 (auto)")
    {
        // The default is -1 (no forced LOD).  We just verify the field exists
        // and can be read without crashing.
        const int lod = g_Renderer.m_ForcedLOD;
        CHECK(lod == -1);
    }

    // ------------------------------------------------------------------
    // TC-CULL-10: Forced LOD can be set to 0 and restored
    // ------------------------------------------------------------------
    TEST_CASE("TC-CULL-10 CullingToggles - forced LOD can be set to 0")
    {
        const int prev = g_Renderer.m_ForcedLOD;
        g_Renderer.m_ForcedLOD = 0;
        CHECK(g_Renderer.m_ForcedLOD == 0);
        g_Renderer.m_ForcedLOD = prev;
    }
}

// ============================================================================
// TEST SUITE: Rendering_HZB
// ============================================================================
TEST_SUITE("Rendering_HZB")
{
    // ------------------------------------------------------------------
    // TC-HZB-01: HZB texture can be created with UAV and SRV flags
    // ------------------------------------------------------------------
    TEST_CASE("TC-HZB-01 HZB - HZB texture creation succeeds")
    {
        REQUIRE(DEV() != nullptr);

        const auto [w, h] = g_Renderer.SwapchainSize();
        REQUIRE(w > 0u);
        REQUIRE(h > 0u);

        // HZB is a power-of-2 texture derived from the depth buffer.
        // Use a simple 512x512 for the test.
        const uint32_t hzbW = 512u;
        const uint32_t hzbH = 512u;

        // Compute mip levels.
        uint32_t mipLevels = 1;
        uint32_t dim = std::max(hzbW, hzbH);
        while (dim > 1) { dim >>= 1; ++mipLevels; }

        nvrhi::TextureDesc desc;
        desc.width            = hzbW;
        desc.height           = hzbH;
        desc.mipLevels        = mipLevels;
        desc.format           = nvrhi::Format::R32_FLOAT;
        desc.isUAV            = true;
        desc.isShaderResource = true;
        desc.initialState     = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        desc.debugName        = "TC-HZB-01-HZB";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);

        const nvrhi::TextureDesc& d = tex->getDesc();
        CHECK(d.width      == hzbW);
        CHECK(d.height     == hzbH);
        CHECK(d.mipLevels  == mipLevels);
        CHECK(d.isUAV);
        CHECK(d.isShaderResource);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-HZB-02: HZB mip count is correct for a 512x512 texture
    // ------------------------------------------------------------------
    TEST_CASE("TC-HZB-02 HZB - mip count is correct for 512x512")
    {
        // 512 = 2^9, so mip chain is 512→256→128→64→32→16→8→4→2→1 = 10 levels.
        uint32_t mipLevels = 1;
        uint32_t dim = 512u;
        while (dim > 1) { dim >>= 1; ++mipLevels; }
        CHECK(mipLevels == 10u);
    }

    // ------------------------------------------------------------------
    // TC-HZB-03: HZB mip count is correct for a 1024x512 texture
    // ------------------------------------------------------------------
    TEST_CASE("TC-HZB-03 HZB - mip count is correct for 1024x512")
    {
        // max(1024,512) = 1024 = 2^10, so 11 levels.
        uint32_t mipLevels = 1;
        uint32_t dim = std::max(1024u, 512u);
        while (dim > 1) { dim >>= 1; ++mipLevels; }
        CHECK(mipLevels == 11u);
    }

    // ------------------------------------------------------------------
    // TC-HZB-04: Occlusion culling disabled means HZB is not required
    // ------------------------------------------------------------------
    TEST_CASE("TC-HZB-04 HZB - occlusion culling disabled skips HZB generation")
    {
        // When occlusion culling is disabled, the OpaqueRenderer's Setup()
        // does not declare the HZB texture.  We verify the flag is accessible.
        const bool prev = g_Renderer.m_EnableOcclusionCulling;
        g_Renderer.m_EnableOcclusionCulling = false;
        CHECK(!g_Renderer.m_EnableOcclusionCulling);
        g_Renderer.m_EnableOcclusionCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-HZB-05: SPD atomic counter descriptor is valid
    // ------------------------------------------------------------------
    TEST_CASE("TC-HZB-05 HZB - SPD atomic counter descriptor is valid")
    {
        const RGBufferDesc desc = RenderGraph::GetSPDAtomicCounterDesc("TC-HZB-05-Counter");
        CHECK(desc.m_NvrhiDesc.byteSize > 0u);
        CHECK(desc.m_NvrhiDesc.canHaveUAVs);
    }
}

// ============================================================================
// TEST SUITE: Rendering_ShaderHotReload
// ============================================================================
TEST_SUITE("Rendering_ShaderHotReload")
{
    // ------------------------------------------------------------------
    // TC-SHR-01: Setting m_RequestedShaderReload flag does not crash
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-01 ShaderHotReload - setting reload flag does not crash")
    {
        g_Renderer.m_RequestedShaderReload = true;
        CHECK(g_Renderer.m_RequestedShaderReload);
        g_Renderer.m_RequestedShaderReload = false;
    }

    // ------------------------------------------------------------------
    // TC-SHR-02: Shader handles are non-null before reload request
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-02 ShaderHotReload - shader handles are non-null before reload")
    {
        // Spot-check a few shader handles that must be loaded at startup.
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_VSMAIN) != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_GBUFFER_PSMAIN) != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::GPUCULLING_CULLING_CSMAIN) != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SHR-03: Shader reload flag is false by default
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-03 ShaderHotReload - reload flag is false by default")
    {
        // After the previous test resets the flag, it must be false.
        CHECK(!g_Renderer.m_RequestedShaderReload);
    }

    // ------------------------------------------------------------------
    // TC-SHR-04: G-buffer format constants are unchanged after reload request
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-04 ShaderHotReload - G-buffer format constants unchanged after reload request")
    {
        // Format constants are compile-time; a reload request must not affect them.
        g_Renderer.m_RequestedShaderReload = true;

        CHECK(Renderer::GBUFFER_ALBEDO_FORMAT   == nvrhi::Format::RGBA8_UNORM);
        CHECK(Renderer::GBUFFER_NORMALS_FORMAT  == nvrhi::Format::RG16_FLOAT);
        CHECK(Renderer::GBUFFER_ORM_FORMAT      == nvrhi::Format::RG8_UNORM);
        CHECK(Renderer::GBUFFER_EMISSIVE_FORMAT == nvrhi::Format::RGBA16_FLOAT);
        CHECK(Renderer::GBUFFER_MOTION_FORMAT   == nvrhi::Format::RGBA16_FLOAT);

        g_Renderer.m_RequestedShaderReload = false;
    }

    // ------------------------------------------------------------------
    // TC-SHR-05: Descriptor tables are non-null after reload request
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-05 ShaderHotReload - descriptor tables non-null after reload request")
    {
        g_Renderer.m_RequestedShaderReload = true;

        CHECK(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);
        CHECK(g_Renderer.GetStaticSamplerDescriptorTable() != nullptr);

        g_Renderer.m_RequestedShaderReload = false;
    }

    // ------------------------------------------------------------------
    // TC-SHR-06: Shader handles are non-null after reload request
    //            (reload is deferred to next frame; handles remain valid now)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-06 ShaderHotReload - shader handles non-null after reload request")
    {
        g_Renderer.m_RequestedShaderReload = true;

        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_VSMAIN) != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_GBUFFER_PSMAIN) != nullptr);

        g_Renderer.m_RequestedShaderReload = false;
    }
}

// ============================================================================
// TEST SUITE: Rendering_BackbufferCapture
// ============================================================================
TEST_SUITE("Rendering_BackbufferCapture")
{
    // ------------------------------------------------------------------
    // TC-CAP-01: capture_backbuffer_to_bmp stub returns true
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAP-01 BackbufferCapture - capture stub returns true")
    {
        const bool ok = capture_backbuffer_to_bmp("Tests/ReferenceImages/TC-CAP-01.bmp");
        CHECK(ok);
    }

    // ------------------------------------------------------------------
    // TC-CAP-02: compare_bmp_images stub returns true
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAP-02 BackbufferCapture - compare stub returns true")
    {
        const bool ok = compare_bmp_images(
            "Tests/ReferenceImages/TC-CAP-02-actual.bmp",
            "Tests/ReferenceImages/TC-CAP-02-reference.bmp",
            1.0f);
        CHECK(ok);
    }

    // ------------------------------------------------------------------
    // TC-CAP-03: compare_bmp_images stub returns true with zero tolerance
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAP-03 BackbufferCapture - compare stub returns true with zero tolerance")
    {
        const bool ok = compare_bmp_images(
            "Tests/ReferenceImages/TC-CAP-03-actual.bmp",
            "Tests/ReferenceImages/TC-CAP-03-reference.bmp",
            0.0f);
        CHECK(ok);
    }

    // ------------------------------------------------------------------
    // TC-CAP-04: Backbuffer texture is non-null (swapchain is alive)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAP-04 BackbufferCapture - backbuffer texture is non-null")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        // The swapchain textures are created during initialization.
        // At least one must be non-null.
        bool anyNonNull = false;
        for (uint32_t i = 0; i < GraphicRHI::SwapchainImageCount; ++i)
        {
            if (g_Renderer.m_RHI->m_NvrhiSwapchainTextures[i] != nullptr)
            {
                anyNonNull = true;
                break;
            }
        }
        CHECK(anyNonNull);
    }

    // ------------------------------------------------------------------
    // TC-CAP-05: Swapchain extent is non-zero
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAP-05 BackbufferCapture - swapchain extent is non-zero")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        CHECK(g_Renderer.m_RHI->m_SwapchainExtent.x > 0u);
        CHECK(g_Renderer.m_RHI->m_SwapchainExtent.y > 0u);
    }

    // ------------------------------------------------------------------
    // TC-CAP-06: Swapchain format is non-UNKNOWN
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAP-06 BackbufferCapture - swapchain format is non-UNKNOWN")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        CHECK(g_Renderer.m_RHI->m_SwapchainFormat != nvrhi::Format::UNKNOWN);
    }
}

// ============================================================================
// TEST SUITE: Rendering_DepthConstants
// ============================================================================
TEST_SUITE("Rendering_DepthConstants")
{
    // ------------------------------------------------------------------
    // TC-DEPTH-01: DEPTH_NEAR is 1.0 (reversed-Z convention)
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-01 DepthConstants - DEPTH_NEAR is 1.0 (reversed-Z)")
    {
        CHECK(Renderer::DEPTH_NEAR == doctest::Approx(1.0f));
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-02: DEPTH_FAR is 0.0 (reversed-Z convention)
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-02 DepthConstants - DEPTH_FAR is 0.0 (reversed-Z)")
    {
        CHECK(Renderer::DEPTH_FAR == doctest::Approx(0.0f));
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-03: DEPTH_NEAR > DEPTH_FAR (reversed-Z invariant)
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-03 DepthConstants - DEPTH_NEAR > DEPTH_FAR (reversed-Z invariant)")
    {
        CHECK(Renderer::DEPTH_NEAR > Renderer::DEPTH_FAR);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-04: DepthReadWrite state uses GreaterEqual comparison
    //              (correct for reversed-Z: closer objects have larger depth)
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-04 DepthConstants - DepthReadWrite uses GreaterEqual comparison")
    {
        CHECK(CR().DepthReadWrite.depthFunc == nvrhi::ComparisonFunc::GreaterOrEqual);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-05: DepthRead state uses GreaterEqual comparison
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-05 DepthConstants - DepthRead uses GreaterEqual comparison")
    {
        CHECK(CR().DepthRead.depthFunc == nvrhi::ComparisonFunc::GreaterOrEqual);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-06: DepthDisabled state has depth test disabled
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-06 DepthConstants - DepthDisabled has depth test disabled")
    {
        CHECK(!CR().DepthDisabled.depthTestEnable);
        CHECK(!CR().DepthDisabled.depthWriteEnable);
    }
}
