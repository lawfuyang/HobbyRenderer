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
extern RGTextureHandle g_RG_SHARCIndirect;       // SHARCQuery output — screen-space indirect radiance
extern RGTextureHandle g_RG_ShadowMask;          // ShadowMaskRenderer output — R8_UNORM screen-space shadow mask (NormalBasic only)
extern RGTextureHandle g_RG_CSMDebugOutput;       // CSMDebugRenderer output — CSM debug overlay (RGBA16_FLOAT; black when off)



class DeferredRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferAlbedo);
        renderGraph.ReadTexture(g_RG_GBufferNormals);
        renderGraph.ReadTexture(g_RG_GBufferORM);
        renderGraph.ReadTexture(g_RG_GBufferEmissive);
        renderGraph.ReadTexture(g_RG_GBufferMotionVectors);

        renderGraph.WriteTexture(g_RG_HDRColor);

        // Conditionally read the RTXDI composited output when ReSTIR DI is enabled
        if (g_Renderer.m_EnableReSTIRDI)
            renderGraph.ReadTexture(g_RG_RTXDIDIComposited);

        // Conditionally read the SHARC indirect output when SHARC is the selected technique
        if (g_Renderer.m_IndirectLightingTechnique == srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_SHARC)
            renderGraph.ReadTexture(g_RG_SHARCIndirect);

        // Conditionally read the CSM shadow mask in NormalBasic mode
        if (g_Renderer.m_Mode == RenderingMode::NormalBasic)
            renderGraph.ReadTexture(g_RG_ShadowMask);

        // Conditionally read the CSM debug overlay when CSM debug mode is active
        if (g_Renderer.m_CSMDebugMode != 0 && g_Renderer.m_EnableCSMShadows)
            renderGraph.ReadTexture(g_RG_CSMDebugOutput);

        return true;
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        
        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;

        nvrhi::TextureHandle depthTexture = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferAlbedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferNormals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferORM = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferEmissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Read);
        nvrhi::TextureHandle gbufferMotionVectors = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read);
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);

        const Vector3 camPos = g_Renderer.m_Scene.m_Camera.GetPosition();
        float skyVisFarPlane = g_Renderer.m_Scene.GetSceneBoundingRadius();


        // Deferred CB
        const nvrhi::BufferDesc deferredCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::DeferredLightingConstants), "DeferredCB", 1);
        const nvrhi::BufferHandle deferredCB = device->createBuffer(deferredCBD);

        srrhi::DeferredLightingConstants dcb;
        dcb.SetView(g_Renderer.m_Scene.m_View);
        dcb.SetCameraPos(DirectX::XMFLOAT4{ camPos.x, camPos.y, camPos.z, 1.0f });
        dcb.SetSunDirection(g_Renderer.m_Scene.GetSunDirection());
        dcb.SetEnableSky(g_Renderer.m_EnableSky ? 1 : 0);
        dcb.SetRenderingMode((uint32_t)g_Renderer.m_Mode);
        dcb.SetRadianceMipCount(CommonResources::GetInstance().m_RadianceMipCount);
        dcb.SetLightCount(g_Renderer.m_Scene.m_LightCount);
        // Disable RT shadows in NormalBasic — shadows come from the CSM shadow mask instead
        dcb.SetEnableRTShadows((g_Renderer.m_Mode != RenderingMode::NormalBasic) && g_Renderer.m_EnableRTShadows ? 1 : 0);
        dcb.SetDebugMode(g_Renderer.m_DebugMode);
        dcb.SetUseReSTIRDI(g_Renderer.m_EnableReSTIRDI ? 1u : 0u);
        dcb.SetUseReSTIRDIDenoised(0u); // compositing is done by CompositingPass
        dcb.SetIndirectLightingMode(g_Renderer.m_IndirectLightingTechnique);
        dcb.SetCSMDebugMode(g_Renderer.m_CSMDebugMode);
        commandList->writeBuffer(deferredCB, &dcb, sizeof(dcb), 0);

        // t8: RTXDI composited output (DI + emissive, already remodulated by CompositingPass)
        nvrhi::TextureHandle rtxdiComposited = g_Renderer.m_EnableReSTIRDI
            ? renderGraph.GetTexture(g_RG_RTXDIDIComposited, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;

        // t14: SHARC indirect radiance (written by SHARCQuery pass; black fallback when SHARC is inactive)
        nvrhi::TextureHandle sharcIndirect =
            (g_Renderer.m_IndirectLightingTechnique == srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_SHARC)
            ? renderGraph.GetTexture(g_RG_SHARCIndirect, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;

        srrhi::DeferredLightingInputs dlInputs;
        dlInputs.SetDeferredCB(deferredCB);
        dlInputs.SetGBufferAlbedo(gbufferAlbedo);
        dlInputs.SetGBufferNormals(gbufferNormals);
        dlInputs.SetGBufferORM(gbufferORM);
        dlInputs.SetGBufferEmissive(gbufferEmissive);
        dlInputs.SetGBufferMotion(gbufferMotionVectors);
        dlInputs.SetDepth(depthTexture);
        dlInputs.SetSceneAS(g_Renderer.m_Scene.m_TLAS);
        dlInputs.SetLights(g_Renderer.m_Scene.m_LightBuffer);
        dlInputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
        dlInputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
        dlInputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
        dlInputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
        dlInputs.SetIndices(g_Renderer.m_Scene.m_IndexBuffer);
        dlInputs.SetRTXDIDIComposited(rtxdiComposited);
        dlInputs.SetSHARCIndirect(sharcIndirect);

        // t15: CSM shadow mask (R8_UNORM; white = fully lit fallback when not in NormalBasic)
        nvrhi::TextureHandle shadowMask =
            (g_Renderer.m_Mode == RenderingMode::NormalBasic)
            ? renderGraph.GetTexture(g_RG_ShadowMask, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureWhite;
        dlInputs.SetShadowMask(shadowMask);

        // t16: CSM debug overlay (RGBA16_FLOAT; black fallback when CSM debug is off)
        nvrhi::TextureHandle csmDebugOutput =
            (g_Renderer.m_CSMDebugMode != 0 && g_Renderer.m_EnableCSMShadows)
            ? renderGraph.GetTexture(g_RG_CSMDebugOutput, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;
        dlInputs.SetCSMDebugOutput(csmDebugOutput);

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
            .shaderID = ShaderID::DEFERREDLIGHTING_DEFERREDLIGHTING_PSMAIN,
            .bindingSetDesc = bset,
            .framebuffer = framebuffer,
            .depthStencilState = &ds
        };

        g_Renderer.AddFullScreenPass(params);
    }

    const char* GetName() const override { return "Deferred"; }
};

REGISTER_RENDERER(DeferredRenderer);
