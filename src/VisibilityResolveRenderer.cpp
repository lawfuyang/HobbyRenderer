#include "Renderer.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/VisibilityResolve.h"

extern RGTextureHandle g_RG_VisibilityBuffer;
extern RGTextureHandle g_RG_GBufferAlbedo;

// Reads the ids the opaque cluster bucket rasterised and writes them back into the G-buffer. Today the resolve is
// the id visualisation: one hash colour per id into albedo, so the ids can be read before the material work lands.
// The declarations do not change when the body does - the pass already reads the id buffer and writes a G-buffer
// target, which is the edge that orders the deferred pass behind it.
class VisibilityResolveRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        if (!g_Renderer.IsVisibilityBufferActive())
            return false;

        renderGraph.ReadTexture(g_RG_VisibilityBuffer);
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_GPU_SCOPED("VisibilityResolve", commandList);

        nvrhi::TextureHandle visibility = renderGraph.GetTexture(g_RG_VisibilityBuffer, RGResourceAccessMode::Read);
        nvrhi::TextureHandle albedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);

        srrhi::VisibilityResolveInputs inputs;
        inputs.m_ResolveConstants.SetWidth((uint32_t)visibility->getDesc().width);
        inputs.m_ResolveConstants.SetHeight((uint32_t)visibility->getDesc().height);
        inputs.SetVisibilityBuffer(visibility);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(albedo);
        const nvrhi::FramebufferHandle framebuffer = g_Renderer.m_RHI->m_NvrhiDevice->createFramebuffer(fbDesc);

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderID = ShaderID::VISIBILITYRESOLVE_PSMAIN,
            .bindingSetDesc = Renderer::CreateBindingSetDesc(inputs),
            .pushConstants = &inputs.m_ResolveConstants,
            .pushConstantsSize = srrhi::VisibilityResolveInputs::PushConstantBytes,
            .framebuffer = framebuffer
        };

        g_Renderer.AddFullScreenPass(params);
    }

    const char* GetName() const override { return "Visibility Resolve"; }
};

REGISTER_RENDERER(VisibilityResolveRenderer);
