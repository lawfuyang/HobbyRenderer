#include "Renderer.h"
#include "Config.h"
#include "Utilities.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/HDR.h"

extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_BloomUpPyramid;
extern RGTextureHandle g_RG_TAAOutput;
extern RGTextureHandle g_RG_ExposureTexture;

static constexpr float kMinLogLuminance = -10.0f;
static constexpr float kMaxLogLuminance = 20.0f;

class HDRRenderer : public IRenderer
{
public:
    RGBufferHandle m_RG_LuminanceHistogram;
    RGBufferHandle m_RG_ExposureBuffer;

    // Readback: double-buffered staging buffers for reading back exposure to CPU
    nvrhi::BufferHandle m_ExposureReadbackBuffers[2];
    bool m_ReadbackInitialized = false;

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
            renderGraph.DeclareBuffer(desc, m_RG_LuminanceHistogram);
        }

        // Exposure Buffer
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.byteSize = sizeof(float);
            desc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
            desc.m_NvrhiDesc.canHaveTypedViews = true;
            desc.m_NvrhiDesc.debugName = "ExposureBuffer_RG";
            desc.m_NvrhiDesc.canHaveUAVs = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            renderGraph.DeclarePersistentBuffer(desc, m_RG_ExposureBuffer);
        }

        // Create readback staging buffers (once)
        if (!m_ReadbackInitialized)
        {
            for (int i = 0; i < 2; i++)
            {
                nvrhi::BufferDesc rbDesc;
                rbDesc.byteSize = sizeof(float);
                rbDesc.debugName = i == 0 ? "ExposureReadback0" : "ExposureReadback1";
                rbDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
                m_ExposureReadbackBuffers[i] = renderer->m_RHI->m_NvrhiDevice->createBuffer(rbDesc);
            }
            m_ReadbackInitialized = true;
        }
        
        if (renderer->m_EnableBloom && renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            renderGraph.ReadTexture(g_RG_BloomUpPyramid);
        }

        if (renderer->m_bTAAEnabled)
        {
            renderGraph.WriteTexture(g_RG_TAAOutput);
        }
        else
        {
            renderGraph.ReadTexture(g_RG_HDRColor);
        }

        renderGraph.WriteTexture(g_RG_ExposureTexture);
        
        return true;
    }

    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        PROFILE_GPU_SCOPED("HDR Post-Processing", commandList);

        nvrhi::BufferHandle luminanceHistogram = renderer->m_EnableAutoExposure ? renderGraph.GetBuffer(m_RG_LuminanceHistogram, RGResourceAccessMode::Write) : nullptr;
        nvrhi::BufferHandle exposureBuffer = renderGraph.GetBuffer(m_RG_ExposureBuffer, RGResourceAccessMode::Write);
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(renderer->m_bTAAEnabled ? g_RG_TAAOutput : g_RG_HDRColor, RGResourceAccessMode::Read);
        nvrhi::TextureHandle bloomUpPyramid = (renderer->m_EnableBloom && renderer->m_Mode != RenderingMode::ReferencePathTracer) ? renderGraph.GetTexture(g_RG_BloomUpPyramid, RGResourceAccessMode::Read) : nullptr;

        // 1. Histogram Pass
        if (renderer->m_EnableAutoExposure)
        {
            PROFILE_GPU_SCOPED("Luminance Histogram", commandList);

            // Clear histogram buffer
            commandList->clearBufferUInt(luminanceHistogram, 0);

            srrhi::LuminanceHistogramInputs inputs;
            inputs.m_HistogramConstants.SetWidth(renderer->m_RHI->m_SwapchainExtent.x);
            inputs.m_HistogramConstants.SetHeight(renderer->m_RHI->m_SwapchainExtent.y);
            inputs.m_HistogramConstants.SetMinLogLuminance(kMinLogLuminance);
            inputs.m_HistogramConstants.SetMaxLogLuminance(kMaxLogLuminance);

            inputs.SetHDRColor(hdrColor);
            inputs.SetHistogram(luminanceHistogram ? luminanceHistogram : CommonResources::GetInstance().DummyUAVStructuredBuffer);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

            const uint32_t dispatchX = DivideAndRoundUp(renderer->m_RHI->m_SwapchainExtent.x, 16);
            const uint32_t dispatchY = DivideAndRoundUp(renderer->m_RHI->m_SwapchainExtent.y, 16);

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderName = "LuminanceHistogram_LuminanceHistogram_CSMain",
                .bindingSetDesc = bset,
                .pushConstants = &inputs.m_HistogramConstants,
                .pushConstantsSize = srrhi::LuminanceHistogramInputs::PushConstantBytes,
                .dispatchParams = { .x = dispatchX, .y = dispatchY, .z = 1 }
            };

            renderer->AddComputePass(params);
        }

        // 2. Exposure Adaptation Pass
        {
            PROFILE_GPU_SCOPED("Exposure Adaptation", commandList);
            
            if (renderer->m_EnableAutoExposure)
            {
                srrhi::ExposureAdaptationInputs inputs;
                inputs.m_AdaptationConstants.SetDeltaTime((float)renderer->m_FrameTime / 1000.0f);
                inputs.m_AdaptationConstants.SetAdaptationSpeed(renderer->m_AdaptationSpeed);
                inputs.m_AdaptationConstants.SetNumPixels(renderer->m_RHI->m_SwapchainExtent.x * renderer->m_RHI->m_SwapchainExtent.y);
                inputs.m_AdaptationConstants.SetMinLogLuminance(kMinLogLuminance);
                inputs.m_AdaptationConstants.SetMaxLogLuminance(kMaxLogLuminance);
                inputs.m_AdaptationConstants.SetExposureValueMin(renderer->m_Scene.m_Camera.m_ExposureValueMin);
                inputs.m_AdaptationConstants.SetExposureValueMax(renderer->m_Scene.m_Camera.m_ExposureValueMax);
                inputs.m_AdaptationConstants.SetExposureCompensation(renderer->m_Scene.m_Camera.m_ExposureCompensation);

                nvrhi::TextureHandle exposureTexture = renderGraph.GetTexture(g_RG_ExposureTexture, RGResourceAccessMode::Write);

                inputs.SetExposure(exposureBuffer);
                inputs.SetHistogramInput(luminanceHistogram ? luminanceHistogram : CommonResources::GetInstance().DummySRVStructuredBuffer);
                inputs.SetExposureTexture(exposureTexture, 0);

                nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderName = "ExposureAdaptation_ExposureAdaptation_CSMain",
                    .bindingSetDesc = bset,
                    .pushConstants = &inputs.m_AdaptationConstants,
                    .pushConstantsSize = srrhi::ExposureAdaptationInputs::PushConstantBytes,
                    .dispatchParams = { .x = 1, .y = 1, .z = 1 }
                };

                renderer->AddComputePass(params);
            }
            else
            {
                // Manual mode: write the exposure value directly to both buffer and texture
                commandList->writeBuffer(exposureBuffer, &renderer->m_Scene.m_Camera.m_Exposure, sizeof(float));

                // Also write to the 1x1 exposure texture for FSR3
                nvrhi::TextureHandle exposureTexture = renderGraph.GetTexture(g_RG_ExposureTexture, RGResourceAccessMode::Write);
                commandList->writeTexture(exposureTexture, 0, 0, &renderer->m_Scene.m_Camera.m_Exposure, sizeof(float));
            }
        }

        // Readback previous frame's exposure for FSR3 preExposure
        {
            const uint32_t writeIdx = renderer->m_FrameNumber % 2;
            const uint32_t readIdx = 1 - writeIdx;

            // Copy current exposure to staging buffer for next frame readback
            commandList->copyBuffer(m_ExposureReadbackBuffers[writeIdx], 0, exposureBuffer, 0, sizeof(float));

            // Read back the previous frame's value
            float* mapped = static_cast<float*>(renderer->m_RHI->m_NvrhiDevice->mapBuffer(m_ExposureReadbackBuffers[readIdx], nvrhi::CpuAccessMode::Read));
            if (mapped)
            {
                renderer->m_PrevFrameExposure = *mapped;
                renderer->m_RHI->m_NvrhiDevice->unmapBuffer(m_ExposureReadbackBuffers[readIdx]);
            }
        }

        // 3. Tonemapping Pass
        {
            PROFILE_GPU_SCOPED("Tonemapping", commandList);

            srrhi::TonemappingInputs inputs;
            inputs.m_TonemapConstants.SetWidth(renderer->m_RHI->m_SwapchainExtent.x);
            inputs.m_TonemapConstants.SetHeight(renderer->m_RHI->m_SwapchainExtent.y);
            inputs.m_TonemapConstants.SetBloomIntensity(renderer->m_BloomIntensity);
            inputs.m_TonemapConstants.SetEnableBloom((renderer->m_EnableBloom && bloomUpPyramid) ? 1 : 0);
            inputs.m_TonemapConstants.SetDebugBloom((renderer->m_DebugBloom) ? 1 : 0);

            inputs.SetHDRColorInput(hdrColor);
            inputs.SetExposureInput(exposureBuffer);
            inputs.SetBloomInput(bloomUpPyramid ? bloomUpPyramid : CommonResources::GetInstance().DefaultTextureBlack);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

            nvrhi::FramebufferDesc fbDesc;
            fbDesc.addColorAttachment(renderer->GetCurrentBackBufferTexture());
            nvrhi::FramebufferHandle fb = renderer->m_RHI->m_NvrhiDevice->createFramebuffer(fbDesc);

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderName = "Tonemap_Tonemap_PSMain",
                .bindingSetDesc = bset,
                .pushConstants = &inputs.m_TonemapConstants,
                .pushConstantsSize = srrhi::TonemappingInputs::PushConstantBytes,
                .framebuffer = fb
            };

            renderer->AddFullScreenPass(params);
        }
    }

    const char* GetName() const override { return "HDRPostProcess"; }
};

REGISTER_RENDERER(HDRRenderer);
