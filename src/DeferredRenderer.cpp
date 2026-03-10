#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "shaders/ShaderShared.h"

extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_RTXDIDIOutput;
extern RGTextureHandle g_RG_RTXDIDiffuseOutput;  // RELAX-denoised diffuse illumination
extern RGTextureHandle g_RG_RTXDISpecularOutput; // non-denoised raw specular illumination or RELAX-denoised specular
extern RGTextureHandle g_RG_RTXDIRawDiffuseOutput;
extern RGTextureHandle g_RG_RTXDIRawSpecularOutput;

class DeferredRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferAlbedo);
        renderGraph.ReadTexture(g_RG_GBufferNormals);
        renderGraph.ReadTexture(g_RG_GBufferORM);
        renderGraph.ReadTexture(g_RG_GBufferEmissive);
        renderGraph.ReadTexture(g_RG_GBufferMotionVectors);

        renderGraph.WriteTexture(g_RG_HDRColor);

        // Conditionally read the RTXDI DI output when ReSTIR DI is enabled
        if (renderer->m_EnableReSTIRDI)
        {
            if (renderer->m_EnableReSTIRDIRelaxDenoising)
            {
                renderGraph.ReadTexture(g_RG_RTXDIDiffuseOutput);
                renderGraph.ReadTexture(g_RG_RTXDISpecularOutput);
                renderGraph.ReadTexture(g_RG_RTXDIRawDiffuseOutput);
                renderGraph.ReadTexture(g_RG_RTXDIRawSpecularOutput);
            }
            else
            {
                renderGraph.ReadTexture(g_RG_RTXDIDIOutput);
                renderGraph.ReadTexture(g_RG_RTXDISpecularOutput);
            }
        }

        return true;
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::DeviceHandle device = renderer->m_RHI->m_NvrhiDevice;

        nvrhi::TextureHandle depthTexture = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferAlbedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferNormals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferORM = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferEmissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferMotionVectors = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read);
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);

        const Vector3 camPos = renderer->m_Scene.m_Camera.GetPosition();
        float skyVisFarPlane = renderer->m_Scene.GetSceneBoundingRadius();
        const bool bUseRESTIRDIDenoised = renderer->m_EnableReSTIRDI && renderer->m_EnableReSTIRDIRelaxDenoising;

        // Deferred CB
        const nvrhi::BufferDesc deferredCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(DeferredLightingConstants), "DeferredCB", 1);
        const nvrhi::BufferHandle deferredCB = device->createBuffer(deferredCBD);

        DeferredLightingConstants dcb{};
        dcb.m_View = renderer->m_Scene.m_View;
        dcb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 1.0f };
        dcb.m_SunDirection = renderer->m_Scene.m_SunDirection;
        dcb.m_EnableSky = renderer->m_EnableSky ? 1 : 0;
        dcb.m_RenderingMode = (uint32_t)renderer->m_Mode;
        dcb.m_RadianceMipCount = CommonResources::GetInstance().m_RadianceMipCount;
        dcb.m_LightCount = renderer->m_Scene.m_LightCount;
        dcb.m_EnableRTShadows = renderer->m_EnableRTShadows ? 1 : 0;
        dcb.m_DebugMode = renderer->m_DebugMode;
        dcb.m_UseReSTIRDI = renderer->m_EnableReSTIRDI ? 1u : 0u;
        dcb.m_UseReSTIRDIDenoised = bUseRESTIRDIDenoised ? 1u : 0u;
        dcb.m_NoiseMix = renderer->m_ReSTIRDINoiseMix;
        dcb.m_NoiseClampLow = renderer->m_ReSTIRDINoiseClampLow;
        dcb.m_NoiseClampHigh = renderer->m_ReSTIRDINoiseClampHigh;
        commandList->writeBuffer(deferredCB, &dcb, sizeof(dcb), 0);

        // t8/t9: DI diffuse/specular illumination (denoised or raw, depending on mode)
        // t16/t17: raw noisy DI diffuse/specular (for denoiser noise mix-in in denoised mode)
        nvrhi::TextureHandle rtxdiDiffuseOutput = bUseRESTIRDIDenoised
            ? renderGraph.GetTexture(g_RG_RTXDIDiffuseOutput,  RGResourceAccessMode::Read)
            : (renderer->m_EnableReSTIRDI
                ? renderGraph.GetTexture(g_RG_RTXDIDIOutput, RGResourceAccessMode::Read)
                : CommonResources::GetInstance().DefaultTextureBlack);
        nvrhi::TextureHandle rtxdiSpecularOutput = bUseRESTIRDIDenoised
            ? renderGraph.GetTexture(g_RG_RTXDISpecularOutput, RGResourceAccessMode::Read)
            : (renderer->m_EnableReSTIRDI
                ? renderGraph.GetTexture(g_RG_RTXDISpecularOutput, RGResourceAccessMode::Read)
                : CommonResources::GetInstance().DefaultTextureBlack);
        nvrhi::TextureHandle rtxdiRawDiffuseOutput = bUseRESTIRDIDenoised
            ? renderGraph.GetTexture(g_RG_RTXDIRawDiffuseOutput, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;
        nvrhi::TextureHandle rtxdiRawSpecularOutput = bUseRESTIRDIDenoised
            ? renderGraph.GetTexture(g_RG_RTXDIRawSpecularOutput, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;

        nvrhi::BindingSetDesc bset;
        bset.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, deferredCB),
            nvrhi::BindingSetItem::Texture_SRV(0, gbufferAlbedo),
            nvrhi::BindingSetItem::Texture_SRV(1, gbufferNormals),
            nvrhi::BindingSetItem::Texture_SRV(2, gbufferORM),
            nvrhi::BindingSetItem::Texture_SRV(3, gbufferEmissive),
            nvrhi::BindingSetItem::Texture_SRV(7, gbufferMotionVectors),
            nvrhi::BindingSetItem::Texture_SRV(4, depthTexture),
            nvrhi::BindingSetItem::RayTracingAccelStruct(5, renderer->m_Scene.m_TLAS),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, renderer->m_Scene.m_LightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(10, renderer->m_Scene.m_InstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(11, renderer->m_Scene.m_MaterialConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, renderer->m_Scene.m_VertexBufferQuantized),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(13, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(14, renderer->m_Scene.m_IndexBuffer),
            nvrhi::BindingSetItem::Texture_SRV(8,  rtxdiDiffuseOutput),
            nvrhi::BindingSetItem::Texture_SRV(9,  rtxdiSpecularOutput),
            nvrhi::BindingSetItem::Texture_SRV(16, rtxdiRawDiffuseOutput),
            nvrhi::BindingSetItem::Texture_SRV(17, rtxdiRawSpecularOutput),
        };

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(hdrColor);
        fbDesc.setDepthAttachment(depthTexture);

        // On Vulkan, we need to set the depth attachment as read-only to be able to sample from it in the shader while it's also bound as a depth attachment
        // D3D12 allows this without a special flag. NOTE: pretty sure this is a bug in the nvrhi D3D12 backend
        if (renderer->m_RHI->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            fbDesc.depthAttachment.isReadOnly = true;
        }

        nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(fbDesc);

        // Surfaces Pass (Stencil == 1)
        nvrhi::DepthStencilState ds;
        ds.depthTestEnable = false;
        ds.depthWriteEnable = false;
        ds.stencilEnable = true;
        ds.stencilRefValue = 1;
        ds.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Equal;

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderName = "DeferredLighting_DeferredLighting_PSMain",
            .bindingSetDesc = bset,
            .framebuffer = framebuffer,
            .depthStencilState = &ds
        };

        renderer->AddFullScreenPass(params);
    }

    const char* GetName() const override { return "Deferred"; }
};

REGISTER_RENDERER(DeferredRenderer);
