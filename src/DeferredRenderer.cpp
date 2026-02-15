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

class DeferredRenderer : public IRenderer
{
public:

    void Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferAlbedo);
        renderGraph.ReadTexture(g_RG_GBufferNormals);
        renderGraph.ReadTexture(g_RG_GBufferORM);
        renderGraph.ReadTexture(g_RG_GBufferEmissive);
        renderGraph.ReadTexture(g_RG_GBufferMotionVectors);
        renderGraph.WriteTexture(g_RG_HDRColor);
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::utils::ScopedMarker marker(commandList, "Deferred Lighting");

        nvrhi::TextureHandle depthTexture = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferAlbedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferNormals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferORM = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferEmissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferMotionVectors = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read);
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);

        const Vector3 camPos = renderer->m_Camera.GetPosition();

        // Deferred CB
        const nvrhi::BufferDesc deferredCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(DeferredLightingConstants), "DeferredCB", 1);
        const nvrhi::BufferHandle deferredCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(deferredCBD);

        DeferredLightingConstants dcb{};
        dcb.m_View = renderer->m_View;
        dcb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 1.0f };
        dcb.m_LightDirection = renderer->m_Scene.GetDirectionalLightDirection();
        dcb.m_LightIntensity = renderer->m_Scene.m_DirectionalLight.intensity / 10000.0f;
        dcb.m_EnableRTShadows = renderer->m_EnableRTShadows ? 1 : 0;
        dcb.m_DebugMode = renderer->m_DebugMode;
        dcb.m_EnableIBL = renderer->m_EnableIBL ? 1 : 0;
        dcb.m_IBLIntensity = renderer->m_IBLIntensity;
        dcb.m_RadianceMipCount = CommonResources::GetInstance().RadianceTexture->getDesc().mipLevels;
        commandList->writeBuffer(deferredCB, &dcb, sizeof(dcb), 0);

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
            nvrhi::BindingSetItem::StructuredBuffer_SRV(10, renderer->m_Scene.m_InstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(11, renderer->m_Scene.m_MaterialConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, renderer->m_Scene.m_VertexBufferQuantized),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(13, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(14, renderer->m_Scene.m_IndexBuffer)
        };

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderName = "DeferredLighting_DeferredLighting_PSMain",
            .bindingSetDesc = bset,
            .useBindlessResources = true,
            .framebuffer = renderer->m_RHI->m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(hdrColor))
        };

        renderer->AddFullScreenPass(params);
    }

    const char* GetName() const override { return "Deferred"; }
};

REGISTER_RENDERER(DeferredRenderer);
