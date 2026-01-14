#include "pch.h"
#include "CommonResources.h"
#include "Renderer.h"

#include "shaders/ShaderShared.hlsl"

bool CommonResources::Initialize()
{
    Renderer* renderer = Renderer::GetInstance();
    nvrhi::IDevice* device = renderer->m_NvrhiDevice;
    nvrhi::GraphicsAPI api = device->getGraphicsAPI();

    // Helper lambda to create samplers with error checking
    auto createSampler = [device](const char* name, bool linearFilter, nvrhi::SamplerAddressMode addressMode, float anisotropy = 1.0f) -> nvrhi::SamplerHandle
    {
        nvrhi::SamplerDesc desc;
        desc.setAllFilters(linearFilter);
        desc.setAllAddressModes(addressMode);
        if (anisotropy > 1.0f)
            desc.setMaxAnisotropy(anisotropy);
        
        nvrhi::SamplerHandle sampler = device->createSampler(desc);
        if (!sampler)
        {
            SDL_Log("[CommonResources] Failed to create %s sampler", name);
            SDL_assert(false && "Failed to create common sampler");
        }
        return sampler;
    };

    // Create common samplers (all must succeed)
    LinearClamp = createSampler("LinearClamp", true, nvrhi::SamplerAddressMode::ClampToEdge);
    LinearWrap = createSampler("LinearWrap", true, nvrhi::SamplerAddressMode::Wrap);
    PointClamp = createSampler("PointClamp", false, nvrhi::SamplerAddressMode::ClampToEdge);
    PointWrap = createSampler("PointWrap", false, nvrhi::SamplerAddressMode::Wrap);
    Anisotropic = createSampler("Anisotropic", true, nvrhi::SamplerAddressMode::Wrap, 16.0f);

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
        // Helper lambda to create a 1x1 texture with a solid color
        auto createDefaultTexture = [device](const char* name, nvrhi::Color color) -> nvrhi::TextureHandle
        {
            nvrhi::TextureDesc desc;
            desc.width = 1;
            desc.height = 1;
            desc.format = nvrhi::Format::RGBA8_UNORM;
            desc.isShaderResource = true;
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = name;

            nvrhi::TextureHandle texture = device->createTexture(desc);
            if (!texture)
            {
                SDL_Log("[CommonResources] Failed to create %s texture", name);
                SDL_assert(false && "Failed to create default texture");
                return nullptr;
            }

            return texture;
        };

        // Create the textures
        DefaultTextureBlack = createDefaultTexture("DefaultBlack", nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        DefaultTextureWhite = createDefaultTexture("DefaultWhite", nvrhi::Color(1.0f, 1.0f, 1.0f, 1.0f));
        DefaultTextureGray = createDefaultTexture("DefaultGray", nvrhi::Color(0.5f, 0.5f, 0.5f, 1.0f));
        DefaultTextureNormal = createDefaultTexture("DefaultNormal", nvrhi::Color(0.5f, 0.5f, 1.0f, 1.0f));
        DefaultTexturePBR = createDefaultTexture("DefaultPBR", nvrhi::Color(1.0f, 1.0f, 1.0f, 1.0f)); // ORM: Occlusion=1, Roughness=1, Metallic=0

        // Upload texture data using renderer's command list management
        nvrhi::CommandListHandle commandList = renderer->AcquireCommandList("CommonResources_DefaultTextures");

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

        renderer->SubmitCommandList(commandList);

        // Note: Default textures are registered later in Renderer::Initialize after bindless system is set up
    }

    SDL_Log("[CommonResources] Initialized successfully");
    return true;
}

bool CommonResources::RegisterDefaultTextures()
{
    Renderer* renderer = Renderer::GetInstance();

    // Register textures with the global bindless system
    uint32_t index = renderer->RegisterTexture(DefaultTextureBlack);
    SDL_assert(index == DEFAULT_TEXTURE_BLACK);
    index = renderer->RegisterTexture(DefaultTextureWhite);
    SDL_assert(index == DEFAULT_TEXTURE_WHITE);
    index = renderer->RegisterTexture(DefaultTextureGray);
    SDL_assert(index == DEFAULT_TEXTURE_GRAY);
    index = renderer->RegisterTexture(DefaultTextureNormal);
    SDL_assert(index == DEFAULT_TEXTURE_NORMAL);
    index = renderer->RegisterTexture(DefaultTexturePBR);
    SDL_assert(index == DEFAULT_TEXTURE_PBR);
    SDL_Log("[CommonResources] Default textures registered with bindless system");
    return true;
}

void CommonResources::Shutdown()
{
    DefaultTexturePBR = nullptr;
    DefaultTextureNormal = nullptr;
    DefaultTextureGray = nullptr;
    DefaultTextureWhite = nullptr;
    DefaultTextureBlack = nullptr;
    Anisotropic = nullptr;
    PointWrap = nullptr;
    PointClamp = nullptr;
    LinearWrap = nullptr;
    LinearClamp = nullptr;
}
