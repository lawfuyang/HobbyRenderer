#include "Renderer.h"
#include "Config.h"
#include "Utilities.h"
#include "CommonResources.h"
#include "shaders/ShaderShared.h"

extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_BloomUpPyramid;
RGBufferHandle g_RG_LuminanceHistogram;
RGBufferHandle g_RG_ExposureBuffer;

static constexpr float kMinLogLuminance = -10.0f;
static constexpr float kMaxLogLuminance = 20.0f;

class HDRRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        
        // Luminance Histogram
        if (renderer->m_EnableAutoExposure)
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.structStride = sizeof(uint32_t);
            desc.m_NvrhiDesc.byteSize = 256 * sizeof(uint32_t);
            desc.m_NvrhiDesc.debugName = "LuminanceHistogram_RG";
            desc.m_NvrhiDesc.canHaveUAVs = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            renderGraph.DeclareBuffer(desc, g_RG_LuminanceHistogram);
        }

        // Exposure Buffer
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.structStride = sizeof(float);
            desc.m_NvrhiDesc.byteSize = sizeof(float);
            desc.m_NvrhiDesc.debugName = "ExposureBuffer_RG";
            desc.m_NvrhiDesc.canHaveUAVs = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            renderGraph.DeclareBuffer(desc, g_RG_ExposureBuffer);
        }

        if (renderer->m_EnableAutoExposure)
        {
            renderGraph.WriteBuffer(g_RG_LuminanceHistogram);
        }
        renderGraph.WriteBuffer(g_RG_ExposureBuffer);
        renderGraph.ReadTexture(g_RG_HDRColor);
        if (renderer->m_EnableBloom && renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            renderGraph.ReadTexture(g_RG_BloomUpPyramid);
        }

        return true;
    }

    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        nvrhi::utils::ScopedMarker marker(commandList, "HDR Post-Processing");

        nvrhi::BufferHandle luminanceHistogram = renderer->m_EnableAutoExposure ? renderGraph.GetBuffer(g_RG_LuminanceHistogram, RGResourceAccessMode::Write) : nullptr;
        nvrhi::BufferHandle exposureBuffer = renderGraph.GetBuffer(g_RG_ExposureBuffer, RGResourceAccessMode::Write);
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Read);
        nvrhi::TextureHandle bloomUpPyramid = (renderer->m_EnableBloom && renderer->m_Mode != RenderingMode::ReferencePathTracer) ? renderGraph.GetTexture(g_RG_BloomUpPyramid, RGResourceAccessMode::Read) : nullptr;

        // 1. Histogram Pass
        if (renderer->m_EnableAutoExposure)
        {
            nvrhi::utils::ScopedMarker histMarker(commandList, "Luminance Histogram");
            
            // Clear histogram buffer
            commandList->clearBufferUInt(luminanceHistogram, 0);

            HistogramConstants consts;
            consts.m_Width = renderer->m_RHI->m_SwapchainExtent.x;
            consts.m_Height = renderer->m_RHI->m_SwapchainExtent.y;
            consts.m_MinLogLuminance = kMinLogLuminance;
            consts.m_MaxLogLuminance = kMaxLogLuminance;

            nvrhi::BindingSetDesc bset;
            bset.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(HistogramConstants)),
                nvrhi::BindingSetItem::Texture_SRV(0, hdrColor),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(0, luminanceHistogram ? luminanceHistogram : CommonResources::GetInstance().DummyUAVBuffer)
            };

            const uint32_t dispatchX = DivideAndRoundUp(consts.m_Width, 16);
            const uint32_t dispatchY = DivideAndRoundUp(consts.m_Height, 16);

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderName = "LuminanceHistogram_LuminanceHistogram_CSMain",
                .bindingSetDesc = bset,
                .pushConstants = &consts,
                .pushConstantsSize = sizeof(consts),
                .dispatchParams = { .x = dispatchX, .y = dispatchY, .z = 1 }
            };

            renderer->AddComputePass(params);
        }

        // 2. Exposure Adaptation Pass
        {
            nvrhi::utils::ScopedMarker adaptMarker(commandList, "Exposure Adaptation");

            if (renderer->m_EnableAutoExposure)
            {
                AdaptationConstants consts;
                consts.m_DeltaTime = (float)renderer->m_FrameTime / 1000.0f;
                consts.m_AdaptationSpeed = renderer->m_AdaptationSpeed;
                consts.m_NumPixels = renderer->m_RHI->m_SwapchainExtent.x * renderer->m_RHI->m_SwapchainExtent.y;
                consts.m_MinLogLuminance = kMinLogLuminance;
                consts.m_MaxLogLuminance = kMaxLogLuminance;
                consts.m_ExposureValueMin = renderer->m_Scene.m_Camera.m_ExposureValueMin;
                consts.m_ExposureValueMax = renderer->m_Scene.m_Camera.m_ExposureValueMax;
                consts.m_ExposureCompensation = renderer->m_Scene.m_Camera.m_ExposureCompensation;

                nvrhi::BindingSetDesc bset;
                bset.bindings = {
                    nvrhi::BindingSetItem::PushConstants(0, sizeof(AdaptationConstants)),
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(0, exposureBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(0, luminanceHistogram ? luminanceHistogram : CommonResources::GetInstance().DummyUAVBuffer)
                };

                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderName = "ExposureAdaptation_ExposureAdaptation_CSMain",
                    .bindingSetDesc = bset,
                    .pushConstants = &consts,
                    .pushConstantsSize = sizeof(consts),
                    .dispatchParams = { .x = 1, .y = 1, .z = 1 }
                };

                renderer->AddComputePass(params);
            }
            else
            {
                // Manual mode: just update the buffer from CPU
                commandList->writeBuffer(exposureBuffer, &renderer->m_Scene.m_Camera.m_Exposure, sizeof(float));
            }
        }

        // 3. Tonemapping Pass
        {
            nvrhi::utils::ScopedMarker tonemapMarker(commandList, "Tonemapping");

            TonemapConstants consts;
            consts.m_Width = renderer->m_RHI->m_SwapchainExtent.x;
            consts.m_Height = renderer->m_RHI->m_SwapchainExtent.y;
            consts.m_BloomIntensity = renderer->m_BloomIntensity;
            consts.m_EnableBloom = (renderer->m_EnableBloom && bloomUpPyramid) ? 1 : 0;
            consts.m_DebugBloom = (renderer->m_DebugBloom) ? 1 : 0;

            nvrhi::BindingSetDesc bset;
            bset.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(TonemapConstants)),
                nvrhi::BindingSetItem::Texture_SRV(0, hdrColor),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(1, exposureBuffer),
                nvrhi::BindingSetItem::Texture_SRV(2, bloomUpPyramid ? bloomUpPyramid : CommonResources::GetInstance().DefaultTextureBlack),
                nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().LinearClamp)
            };

            nvrhi::FramebufferDesc fbDesc;
            fbDesc.addColorAttachment(renderer->GetCurrentBackBufferTexture());
            nvrhi::FramebufferHandle fb = renderer->m_RHI->m_NvrhiDevice->createFramebuffer(fbDesc);

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderName = "Tonemap_Tonemap_PSMain",
                .bindingSetDesc = bset,
                .pushConstants = &consts,
                .pushConstantsSize = sizeof(consts),
                .framebuffer = fb
            };

            renderer->AddFullScreenPass(params);
        }
    }

    const char* GetName() const override { return "HDRPostProcess"; }
};

REGISTER_RENDERER(HDRRenderer);
