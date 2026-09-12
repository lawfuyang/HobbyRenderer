#pragma once

// Holds the GPU data for a single triangle mesh (VB, IB, count, and RT BLAS).
struct CommonMesh
{
    nvrhi::BufferHandle              m_VertexBuffer;      // float3 positions, stride 12
    nvrhi::BufferHandle              m_IndexBuffer;       // uint16_t indices
    uint32_t                         m_IndexCount = 0;
    nvrhi::rt::AccelStructHandle     m_BLAS;
};

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
    nvrhi::SamplerHandle PointMaxReductionClamp;  // Point + max reduction, clamp to edge
    nvrhi::SamplerHandle PointMaxReductionWrap;   // Point + max reduction, wrap
    nvrhi::SamplerHandle LinearClampBorderWhite; // Bilinear filtering, clamp to border (white color)

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
    nvrhi::BlendState::RenderTarget BlendTargetImGui;               // ImGui-specific

    // Common depth-stencil states
    nvrhi::DepthStencilState DepthDisabled;          // No depth test/write
    nvrhi::DepthStencilState DepthRead;              // Depth test, no write (GreaterEqual for reversed-Z)
    nvrhi::DepthStencilState DepthReadWrite;         // Depth test + write (GreaterEqual for reversed-Z)
    nvrhi::DepthStencilState DepthGreaterRead;       // Depth test GreaterEqual, no write
    nvrhi::DepthStencilState DepthGreaterReadWrite;  // Depth test GreaterEqual + write

    // Default textures
    nvrhi::TextureHandle DefaultTextureBlack;        // RGBA(0,0,0,1)
    nvrhi::TextureHandle DefaultTextureWhite;        // RGBA(1,1,1,1)
    nvrhi::TextureHandle DefaultTexture3DWhite;      // 1x1x1 RGBA(1,1,1,1)
    nvrhi::TextureHandle DefaultTextureGray;         // RGBA(0.5,0.5,0.5,1)
    nvrhi::TextureHandle DefaultTextureNormal;       // RGBA(0.5,0.5,1,1) - neutral normal map
    nvrhi::TextureHandle DefaultTexturePBR;          // RGB(1, 0,1) - ORM: Occlusion = 1, Metallic=0, Roughness=1
    nvrhi::TextureHandle DummyUAVTexture;            // 1x1 UAV texture for filling slots
    nvrhi::TextureHandle DummyUAVTextureArray;       // 1x1x1 Texture2DArray UAV for filling array slots
    nvrhi::TextureHandle DummyUAVTexture4;           // 1x1 RGBA32_FLOAT UAV texture for filling float4 UAV slots
    nvrhi::TextureHandle DummySRVTexture;            // 1x1 SRV texture for filling slots
    nvrhi::TextureHandle DummySRVTextureArray;       // 1x1x1 Texture2DArray SRV for filling array slots
    nvrhi::TextureHandle DummySRVFloat4Array;        // 1x1x1 Texture2DArray RGBA32_FLOAT SRV for float4 array slots
    nvrhi::TextureHandle DummySRVTexture4;           // 1x1 RGBA32_FLOAT SRV texture for filling float4 SRV slots
    // Bruneton Atmosphere textures
    nvrhi::TextureHandle BrunetonTransmittance;
    nvrhi::TextureHandle BrunetonScattering;
    nvrhi::TextureHandle BrunetonIrradiance;

    // Blue noise texture (LDR_RG01_0.png — 2-channel, tiling)
    nvrhi::TextureHandle BlueNoiseTexture;

    // Default buffers
    nvrhi::BufferHandle DummySRVByteAddressBuffer;
    nvrhi::BufferHandle DummyUAVByteAddressBuffer;
    nvrhi::BufferHandle DummySRVStructuredBuffer;
    nvrhi::BufferHandle DummyUAVStructuredBuffer;
    nvrhi::BufferHandle DummySRVTypedBuffer;
    nvrhi::BufferHandle DummyUAVTypedBuffer;

    // Default meshes
    CommonMesh UnitSphereMesh;

    CommonResources() = default;
};
