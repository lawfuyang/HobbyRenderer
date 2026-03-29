#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "shaders/srrhi/cpp/DeferredLighting.h"

extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_RTXDIDIComposited;   // CompositingPass output — final DI + emissive composite

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

        // Conditionally read the RTXDI composited output when ReSTIR DI is enabled
        if (renderer->m_EnableReSTIRDI)
            renderGraph.ReadTexture(g_RG_RTXDIDIComposited);

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


        // Deferred CB
        const nvrhi::BufferDesc deferredCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::DeferredLightingConstants), "DeferredCB", 1);
        const nvrhi::BufferHandle deferredCB = device->createBuffer(deferredCBD);

        srrhi::DeferredLightingConstants dcb;
        dcb.SetView(renderer->m_Scene.m_View);
        dcb.SetCameraPos(DirectX::XMFLOAT4{ camPos.x, camPos.y, camPos.z, 1.0f });
        dcb.SetSunDirection(renderer->m_Scene.GetSunDirection());
        dcb.SetEnableSky(renderer->m_EnableSky ? 1 : 0);
        dcb.SetRenderingMode((uint32_t)renderer->m_Mode);
        dcb.SetRadianceMipCount(CommonResources::GetInstance().m_RadianceMipCount);
        dcb.SetLightCount(renderer->m_Scene.m_LightCount);
        dcb.SetEnableRTShadows(renderer->m_EnableRTShadows ? 1 : 0);
        dcb.SetDebugMode(renderer->m_DebugMode);
        dcb.SetUseReSTIRDI(renderer->m_EnableReSTIRDI ? 1u : 0u);
        dcb.SetUseReSTIRDIDenoised(0u); // compositing is done by CompositingPass
        commandList->writeBuffer(deferredCB, &dcb, sizeof(dcb), 0);

        // t8: RTXDI composited output (DI + emissive, already remodulated by CompositingPass)
        nvrhi::TextureHandle rtxdiComposited = renderer->m_EnableReSTIRDI
            ? renderGraph.GetTexture(g_RG_RTXDIDIComposited, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;

        srrhi::DeferredLightingInputs dlInputs;
        dlInputs.SetDeferredCB(deferredCB);
        dlInputs.SetGBufferAlbedo(gbufferAlbedo);
        dlInputs.SetGBufferNormals(gbufferNormals);
        dlInputs.SetGBufferORM(gbufferORM);
        dlInputs.SetGBufferEmissive(gbufferEmissive);
        dlInputs.SetGBufferMotion(gbufferMotionVectors);
        dlInputs.SetDepth(depthTexture);
        dlInputs.SetSceneAS(renderer->m_Scene.m_TLAS);
        dlInputs.SetLights(renderer->m_Scene.m_LightBuffer);
        dlInputs.SetInstances(renderer->m_Scene.m_InstanceDataBuffer);
        dlInputs.SetMaterials(renderer->m_Scene.m_MaterialConstantsBuffer);
        dlInputs.SetVertices(renderer->m_Scene.m_VertexBufferQuantized);
        dlInputs.SetMeshData(renderer->m_Scene.m_MeshDataBuffer);
        dlInputs.SetIndices(renderer->m_Scene.m_IndexBuffer);
        dlInputs.SetRTXDIDIComposited(rtxdiComposited);
        nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(dlInputs);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(hdrColor);
        fbDesc.setDepthAttachment(depthTexture);

        // On Vulkan, we need to set the depth attachment as read-only to be able to sample from it in the shader while it's also bound as a depth attachment
        // D3D12 allows this without a special flag. As a result, D3D12 will push 2 of the same barriers for the depth buffer
        // pretty sure this is a bug in the nvrhi D3D12 backend
        //fbDesc.depthAttachment.isReadOnly = true;

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
