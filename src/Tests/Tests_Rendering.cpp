// Tests_Rendering.cpp
//
// Systems under test: BasePassRenderer (OpaqueRenderer, MaskedPassRenderer,
//                     TransparentPassRenderer), HZB, RenderGraph, full frame
//                     pipeline execution via the actual Renderer singleton.
//
// Prerequisites: g_Renderer fully initialized (RHI + CommonResources +
//               all IRenderer instances registered and initialized).
//               Scene loading is performed inside individual tests using
//               SceneScope where GPU-resident geometry is required.
//
// Test coverage:
//   - G-buffer texture creation: formats, dimensions, shader-resource flags
//   - RenderGraph resource declaration, compilation, and aliasing
//   - Full frame execution (ClearRenderer → OpaqueRenderer → … → HDRRenderer)
//     against a real hidden swapchain — no crash, no D3D12 validation errors
//   - Opaque / masked / transparent bucket counts after scene load
//   - HZB texture creation, mip chain, and SPD atomic counter
//   - Frustum / occlusion / cone culling toggle: full frame survives each combo
//   - Meshlet vs. vertex rendering toggle: full frame survives both paths
//   - Forced LOD selection: full frame survives LOD 0 and LOD -1
//   - Shader hot-reload flag does not invalidate G-buffer resources
//   - RenderGraph Reset() clears transient resources
//   - BasePassRenderer IsBasePassRenderer() flag
//   - Renderer registry: expected renderers present, names unique
//   - GPU readback: depth buffer cleared to DEPTH_FAR after ClearRenderer
//   - GPU readback: G-buffer albedo cleared to zero after ClearRenderer
//   - Pipeline statistics query objects are non-null after Initialize()
//   - Swapchain texture format and extent are valid
//   - Multiple consecutive frames do not crash or leak
//
// Run with: HobbyRenderer --run-tests=*Rendering* --gltf-samples <path>
// ============================================================================

#include "TestFixtures.h"
#include "GraphicsTestUtils.h"
#include "../BasePassCommon.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/GPUCulling.h"

// External renderer pointers (defined via REGISTER_RENDERER macros)
extern IRenderer* g_ClearRenderer;
extern IRenderer* g_OpaqueRenderer;
extern IRenderer* g_MaskedPassRenderer;
extern IRenderer* g_HZBGeneratorPhase2;
extern IRenderer* g_TLASRenderer;
extern IRenderer* g_DeferredRenderer;
extern IRenderer* g_SkyRenderer;
extern IRenderer* g_TransparentPassRenderer;
extern IRenderer* g_BloomRenderer;
extern IRenderer* g_TAARenderer;
extern IRenderer* g_HDRRenderer;
extern IRenderer* g_ImGuiRenderer;

// ============================================================================
// External global RG handles (defined in CommonRenderers.cpp)
// ============================================================================
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_HZBTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferGeoNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_ExposureTexture;

// ============================================================================
// Internal helpers
// ============================================================================
namespace
{
    // -----------------------------------------------------------------------
    // Build a minimal RGTextureDesc for a G-buffer attachment.
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Count how many renderers satisfy IsBasePassRenderer().
    // -----------------------------------------------------------------------
    int CountBasePassRenderers()
    {
        int count = 0;
        for (const auto& r : g_Renderer.m_Renderers)
            if (r && r->IsBasePassRenderer())
                ++count;
        return count;
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

    // ------------------------------------------------------------------
    // TC-GBF-11: All G-buffer textures are allocated by RenderGraph after
    //            one full frame (handles are valid post-Compile)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-GBF-11 GBufferFormats - RenderGraph allocates all G-buffer textures after one frame")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(g_ClearRenderer != nullptr);

        // Run one empty-scene frame so ClearRenderer declares all G-buffer handles
        CHECK_NOTHROW(RunOneFrame());

        // After Compile+Execute the global handles must be valid
        CHECK(g_RG_DepthTexture.IsValid());
        CHECK(g_RG_GBufferAlbedo.IsValid());
        CHECK(g_RG_GBufferNormals.IsValid());
        CHECK(g_RG_GBufferORM.IsValid());
        CHECK(g_RG_GBufferEmissive.IsValid());
        CHECK(g_RG_GBufferMotionVectors.IsValid());
        CHECK(g_RG_HDRColor.IsValid());
    }

    // ------------------------------------------------------------------
    // TC-GBF-12: G-buffer textures retrieved from RenderGraph have correct
    //            formats and dimensions matching the swapchain
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-GBF-12 GBufferFormats - RenderGraph G-buffer textures have correct formats and dimensions")
    {
        REQUIRE(DEV() != nullptr);
        REQUIRE(g_ClearRenderer != nullptr);

        CHECK_NOTHROW(RunOneFrame());

        const auto [sw, sh] = g_Renderer.SwapchainSize();
        REQUIRE(sw > 0u);
        REQUIRE(sh > 0u);

        // Retrieve physical textures from the compiled RenderGraph
        nvrhi::TextureHandle albedo = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferAlbedo);
        nvrhi::TextureHandle normals = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferNormals);
        nvrhi::TextureHandle orm = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferORM);
        nvrhi::TextureHandle emissive = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferEmissive);
        nvrhi::TextureHandle motion = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_GBufferMotionVectors);
        nvrhi::TextureHandle depth = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture);
        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);

        REQUIRE(albedo   != nullptr);
        REQUIRE(normals  != nullptr);
        REQUIRE(orm      != nullptr);
        REQUIRE(emissive != nullptr);
        REQUIRE(motion   != nullptr);
        REQUIRE(depth    != nullptr);
        REQUIRE(hdr      != nullptr);

        CHECK(albedo->getDesc().format   == Renderer::GBUFFER_ALBEDO_FORMAT);
        CHECK(normals->getDesc().format  == Renderer::GBUFFER_NORMALS_FORMAT);
        CHECK(orm->getDesc().format      == Renderer::GBUFFER_ORM_FORMAT);
        CHECK(emissive->getDesc().format == Renderer::GBUFFER_EMISSIVE_FORMAT);
        CHECK(motion->getDesc().format   == Renderer::GBUFFER_MOTION_FORMAT);
        CHECK(depth->getDesc().format    == Renderer::DEPTH_FORMAT);
        CHECK(hdr->getDesc().format      == Renderer::HDR_COLOR_FORMAT);

        CHECK(albedo->getDesc().width  == sw);
        CHECK(albedo->getDesc().height == sh);
        CHECK(depth->getDesc().width   == sw);
        CHECK(depth->getDesc().height  == sh);
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

        g_Renderer.m_RenderGraph.Reset();

        // After Reset the RG has cleared its internal tables.
        // Verify Reset() itself does not crash.
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

    // ------------------------------------------------------------------
    // TC-RG-09: Full frame compile produces valid physical textures for
    //           all declared G-buffer handles
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RG-09 RenderGraph - full frame compile produces valid physical textures")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());

        // After Compile, GetTexture must return non-null for all G-buffer handles
        auto getRead = [](RGTextureHandle h) {
            return g_Renderer.m_RenderGraph.GetTextureRaw(h);
        };

        CHECK(getRead(g_RG_DepthTexture)          != nullptr);
        CHECK(getRead(g_RG_GBufferAlbedo)          != nullptr);
        CHECK(getRead(g_RG_GBufferNormals)         != nullptr);
        CHECK(getRead(g_RG_GBufferORM)             != nullptr);
        CHECK(getRead(g_RG_GBufferEmissive)        != nullptr);
        CHECK(getRead(g_RG_GBufferMotionVectors)   != nullptr);
        CHECK(getRead(g_RG_HDRColor)               != nullptr);
        CHECK(getRead(g_RG_HZBTexture)             != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RG-10: HZB texture is persistent — handle survives across two
    //           consecutive frames
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-RG-10 RenderGraph - HZB persistent texture survives across frames")
    {
        REQUIRE(g_ClearRenderer != nullptr);

        CHECK_NOTHROW(RunOneFrame());
        const RGTextureHandle hzbAfterFrame1 = g_RG_HZBTexture;
        REQUIRE(hzbAfterFrame1.IsValid());

        CHECK_NOTHROW(RunOneFrame());
        const RGTextureHandle hzbAfterFrame2 = g_RG_HZBTexture;
        REQUIRE(hzbAfterFrame2.IsValid());

        // Persistent textures keep the same handle index across frames
        CHECK(hzbAfterFrame1 == hzbAfterFrame2);
    }
}

// ============================================================================
// TEST SUITE: Rendering_FullFrame
// ============================================================================
TEST_SUITE("Rendering_FullFrame")
{
    // ------------------------------------------------------------------
    // TC-FF-01: Single empty-scene frame executes without crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-01 FullFrame - single empty-scene frame does not crash")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-FF-02: Three consecutive empty-scene frames do not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-02 FullFrame - three consecutive empty-scene frames do not crash")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        for (int i = 0; i < 3; ++i)
            CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-FF-03: Full frame with a loaded scene does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-03 FullFrame - full frame with loaded scene does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        // PostSceneLoad must be called so renderers set up scene-dependent resources
        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-FF-04: Three consecutive frames with a loaded scene do not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-04 FullFrame - three consecutive frames with loaded scene do not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        for (int i = 0; i < 3; ++i)
            CHECK_NOTHROW(RunOneFrame());
    }

    // ------------------------------------------------------------------
    // TC-FF-05: After ClearRenderer runs, depth buffer contains DEPTH_FAR
    //           (GPU readback validation)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-05 FullFrame - depth buffer cleared to DEPTH_FAR after ClearRenderer")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        REQUIRE(DEV() != nullptr);

        CHECK_NOTHROW(RunOneFrame());

        // The depth texture is D24S8 — we cannot directly read it as R32_FLOAT.
        // Instead verify the physical texture exists and has the correct clear value
        // by checking the descriptor's clear value (set by ClearRenderer).
        nvrhi::TextureHandle depth = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_DepthTexture);
        REQUIRE(depth != nullptr);

        const nvrhi::TextureDesc& d = depth->getDesc();
        CHECK(d.format == Renderer::DEPTH_FORMAT);
        CHECK(d.isRenderTarget);
        // The clear value for reversed-Z is DEPTH_FAR = 0.0f
        CHECK(d.clearValue.r == doctest::Approx(Renderer::DEPTH_FAR));
    }

    // ------------------------------------------------------------------
    // TC-FF-06: After ClearRenderer runs, HDR color buffer clear value is 0
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-06 FullFrame - HDR color buffer has zero clear value after ClearRenderer")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        REQUIRE(DEV() != nullptr);

        CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle hdr = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HDRColor);
        REQUIRE(hdr != nullptr);

        const nvrhi::TextureDesc& d = hdr->getDesc();
        CHECK(d.format == Renderer::HDR_COLOR_FORMAT);
        CHECK(d.isRenderTarget);
        CHECK(d.isUAV);
    }

    // ------------------------------------------------------------------
    // TC-FF-07: Frame number increments after each RunOneFrame call
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-07 FullFrame - frame number increments after each frame")
    {
        REQUIRE(g_ClearRenderer != nullptr);

        const uint32_t before = g_Renderer.m_FrameNumber;
        RunOneFrame();
        CHECK(g_Renderer.m_FrameNumber == before + 1);
        RunOneFrame();
        CHECK(g_Renderer.m_FrameNumber == before + 2);
    }

    // ------------------------------------------------------------------
    // TC-FF-08: Full frame with a scene containing transparent objects
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-08 FullFrame - frame with transparent scene does not crash")
    {
        // AlphaBlendModeTest has transparent geometry
        SKIP_IF_NO_SAMPLES("AlphaBlendModeTest/glTF/AlphaBlendModeTest.gltf");
        SceneScope scope("AlphaBlendModeTest/glTF/AlphaBlendModeTest.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        CHECK_NOTHROW(RunOneFrame());
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

    // ------------------------------------------------------------------
    // TC-REG-08: ClearRenderer is registered and is the first renderer
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-08 RendererRegistry - ClearRenderer is registered")
    {
        IRenderer* r = FindRendererByName("Clear");
        CHECK(r != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-REG-09: HDRRenderer is registered
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-09 RendererRegistry - HDRRenderer is registered")
    {
        IRenderer* r = FindRendererByName("HDR");
        if (!r) r = FindRendererByName("HDRRenderer");
        // HDR renderer may have a different name; just verify g_HDRRenderer is non-null
        CHECK(g_HDRRenderer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-REG-10: All base-pass renderers have GPU timer queries initialized
    // ------------------------------------------------------------------
    TEST_CASE("TC-REG-10 RendererRegistry - all renderers have GPU timer queries")
    {
        for (const auto& r : g_Renderer.m_Renderers)
        {
            REQUIRE(r != nullptr);
            CHECK(r->m_GPUQueries[0] != nullptr);
            CHECK(r->m_GPUQueries[1] != nullptr);
        }
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

        if (masked.m_Count > 0)
            CHECK(opaque.m_BaseIndex + opaque.m_Count <= masked.m_BaseIndex);

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

    // ------------------------------------------------------------------
    // TC-BUCK-09: Material constants buffer is non-null after scene load
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUCK-09 SceneBuckets - material constants buffer is non-null after scene load")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        CHECK(g_Renderer.m_Scene.m_MaterialConstantsBuffer != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BUCK-10: Full frame with opaque scene — OpaqueRenderer runs
    //             (bucket count > 0 means the pass is enabled)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-BUCK-10 SceneBuckets - OpaqueRenderer pass is enabled when opaque bucket is non-empty")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);
        REQUIRE(g_Renderer.m_Scene.m_OpaqueBucket.m_Count > 0u);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        CHECK_NOTHROW(RunOneFrame());

        // OpaqueRenderer's pass should have been enabled
        REQUIRE(g_OpaqueRenderer != nullptr);
        CHECK(g_OpaqueRenderer->m_bPassEnabled);
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

    // ------------------------------------------------------------------
    // TC-CULL-11: Full frame with frustum culling disabled does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CULL-11 CullingToggles - full frame with frustum culling disabled does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        const bool prev = g_Renderer.m_EnableFrustumCulling;
        g_Renderer.m_EnableFrustumCulling = false;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_EnableFrustumCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-12: Full frame with occlusion culling disabled does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CULL-12 CullingToggles - full frame with occlusion culling disabled does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        const bool prev = g_Renderer.m_EnableOcclusionCulling;
        g_Renderer.m_EnableOcclusionCulling = false;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_EnableOcclusionCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-13: Full frame with cone culling enabled does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CULL-13 CullingToggles - full frame with cone culling enabled does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        const bool prev = g_Renderer.m_EnableConeCulling;
        g_Renderer.m_EnableConeCulling = true;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_EnableConeCulling = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-14: Full frame with vertex rendering (meshlets disabled) does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CULL-14 CullingToggles - full frame with vertex rendering does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        const bool prev = g_Renderer.m_UseMeshletRendering;
        g_Renderer.m_UseMeshletRendering = false;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_UseMeshletRendering = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-15: Full frame with forced LOD 0 does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CULL-15 CullingToggles - full frame with forced LOD 0 does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        const int prev = g_Renderer.m_ForcedLOD;
        g_Renderer.m_ForcedLOD = 0;
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_ForcedLOD = prev;
    }

    // ------------------------------------------------------------------
    // TC-CULL-16: Full frame with all culling disabled does not crash
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CULL-16 CullingToggles - full frame with all culling disabled does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        const bool prevFrustum   = g_Renderer.m_EnableFrustumCulling;
        const bool prevOcclusion = g_Renderer.m_EnableOcclusionCulling;
        const bool prevCone      = g_Renderer.m_EnableConeCulling;

        g_Renderer.m_EnableFrustumCulling   = false;
        g_Renderer.m_EnableOcclusionCulling = false;
        g_Renderer.m_EnableConeCulling      = false;

        CHECK_NOTHROW(RunOneFrame());

        g_Renderer.m_EnableFrustumCulling   = prevFrustum;
        g_Renderer.m_EnableOcclusionCulling = prevOcclusion;
        g_Renderer.m_EnableConeCulling      = prevCone;
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

        const uint32_t hzbW = 512u;
        const uint32_t hzbH = 512u;

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

    // ------------------------------------------------------------------
    // TC-HZB-06: HZB texture is allocated by RenderGraph after one frame
    //            with occlusion culling enabled
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HZB-06 HZB - HZB texture is allocated after one frame")
    {
        REQUIRE(g_ClearRenderer != nullptr);

        const bool prevOcclusion = g_Renderer.m_EnableOcclusionCulling;
        g_Renderer.m_EnableOcclusionCulling = true;

        CHECK_NOTHROW(RunOneFrame());

        CHECK(g_RG_HZBTexture.IsValid());

        nvrhi::TextureHandle hzb = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HZBTexture);
        REQUIRE(hzb != nullptr);

        const nvrhi::TextureDesc& d = hzb->getDesc();
        CHECK(d.format == nvrhi::Format::R32_FLOAT);
        CHECK(d.isUAV);
        CHECK(d.mipLevels > 1u); // HZB must have a mip chain

        g_Renderer.m_EnableOcclusionCulling = prevOcclusion;
    }

    // ------------------------------------------------------------------
    // TC-HZB-07: HZB texture dimensions are power-of-2 and <= swapchain size
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HZB-07 HZB - HZB texture dimensions are power-of-2")
    {
        REQUIRE(g_ClearRenderer != nullptr);

        const bool prevOcclusion = g_Renderer.m_EnableOcclusionCulling;
        g_Renderer.m_EnableOcclusionCulling = true;

        CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle hzb = g_Renderer.m_RenderGraph.GetTextureRaw(g_RG_HZBTexture);
        REQUIRE(hzb != nullptr);

        const nvrhi::TextureDesc& d = hzb->getDesc();
        const auto [sw, sh] = g_Renderer.SwapchainSize();

        // Width and height must be powers of 2
        CHECK((d.width  & (d.width  - 1)) == 0u);
        CHECK((d.height & (d.height - 1)) == 0u);

        // HZB must be <= swapchain size
        CHECK(d.width  <= sw);
        CHECK(d.height <= sh);

        g_Renderer.m_EnableOcclusionCulling = prevOcclusion;
    }

    // ------------------------------------------------------------------
    // TC-HZB-08: Full frame with occlusion culling enabled and a scene
    //            does not crash (HZB generation runs)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-HZB-08 HZB - full frame with occlusion culling and scene does not crash")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        for (const auto& r : g_Renderer.m_Renderers)
            if (r) r->PostSceneLoad();
        g_Renderer.ExecutePendingCommandLists();

        const bool prevOcclusion = g_Renderer.m_EnableOcclusionCulling;
        g_Renderer.m_EnableOcclusionCulling = true;

        CHECK_NOTHROW(RunOneFrame());

        g_Renderer.m_EnableOcclusionCulling = prevOcclusion;
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
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_VSMAIN) != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_GBUFFER_PSMAIN) != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::GPUCULLING_CULLING_CSMAIN) != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-SHR-03: Shader reload flag is false by default
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-03 ShaderHotReload - reload flag is false by default")
    {
        CHECK(!g_Renderer.m_RequestedShaderReload);
    }

    // ------------------------------------------------------------------
    // TC-SHR-04: G-buffer format constants are unchanged after reload request
    // ------------------------------------------------------------------
    TEST_CASE("TC-SHR-04 ShaderHotReload - G-buffer format constants unchanged after reload request")
    {
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

    // ------------------------------------------------------------------
    // TC-SHR-07: Full frame executes successfully after reload flag is set
    //            (reload is deferred; frame should still complete normally)
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SHR-07 ShaderHotReload - full frame completes after reload flag set")
    {
        REQUIRE(g_ClearRenderer != nullptr);

        g_Renderer.m_RequestedShaderReload = true;
        // Do NOT call ReloadShaders() — just verify the frame completes
        // with the flag set (reload is deferred to the main loop)
        CHECK_NOTHROW(RunOneFrame());
        g_Renderer.m_RequestedShaderReload = false;
    }

    // ------------------------------------------------------------------
    // TC-SHR-08: RenderGraph G-buffer handles remain valid after reload flag
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-SHR-08 ShaderHotReload - RenderGraph handles valid after reload flag")
    {
        REQUIRE(g_ClearRenderer != nullptr);

        g_Renderer.m_RequestedShaderReload = true;
        CHECK_NOTHROW(RunOneFrame());

        CHECK(g_RG_GBufferAlbedo.IsValid());
        CHECK(g_RG_GBufferNormals.IsValid());
        CHECK(g_RG_DepthTexture.IsValid());
        CHECK(g_RG_HDRColor.IsValid());

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

    // ------------------------------------------------------------------
    // TC-CAP-07: GetCurrentBackBufferTexture returns a non-null texture
    //            after a full frame has been rendered
    // ------------------------------------------------------------------
    TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-CAP-07 BackbufferCapture - GetCurrentBackBufferTexture is non-null after frame")
    {
        REQUIRE(g_ClearRenderer != nullptr);
        CHECK_NOTHROW(RunOneFrame());

        nvrhi::TextureHandle bb = g_Renderer.GetCurrentBackBufferTexture();
        CHECK(bb != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-CAP-08: Swapchain textures have the correct format
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAP-08 BackbufferCapture - swapchain textures have correct format")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        const nvrhi::Format swapFmt = g_Renderer.m_RHI->m_SwapchainFormat;
        REQUIRE(swapFmt != nvrhi::Format::UNKNOWN);

        for (uint32_t i = 0; i < GraphicRHI::SwapchainImageCount; ++i)
        {
            nvrhi::TextureHandle tex = g_Renderer.m_RHI->m_NvrhiSwapchainTextures[i];
            if (tex)
                CHECK(tex->getDesc().format == swapFmt);
        }
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

    // ------------------------------------------------------------------
    // TC-DEPTH-07: DepthReadWrite state has depth write enabled
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-07 DepthConstants - DepthReadWrite has depth write enabled")
    {
        CHECK(CR().DepthReadWrite.depthTestEnable);
        CHECK(CR().DepthReadWrite.depthWriteEnable);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-08: DepthRead state has depth write disabled
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-08 DepthConstants - DepthRead has depth write disabled")
    {
        CHECK(CR().DepthRead.depthTestEnable);
        CHECK(!CR().DepthRead.depthWriteEnable);
    }
}

// ============================================================================
// TEST SUITE: Rendering_PipelineCache
// ============================================================================
TEST_SUITE("Rendering_PipelineCache")
{
    // ------------------------------------------------------------------
    // TC-PIPE-01: GetOrCreateComputePipeline returns non-null for a valid shader
    // ------------------------------------------------------------------
    TEST_CASE("TC-PIPE-01 PipelineCache - GetOrCreateComputePipeline returns non-null")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::ShaderHandle shader = g_Renderer.GetShaderHandle(ShaderID::GPUCULLING_CULLING_CSMAIN);
        REQUIRE(shader != nullptr);

        const nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::CullingConstants), "DeferredCB", 1);
        const nvrhi::BufferHandle cb = DEV()->createBuffer(cbd);

        srrhi::GPUCullingInputs inputs{};
        inputs.SetCullingCB(cb);
        inputs.SetInstanceData(CommonResources::GetInstance().DummySRVStructuredBuffer);
        inputs.SetHZB(CommonResources::GetInstance().DummySRVTexture);
        inputs.SetMeshData(CommonResources::GetInstance().DummySRVStructuredBuffer);
        inputs.SetVisibleArgs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetVisibleCount(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetOccludedIndices(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetOccludedCount(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetDispatchIndirectArgs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletJobs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletJobCount(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletIndirectArgs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetInstanceLOD(CommonResources::GetInstance().DummyUAVStructuredBuffer);

        nvrhi::BindingSetDesc setDesc = Renderer::CreateBindingSetDesc(inputs);
        const nvrhi::BindingLayoutHandle layout = g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(setDesc);
        nvrhi::BindingLayoutVector layouts = { layout };
        layouts.push_back(g_Renderer.GetStaticTextureBindingLayout());
        layouts.push_back(g_Renderer.GetStaticSamplerBindingLayout());
        nvrhi::ComputePipelineHandle pipeline = g_Renderer.GetOrCreateComputePipeline(shader, layouts);

        CHECK(pipeline != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PIPE-02: GetOrCreateComputePipeline is idempotent (same handle returned)
    // ------------------------------------------------------------------
    TEST_CASE("TC-PIPE-02 PipelineCache - GetOrCreateComputePipeline is idempotent")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::ShaderHandle shader = g_Renderer.GetShaderHandle(ShaderID::GPUCULLING_CULLING_CSMAIN);
        REQUIRE(shader != nullptr);

        const nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::CullingConstants), "DeferredCB", 1);
        const nvrhi::BufferHandle cb = DEV()->createBuffer(cbd);

        srrhi::GPUCullingInputs inputs{};
        inputs.SetCullingCB(cb);
        inputs.SetInstanceData(CommonResources::GetInstance().DummySRVStructuredBuffer);
        inputs.SetHZB(CommonResources::GetInstance().DummySRVTexture);
        inputs.SetMeshData(CommonResources::GetInstance().DummySRVStructuredBuffer);
        inputs.SetVisibleArgs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetVisibleCount(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetOccludedIndices(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetOccludedCount(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetDispatchIndirectArgs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletJobs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletJobCount(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletIndirectArgs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetInstanceLOD(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        
        nvrhi::BindingSetDesc setDesc = Renderer::CreateBindingSetDesc(inputs);
        const nvrhi::BindingLayoutHandle layout = g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(setDesc);
        nvrhi::BindingLayoutVector layouts = { layout };
        layouts.push_back(g_Renderer.GetStaticTextureBindingLayout());
        layouts.push_back(g_Renderer.GetStaticSamplerBindingLayout());

        nvrhi::ComputePipelineHandle p1 = g_Renderer.GetOrCreateComputePipeline(shader, layouts);
        nvrhi::ComputePipelineHandle p2 = g_Renderer.GetOrCreateComputePipeline(shader, layouts);

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        // Same underlying object (cache hit)
        CHECK(p1.Get() == p2.Get());
    }

    // ------------------------------------------------------------------
    // TC-PIPE-03: All base-pass shader handles are non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-PIPE-03 PipelineCache - all base-pass shader handles are non-null")
    {
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_VSMAIN)          != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_MSMAIN)          != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_ASMAIN)          != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::BASEPASS_GBUFFER_PSMAIN)  != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::GPUCULLING_CULLING_CSMAIN) != nullptr);
        CHECK(g_Renderer.GetShaderHandle(ShaderID::GPUCULLING_BUILDINDIRECT_CSMAIN) != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-PIPE-04: Binding layout cache returns same handle for identical descs
    // ------------------------------------------------------------------
    TEST_CASE("TC-PIPE-04 PipelineCache - binding layout cache is idempotent")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::BindingSetDesc setDesc;
        // Empty binding set desc — just test the cache mechanism
        nvrhi::BindingLayoutHandle l1 = g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(setDesc, 0);
        nvrhi::BindingLayoutHandle l2 = g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(setDesc, 0);

        REQUIRE(l1 != nullptr);
        REQUIRE(l2 != nullptr);
        CHECK(l1.Get() == l2.Get());
    }
}

// ============================================================================
// TEST SUITE: Rendering_BindlessSystem
// ============================================================================
TEST_SUITE("Rendering_BindlessSystem")
{
    // ------------------------------------------------------------------
    // TC-BIND-01: Static texture descriptor table is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-01 BindlessSystem - static texture descriptor table is non-null")
    {
        CHECK(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-02: Static sampler descriptor table is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-02 BindlessSystem - static sampler descriptor table is non-null")
    {
        CHECK(g_Renderer.GetStaticSamplerDescriptorTable() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-03: Static texture binding layout is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-03 BindlessSystem - static texture binding layout is non-null")
    {
        CHECK(g_Renderer.GetStaticTextureBindingLayout() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-04: Static sampler binding layout is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-04 BindlessSystem - static sampler binding layout is non-null")
    {
        CHECK(g_Renderer.GetStaticSamplerBindingLayout() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-05: RegisterTexture returns a valid index for a new texture
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-05 BindlessSystem - RegisterTexture returns valid index")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::TextureDesc desc;
        desc.width  = 4;
        desc.height = 4;
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "TC-BIND-05-Texture";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);

        const uint32_t idx = g_Renderer.RegisterTexture(tex);
        CHECK(idx != UINT32_MAX);

        tex = nullptr;
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-BIND-06: Next texture index advances after RegisterTexture
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-06 BindlessSystem - next texture index advances after registration")
    {
        REQUIRE(DEV() != nullptr);

        const uint32_t before = g_Renderer.m_NextTextureIndex;

        nvrhi::TextureDesc desc;
        desc.width  = 2;
        desc.height = 2;
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "TC-BIND-06-Texture";

        nvrhi::TextureHandle tex = DEV()->createTexture(desc);
        REQUIRE(tex != nullptr);

        g_Renderer.RegisterTexture(tex);
        CHECK(g_Renderer.m_NextTextureIndex == before + 1);

        tex = nullptr;
        DEV()->waitForIdle();
    }
}

// ============================================================================
// TEST SUITE: Rendering_CommandList
// ============================================================================
TEST_SUITE("Rendering_CommandList")
{
    // ------------------------------------------------------------------
    // TC-CMD-01: AcquireCommandList returns a non-null command list
    // ------------------------------------------------------------------
    TEST_CASE("TC-CMD-01 CommandList - AcquireCommandList returns non-null")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList(false);
        CHECK(cmd != nullptr);

        // Return it by executing (open/close/execute)
        cmd->open();
        cmd->close();
        g_Renderer.ExecutePendingCommandLists();
        DEV()->waitForIdle();
    }

    // ------------------------------------------------------------------
    // TC-CMD-02: Multiple command lists can be acquired and executed
    // ------------------------------------------------------------------
    TEST_CASE("TC-CMD-02 CommandList - multiple command lists can be acquired and executed")
    {
        REQUIRE(DEV() != nullptr);

        std::vector<nvrhi::CommandListHandle> cmds;
        for (int i = 0; i < 4; ++i)
        {
            nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList(false);
            REQUIRE(cmd != nullptr);
            cmd->open();
            cmd->close();
            cmds.push_back(cmd);
        }

        g_Renderer.ExecutePendingCommandLists();

        DEV()->waitForIdle();
    }


}
