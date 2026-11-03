#include "Renderer.h"
#include "Config.h"
#include "Utilities.h"
#include "CommonResources.h"

#include "shaders/srrhi/cpp/HDR.h"

extern RGTextureHandle g_RG_TAAOutput;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_ExposureTexture;

static constexpr float kMinLogLuminance = -10.0f;
static constexpr float kMaxLogLuminance = 20.0f;

class HDRRenderer : public IRenderer
{
public:
    RGBufferHandle m_RG_LuminanceHistogram;
    RGBufferHandle m_RG_ExposureBuffer;

    // Readback: ring of staging buffers for reading the exposure back to the CPU.
    // Three of them, because a copy is polled two frames after it was recorded: a frame never waits for
    // the GPU, and the frame that submits (which does wait for idle) is what guarantees the copy of two
    // frames ago has completed.
    static constexpr uint32_t kNumReadbackBuffers = 3;
    nvrhi::BufferHandle m_ExposureReadbackBuffers[kNumReadbackBuffers];
    // Signalled once the copy into each buffer has executed
    nvrhi::EventQueryHandle m_ExposureReadbackEvents[kNumReadbackBuffers];
    bool m_ReadbackInitialized = false;

    bool Setup(RenderGraph& renderGraph) override
    {
        // Luminance Histogram
        if (g_Renderer.m_EnableAutoExposure)
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
            for (uint32_t i = 0; i < kNumReadbackBuffers; i++)
            {
                nvrhi::BufferDesc rbDesc;
                rbDesc.byteSize = sizeof(float);
                rbDesc.debugName = "ExposureReadback" + std::to_string(i);
                rbDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
                m_ExposureReadbackBuffers[i] = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(rbDesc);
                m_ExposureReadbackEvents[i] = g_Renderer.m_RHI->m_NvrhiDevice->createEventQuery();
            }
            m_ReadbackInitialized = true;
        }
        
        if (g_Renderer.m_Mode == RenderingMode::ReferencePathTracer)
        {
            renderGraph.ReadTexture(g_RG_HDRColor);
        }
        else
        {
            renderGraph.ReadTexture(g_RG_TAAOutput);
        }

        renderGraph.WriteTexture(g_RG_ExposureTexture);

        return true;
    }

    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_GPU_SCOPED("HDR Post-Processing", commandList);

        nvrhi::BufferHandle luminanceHistogram = g_Renderer.m_EnableAutoExposure ? renderGraph.GetBuffer(m_RG_LuminanceHistogram, RGResourceAccessMode::Write) : nullptr;
        nvrhi::BufferHandle exposureBuffer = renderGraph.GetBuffer(m_RG_ExposureBuffer, RGResourceAccessMode::Write);

        nvrhi::TextureHandle HDRInput;
        if (g_Renderer.m_Mode == RenderingMode::ReferencePathTracer)
        {
            HDRInput = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Read);
        }
        else
        {
            HDRInput = renderGraph.GetTexture(g_RG_TAAOutput, RGResourceAccessMode::Read);
        }

        // 1. Histogram Pass
        if (g_Renderer.m_EnableAutoExposure)
        {
            PROFILE_GPU_SCOPED("Luminance Histogram", commandList);

            // Clear histogram buffer
            commandList->clearBufferUInt(luminanceHistogram, 0);

            srrhi::LuminanceHistogramInputs inputs;
            inputs.m_HistogramConstants.SetWidth(g_Renderer.m_RHI->m_SwapchainExtent.x);
            inputs.m_HistogramConstants.SetHeight(g_Renderer.m_RHI->m_SwapchainExtent.y);
            inputs.m_HistogramConstants.SetMinLogLuminance(kMinLogLuminance);
            inputs.m_HistogramConstants.SetMaxLogLuminance(kMaxLogLuminance);

            inputs.SetHDRColor(HDRInput);
            inputs.SetHistogram(luminanceHistogram ? luminanceHistogram : CommonResources::GetInstance().DummyUAVStructuredBuffer);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

            const uint32_t dispatchX = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.x, 16);
            const uint32_t dispatchY = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.y, 16);

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderID = ShaderID::LUMINANCEHISTOGRAM_LUMINANCEHISTOGRAM_CSMAIN,
                .bindingSetDesc = bset,
                .pushConstants = &inputs.m_HistogramConstants,
                .pushConstantsSize = srrhi::LuminanceHistogramInputs::PushConstantBytes,
                .dispatchParams = { .x = dispatchX, .y = dispatchY, .z = 1 }
            };

            g_Renderer.AddComputePass(params);
        }

        // 2. Exposure Adaptation Pass
        {
            PROFILE_GPU_SCOPED("Exposure Adaptation", commandList);
            
            if (g_Renderer.m_EnableAutoExposure)
            {
                srrhi::ExposureAdaptationInputs inputs;
                inputs.m_AdaptationConstants.SetDeltaTime((float)g_Renderer.m_FrameTime / 1000.0f);
                inputs.m_AdaptationConstants.SetAdaptationSpeed(g_Renderer.m_AdaptationSpeed);
                inputs.m_AdaptationConstants.SetNumPixels(g_Renderer.m_RHI->m_SwapchainExtent.x * g_Renderer.m_RHI->m_SwapchainExtent.y);
                inputs.m_AdaptationConstants.SetMinLogLuminance(kMinLogLuminance);
                inputs.m_AdaptationConstants.SetMaxLogLuminance(kMaxLogLuminance);
                inputs.m_AdaptationConstants.SetExposureValueMin(g_Renderer.m_Scene.m_Camera.m_ExposureValueMin);
                inputs.m_AdaptationConstants.SetExposureValueMax(g_Renderer.m_Scene.m_Camera.m_ExposureValueMax);
                inputs.m_AdaptationConstants.SetExposureCompensation(g_Renderer.m_Scene.m_Camera.m_ExposureCompensation);

                nvrhi::TextureHandle exposureTexture = renderGraph.GetTexture(g_RG_ExposureTexture, RGResourceAccessMode::Write);

                inputs.SetExposure(exposureBuffer);
                inputs.SetHistogramInput(luminanceHistogram ? luminanceHistogram : CommonResources::GetInstance().DummySRVStructuredBuffer);
                inputs.SetExposureTexture(exposureTexture, 0);

                nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderID = ShaderID::EXPOSUREADAPTATION_EXPOSUREADAPTATION_CSMAIN,
                    .bindingSetDesc = bset,
                    .pushConstants = &inputs.m_AdaptationConstants,
                    .pushConstantsSize = srrhi::ExposureAdaptationInputs::PushConstantBytes,
                    .dispatchParams = { .x = 1, .y = 1, .z = 1 }
                };

                g_Renderer.AddComputePass(params);
            }
            else
            {
                // Manual mode: write the exposure value directly to both buffer and texture
                commandList->writeBuffer(exposureBuffer, &g_Renderer.m_Scene.m_Camera.m_Exposure, sizeof(float));

                // Also write to the 1x1 exposure texture for FSR3
                nvrhi::TextureHandle exposureTexture = renderGraph.GetTexture(g_RG_ExposureTexture, RGResourceAccessMode::Write);
                commandList->writeTexture(exposureTexture, 0, 0, &g_Renderer.m_Scene.m_Camera.m_Exposure, sizeof(float));
            }
        }

        // Readback previous frame's exposure for FSR3 preExposure
        {
            const uint32_t writeIdx = g_Renderer.m_FrameNumber % kNumReadbackBuffers;
            const uint32_t readIdx = (g_Renderer.m_FrameNumber + 1) % kNumReadbackBuffers;

            // Copy current exposure to staging buffer for next frame readback
            commandList->copyBuffer(m_ExposureReadbackBuffers[writeIdx], 0, exposureBuffer, 0, sizeof(float));

            // Arm the copy's completion event. The buffer is read two frames later, by which point the
            // submission of this frame has waited for idle and the copy is guaranteed complete, so the
            // read below never stalls; until then the previous value is kept.
            g_Renderer.SignalEventAfterFrame(m_ExposureReadbackEvents[writeIdx]);

            if (g_Renderer.m_RHI->m_NvrhiDevice->pollEventQuery(m_ExposureReadbackEvents[readIdx].Get()))
            {
                float* mapped = static_cast<float*>(g_Renderer.m_RHI->m_NvrhiDevice->mapBuffer(m_ExposureReadbackBuffers[readIdx], nvrhi::CpuAccessMode::Read));
                if (mapped)
                {
                    g_Renderer.m_PrevFrameExposure = *mapped;
                    g_Renderer.m_RHI->m_NvrhiDevice->unmapBuffer(m_ExposureReadbackBuffers[readIdx]);
                }
            }
        }

        // 3. Tonemapping Pass
        {
            PROFILE_GPU_SCOPED("Tonemapping", commandList);

            const bool bIsHDR = g_Renderer.m_RHI->m_bIsHDR;

            srrhi::TonemappingInputs inputs;
            inputs.m_TonemapConstants.SetWidth(g_Renderer.m_RHI->m_SwapchainExtent.x);
            inputs.m_TonemapConstants.SetHeight(g_Renderer.m_RHI->m_SwapchainExtent.y);
            inputs.m_TonemapConstants.SetMaxDisplayNits(g_Renderer.m_RHI->m_MaxDisplayNits);

            inputs.SetHDRColorInput(HDRInput);
            inputs.SetExposureInput(exposureBuffer);

            nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

            nvrhi::FramebufferDesc fbDesc;
            fbDesc.addColorAttachment(g_Renderer.GetCurrentBackBufferTexture());
            nvrhi::FramebufferHandle fb = g_Renderer.m_RHI->m_NvrhiDevice->createFramebuffer(fbDesc);

            Renderer::RenderPassParams params{
                .commandList = commandList,
                .shaderID = bIsHDR ? ShaderID::TONEMAP_TONEMAPHDR_PSMAIN : ShaderID::TONEMAP_TONEMAP_PSMAIN,
                .bindingSetDesc = bset,
                .pushConstants = &inputs.m_TonemapConstants,
                .pushConstantsSize = srrhi::TonemappingInputs::PushConstantBytes,
                .framebuffer = fb
            };

            g_Renderer.AddFullScreenPass(params);
        }
    }

    const char* GetName() const override { return "HDRPostProcess"; }
};

REGISTER_RENDERER(HDRRenderer);
