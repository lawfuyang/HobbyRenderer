#include "Renderer.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/ShadowMask.h"

// ---------------------------------------------------------------------------
// Render Graph handles — g_RG_CSMShadowMap is defined in ShadowRenderer.cpp
// ---------------------------------------------------------------------------
extern RGTextureHandle g_RG_CSMShadowMap;
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferNormals;
RGTextureHandle        g_RG_ShadowMask;

// ---------------------------------------------------------------------------
// ShadowMaskRenderer — fullscreen compute: CSM evaluation → R8_UNORM mask
// ---------------------------------------------------------------------------
class ShadowMaskRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        if (g_Renderer.m_Mode != RenderingMode::NormalBasic)
            return false;

        // Declare the R8_UNORM shadow mask at screen resolution
        auto [width, height] = g_Renderer.SwapchainSize();

        RGTextureDesc maskDesc;
        maskDesc.m_NvrhiDesc.width        = width;
        maskDesc.m_NvrhiDesc.height       = height;
        maskDesc.m_NvrhiDesc.format       = nvrhi::Format::R8_UNORM;
        maskDesc.m_NvrhiDesc.isUAV        = true;
        maskDesc.m_NvrhiDesc.debugName    = "ShadowMask_RG";
        maskDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        maskDesc.m_NvrhiDesc.keepInitialState = true;
        renderGraph.DeclareTexture(maskDesc, g_RG_ShadowMask);

        // if CSM shadows are disabled, we will only clear the shadow mask to white (fully lit) in Render(), so no need to read any other resources
        if (!g_Renderer.m_EnableCSMShadows)
        {
            return true;
        }

        renderGraph.ReadTexture(g_RG_CSMShadowMap);
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferNormals);
        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        PROFILE_GPU_SCOPED("ShadowMaskRenderer", commandList);

        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;

        nvrhi::TextureHandle shadowMask = renderGraph.GetTexture(g_RG_ShadowMask, RGResourceAccessMode::Write);

        // If CSM shadows are disabled, just clear the shadow mask to white (fully lit) and return early
        if (!g_Renderer.m_EnableCSMShadows)
        {
            commandList->clearTextureFloat(shadowMask, nvrhi::AllSubresources, nvrhi::Color{ 1.0f }); // white = fully lit
            return;
        }

        // Build ShadowMaskCB
        const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::ShadowMaskConstants), "ShadowMaskCB", 1);
        const nvrhi::BufferHandle shadowMaskCB = device->createBuffer(cbDesc);

        srrhi::ShadowMaskConstants cb;
        // SetShadowViewProj takes a pointer to all 4 matrices packed (256 bytes)
        Matrix allVPs[4];
        for (uint32_t i = 0; i < 4; i++)
            allVPs[i] = g_Renderer.m_CSMCascades[i].m_ViewProj;
        cb.SetShadowViewProj(allVPs);

        cb.SetClipToWorld(g_Renderer.m_Scene.m_View.m_MatClipToWorld);
        cb.SetWorldToView(g_Renderer.m_Scene.m_View.m_MatWorldToView);

        // Cascade splits: far depths of cascades 0-3 (indices 1..4 of m_CSMCascadeSplits)
        cb.SetCascadeSplits(Vector4{
            g_Renderer.m_CSMCascadeSplits[1],
            g_Renderer.m_CSMCascadeSplits[2],
            g_Renderer.m_CSMCascadeSplits[3],
            g_Renderer.m_CSMCascadeSplits[4]
        });

        auto [width, height] = g_Renderer.SwapchainSize();
        cb.SetOutputSize(Vector2{ (float)width, (float)height });
        cb.SetNormalBias(g_Renderer.m_CSMNormalBias);
        cb.SetEnableCascadeBlend(g_Renderer.m_EnableCascadeBlend ? 1u : 0u);

        commandList->writeBuffer(shadowMaskCB, &cb, sizeof(cb), 0);

        // Resolve textures
        nvrhi::TextureHandle depth      = renderGraph.GetTexture(g_RG_DepthTexture,    RGResourceAccessMode::Read);
        nvrhi::TextureHandle normals    = renderGraph.GetTexture(g_RG_GBufferNormals,  RGResourceAccessMode::Read);
        nvrhi::TextureHandle shadowMap  = renderGraph.GetTexture(g_RG_CSMShadowMap,    RGResourceAccessMode::Read);

        // Build binding set
        srrhi::ShadowMaskInputs inputs;
        inputs.SetCB(shadowMaskCB);
        inputs.SetDepth(depth);
        inputs.SetGBufferNormals(normals);
        inputs.SetCSMShadowMap(shadowMap);
        inputs.SetRWShadowMask(shadowMask, 0);
        inputs.SetShadowSampler(CommonResources::GetInstance().ShadowComparison);
        inputs.SetPointSampler(CommonResources::GetInstance().PointClamp);

        nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

        // Select shader permutation based on toggles
        uint32_t shaderID;
        if (g_Renderer.m_EnablePCSS && g_Renderer.m_EnableCascadeBlend)
            shaderID = ShaderID::SHADOWMASK_SHADOWMASK_CSMAIN_PCSS_BLEND_CASCADE_BLEND_1_PCSS_1;
        else if (g_Renderer.m_EnablePCSS)
            shaderID = ShaderID::SHADOWMASK_SHADOWMASK_CSMAIN_PCSS_PCSS_1;
        else if (g_Renderer.m_EnableCascadeBlend)
            shaderID = ShaderID::SHADOWMASK_SHADOWMASK_CSMAIN_BLEND_CASCADE_BLEND_1;
        else
            shaderID = ShaderID::SHADOWMASK_SHADOWMASK_CSMAIN;

        Renderer::RenderPassParams params;
        params.commandList    = commandList;
        params.shaderID       = shaderID;
        params.bindingSetDesc = bset;
        params.dispatchParams = {
            .x = DivideAndRoundUp(width,  8u),
            .y = DivideAndRoundUp(height, 8u),
            .z = 1u
        };
        g_Renderer.AddComputePass(params);
    }

    const char* GetName() const override { return "ShadowMask"; }
};

REGISTER_RENDERER(ShadowMaskRenderer);
