#include "pch.h"
#include "CommonResources.h"
#include "Renderer.h"
#include "TextureLoader.h"
#include "Config.h"

#include "shaders/ShaderShared.h"

void CommonResources::Initialize()
{
    Renderer* renderer = Renderer::GetInstance();
    nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;
    nvrhi::GraphicsAPI api = device->getGraphicsAPI();

    // Helper lambda to create samplers with error checking
    auto createSampler = [device](const char* name, const nvrhi::SamplerDesc& desc) -> nvrhi::SamplerHandle
    {
        nvrhi::SamplerHandle sampler = device->createSampler(desc);
        if (!sampler)
        {
            SDL_LOG_ASSERT_FAIL("Failed to create common sampler", "[CommonResources] Failed to create %s sampler", name);
        }
        return sampler;
    };

    // Create common samplers (all must succeed)
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(true);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
        LinearClamp = createSampler("LinearClamp", desc);
    }
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(true);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
        LinearWrap = createSampler("LinearWrap", desc);
    }
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(false);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
        PointClamp = createSampler("PointClamp", desc);
    }
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(false);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
        PointWrap = createSampler("PointWrap", desc);
    }
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(true);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
        desc.setMaxAnisotropy(16.0f);
        AnisotropicClamp = createSampler("AnisotropicClamp", desc);
    }
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(true);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
        desc.setMaxAnisotropy(16.0f);
        AnisotropicWrap = createSampler("AnisotropicWrap", desc);
    }
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(true);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
        desc.reductionType = nvrhi::SamplerReductionType::Maximum;
        MaxReductionClamp = createSampler("MaxReductionClamp", desc);
    }
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(true);
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
        desc.reductionType = nvrhi::SamplerReductionType::Minimum;
        MinReductionClamp = createSampler("MinReductionClamp", desc);
    }

    // Register common samplers with global sampler descriptor heap
    renderer->RegisterSamplerAtIndex(SAMPLER_ANISOTROPIC_CLAMP_INDEX, AnisotropicClamp);
    renderer->RegisterSamplerAtIndex(SAMPLER_ANISOTROPIC_WRAP_INDEX, AnisotropicWrap);
    renderer->RegisterSamplerAtIndex(SAMPLER_POINT_CLAMP_INDEX, PointClamp);
    renderer->RegisterSamplerAtIndex(SAMPLER_POINT_WRAP_INDEX, PointWrap);
    renderer->RegisterSamplerAtIndex(SAMPLER_LINEAR_CLAMP_INDEX, LinearClamp);
    renderer->RegisterSamplerAtIndex(SAMPLER_LINEAR_WRAP_INDEX, LinearWrap);
    renderer->RegisterSamplerAtIndex(SAMPLER_MIN_REDUCTION_INDEX, MinReductionClamp);
    renderer->RegisterSamplerAtIndex(SAMPLER_MAX_REDUCTION_INDEX, MaxReductionClamp);

    // Initialize common raster states
    // glTF spec says counter-clockwise is front face, but Vulkan viewport flip reverses winding
    // With X negation in projection for Vulkan, winding is flipped again
    bool frontCCW = true;

    // Solid, no cull
    RasterCullNone.fillMode = nvrhi::RasterFillMode::Solid;
    RasterCullNone.cullMode = nvrhi::RasterCullMode::None;
    RasterCullNone.frontCounterClockwise = frontCCW;

    // Solid, back-face cull
    RasterCullBack.fillMode = nvrhi::RasterFillMode::Solid;
    RasterCullBack.cullMode = nvrhi::RasterCullMode::Back;
    RasterCullBack.frontCounterClockwise = frontCCW;

    // Solid, front-face cull
    RasterCullFront.fillMode = nvrhi::RasterFillMode::Solid;
    RasterCullFront.cullMode = nvrhi::RasterCullMode::Front;
    RasterCullFront.frontCounterClockwise = frontCCW;

    // Wireframe, no cull
    RasterWireframeNoCull.fillMode = nvrhi::RasterFillMode::Wireframe;
    RasterWireframeNoCull.cullMode = nvrhi::RasterCullMode::None;
    RasterWireframeNoCull.frontCounterClockwise = frontCCW;

    // Initialize common blend states
    {
        using BF = nvrhi::BlendFactor;
        using BO = nvrhi::BlendOp;

        // Opaque: blending disabled
        BlendTargetOpaque = nvrhi::BlendState::RenderTarget{};
        BlendTargetOpaque.blendEnable = false;

        // Standard alpha blending (straight alpha)
        BlendTargetAlpha = nvrhi::BlendState::RenderTarget{};
        BlendTargetAlpha.blendEnable = true;
        BlendTargetAlpha.srcBlend = BF::SrcAlpha;
        BlendTargetAlpha.destBlend = BF::InvSrcAlpha;
        BlendTargetAlpha.blendOp = BO::Add;
        BlendTargetAlpha.srcBlendAlpha = BF::One;
        BlendTargetAlpha.destBlendAlpha = BF::InvSrcAlpha;
        BlendTargetAlpha.blendOpAlpha = BO::Add;

        // Premultiplied alpha
        BlendTargetPremultipliedAlpha = nvrhi::BlendState::RenderTarget{};
        BlendTargetPremultipliedAlpha.blendEnable = true;
        BlendTargetPremultipliedAlpha.srcBlend = BF::One;
        BlendTargetPremultipliedAlpha.destBlend = BF::InvSrcAlpha;
        BlendTargetPremultipliedAlpha.blendOp = BO::Add;
        BlendTargetPremultipliedAlpha.srcBlendAlpha = BF::One;
        BlendTargetPremultipliedAlpha.destBlendAlpha = BF::InvSrcAlpha;
        BlendTargetPremultipliedAlpha.blendOpAlpha = BO::Add;

        // Additive
        BlendTargetAdditive = nvrhi::BlendState::RenderTarget{};
        BlendTargetAdditive.blendEnable = true;
        BlendTargetAdditive.srcBlend = BF::One;
        BlendTargetAdditive.destBlend = BF::One;
        BlendTargetAdditive.blendOp = BO::Add;
        BlendTargetAdditive.srcBlendAlpha = BF::One;
        BlendTargetAdditive.destBlendAlpha = BF::One;
        BlendTargetAdditive.blendOpAlpha = BO::Add;

        // Multiply
        BlendTargetMultiply = nvrhi::BlendState::RenderTarget{};
        BlendTargetMultiply.blendEnable = true;
        BlendTargetMultiply.srcBlend = BF::DstColor;
        BlendTargetMultiply.destBlend = BF::Zero;
        BlendTargetMultiply.blendOp = BO::Add;
        BlendTargetMultiply.srcBlendAlpha = BF::DstAlpha;
        BlendTargetMultiply.destBlendAlpha = BF::Zero;
        BlendTargetMultiply.blendOpAlpha = BO::Add;

        // ImGui blend (straight alpha, matches imgui implementation)
        BlendTargetImGui = BlendTargetAlpha;
    }

    // Initialize common depth-stencil states
    {
        // Disabled: no test, no write
        DepthDisabled = nvrhi::DepthStencilState{};
        DepthDisabled.depthTestEnable = false;
        DepthDisabled.depthWriteEnable = false;
        DepthDisabled.stencilEnable = false;

        // Read-only (GreaterEqual for reversed-Z)
        DepthRead = nvrhi::DepthStencilState{};
        DepthRead.depthTestEnable = true;
        DepthRead.depthWriteEnable = false;
        DepthRead.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
        DepthRead.stencilEnable = false;

        // Read-write (GreaterEqual for reversed-Z)
        DepthReadWrite = nvrhi::DepthStencilState{};
        DepthReadWrite.depthTestEnable = true;
        DepthReadWrite.depthWriteEnable = true;
        DepthReadWrite.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
        DepthReadWrite.stencilEnable = false;

        // GreaterEqual read-only
        DepthGreaterRead = nvrhi::DepthStencilState{};
        DepthGreaterRead.depthTestEnable = true;
        DepthGreaterRead.depthWriteEnable = false;
        DepthGreaterRead.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
        DepthGreaterRead.stencilEnable = false;

        // GreaterEqual read-write
        DepthGreaterReadWrite = nvrhi::DepthStencilState{};
        DepthGreaterReadWrite.depthTestEnable = true;
        DepthGreaterReadWrite.depthWriteEnable = true;
        DepthGreaterReadWrite.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
        DepthGreaterReadWrite.stencilEnable = false;
    }

    // Create default textures
    {
        // Helper lambda to create a 1x1 texture
        auto createDefaultTexture = [device](const char* name, nvrhi::Format format = nvrhi::Format::RGBA8_UNORM, bool isUAV = false) -> nvrhi::TextureHandle
        {
            nvrhi::TextureDesc desc;
            desc.width = 1;
            desc.height = 1;
            desc.format = format;
            desc.isShaderResource = true;
            desc.isUAV = isUAV;
            desc.initialState = isUAV ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = name;

            nvrhi::TextureHandle texture = device->createTexture(desc);
            if (!texture)
            {
                SDL_LOG_ASSERT_FAIL("Failed to create default texture", "[CommonResources] Failed to create %s texture", name);
                return nullptr;
            }

            return texture;
        };

        // Create the textures
        DefaultTextureBlack = createDefaultTexture("DefaultBlack");
        DefaultTextureWhite = createDefaultTexture("DefaultWhite");
        DefaultTextureGray = createDefaultTexture("DefaultGray");
        DefaultTextureNormal = createDefaultTexture("DefaultNormal");
        DefaultTexturePBR = createDefaultTexture("DefaultPBR"); // ORM: Occlusion=1, Roughness=1, Metallic=0

        // Create dummy UAV texture
        DummyUAVTexture = createDefaultTexture("DummyUAV", nvrhi::Format::R32_FLOAT, true);

        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = 4;
        bufferDesc.structStride = 4;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        bufferDesc.debugName = "DummyUAVBuffer";
        DummyUAVBuffer = device->createBuffer(bufferDesc);

        // Upload texture data using renderer's command list management
        nvrhi::CommandListHandle cmd = renderer->AcquireCommandList("CommonResources_DefaultTextures");
        ScopedCommandList commandList{ cmd };

        const char* basePath = SDL_GetBasePath();
        const Config& config = Config::Get();

        auto LoadAndUpload = [&](const std::string& configPath, const char* debugName, nvrhi::TextureHandle& outTexture, bool expectCube = false)
        {
            nvrhi::TextureDesc desc;
            std::unique_ptr<ITextureDataReader> data;
            std::filesystem::path path(configPath);
            if (path.is_relative() && basePath)
            {
                path = std::filesystem::path(basePath) / path;
            }

            if (LoadTexture(path.generic_string(), desc, data))
            {
                if (expectCube && desc.dimension != nvrhi::TextureDimension::TextureCube)
                {
                    SDL_LOG_ASSERT_FAIL("Texture must be a cubemap", "%s must be a TextureCube", debugName);
                }

                desc.debugName = debugName;
                outTexture = device->createTexture(desc);
                SDL_assert(outTexture);
                ::UploadTexture(commandList, outTexture, desc, data->GetData(), data->GetSize());
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Failed to load texture", "Failed to load texture: %s", path.generic_string().c_str());
            }
        };

        // Load BRDF LUT
        LoadAndUpload(renderer->m_BRDFLutTexture, "BRDF_LUT", BRDF_LUT);

        // Load IBL textures
        LoadAndUpload(renderer->m_IrradianceTexture, "IrradianceTexture", IrradianceTexture, true);
        LoadAndUpload(renderer->m_RadianceTexture, "RadianceTexture", RadianceTexture, true);

        // Black texture
        uint32_t blackPixel = 0xFF000000; // RGBA(0,0,0,255)
        commandList->writeTexture(DefaultTextureBlack, 0, 0, &blackPixel, sizeof(uint32_t), 0);

        // White texture
        uint32_t whitePixel = 0xFFFFFFFF; // RGBA(255,255,255,255)
        commandList->writeTexture(DefaultTextureWhite, 0, 0, &whitePixel, sizeof(uint32_t), 0);

        // Gray texture
        uint32_t grayPixel = 0xFF808080; // RGBA(128,128,128,255)
        commandList->writeTexture(DefaultTextureGray, 0, 0, &grayPixel, sizeof(uint32_t), 0);

        // Normal texture
        uint32_t normalPixel = 0xFFFF8080; // RGBA(128,128,255,255) - note: ABGR order in memory
        commandList->writeTexture(DefaultTextureNormal, 0, 0, &normalPixel, sizeof(uint32_t), 0);

        // PBR texture (ORM: Occlusion=1, Roughness=1, Metallic=0)
        uint32_t pbrPixel = 0xFFFFFF00; // RGBA(0,255,255,255) - R=Metallic(0), G=Roughness(255), B=Occlusion(255), A=255
        commandList->writeTexture(DefaultTexturePBR, 0, 0, &pbrPixel, sizeof(uint32_t), 0);

        // Note: Default textures are registered later in Renderer::Initialize after bindless system is set up
    }

    SDL_Log("[CommonResources] Initialized successfully");
}

void CommonResources::RegisterDefaultTextures()
{
    Renderer* renderer = Renderer::GetInstance();

    // Register textures with the global bindless system
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_BLACK, DefaultTextureBlack);
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_WHITE, DefaultTextureWhite);
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_GRAY, DefaultTextureGray);
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_NORMAL, DefaultTextureNormal);
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_PBR, DefaultTexturePBR);
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_BRDF_LUT, BRDF_LUT);
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_IRRADIANCE, IrradianceTexture);
    renderer->RegisterTextureAtIndex(DEFAULT_TEXTURE_RADIANCE, RadianceTexture);
    SDL_Log("[CommonResources] Default textures registered with bindless system");
}

void CommonResources::Shutdown()
{
    RadianceTexture = nullptr;
    IrradianceTexture = nullptr;
    BRDF_LUT = nullptr;
    DummyUAVTexture = nullptr;
    DummyUAVBuffer = nullptr;
    DefaultTexturePBR = nullptr;
    DefaultTextureNormal = nullptr;
    DefaultTextureGray = nullptr;
    DefaultTextureWhite = nullptr;
    DefaultTextureBlack = nullptr;
    AnisotropicClamp = nullptr;
    AnisotropicWrap = nullptr;
    PointWrap = nullptr;
    PointClamp = nullptr;
    LinearWrap = nullptr;
    LinearClamp = nullptr;
    MaxReductionClamp = nullptr;
    MinReductionClamp = nullptr;
}
