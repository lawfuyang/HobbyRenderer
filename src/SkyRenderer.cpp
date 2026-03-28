#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

#include "shaders/srrhi/cpp/Sky.h"

extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_HDRColor;

class SkyRenderer : public IRenderer
{
public:

    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        if (renderer->m_Mode == RenderingMode::ReferencePathTracer)
        {
            return false;
        }

        // SkyRenderer handles both atmospheric sky (Normal mode) and IBL background (IBL mode)
        if (renderer->m_Mode == RenderingMode::Normal && !renderer->m_EnableSky)
        {
            return false;
        }

        renderGraph.WriteTexture(g_RG_HDRColor);
        renderGraph.ReadTexture(g_RG_DepthTexture);

        return true;
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        PROFILE_GPU_SCOPED("Sky Pass", commandList);

        nvrhi::TextureHandle depthTexture = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);

        const Vector3 camPos = renderer->m_Scene.m_Camera.GetPosition();

        // Sky CB
        const nvrhi::BufferDesc skyCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::SkyCB), "SkyCB", 1);
        const nvrhi::BufferHandle skyCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(skyCBD);
        
        srrhi::SkyInputs skyInputs;
        skyInputs.m_SkyCB.SetView(renderer->m_Scene.m_View);
        skyInputs.m_SkyCB.SetCameraPos(Vector4{ camPos.x, camPos.y, camPos.z, 1.0f });
        skyInputs.m_SkyCB.SetSunDirection(renderer->m_Scene.GetSunDirection());
        skyInputs.m_SkyCB.SetSunIntensity(renderer->m_Scene.GetSunIntensity());
        skyInputs.m_SkyCB.SetRenderingMode((uint32_t)renderer->m_Mode);

        commandList->writeBuffer(skyCB, &skyInputs.m_SkyCB, sizeof(skyInputs.m_SkyCB), 0);
        skyInputs.SetSkyCB(skyCB);

        nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(skyInputs);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(hdrColor);
        fbDesc.setDepthAttachment(depthTexture);
        fbDesc.depthAttachment.isReadOnly = true;

        nvrhi::FramebufferHandle framebuffer = renderer->m_RHI->m_NvrhiDevice->createFramebuffer(fbDesc);

        // Sky Pass (Stencil == 0)
        nvrhi::DepthStencilState ds;
        ds.depthTestEnable = false;
        ds.depthWriteEnable = false;
        ds.stencilEnable = true;
        ds.stencilRefValue = 0;
        ds.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Equal;

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderName = "Sky_Sky_PSMain",
            .bindingSetDesc = bset,
            .framebuffer = framebuffer,
            .depthStencilState = &ds
        };

        renderer->AddFullScreenPass(params);
    }

    const char* GetName() const override { return "Sky"; }
};

REGISTER_RENDERER(SkyRenderer);
