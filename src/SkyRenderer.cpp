#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "shaders/ShaderShared.h"

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

        return true;
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::utils::ScopedMarker marker(commandList, "Sky Pass");

        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);

        const Vector3 camPos = renderer->m_Scene.m_Camera.GetPosition();

        // Sky CB
        const nvrhi::BufferDesc skyCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(SkyConstants), "SkyCB", 1);
        const nvrhi::BufferHandle skyCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(skyCBD);

        SkyConstants scb{};
        scb.m_View = renderer->m_Scene.m_View;
        scb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 1.0f };
        scb.m_SunDirection = renderer->m_Scene.m_SunDirection;
        scb.m_SunIntensity = renderer->m_Scene.GetSunIntensity();
        scb.m_RenderingMode = (uint32_t)renderer->m_Mode;
        
        commandList->writeBuffer(skyCB, &scb, sizeof(scb), 0);

        nvrhi::BindingSetDesc bset;
        bset.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, skyCB),
        };

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(hdrColor);

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
            .useBindlessResources = true,
            .framebuffer = framebuffer,
            .depthStencilState = &ds
        };

        renderer->AddFullScreenPass(params);
    }

    const char* GetName() const override { return "Sky"; }
};

REGISTER_RENDERER(SkyRenderer);
