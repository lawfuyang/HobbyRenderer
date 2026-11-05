#include "Renderer.h"

#include "CommonResources.h"
#include "shaders/srrhi/cpp/OmmDebug.h"

// Draws the texel classification of one baked alpha texture over the back buffer, so the micromap input can be
// compared with what the shadow rays resolve. Colours come from the uploaded texture: green = opaque,
// red = transparent, yellow = mixed (a 2x2 footprint straddling the alpha cutoff).
class OmmDebugRenderer : public IRenderer
{
public:
    void Initialize() override {}

    bool Setup(RenderGraph& renderGraph) override
    {
        return g_Renderer.m_OmmDebugMode == 1 && g_Renderer.m_OmmDebugTexture;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        if (g_Renderer.m_OmmDebugMode != 1 || !g_Renderer.m_OmmDebugTexture)
            return;

        nvrhi::TextureHandle backbuffer = g_Renderer.GetCurrentBackBufferTexture();
        nvrhi::FramebufferHandle framebuffer = g_Renderer.m_RHI->m_NvrhiDevice->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(backbuffer));

        srrhi::OmmDebugInputs inputs;
        inputs.SetSrcClass(g_Renderer.m_OmmDebugTexture);

        nvrhi::BindingSetDesc bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().PointClamp));

        // A corner panel rather than the whole screen, so the scene the masks feed stays visible next to it.
        const float size = 700.0f;
        const float margin = 10.0f;
        const float bbWidth = (float)backbuffer->getDesc().width;
        const nvrhi::Viewport viewport(bbWidth - size - margin, bbWidth - margin, margin, margin + size, 0.0f, 1.0f);

        Renderer::RenderPassParams params;
        params.commandList   = commandList;
        params.shaderID      = ShaderID::OMMDEBUG_PSMAIN;
        params.bindingSetDesc = bindingSetDesc;
        params.framebuffer   = framebuffer;
        params.viewport      = &viewport;
        g_Renderer.AddFullScreenPass(params);
    }

    const char* GetName() const override { return "OmmDebug"; }
};

REGISTER_RENDERER(OmmDebugRenderer);
