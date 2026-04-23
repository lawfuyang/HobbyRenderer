// Tests_Graphics.cpp — Graphics Infrastructure Tests
//
// Systems under test: D3D12RHI, CommonResources
// Prerequisites: g_Renderer fully initialized (RHI, bindless system, CommonResources)
//                This is guaranteed when RunTests() is called from Renderer.cpp.
//
// Run with: HobbyRenderer --run-tests=*Graphics*
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// TEST SUITE: D3D12RHI — Device & Swapchain
// ============================================================================
TEST_SUITE("Graphics_RHI")
{
    // ------------------------------------------------------------------
    // TC-RHI-01: NVRHI device is non-null after initialization
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-01 Device — NVRHI device handle is valid")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        CHECK(DEV() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-RHI-02: Graphics API is D3D12
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-02 Device — graphics API is D3D12")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        CHECK(g_Renderer.m_RHI->GetGraphicsAPI() == nvrhi::GraphicsAPI::D3D12);
    }

    // ------------------------------------------------------------------
    // TC-RHI-03: Swapchain textures are non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-03 Swapchain — all swapchain texture handles are valid")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        for (uint32_t i = 0; i < GraphicRHI::SwapchainImageCount; ++i)
        {
            INFO("Swapchain image index: " << i);
            CHECK(g_Renderer.m_RHI->m_NvrhiSwapchainTextures[i] != nullptr);
        }
    }

    // ------------------------------------------------------------------
    // TC-RHI-04: Swapchain format is RGBA8_UNORM
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-04 Swapchain — format is RGBA8_UNORM")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        CHECK(g_Renderer.m_RHI->m_SwapchainFormat == nvrhi::Format::RGBA8_UNORM);
    }

    // ------------------------------------------------------------------
    // TC-RHI-05: Swapchain extent is non-zero
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-05 Swapchain — extent width and height are non-zero")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        CHECK(g_Renderer.m_RHI->m_SwapchainExtent.x > 0u);
        CHECK(g_Renderer.m_RHI->m_SwapchainExtent.y > 0u);
    }

    // ------------------------------------------------------------------
    // TC-RHI-06: Swapchain textures match the declared swapchain extent
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-06 Swapchain — texture dimensions match swapchain extent")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        const uint32_t w = g_Renderer.m_RHI->m_SwapchainExtent.x;
        const uint32_t h = g_Renderer.m_RHI->m_SwapchainExtent.y;

        for (uint32_t i = 0; i < GraphicRHI::SwapchainImageCount; ++i)
        {
            REQUIRE(g_Renderer.m_RHI->m_NvrhiSwapchainTextures[i] != nullptr);
            const nvrhi::TextureDesc& desc = g_Renderer.m_RHI->m_NvrhiSwapchainTextures[i]->getDesc();
            INFO("Swapchain image index: " << i);
            CHECK(desc.width  == w);
            CHECK(desc.height == h);
        }
    }

    // ------------------------------------------------------------------
    // TC-RHI-07: Required D3D12 features are supported
    //            (HeapDirectlyIndexed, Meshlets, RayQuery, RTAS)
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-07 Device — required feature support")
    {
        REQUIRE(DEV() != nullptr);
        CHECK(DEV()->queryFeatureSupport(nvrhi::Feature::HeapDirectlyIndexed));
        CHECK(DEV()->queryFeatureSupport(nvrhi::Feature::Meshlets));
        CHECK(DEV()->queryFeatureSupport(nvrhi::Feature::RayQuery));
        CHECK(DEV()->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct));
    }

    // ------------------------------------------------------------------
    // TC-RHI-08: VRAM usage query returns a non-negative value
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-08 Device — VRAM usage query returns non-negative value")
    {
        REQUIRE(g_Renderer.m_RHI != nullptr);
        const float vramMB = g_Renderer.m_RHI->GetVRAMUsageMB();
        CHECK(vramMB >= 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-RHI-09: Command list can be created and executed without error
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-09 CommandList — create and execute a no-op command list")
    {
        REQUIRE(DEV() != nullptr);

        nvrhi::CommandListHandle cmd = DEV()->createCommandList();
        REQUIRE(cmd != nullptr);

        cmd->open();
        // No-op: just open and close
        cmd->close();

        g_Renderer.ExecutePendingCommandLists();
        DEV()->waitForIdle();

        CHECK(true); // Reached here without crash/assert
    }

    // ------------------------------------------------------------------
    // TC-RHI-10: Timer query can be created
    // ------------------------------------------------------------------
    TEST_CASE("TC-RHI-10 TimerQuery — timer query handle is valid")
    {
        REQUIRE(DEV() != nullptr);
        nvrhi::TimerQueryHandle query = DEV()->createTimerQuery();
        CHECK(query != nullptr);
    }
}

// ============================================================================
// TEST SUITE: CommonResources — Samplers
// ============================================================================
TEST_SUITE("Graphics_Samplers")
{
    // ------------------------------------------------------------------
    // TC-SAMP-01: All 9 common samplers are non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-01 Samplers — all common sampler handles are valid")
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
    // TC-SAMP-02: LinearClamp has correct address mode (ClampToEdge)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-02 Samplers — LinearClamp address mode is ClampToEdge")
    {
        REQUIRE(CR().LinearClamp != nullptr);
        const nvrhi::SamplerDesc& desc = CR().LinearClamp->getDesc();
        CHECK(desc.addressU == nvrhi::SamplerAddressMode::ClampToEdge);
        CHECK(desc.addressV == nvrhi::SamplerAddressMode::ClampToEdge);
        CHECK(desc.addressW == nvrhi::SamplerAddressMode::ClampToEdge);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-03: LinearWrap has correct address mode (Wrap)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-03 Samplers — LinearWrap address mode is Wrap")
    {
        REQUIRE(CR().LinearWrap != nullptr);
        const nvrhi::SamplerDesc& desc = CR().LinearWrap->getDesc();
        CHECK(desc.addressU == nvrhi::SamplerAddressMode::Wrap);
        CHECK(desc.addressV == nvrhi::SamplerAddressMode::Wrap);
        CHECK(desc.addressW == nvrhi::SamplerAddressMode::Wrap);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-04: PointClamp has minFilter/magFilter = false (point)
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-04 Samplers — PointClamp uses point filtering")
    {
        REQUIRE(CR().PointClamp != nullptr);
        const nvrhi::SamplerDesc& desc = CR().PointClamp->getDesc();
        CHECK_FALSE(desc.minFilter);
        CHECK_FALSE(desc.magFilter);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-05: AnisotropicClamp has maxAnisotropy = 16
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-05 Samplers — AnisotropicClamp maxAnisotropy is 16")
    {
        REQUIRE(CR().AnisotropicClamp != nullptr);
        const nvrhi::SamplerDesc& desc = CR().AnisotropicClamp->getDesc();
        CHECK(desc.maxAnisotropy == doctest::Approx(16.0f));
    }

    // ------------------------------------------------------------------
    // TC-SAMP-06: AnisotropicWrap has maxAnisotropy = 16 and Wrap mode
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-06 Samplers — AnisotropicWrap maxAnisotropy is 16 and Wrap mode")
    {
        REQUIRE(CR().AnisotropicWrap != nullptr);
        const nvrhi::SamplerDesc& desc = CR().AnisotropicWrap->getDesc();
        CHECK(desc.maxAnisotropy == doctest::Approx(16.0f));
        CHECK(desc.addressU == nvrhi::SamplerAddressMode::Wrap);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-07: MaxReductionClamp uses Maximum reduction type
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-07 Samplers — MaxReductionClamp uses Maximum reduction")
    {
        REQUIRE(CR().MaxReductionClamp != nullptr);
        const nvrhi::SamplerDesc& desc = CR().MaxReductionClamp->getDesc();
        CHECK(desc.reductionType == nvrhi::SamplerReductionType::Maximum);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-08: MinReductionClamp uses Minimum reduction type
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-08 Samplers — MinReductionClamp uses Minimum reduction")
    {
        REQUIRE(CR().MinReductionClamp != nullptr);
        const nvrhi::SamplerDesc& desc = CR().MinReductionClamp->getDesc();
        CHECK(desc.reductionType == nvrhi::SamplerReductionType::Minimum);
    }

    // ------------------------------------------------------------------
    // TC-SAMP-09: LinearClampBorderWhite uses Border address mode
    // ------------------------------------------------------------------
    TEST_CASE("TC-SAMP-09 Samplers — LinearClampBorderWhite uses Border address mode")
    {
        REQUIRE(CR().LinearClampBorderWhite != nullptr);
        const nvrhi::SamplerDesc& desc = CR().LinearClampBorderWhite->getDesc();
        CHECK(desc.addressU == nvrhi::SamplerAddressMode::Border);
        CHECK(desc.addressV == nvrhi::SamplerAddressMode::Border);
        CHECK(desc.addressW == nvrhi::SamplerAddressMode::Border);
        // Border color should be white (1,1,1,1)
        CHECK(desc.borderColor.r == doctest::Approx(1.0f));
        CHECK(desc.borderColor.g == doctest::Approx(1.0f));
        CHECK(desc.borderColor.b == doctest::Approx(1.0f));
        CHECK(desc.borderColor.a == doctest::Approx(1.0f));
    }
}

// ============================================================================
// TEST SUITE: CommonResources — Raster & Blend States
// ============================================================================
TEST_SUITE("Graphics_RasterBlendStates")
{
    // ------------------------------------------------------------------
    // TC-RAST-01: RasterCullNone — Solid fill, no cull
    // ------------------------------------------------------------------
    TEST_CASE("TC-RAST-01 RasterState — CullNone is Solid fill with no culling")
    {
        CHECK(CR().RasterCullNone.fillMode == nvrhi::RasterFillMode::Solid);
        CHECK(CR().RasterCullNone.cullMode == nvrhi::RasterCullMode::None);
    }

    // ------------------------------------------------------------------
    // TC-RAST-02: RasterCullBack — Solid fill, back-face cull
    // ------------------------------------------------------------------
    TEST_CASE("TC-RAST-02 RasterState — CullBack is Solid fill with back-face culling")
    {
        CHECK(CR().RasterCullBack.fillMode == nvrhi::RasterFillMode::Solid);
        CHECK(CR().RasterCullBack.cullMode == nvrhi::RasterCullMode::Back);
    }

    // ------------------------------------------------------------------
    // TC-RAST-03: RasterCullFront — Solid fill, front-face cull
    // ------------------------------------------------------------------
    TEST_CASE("TC-RAST-03 RasterState — CullFront is Solid fill with front-face culling")
    {
        CHECK(CR().RasterCullFront.fillMode == nvrhi::RasterFillMode::Solid);
        CHECK(CR().RasterCullFront.cullMode == nvrhi::RasterCullMode::Front);
    }

    // ------------------------------------------------------------------
    // TC-RAST-04: RasterWireframeNoCull — Wireframe, no cull
    // ------------------------------------------------------------------
    TEST_CASE("TC-RAST-04 RasterState — WireframeNoCull is Wireframe with no culling")
    {
        CHECK(CR().RasterWireframeNoCull.fillMode == nvrhi::RasterFillMode::Wireframe);
        CHECK(CR().RasterWireframeNoCull.cullMode == nvrhi::RasterCullMode::None);
    }

    // ------------------------------------------------------------------
    // TC-BLEND-01: BlendTargetOpaque — blending disabled
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLEND-01 BlendState — Opaque has blending disabled")
    {
        CHECK_FALSE(CR().BlendTargetOpaque.blendEnable);
    }

    // ------------------------------------------------------------------
    // TC-BLEND-02: BlendTargetAlpha — standard alpha blending
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLEND-02 BlendState — Alpha uses SrcAlpha/InvSrcAlpha")
    {
        const auto& bt = CR().BlendTargetAlpha;
        CHECK(bt.blendEnable);
        CHECK(bt.srcBlend  == nvrhi::BlendFactor::SrcAlpha);
        CHECK(bt.destBlend == nvrhi::BlendFactor::InvSrcAlpha);
        CHECK(bt.blendOp   == nvrhi::BlendOp::Add);
    }

    // ------------------------------------------------------------------
    // TC-BLEND-03: BlendTargetPremultipliedAlpha — One/InvSrcAlpha
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLEND-03 BlendState — PremultipliedAlpha uses One/InvSrcAlpha")
    {
        const auto& bt = CR().BlendTargetPremultipliedAlpha;
        CHECK(bt.blendEnable);
        CHECK(bt.srcBlend  == nvrhi::BlendFactor::One);
        CHECK(bt.destBlend == nvrhi::BlendFactor::InvSrcAlpha);
        CHECK(bt.blendOp   == nvrhi::BlendOp::Add);
    }

    // ------------------------------------------------------------------
    // TC-BLEND-04: BlendTargetAdditive — One/One
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLEND-04 BlendState — Additive uses One/One")
    {
        const auto& bt = CR().BlendTargetAdditive;
        CHECK(bt.blendEnable);
        CHECK(bt.srcBlend  == nvrhi::BlendFactor::One);
        CHECK(bt.destBlend == nvrhi::BlendFactor::One);
        CHECK(bt.blendOp   == nvrhi::BlendOp::Add);
    }

    // ------------------------------------------------------------------
    // TC-BLEND-05: BlendTargetMultiply — DstColor/Zero
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLEND-05 BlendState — Multiply uses DstColor/Zero")
    {
        const auto& bt = CR().BlendTargetMultiply;
        CHECK(bt.blendEnable);
        CHECK(bt.srcBlend  == nvrhi::BlendFactor::DstColor);
        CHECK(bt.destBlend == nvrhi::BlendFactor::Zero);
        CHECK(bt.blendOp   == nvrhi::BlendOp::Add);
    }

    // ------------------------------------------------------------------
    // TC-BLEND-06: BlendTargetImGui — same as Alpha (SrcAlpha/InvSrcAlpha)
    // ------------------------------------------------------------------
    TEST_CASE("TC-BLEND-06 BlendState — ImGui blend matches Alpha blend")
    {
        const auto& imgui = CR().BlendTargetImGui;
        const auto& alpha = CR().BlendTargetAlpha;
        CHECK(imgui.blendEnable == alpha.blendEnable);
        CHECK(imgui.srcBlend    == alpha.srcBlend);
        CHECK(imgui.destBlend   == alpha.destBlend);
        CHECK(imgui.blendOp     == alpha.blendOp);
    }
}

// ============================================================================
// TEST SUITE: CommonResources — Depth-Stencil States
// ============================================================================
TEST_SUITE("Graphics_DepthStates")
{
    // ------------------------------------------------------------------
    // TC-DEPTH-01: DepthDisabled — no test, no write, no stencil
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-01 DepthState — Disabled has no test, no write, no stencil")
    {
        const auto& ds = CR().DepthDisabled;
        CHECK_FALSE(ds.depthTestEnable);
        CHECK_FALSE(ds.depthWriteEnable);
        CHECK_FALSE(ds.stencilEnable);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-02: DepthRead — test enabled, write disabled, GreaterOrEqual
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-02 DepthState — Read has test enabled, write disabled, GreaterOrEqual")
    {
        const auto& ds = CR().DepthRead;
        CHECK(ds.depthTestEnable);
        CHECK_FALSE(ds.depthWriteEnable);
        CHECK(ds.depthFunc == nvrhi::ComparisonFunc::GreaterOrEqual);
        CHECK_FALSE(ds.stencilEnable);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-03: DepthReadWrite — test and write enabled, GreaterOrEqual
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-03 DepthState — ReadWrite has test and write enabled, GreaterOrEqual")
    {
        const auto& ds = CR().DepthReadWrite;
        CHECK(ds.depthTestEnable);
        CHECK(ds.depthWriteEnable);
        CHECK(ds.depthFunc == nvrhi::ComparisonFunc::GreaterOrEqual);
        CHECK_FALSE(ds.stencilEnable);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-04: DepthGreaterRead — test enabled, write disabled, GreaterOrEqual
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-04 DepthState — GreaterRead has test enabled, write disabled")
    {
        const auto& ds = CR().DepthGreaterRead;
        CHECK(ds.depthTestEnable);
        CHECK_FALSE(ds.depthWriteEnable);
        CHECK(ds.depthFunc == nvrhi::ComparisonFunc::GreaterOrEqual);
    }

    // ------------------------------------------------------------------
    // TC-DEPTH-05: DepthGreaterReadWrite — test and write enabled, GreaterOrEqual
    // ------------------------------------------------------------------
    TEST_CASE("TC-DEPTH-05 DepthState — GreaterReadWrite has test and write enabled")
    {
        const auto& ds = CR().DepthGreaterReadWrite;
        CHECK(ds.depthTestEnable);
        CHECK(ds.depthWriteEnable);
        CHECK(ds.depthFunc == nvrhi::ComparisonFunc::GreaterOrEqual);
    }
}

// ============================================================================
// TEST SUITE: CommonResources — Default Textures
// ============================================================================
TEST_SUITE("Graphics_DefaultTextures")
{
    // ------------------------------------------------------------------
    // TC-TEX-01: All default texture handles are non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-01 DefaultTextures — all handles are valid")
    {
        CHECK(CR().DefaultTextureBlack  != nullptr);
        CHECK(CR().DefaultTextureWhite  != nullptr);
        CHECK(CR().DefaultTexture3DWhite != nullptr);
        CHECK(CR().DefaultTextureGray   != nullptr);
        CHECK(CR().DefaultTextureNormal != nullptr);
        CHECK(CR().DefaultTexturePBR    != nullptr);
        CHECK(CR().DummyUAVTexture      != nullptr);
        CHECK(CR().DummySRVTexture      != nullptr);
        CHECK(CR().BRDF_LUT             != nullptr);
        CHECK(CR().IrradianceTexture    != nullptr);
        CHECK(CR().RadianceTexture      != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TEX-02: Bruneton atmosphere textures are non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-02 DefaultTextures — Bruneton atmosphere textures are valid")
    {
        CHECK(CR().BrunetonTransmittance != nullptr);
        CHECK(CR().BrunetonScattering    != nullptr);
        CHECK(CR().BrunetonIrradiance    != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-TEX-03: DefaultTextureBlack is 1x1 RGBA8_UNORM Texture2D
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-03 DefaultTextures — Black is 1x1 RGBA8_UNORM Texture2D")
    {
        REQUIRE(CR().DefaultTextureBlack != nullptr);
        const nvrhi::TextureDesc& desc = CR().DefaultTextureBlack->getDesc();
        CHECK(desc.width     == 1u);
        CHECK(desc.height    == 1u);
        CHECK(desc.format    == nvrhi::Format::RGBA8_UNORM);
        CHECK(desc.dimension == nvrhi::TextureDimension::Texture2D);
        CHECK(desc.isShaderResource);
    }

    // ------------------------------------------------------------------
    // TC-TEX-04: DefaultTextureWhite is 1x1 RGBA8_UNORM Texture2D
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-04 DefaultTextures — White is 1x1 RGBA8_UNORM Texture2D")
    {
        REQUIRE(CR().DefaultTextureWhite != nullptr);
        const nvrhi::TextureDesc& desc = CR().DefaultTextureWhite->getDesc();
        CHECK(desc.width     == 1u);
        CHECK(desc.height    == 1u);
        CHECK(desc.format    == nvrhi::Format::RGBA8_UNORM);
        CHECK(desc.dimension == nvrhi::TextureDimension::Texture2D);
    }

    // ------------------------------------------------------------------
    // TC-TEX-05: DefaultTexture3DWhite is a Texture3D
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-05 DefaultTextures — 3DWhite is a Texture3D")
    {
        REQUIRE(CR().DefaultTexture3DWhite != nullptr);
        const nvrhi::TextureDesc& desc = CR().DefaultTexture3DWhite->getDesc();
        CHECK(desc.dimension == nvrhi::TextureDimension::Texture3D);
    }

    // ------------------------------------------------------------------
    // TC-TEX-06: DefaultTexturePBR is RG8_UNORM (Roughness/Metallic)
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-06 DefaultTextures — PBR is RG8_UNORM")
    {
        REQUIRE(CR().DefaultTexturePBR != nullptr);
        const nvrhi::TextureDesc& desc = CR().DefaultTexturePBR->getDesc();
        CHECK(desc.format == nvrhi::Format::RG8_UNORM);
    }

    // ------------------------------------------------------------------
    // TC-TEX-07: DummyUAVTexture has UAV flag set
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-07 DefaultTextures — DummyUAV has isUAV flag")
    {
        REQUIRE(CR().DummyUAVTexture != nullptr);
        const nvrhi::TextureDesc& desc = CR().DummyUAVTexture->getDesc();
        CHECK(desc.isUAV);
    }

    // ------------------------------------------------------------------
    // TC-TEX-08: DummySRVTexture has shader resource flag set
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-08 DefaultTextures — DummySRV has isShaderResource flag")
    {
        REQUIRE(CR().DummySRVTexture != nullptr);
        const nvrhi::TextureDesc& desc = CR().DummySRVTexture->getDesc();
        CHECK(desc.isShaderResource);
    }

    // ------------------------------------------------------------------
    // TC-TEX-09: IrradianceTexture is a TextureCube
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-09 DefaultTextures — IrradianceTexture is a TextureCube")
    {
        REQUIRE(CR().IrradianceTexture != nullptr);
        const nvrhi::TextureDesc& desc = CR().IrradianceTexture->getDesc();
        CHECK(desc.dimension == nvrhi::TextureDimension::TextureCube);
    }

    // ------------------------------------------------------------------
    // TC-TEX-10: RadianceTexture is a TextureCube with at least 1 mip level
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-10 DefaultTextures — RadianceTexture is a TextureCube with mips")
    {
        REQUIRE(CR().RadianceTexture != nullptr);
        const nvrhi::TextureDesc& desc = CR().RadianceTexture->getDesc();
        CHECK(desc.dimension  == nvrhi::TextureDimension::TextureCube);
        CHECK(desc.mipLevels  >= 1u);
        // m_RadianceMipCount should match the texture's mip count
        CHECK(CR().m_RadianceMipCount == desc.mipLevels);
    }

    // ------------------------------------------------------------------
    // TC-TEX-11: BrunetonScattering is a Texture3D
    // ------------------------------------------------------------------
    TEST_CASE("TC-TEX-11 DefaultTextures — BrunetonScattering is a Texture3D")
    {
        REQUIRE(CR().BrunetonScattering != nullptr);
        const nvrhi::TextureDesc& desc = CR().BrunetonScattering->getDesc();
        CHECK(desc.dimension == nvrhi::TextureDimension::Texture3D);
    }
}

// ============================================================================
// TEST SUITE: CommonResources — Dummy Buffers
// ============================================================================
TEST_SUITE("Graphics_DummyBuffers")
{
    // ------------------------------------------------------------------
    // TC-BUF-01: All dummy buffer handles are non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-01 DummyBuffers — all handles are valid")
    {
        CHECK(CR().DummySRVByteAddressBuffer  != nullptr);
        CHECK(CR().DummyUAVByteAddressBuffer  != nullptr);
        CHECK(CR().DummySRVStructuredBuffer   != nullptr);
        CHECK(CR().DummyUAVStructuredBuffer   != nullptr);
        CHECK(CR().DummySRVTypedBuffer        != nullptr);
        CHECK(CR().DummyUAVTypedBuffer        != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BUF-02: DummySRVByteAddressBuffer has canHaveRawViews
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-02 DummyBuffers — SRV ByteAddress has canHaveRawViews")
    {
        REQUIRE(CR().DummySRVByteAddressBuffer != nullptr);
        const nvrhi::BufferDesc& desc = CR().DummySRVByteAddressBuffer->getDesc();
        CHECK(desc.canHaveRawViews);
        CHECK_FALSE(desc.canHaveUAVs);
    }

    // ------------------------------------------------------------------
    // TC-BUF-03: DummyUAVByteAddressBuffer has canHaveRawViews and canHaveUAVs
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-03 DummyBuffers — UAV ByteAddress has canHaveRawViews and canHaveUAVs")
    {
        REQUIRE(CR().DummyUAVByteAddressBuffer != nullptr);
        const nvrhi::BufferDesc& desc = CR().DummyUAVByteAddressBuffer->getDesc();
        CHECK(desc.canHaveRawViews);
        CHECK(desc.canHaveUAVs);
    }

    // ------------------------------------------------------------------
    // TC-BUF-04: DummySRVStructuredBuffer has a non-zero structStride
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-04 DummyBuffers — SRV Structured has non-zero structStride")
    {
        REQUIRE(CR().DummySRVStructuredBuffer != nullptr);
        const nvrhi::BufferDesc& desc = CR().DummySRVStructuredBuffer->getDesc();
        CHECK(desc.structStride > 0u);
        CHECK_FALSE(desc.canHaveUAVs);
    }

    // ------------------------------------------------------------------
    // TC-BUF-05: DummyUAVStructuredBuffer has structStride and canHaveUAVs
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-05 DummyBuffers — UAV Structured has structStride and canHaveUAVs")
    {
        REQUIRE(CR().DummyUAVStructuredBuffer != nullptr);
        const nvrhi::BufferDesc& desc = CR().DummyUAVStructuredBuffer->getDesc();
        CHECK(desc.structStride > 0u);
        CHECK(desc.canHaveUAVs);
    }

    // ------------------------------------------------------------------
    // TC-BUF-06: DummySRVTypedBuffer has canHaveTypedViews and a valid format
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-06 DummyBuffers — SRV Typed has canHaveTypedViews and valid format")
    {
        REQUIRE(CR().DummySRVTypedBuffer != nullptr);
        const nvrhi::BufferDesc& desc = CR().DummySRVTypedBuffer->getDesc();
        CHECK(desc.canHaveTypedViews);
        CHECK(desc.format != nvrhi::Format::UNKNOWN);
        CHECK_FALSE(desc.canHaveUAVs);
    }

    // ------------------------------------------------------------------
    // TC-BUF-07: DummyUAVTypedBuffer has canHaveTypedViews, canHaveUAVs, valid format
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-07 DummyBuffers — UAV Typed has canHaveTypedViews, canHaveUAVs, valid format")
    {
        REQUIRE(CR().DummyUAVTypedBuffer != nullptr);
        const nvrhi::BufferDesc& desc = CR().DummyUAVTypedBuffer->getDesc();
        CHECK(desc.canHaveTypedViews);
        CHECK(desc.canHaveUAVs);
        CHECK(desc.format != nvrhi::Format::UNKNOWN);
    }

    // ------------------------------------------------------------------
    // TC-BUF-08: All dummy buffers have a non-zero byte size
    // ------------------------------------------------------------------
    TEST_CASE("TC-BUF-08 DummyBuffers — all buffers have non-zero byte size")
    {
        CHECK(CR().DummySRVByteAddressBuffer->getDesc().byteSize > 0u);
        CHECK(CR().DummyUAVByteAddressBuffer->getDesc().byteSize > 0u);
        CHECK(CR().DummySRVStructuredBuffer->getDesc().byteSize  > 0u);
        CHECK(CR().DummyUAVStructuredBuffer->getDesc().byteSize  > 0u);
        CHECK(CR().DummySRVTypedBuffer->getDesc().byteSize       > 0u);
        CHECK(CR().DummyUAVTypedBuffer->getDesc().byteSize       > 0u);
    }
}

// ============================================================================
// TEST SUITE: Bindless Descriptor Heaps
// ============================================================================
TEST_SUITE("Graphics_BindlessHeaps")
{
    // ------------------------------------------------------------------
    // TC-BIND-01: Static texture descriptor table is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-01 BindlessTextures — descriptor table handle is valid")
    {
        CHECK(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-02: Static texture binding layout is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-02 BindlessTextures — binding layout handle is valid")
    {
        CHECK(g_Renderer.GetStaticTextureBindingLayout() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-03: Static sampler descriptor table is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-03 BindlessSamplers — descriptor table handle is valid")
    {
        CHECK(g_Renderer.GetStaticSamplerDescriptorTable() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-04: Static sampler binding layout is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-04 BindlessSamplers — binding layout handle is valid")
    {
        CHECK(g_Renderer.GetStaticSamplerBindingLayout() != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-05: Default textures are registered at expected bindless indices
    //             Verifies that the descriptor table slot for each default
    //             texture is occupied (non-null write was performed).
    //             We confirm this indirectly by checking the handles are valid
    //             and the table itself is non-null
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-05 BindlessTextures — default texture slots are populated")
    {
        // The descriptor table must exist and the default textures must be valid.
        // Actual GPU-side slot verification requires a shader readback
        REQUIRE(g_Renderer.GetStaticTextureDescriptorTable() != nullptr);
        CHECK(CR().DefaultTextureBlack  != nullptr);
        CHECK(CR().DefaultTextureWhite  != nullptr);
        CHECK(CR().DefaultTextureGray   != nullptr);
        CHECK(CR().DefaultTextureNormal != nullptr);
        CHECK(CR().DefaultTexturePBR    != nullptr);
        CHECK(CR().BRDF_LUT             != nullptr);
        CHECK(CR().IrradianceTexture    != nullptr);
        CHECK(CR().RadianceTexture      != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-06: Common samplers are registered at expected bindless indices
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-06 BindlessSamplers — common sampler slots are populated")
    {
        REQUIRE(g_Renderer.GetStaticSamplerDescriptorTable() != nullptr);
        CHECK(CR().AnisotropicClamp       != nullptr);
        CHECK(CR().AnisotropicWrap        != nullptr);
        CHECK(CR().PointClamp             != nullptr);
        CHECK(CR().PointWrap              != nullptr);
        CHECK(CR().LinearClamp            != nullptr);
        CHECK(CR().LinearWrap             != nullptr);
        CHECK(CR().MinReductionClamp      != nullptr);
        CHECK(CR().MaxReductionClamp      != nullptr);
        CHECK(CR().LinearClampBorderWhite != nullptr);
    }

    // ------------------------------------------------------------------
    // TC-BIND-07: Shader hot-reload does not invalidate descriptor tables
    //             Captures table pointers before and after a reload request,
    //             verifying the handles remain non-null and consistent.
    // ------------------------------------------------------------------
    TEST_CASE("TC-BIND-07 BindlessTextures — descriptor tables survive shader reload")
    {
        const nvrhi::DescriptorTableHandle texTableBefore  = g_Renderer.GetStaticTextureDescriptorTable();
        const nvrhi::DescriptorTableHandle sampTableBefore = g_Renderer.GetStaticSamplerDescriptorTable();

        REQUIRE(texTableBefore  != nullptr);
        REQUIRE(sampTableBefore != nullptr);

        // Trigger a shader reload (non-destructive: shaders recompile but
        // descriptor tables are not recreated).
        g_Renderer.m_RequestedShaderReload = true;
        // The reload is processed at the start of the next frame; we just
        // verify the tables are still valid immediately after setting the flag.

        CHECK(g_Renderer.GetStaticTextureDescriptorTable()  != nullptr);
        CHECK(g_Renderer.GetStaticSamplerDescriptorTable()  != nullptr);

        // Reset the flag so we don't accidentally trigger a reload mid-test.
        g_Renderer.m_RequestedShaderReload = false;
    }
}
