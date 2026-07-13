#include "Renderer.h"
#include "FeedbackManager.h"

#include "CommonResources.h"
#include "shaders/srrhi/cpp/TileResidencyDebug.h"

// Renders a tile‑residency debug overlay: for the selected streamable texture,
// blits each mip level of its reserved tiled‑resource texture to the back‑buffer
// at decreasing sizes.  Unmapped tiles naturally show as black (D3D12 behaviour).
class TileResidencyDebugRenderer : public IRenderer
{
public:
    void Initialize() override {}

    bool Setup(RenderGraph& renderGraph) override
    {
        const int texIdx = g_Renderer.m_TileResidencyDebugTextureIdx;
        if (texIdx < 0) return false;

        nvfeedback::FeedbackTexture* feedbackTex = g_Renderer.m_FeedbackManager->GetTextureByIndex((uint32_t)texIdx);
        if (!feedbackTex) return false;

        nvrhi::TextureHandle reservedTex = feedbackTex->GetReservedTexture();
        if (!reservedTex) return false;

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        const int texIdx = g_Renderer.m_TileResidencyDebugTextureIdx;
        if (texIdx < 0) return;

        nvfeedback::FeedbackTexture* feedbackTex = g_Renderer.m_FeedbackManager->GetTextureByIndex((uint32_t)texIdx);
        if (!feedbackTex) return;

        nvrhi::TextureHandle reservedTex = feedbackTex->GetReservedTexture();
        if (!reservedTex) return;

        const nvrhi::TextureDesc& texDesc = reservedTex->getDesc();
        const uint32_t numMips = std::min(texDesc.mipLevels, 8u);

        nvrhi::TextureHandle backbuffer = g_Renderer.GetCurrentBackBufferTexture();
        nvrhi::FramebufferHandle framebuffer = g_Renderer.m_RHI->m_NvrhiDevice->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(backbuffer));

        const float bbHeight = (float)backbuffer->getDesc().height;

        float size = 400.0f;
        const float margin = 10.0f;
        float x = margin;

        for (uint32_t mip = 0; mip < numMips; mip++)
        {
            // Build srrhi inputs: texture SRV at t0 + point-clamp sampler at s0
            srrhi::TileResidencyDebugInputs tdInputs;
            tdInputs.SetSrcTexture(reservedTex, (int32_t)mip, 1);

            nvrhi::BindingSetDesc bindingSetDesc = Renderer::CreateBindingSetDesc(tdInputs);
            bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().PointClamp));

            const nvrhi::BindingLayoutHandle layout = g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(bindingSetDesc, 0);
            const nvrhi::BindingSetHandle bindingSet = g_Renderer.m_RHI->m_NvrhiDevice->createBindingSet(bindingSetDesc, layout);

            nvrhi::MeshletPipelineDesc pipelineDesc;
            pipelineDesc.MS = g_Renderer.GetShaderHandle(ShaderID::FULLSCREEN_MSMAIN);
            pipelineDesc.PS = g_Renderer.GetShaderHandle(ShaderID::TILERESIDENCYDEBUG_PSMAIN);
            pipelineDesc.bindingLayouts = { layout };
            pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            pipelineDesc.renderState.depthStencilState = CommonResources::GetInstance().DepthDisabled;

            nvrhi::MeshletPipelineHandle pipeline = g_Renderer.GetOrCreateMeshletPipeline(pipelineDesc, framebuffer->getFramebufferInfo());

            nvrhi::Viewport viewport(
                x, x + size,
                bbHeight - size - margin, bbHeight - margin,
                0.0f, 1.0f);

            nvrhi::Rect scissorRect(
                (int)x, (int)(x + size),
                (int)(bbHeight - size - margin), (int)(bbHeight - margin));

            nvrhi::MeshletState state;
            state.pipeline    = pipeline;
            state.framebuffer = framebuffer;
            state.bindings    = { bindingSet };
            state.viewport.viewports.push_back(viewport);
            state.viewport.scissorRects.push_back(scissorRect);

            commandList->setMeshletState(state);
            commandList->dispatchMesh(1, 1, 1);

            x += size + margin;
            size /= 2.0f;
        }
    }

    const char* GetName() const override { return "TileResidencyDebug"; }
};

REGISTER_RENDERER(TileResidencyDebugRenderer);
