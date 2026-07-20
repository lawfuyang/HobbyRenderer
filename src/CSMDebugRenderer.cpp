#include "Renderer.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/CSMDebug.h"

// ---------------------------------------------------------------------------
// Render Graph handles
// ---------------------------------------------------------------------------
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_CSMShadowMap;
extern RGTextureHandle g_RG_ShadowMask;
RGTextureHandle        g_RG_CSMDebugOutput;   // RGBA16_FLOAT — CSM debug overlay, sampled by DeferredLighting

// ---------------------------------------------------------------------------
// CSMDebugRenderer — fullscreen PS debug overlay (skips when mode == Off)
// ---------------------------------------------------------------------------
class CSMDebugRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        // Always declare the CSMDebugOutput texture so DeferredLighting can read it.
        // When CSM debug is off, the texture will be cleared to black and skipped during overlay.
        auto [width, height] = g_Renderer.SwapchainSize();

        RGTextureDesc debugOutDesc;
        debugOutDesc.m_NvrhiDesc.width        = width;
        debugOutDesc.m_NvrhiDesc.height       = height;
        debugOutDesc.m_NvrhiDesc.format       = nvrhi::Format::RGBA16_FLOAT;
        debugOutDesc.m_NvrhiDesc.isRenderTarget = true;
        debugOutDesc.m_NvrhiDesc.debugName    = "CSMDebugOutput_RG";
        debugOutDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        debugOutDesc.m_NvrhiDesc.setClearValue(nvrhi::Color{ 0.0f, 0.0f, 0.0f, 0.0f });
        debugOutDesc.m_NvrhiDesc.keepInitialState = true;
        renderGraph.DeclareTexture(debugOutDesc, g_RG_CSMDebugOutput);

        // Skip the rest when debug is off — just clear to black
        if (g_Renderer.m_CSMDebugMode == 0 || !g_Renderer.m_EnableCSMShadows)
            return true;

        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferAlbedo);
        renderGraph.ReadTexture(g_RG_CSMShadowMap);
        renderGraph.ReadTexture(g_RG_ShadowMask);
        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;

        nvrhi::TextureHandle debugOutput = renderGraph.GetTexture(g_RG_CSMDebugOutput, RGResourceAccessMode::Write);

        // When CSM debug is off, clear to black and skip rendering
        if (g_Renderer.m_CSMDebugMode == 0 || !g_Renderer.m_EnableCSMShadows)
        {
            commandList->clearTextureFloat(debugOutput, nvrhi::AllSubresources, nvrhi::Color{ 0.0f, 0.0f, 0.0f, 0.0f });
            return;
        }

        // Build CSMDebugCB
        const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::CSMDebugConstants), "CSMDebugCB", 1);
        const nvrhi::BufferHandle debugCB = device->createBuffer(cbDesc);

        srrhi::CSMDebugConstants cb;
        // SetShadowViewProj takes a pointer to all 4 matrices packed (256 bytes)
        Matrix allVPs[4];
        for (uint32_t i = 0; i < 4; i++)
            allVPs[i] = g_Renderer.m_CSMCascades[i].m_ViewProj;
        cb.SetShadowViewProj(allVPs);

        cb.SetClipToView(g_Renderer.m_Scene.m_View.m_MatClipToView);
        cb.SetClipToWorld(g_Renderer.m_Scene.m_View.m_MatClipToWorld);

        cb.SetCascadeSplits(Vector4{
            g_Renderer.m_CSMCascadeSplits[1],
            g_Renderer.m_CSMCascadeSplits[2],
            g_Renderer.m_CSMCascadeSplits[3],
            g_Renderer.m_CSMCascadeSplits[4]
        });

        auto [width, height] = g_Renderer.SwapchainSize();
        cb.SetOutputSize(Vector2{ (float)width, (float)height });
        cb.SetDebugMode(g_Renderer.m_CSMDebugMode);

        commandList->writeBuffer(debugCB, &cb, sizeof(cb), 0);

        // Resolve textures
        nvrhi::TextureHandle depth      = renderGraph.GetTexture(g_RG_DepthTexture,   RGResourceAccessMode::Read);
        nvrhi::TextureHandle albedo     = renderGraph.GetTexture(g_RG_GBufferAlbedo,  RGResourceAccessMode::Read);
        nvrhi::TextureHandle shadowMap  = renderGraph.GetTexture(g_RG_CSMShadowMap,   RGResourceAccessMode::Read);
        nvrhi::TextureHandle shadowMask = renderGraph.GetTexture(g_RG_ShadowMask,     RGResourceAccessMode::Read);

        // Build binding set
        srrhi::CSMDebugInputs inputs;
        inputs.SetCSMDebugCB(debugCB);
        inputs.SetDepth(depth);
        inputs.SetGBufferAlbedo(albedo);
        inputs.SetCSMShadowMap(shadowMap);
        inputs.SetShadowMask(shadowMask);
        inputs.SetPointSampler(CommonResources::GetInstance().PointClamp);

        nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

        // Framebuffer: write into CSM debug output texture
        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(debugOutput);
        nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(fbDesc);

        Renderer::RenderPassParams params;
        params.commandList    = commandList;
        params.shaderID       = ShaderID::CSMDEBUG_CSMDEBUG_PSMAIN;
        params.bindingSetDesc = bset;
        params.framebuffer    = framebuffer;
        g_Renderer.AddFullScreenPass(params);
    }

    const char* GetName() const override { return "CSMDebug"; }
};

REGISTER_RENDERER(CSMDebugRenderer);
