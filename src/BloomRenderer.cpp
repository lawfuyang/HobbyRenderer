#include "Renderer.h"
#include "Config.h"
#include "Utilities.h"
#include "CommonResources.h"
#include "shaders/ShaderShared.h"

extern RGTextureHandle g_RG_HDRColor;
RGTextureHandle g_RG_BloomDownPyramid;
RGTextureHandle g_RG_BloomUpPyramid;

static constexpr uint32_t kBloomMipCount = 6;

class BloomRenderer : public IRenderer
{
public:

    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (!renderer->m_EnableBloom || renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

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
        renderGraph.DeclareTexture(desc, g_RG_BloomDownPyramid);

        desc.m_NvrhiDesc.debugName = "Bloom_UpPyramid_RG";
        renderGraph.DeclareTexture(desc, g_RG_BloomUpPyramid);

        renderGraph.ReadTexture(g_RG_HDRColor);

        return true;
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::DeviceHandle device = renderer->m_RHI->m_NvrhiDevice;

        nvrhi::utils::ScopedMarker marker(commandList, "Bloom");

        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;
        
        nvrhi::TextureHandle bloomDownPyramid = renderGraph.GetTexture(g_RG_BloomDownPyramid, RGResourceAccessMode::Write);
        nvrhi::TextureHandle bloomUpPyramid = renderGraph.GetTexture(g_RG_BloomUpPyramid, RGResourceAccessMode::Write);
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Read);

        // 1. Prefilter (HDR -> Down[0])
        {
            BloomConstants consts{};
            consts.m_Knee = renderer->m_BloomKnee;
            consts.m_Strength = 1.0f;
            consts.m_Width = width / 2;
            consts.m_Height = height / 2;

            nvrhi::BindingSetDesc bset;
            bset.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)),
                nvrhi::BindingSetItem::Texture_SRV(0, hdrColor)
            };

            nvrhi::FramebufferHandle fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bloomDownPyramid, nvrhi::TextureSubresourceSet(0, 1, 0, 1)));

            commandList->clearTextureFloat(bloomDownPyramid, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::Color(0.f, 0.f, 0.f, 0.f));

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderName = "Bloom_Prefilter_PSMain",
                .bindingSetDesc = bset,
                .pushConstants = &consts,
                .pushConstantsSize = sizeof(consts),
                .framebuffer = fb
            };
            renderer->AddFullScreenPass(params);
        }

        // 2. Downsample chain (Down[i-1] -> Down[i])
        for (uint32_t i = 1; i < kBloomMipCount; ++i)
        {
            uint32_t mipW = (width / 2) >> i;
            uint32_t mipH = (height / 2) >> i;
            if (mipW == 0 || mipH == 0) break;

            BloomConstants consts{};
            consts.m_Width = mipW;
            consts.m_Height = mipH;

            nvrhi::BindingSetDesc bset;
            bset.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)),
                nvrhi::BindingSetItem::Texture_SRV(0, bloomDownPyramid, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(i - 1, 1, 0, 1))
            };

            nvrhi::FramebufferHandle fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bloomDownPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1)));

            commandList->clearTextureFloat(bloomDownPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1), nvrhi::Color(0.f, 0.f, 0.f, 0.f));

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderName = "Bloom_Downsample_PSMain",
                .bindingSetDesc = bset,
                .pushConstants = &consts,
                .pushConstantsSize = sizeof(consts),
                .framebuffer = fb
            };
            renderer->AddFullScreenPass(params);
        }

        // 3. Upsample chain (Up[i+1] + Down[i] -> Up[i])
        // Seed the up-chain with the smallest down mip

        nvrhi::TextureSlice slice;
        slice.mipLevel = kBloomMipCount - 1;

        commandList->copyTexture(bloomUpPyramid, slice, bloomDownPyramid, slice);

        for (int i = kBloomMipCount - 2; i >= 0; --i)
        {
            uint32_t mipW = (width / 2) >> i;
            uint32_t mipH = (height / 2) >> i;

            BloomConstants consts{};
            consts.m_Width = mipW;
            consts.m_Height = mipH;
            consts.m_UpsampleRadius = renderer->m_UpsampleRadius;

            nvrhi::BindingSetDesc bset;
            bset.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)),
                nvrhi::BindingSetItem::Texture_SRV(0, bloomUpPyramid,   nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(i + 1, 1, 0, 1)),
                nvrhi::BindingSetItem::Texture_SRV(1, bloomDownPyramid, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(i,     1, 0, 1))
            };

            nvrhi::FramebufferHandle fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bloomUpPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1)));

            commandList->clearTextureFloat(bloomUpPyramid, nvrhi::TextureSubresourceSet(i, 1, 0, 1), nvrhi::Color(0.f, 0.f, 0.f, 0.f));

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderName = "Bloom_Upsample_PSMain",
                .bindingSetDesc = bset,
                .pushConstants = &consts,
                .pushConstantsSize = sizeof(consts),
                .framebuffer = fb
            };
            renderer->AddFullScreenPass(params);
        }
    }

    const char* GetName() const override { return "Bloom"; }
};

REGISTER_RENDERER(BloomRenderer);
