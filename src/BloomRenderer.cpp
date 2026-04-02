#include "Renderer.h"
#include "Config.h"
#include "Utilities.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/Bloom.h"

extern RGTextureHandle g_RG_TAAOutput;

static constexpr uint32_t kBloomMipCount = 6;

class BloomRenderer : public IRenderer
{
    RGTextureHandle m_RG_BloomDownPyramid;
    RGTextureHandle m_RG_BloomUpPyramid;

public:

    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (!renderer->m_EnableBloom) return false;

        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Create Bloom textures
        RGTextureDesc desc;
        desc.m_NvrhiDesc.width = width / 2;
        desc.m_NvrhiDesc.height = height / 2;
        desc.m_NvrhiDesc.format = nvrhi::Format::R11G11B10_FLOAT;
        desc.m_NvrhiDesc.mipLevels = kBloomMipCount;
        desc.m_NvrhiDesc.isRenderTarget = true;
        desc.m_NvrhiDesc.debugName = "Bloom_DownPyramid_RG";
        desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.m_NvrhiDesc.setClearValue(nvrhi::Color{});
        renderGraph.DeclareTexture(desc, m_RG_BloomDownPyramid);

        desc.m_NvrhiDesc.debugName = "Bloom_UpPyramid_RG";
        renderGraph.DeclareTexture(desc, m_RG_BloomUpPyramid);

        renderGraph.WriteTexture(g_RG_TAAOutput);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::DeviceHandle device = renderer->m_RHI->m_NvrhiDevice;

        PROFILE_GPU_SCOPED("Bloom", commandList);

        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        nvrhi::TextureHandle bloomDownPyramid = renderGraph.GetTexture(m_RG_BloomDownPyramid, RGResourceAccessMode::Write);
        nvrhi::TextureHandle bloomUpPyramid   = renderGraph.GetTexture(m_RG_BloomUpPyramid,   RGResourceAccessMode::Write);
        nvrhi::TextureHandle taaOutput = renderGraph.GetTexture(g_RG_TAAOutput, RGResourceAccessMode::Write);

        // 1. Prefilter (TAAOutput -> Down[0])
        {
            srrhi::BloomPrefilterInputs inputs;
            inputs.m_PrefilterConstants.SetKnee(renderer->m_BloomKnee);
            inputs.m_PrefilterConstants.SetStrength(1.0f);
            inputs.SetInputTexture(taaOutput);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);
            nvrhi::FramebufferHandle fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bloomDownPyramid, nvrhi::TextureSubresourceSet(0, 1, 0, 1)));

            commandList->clearTextureFloat(bloomDownPyramid, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::Color(0.f, 0.f, 0.f, 0.f));

            Renderer::RenderPassParams params{
                .commandList       = commandList,
                .shaderID          = ShaderID::BLOOM_PREFILTER_PSMAIN,
                .bindingSetDesc    = bset,
                .pushConstants     = &inputs.m_PrefilterConstants,
                .pushConstantsSize = srrhi::BloomPrefilterInputs::PushConstantBytes,
                .framebuffer       = fb
            };
            renderer->AddFullScreenPass(params);
        }

        // 2. Downsample chain (Down[i-1] -> Down[i])
        for (uint32_t i = 1; i < kBloomMipCount; ++i)
        {
            uint32_t mipW = (width / 2) >> i;
            uint32_t mipH = (height / 2) >> i;
            if (mipW == 0 || mipH == 0) break;

            srrhi::BloomDownsampleInputs inputs;
            inputs.m_DownsampleConstants.SetWidth(mipW);
            inputs.m_DownsampleConstants.SetHeight(mipH);
            inputs.SetInputTexture(bloomDownPyramid, static_cast<int32_t>(i - 1), 1);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);
            nvrhi::FramebufferHandle fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bloomDownPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1)));

            commandList->clearTextureFloat(bloomDownPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1), nvrhi::Color(0.f, 0.f, 0.f, 0.f));

            Renderer::RenderPassParams params{
                .commandList       = commandList,
                .shaderID          = ShaderID::BLOOM_DOWNSAMPLE_PSMAIN,
                .bindingSetDesc    = bset,
                .pushConstants     = &inputs.m_DownsampleConstants,
                .pushConstantsSize = srrhi::BloomDownsampleInputs::PushConstantBytes,
                .framebuffer       = fb
            };
            renderer->AddFullScreenPass(params);
        }

        // 3. Upsample chain (Up[i+1] + Down[i] -> Up[i])
        // Seed the up-chain with the smallest down mip
        {
            nvrhi::TextureSlice slice;
            slice.mipLevel = kBloomMipCount - 1;
            commandList->copyTexture(bloomUpPyramid, slice, bloomDownPyramid, slice);
        }

        for (int i = kBloomMipCount - 2; i >= 0; --i)
        {
            uint32_t mipW = (width / 2) >> i;
            uint32_t mipH = (height / 2) >> i;

            srrhi::BloomUpsampleInputs inputs;
            inputs.m_UpsampleConstants.SetWidth(mipW);
            inputs.m_UpsampleConstants.SetHeight(mipH);
            inputs.m_UpsampleConstants.SetUpsampleRadius(renderer->m_UpsampleRadius);
            inputs.SetSourceTexture(bloomUpPyramid,   i + 1, 1);
            inputs.SetBloomTexture(bloomDownPyramid,  i,     1);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);
            nvrhi::FramebufferHandle fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bloomUpPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1)));

            commandList->clearTextureFloat(bloomUpPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1), nvrhi::Color(0.f, 0.f, 0.f, 0.f));

            Renderer::RenderPassParams params{
                .commandList       = commandList,
                .shaderID          = ShaderID::BLOOM_UPSAMPLE_PSMAIN,
                .bindingSetDesc    = bset,
                .pushConstants     = &inputs.m_UpsampleConstants,
                .pushConstantsSize = srrhi::BloomUpsampleInputs::PushConstantBytes,
                .framebuffer       = fb
            };
            renderer->AddFullScreenPass(params);
        }

        // 4. Composite: additively blend bloom up-pyramid mip 0 into TAAOutput
        {
            PROFILE_GPU_SCOPED("Bloom Composite", commandList);

            srrhi::BloomCompositeInputs inputs;
            inputs.m_CompositeConstants.SetBloomIntensity(renderer->m_BloomIntensity);
            inputs.SetBloomTexture(bloomUpPyramid, 0, 1);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

            nvrhi::FramebufferHandle fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(taaOutput));

            nvrhi::BlendState::RenderTarget additiveBlend = CommonResources::GetInstance().BlendTargetAdditive;

            Renderer::RenderPassParams params{
                .commandList       = commandList,
                .shaderID          = ShaderID::BLOOM_COMPOSITE_PSMAIN,
                .bindingSetDesc    = bset,
                .pushConstants     = &inputs.m_CompositeConstants,
                .pushConstantsSize = srrhi::BloomCompositeInputs::PushConstantBytes,
                .framebuffer       = fb,
                .blendState        = &additiveBlend
            };
            renderer->AddFullScreenPass(params);
        }
    }

    const char* GetName() const override { return "Bloom"; }
};

REGISTER_RENDERER(BloomRenderer);
