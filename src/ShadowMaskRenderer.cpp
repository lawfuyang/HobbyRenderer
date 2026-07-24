#include "Renderer.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/ShadowMask.h"

// ---------------------------------------------------------------------------
// Render Graph handles — g_RG_CSMShadowMap is defined in ShadowRenderer.cpp
// ---------------------------------------------------------------------------
extern RGTextureHandle g_RG_CSMShadowMap;
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_CSMShadowMapMips;
RGTextureHandle        g_RG_ShadowMask;
RGTextureHandle        g_RG_ShadowDebugOutput;  // RGBA32_FLOAT PCSS debug data, read by CSMDebugRenderer

// ---------------------------------------------------------------------------
// ShadowMaskRenderer — fullscreen compute: CSM evaluation → R8_UNORM mask
//
// When m_EnablePCSS=true, two dispatches are issued:
//   1. ShadowMask_CSMain (PCSS=1)  — stochastic PCSS → raw shadow mask
//   2. ShadowMaskTemporal_CSMain   — temporal resolve → final mask + history
// ---------------------------------------------------------------------------
class ShadowMaskRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        if (g_Renderer.m_Mode != RenderingMode::NormalBasic)
            return false;

        auto [width, height] = g_Renderer.SwapchainSize();

        // Declare the R8_UNORM shadow mask (transient, written each frame)
        RGTextureDesc maskDesc;
        maskDesc.m_NvrhiDesc.width            = width;
        maskDesc.m_NvrhiDesc.height           = height;
        maskDesc.m_NvrhiDesc.format           = nvrhi::Format::R8_UNORM;
        maskDesc.m_NvrhiDesc.isUAV            = true;
        maskDesc.m_NvrhiDesc.debugName        = "ShadowMask_RG";
        maskDesc.m_NvrhiDesc.initialState     = nvrhi::ResourceStates::UnorderedAccess;
        maskDesc.m_NvrhiDesc.keepInitialState = true;
        renderGraph.DeclareTexture(maskDesc, g_RG_ShadowMask);

        // Declare the RGBA32_FLOAT PCSS debug texture (always declared so CSMDebugRenderer can read it)
        RGTextureDesc dbgDesc;
        dbgDesc.m_NvrhiDesc.width            = width;
        dbgDesc.m_NvrhiDesc.height           = height;
        dbgDesc.m_NvrhiDesc.format           = nvrhi::Format::RGBA32_FLOAT;
        dbgDesc.m_NvrhiDesc.isUAV            = true;
        dbgDesc.m_NvrhiDesc.debugName        = "ShadowDebugOutput_RG";
        dbgDesc.m_NvrhiDesc.initialState     = nvrhi::ResourceStates::UnorderedAccess;
        dbgDesc.m_NvrhiDesc.keepInitialState = true;
        renderGraph.DeclareTexture(dbgDesc, g_RG_ShadowDebugOutput);

        // Declare the persistent R8_UNORM shadow history (survives across frames)
        if (g_Renderer.m_EnablePCSSShadowTemporal)
        {
            RGTextureDesc histDesc = maskDesc;
            histDesc.m_NvrhiDesc.debugName = "ShadowHistory_RG";
            m_HistoryIsNew = renderGraph.DeclarePersistentTexture(histDesc, m_RG_ShadowHistory);
        }

        // If CSM shadows are disabled, only the clear in Render() is needed
        if (!g_Renderer.m_EnableCSMShadows)
            return true;

        renderGraph.ReadTexture(g_RG_CSMShadowMap);
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferNormals);

        if (g_Renderer.m_EnablePCSS)
        {
            renderGraph.ReadTexture(g_RG_GBufferMotionVectors);
            if (g_Renderer.m_EnablePCSSShadowDepthMips)
                renderGraph.ReadTexture(g_RG_CSMShadowMapMips);
        }

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        PROFILE_GPU_SCOPED("ShadowMaskRenderer", commandList);

        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;

        nvrhi::TextureHandle shadowMask = renderGraph.GetTexture(g_RG_ShadowMask, RGResourceAccessMode::Write);
        nvrhi::TextureHandle debugOutput = renderGraph.GetTexture(g_RG_ShadowDebugOutput, RGResourceAccessMode::Write);

        // If CSM shadows are disabled, clear to white (fully lit) and return
        if (!g_Renderer.m_EnableCSMShadows)
        {
            commandList->clearTextureFloat(shadowMask, nvrhi::AllSubresources, nvrhi::Color{ 1.0f });
            return;
        }

        auto [width, height] = g_Renderer.SwapchainSize();

        // -----------------------------------------------------------------------
        // Build constant buffer
        // -----------------------------------------------------------------------
        const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(
            sizeof(srrhi::ShadowMaskConstants), "ShadowMaskCB", 1);
        const nvrhi::BufferHandle shadowMaskCB = device->createBuffer(cbDesc);

        srrhi::ShadowMaskConstants cb;

        Matrix allVPs[4];
        for (uint32_t i = 0; i < 4; i++)
            allVPs[i] = g_Renderer.m_CSMCascades[i].m_ViewProj;
        cb.SetShadowViewProj(allVPs);

        cb.SetClipToWorld(g_Renderer.m_Scene.m_View.m_MatClipToWorld);
        cb.SetWorldToView(g_Renderer.m_Scene.m_View.m_MatWorldToView);
        cb.SetCascadeSplits(Vector4{
            g_Renderer.m_CSMCascadeSplits[1],
            g_Renderer.m_CSMCascadeSplits[2],
            g_Renderer.m_CSMCascadeSplits[3],
            g_Renderer.m_CSMCascadeSplits[4]
        });
        cb.SetOutputSize(Vector2{ (float)width, (float)height });
        cb.SetNormalBias(g_Renderer.m_CSMNormalBias);
        cb.SetEnableCascadeBlend(g_Renderer.m_EnableCascadeBlend ? 1u : 0u);

        if (g_Renderer.m_EnablePCSS)
        {
            // Precompute per-cascade UV blocker search radius on the CPU
            // searchRadius[i] = tan(lightAngularRadius) * kSearchScale / shadowMapResolution
            // For an ortho projection: worldTexelSize = 2 / (resolution * |VP_row0|)
            // searchRadiusUV = lightRadiusWorld * worldTexelSize
            const float lightAngularRadius = g_Renderer.m_Scene.m_Lights.empty()
                ? 0.00465f  // ~0.267 deg, half of sun's 0.533 deg
                : g_Renderer.m_Scene.m_Lights.back().m_AngularSize * 0.5f;
            // Search radius = tan(lightAngle) * sceneBoundingRadius:
            // the maximum penumbra half-width for an occluder at the scene's far edge.
            const float lightRadiusWorld = tanf(lightAngularRadius) * g_Renderer.m_Scene.GetSceneBoundingRadius();

            Vector4 searchRadii{ 0.0f, 0.0f, 0.0f, 0.0f };
            for (uint32_t i = 0; i < 3; ++i)  // cascades 0-2; cascade 3 uses fixed PCF
            {
                const Matrix& vp = g_Renderer.m_CSMCascades[i].m_ViewProj;
                const float vpRow0Len = sqrtf(vp._11 * vp._11 + vp._21 * vp._21 + vp._31 * vp._31);
                const float worldTexelSize = 2.0f / (
                    (float)srrhi::CommonConsts::kShadowMapResolution * std::max(vpRow0Len, 1e-10f));
                (&searchRadii.x)[i] = lightRadiusWorld * worldTexelSize;
            }
            cb.SetSearchRadii(searchRadii);
            cb.SetLightAngularRadius(lightAngularRadius);
            cb.SetFrameIndex(g_Renderer.m_FrameNumber);
        }

        cb.SetCSMDebugMode(g_Renderer.m_CSMDebugMode);

        commandList->writeBuffer(shadowMaskCB, &cb, sizeof(cb), 0);

        const CommonResources& cr = CommonResources::GetInstance();

        // -----------------------------------------------------------------------
        // Resolve textures
        // -----------------------------------------------------------------------
        nvrhi::TextureHandle depth     = renderGraph.GetTexture(g_RG_DepthTexture,   RGResourceAccessMode::Read);
        nvrhi::TextureHandle normals   = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Read);
        nvrhi::TextureHandle shadowMap = renderGraph.GetTexture(g_RG_CSMShadowMap,   RGResourceAccessMode::Read);
        nvrhi::TextureHandle motionVectors = g_Renderer.m_EnablePCSS ? renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read) : cr.DummySRVTexture;
        nvrhi::TextureHandle history = g_Renderer.m_EnablePCSSShadowTemporal  ? renderGraph.GetTexture(m_RG_ShadowHistory, RGResourceAccessMode::Write) : cr.DummyUAVTexture4;
        nvrhi::TextureHandle shadowMapMips = g_Renderer.m_EnablePCSSShadowDepthMips
            ? renderGraph.GetTexture(g_RG_CSMShadowMapMips, RGResourceAccessMode::Read)
            : cr.DummySRVTextureArray;

        // -----------------------------------------------------------------------
        // Dispatch 1 — shadow mask (PCSS or fixed PCF)
        // -----------------------------------------------------------------------
        {
            srrhi::ShadowMaskInputs inputs;
            inputs.SetCB(shadowMaskCB);
            inputs.SetDepth(depth);
            inputs.SetGBufferNormals(normals);
            inputs.SetCSMShadowMap(shadowMap);
            inputs.SetRWShadowMask(shadowMask, 0);
            inputs.SetRWShadowMask(shadowMask, 0);
            inputs.SetShadowSampler(cr.ShadowComparison);
            inputs.SetShadowSamplerPoint(cr.ShadowSamplerPoint);
            inputs.SetSamplerMinReduction(cr.MinReductionClamp);
            inputs.SetSamplerLinearClamp(cr.LinearClamp);

            if (g_Renderer.m_EnablePCSS)
            {
                inputs.SetCSMShadowMapMips(shadowMapMips);
                inputs.SetBlueNoiseTex(cr.BlueNoiseTex);
                // MotionVectors and ShadowHistory are only consumed by dispatch 2,
                // but the binding set must be complete — bind dummies for dispatch 1.
                inputs.SetMotionVectors(motionVectors);
                inputs.SetRWShadowHistory(history, 0);
                inputs.SetRWDebugOutput(debugOutput, 0);
            }
            else
            {
                // PCSS=0: fill new slots with dummies so the binding set is valid
                inputs.SetCSMShadowMapMips(cr.DummySRVTextureArray);
                inputs.SetBlueNoiseTex(cr.DummySRVTexture);
                inputs.SetMotionVectors(cr.DummySRVTexture);
                inputs.SetRWShadowHistory(cr.DummyUAVTexture, 0);
                inputs.SetRWDebugOutput(cr.DummyUAVTexture4, 0);
                inputs.SetRWDebugOutput(cr.DummyUAVTexture, 0);
            }

            // Select PCSS × CASCADE_BLEND permutation
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
            params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
            params.dispatchParams = {
                .x = DivideAndRoundUp(width,  8u),
                .y = DivideAndRoundUp(height, 8u),
                .z = 1u
            };
            g_Renderer.AddComputePass(params);
        }

        // -----------------------------------------------------------------------
        // Dispatch 2 — temporal resolve (PCSS only)
        // -----------------------------------------------------------------------
        if (g_Renderer.m_EnablePCSS && g_Renderer.m_EnablePCSSShadowTemporal)
        {
            // On the very first frame, clear history to 1.0 (fully lit) to avoid
            // blending against uninitialised data.
            if (m_HistoryIsNew)
            {
                commandList->clearTextureFloat(history, nvrhi::AllSubresources, nvrhi::Color{ 1.0f });
                m_HistoryIsNew = false;
            }

            srrhi::ShadowMaskInputs inputs;
            inputs.SetCB(shadowMaskCB);
            inputs.SetDepth(depth);
            inputs.SetGBufferNormals(normals);
            inputs.SetCSMShadowMap(shadowMap);
            inputs.SetCSMShadowMapMips(shadowMapMips);
            inputs.SetBlueNoiseTex(cr.BlueNoiseTex);
            inputs.SetMotionVectors(motionVectors);
            inputs.SetRWShadowMask(shadowMask, 0);
            inputs.SetRWShadowHistory(history, 0);
            inputs.SetRWDebugOutput(cr.DummyUAVTexture4, 0);
            inputs.SetShadowSampler(cr.ShadowComparison);
            inputs.SetShadowSamplerPoint(cr.ShadowSamplerPoint);
            inputs.SetSamplerMinReduction(cr.MinReductionClamp);
            inputs.SetSamplerLinearClamp(cr.LinearClamp);

            Renderer::RenderPassParams params;
            params.commandList    = commandList;
            params.shaderID       = ShaderID::SHADOWMASK_SHADOWMASKTEMPORAL_CSMAIN_TEMPORAL;
            params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
            params.dispatchParams = {
                .x = DivideAndRoundUp(width,  8u),
                .y = DivideAndRoundUp(height, 8u),
                .z = 1u
            };
            g_Renderer.AddComputePass(params);
        }
    }

    const char* GetName() const override { return "ShadowMask"; }

private:
    RGTextureHandle m_RG_ShadowHistory;
    bool            m_HistoryIsNew = true;
};

REGISTER_RENDERER(ShadowMaskRenderer);
