#pragma once

#include "pch.h"

// Common GPU resources shared across the application (samplers, default textures, etc.)
class CommonResources
{
public:
    static CommonResources& GetInstance()
    {
        static CommonResources instance;
        return instance;
    }

    // Delete copy and move constructors/operators
    CommonResources(const CommonResources&) = delete;
    CommonResources& operator=(const CommonResources&) = delete;
    CommonResources(CommonResources&&) = delete;
    CommonResources& operator=(CommonResources&&) = delete;

    void Initialize();
    void Shutdown();

    // Register default textures with the global bindless system
    void RegisterDefaultTextures();

    // Common sampler states
    nvrhi::SamplerHandle LinearClamp;   // Bilinear filtering, clamp to edge
    nvrhi::SamplerHandle LinearWrap;    // Bilinear filtering, wrap/repeat
    nvrhi::SamplerHandle PointClamp;    // Point/nearest filtering, clamp to edge
    nvrhi::SamplerHandle PointWrap;     // Point/nearest filtering, wrap/repeat
    nvrhi::SamplerHandle AnisotropicClamp; // Anisotropic filtering, clamp
    nvrhi::SamplerHandle AnisotropicWrap;  // Anisotropic filtering, wrap
    nvrhi::SamplerHandle MaxReductionClamp; // Max reduction, clamp to edge
    nvrhi::SamplerHandle MinReductionClamp; // Min reduction, clamp to edge

    // Common raster states
    nvrhi::RasterState RasterCullNone;        // Solid fill, no cull
    nvrhi::RasterState RasterCullBack;        // Solid fill, back-face cull
    nvrhi::RasterState RasterCullFront;       // Solid fill, front-face cull
    nvrhi::RasterState RasterWireframeNoCull; // Wireframe, no cull

    // Common blend states
    // Individual render-target descriptors for composing blend states
    nvrhi::BlendState::RenderTarget BlendTargetOpaque;              // No blending
    nvrhi::BlendState::RenderTarget BlendTargetAlpha;               // Standard alpha blending (SrcAlpha, InvSrcAlpha)
    nvrhi::BlendState::RenderTarget BlendTargetPremultipliedAlpha;  // Premultiplied alpha (One, InvSrcAlpha)
    nvrhi::BlendState::RenderTarget BlendTargetAdditive;            // Additive (One, One)
    nvrhi::BlendState::RenderTarget BlendTargetMultiply;            // Multiply (DstColor, Zero)
    nvrhi::BlendState::RenderTarget BlendTargetImGui;               // ImGui-specific (matches imgui_impl_vulkan)

    // Common depth-stencil states
    nvrhi::DepthStencilState DepthDisabled;          // No depth test/write
    nvrhi::DepthStencilState DepthRead;              // Depth test, no write (GreaterEqual for reversed-Z)
    nvrhi::DepthStencilState DepthReadWrite;         // Depth test + write (GreaterEqual for reversed-Z)
    nvrhi::DepthStencilState DepthGreaterRead;       // Depth test GreaterEqual, no write
    nvrhi::DepthStencilState DepthGreaterReadWrite;  // Depth test GreaterEqual + write

    // Default textures
    nvrhi::TextureHandle DefaultTextureBlack;        // RGBA(0,0,0,1)
    nvrhi::TextureHandle DefaultTextureWhite;        // RGBA(1,1,1,1)
    nvrhi::TextureHandle DefaultTextureGray;         // RGBA(0.5,0.5,0.5,1)
    nvrhi::TextureHandle DefaultTextureNormal;       // RGBA(0.5,0.5,1,1) - neutral normal map
    nvrhi::TextureHandle DefaultTexturePBR;          // RGBA(0,1,1,1) - ORM: Metallic=0, Roughness=1, Occlusion=1
    nvrhi::TextureHandle DummyUAVTexture;            // 1x1 UAV texture for filling slots

    // Default buffers
    nvrhi::BufferHandle DummyUAVBuffer;    // Empty structured buffer

    CommonResources() = default;
};
