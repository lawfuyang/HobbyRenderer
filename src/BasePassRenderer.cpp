#include "Renderer.h"
#include "CommonResources.h"
#include "Camera.h"

#include "shaders/ShaderShared.h"

class BasePassRenderer : public IRenderer
{
public:
    bool Initialize() override;
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "BasePass"; }

private:
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::PipelineStatisticsQueryHandle m_PipelineQueries[2];
};

REGISTER_RENDERER(BasePassRenderer);

bool BasePassRenderer::Initialize()
{
    Renderer* renderer = Renderer::GetInstance();

    // Create input layout matching shared VertexInput (pos, normal, uv)
    nvrhi::VertexAttributeDesc attributes[] = {
        { "POSITION", nvrhi::Format::RGB32_FLOAT,  1, 0, offsetof(VertexInput, m_Pos),    sizeof(VertexInput), false },
        { "NORMAL",   nvrhi::Format::RGB32_FLOAT,  1, 0, offsetof(VertexInput, m_Normal), sizeof(VertexInput), false },
        { "TEXCOORD0",nvrhi::Format::RG32_FLOAT,   1, 0, offsetof(VertexInput, m_Uv),     sizeof(VertexInput), false },
    };

    // Create input layout (vertexShader parameter unused for Vulkan backend)
    m_InputLayout = renderer->m_NvrhiDevice->createInputLayout(attributes, 3, nullptr);
    if (!m_InputLayout)
    {
        SDL_LOG_ASSERT_FAIL("ImGui input layout creation failed", "[BasePass] Failed to create input layout");
        return false;
    }

    // Create pipeline statistics queries for double buffering
    m_PipelineQueries[0] = renderer->m_NvrhiDevice->createPipelineStatisticsQuery();
    m_PipelineQueries[1] = renderer->m_NvrhiDevice->createPipelineStatisticsQuery();

    return true;
}

void BasePassRenderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();

    // ============================================================================
    // Pipeline Statistics Query
    // ============================================================================
    const int readIndex = renderer->m_FrameNumber % 2;
    const int writeIndex = (renderer->m_FrameNumber + 1) % 2;
    if (renderer->m_NvrhiDevice->pollPipelineStatisticsQuery(m_PipelineQueries[readIndex])) 
    {
        renderer->m_MainViewPipelineStatistics = renderer->m_NvrhiDevice->getPipelineStatistics(m_PipelineQueries[readIndex]);
        renderer->m_NvrhiDevice->resetPipelineStatisticsQuery(m_PipelineQueries[readIndex]);
    }
    commandList->beginPipelineStatisticsQuery(m_PipelineQueries[writeIndex]);

    // ============================================================================
    // Dispatch Culling Compute Shader
    // ============================================================================
    Camera* cam = &renderer->m_Camera;
    Matrix viewProj = cam->GetViewProjMatrix();
    Matrix view = cam->GetViewMatrix();
    Matrix proj = cam->GetProjMatrix();
    Vector3 camPos = renderer->m_Camera.GetPosition();

    // Compute frustum planes in LH view space
    float xScale = fabs(proj._11);
    float yScale = fabs(proj._22);
    float nearZ = proj._43;

    DirectX::XMVECTOR planes[5];
    // Left: normal (-1, 0, 1/xScale), d=0
    planes[0] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(-1.0f, 0.0f, 1.0f / xScale, 0.0f));
    // Right: normal (1, 0, -1/xScale), d=0
    planes[1] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(1.0f, 0.0f, 1.0f / xScale, 0.0f));
    // Bottom: normal (0, -1, 1/yScale), d=0
    planes[2] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, -1.0f, 1.0f / yScale, 0.0f));
    // Top: normal (0, 1, -1/yScale), d=0
    planes[3] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, 1.0f, 1.0f / yScale, 0.0f));
    // Near: normal (0, 0, -1), d=-nearZ
    planes[4] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, nearZ));

    Vector4 frustumPlanes[5];
    for (int i = 0; i < 5; i++) {
        DirectX::XMStoreFloat4(&frustumPlanes[i], planes[i]);
    }

    nvrhi::BufferHandle indirectBuffer;
    nvrhi::BufferHandle countBuffer;
    if (!renderer->m_Scene.m_InstanceData.empty())
    {
        // Create indirect args buffer
        nvrhi::BufferDesc indirectBufDesc = nvrhi::BufferDesc()
            .setByteSize(renderer->m_Scene.m_InstanceData.size() * sizeof(nvrhi::DrawIndexedIndirectArguments))
            .setStructStride(sizeof(nvrhi::DrawIndexedIndirectArguments))
            .setIsDrawIndirectArgs(true)
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::IndirectArgument)
            .setKeepInitialState(true);
        indirectBuffer = renderer->m_NvrhiDevice->createBuffer(indirectBufDesc);
        renderer->m_RHI.SetDebugName(indirectBuffer, "IndirectBuffer");

        // Create count buffer
        nvrhi::BufferDesc countBufDesc = nvrhi::BufferDesc()
            .setByteSize(sizeof(uint32_t))
            .setStructStride(sizeof(uint32_t))
            .setCanHaveUAVs(true)
            .setIsDrawIndirectArgs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true);
        countBuffer = renderer->m_NvrhiDevice->createBuffer(countBufDesc);
        renderer->m_RHI.SetDebugName(countBuffer, "VisibleCount");

        uint32_t zero = 0;
        commandList->writeBuffer(countBuffer, &zero, sizeof(uint32_t), 0);

        // Create constant buffer for culling
        nvrhi::BufferDesc cullCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(CullingConstants), "CullingCB", 1);
        nvrhi::BufferHandle cullCB = renderer->m_NvrhiDevice->createBuffer(cullCBD);
        renderer->m_RHI.SetDebugName(cullCB, "CullingCB");

        nvrhi::BindingSetDesc cullBset;
        cullBset.bindings =
        {
            nvrhi::BindingSetItem::ConstantBuffer(0, cullCB),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, renderer->m_Scene.m_InstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, indirectBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, countBuffer)
        };
        nvrhi::BindingLayoutHandle cullLayout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(cullBset, nvrhi::ShaderType::Compute);
        nvrhi::BindingSetHandle cullBindingSet = renderer->m_NvrhiDevice->createBindingSet(cullBset, cullLayout);

        nvrhi::ComputeState cullState;
        cullState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("GPUCulling_Culling_CSMain"), cullLayout);
        cullState.bindings = { cullBindingSet };

        uint32_t numPrimitives = (uint32_t)renderer->m_Scene.m_InstanceData.size();
        CullingConstants cullData;
        cullData.m_NumPrimitives = numPrimitives;
        memcpy(cullData.m_FrustumPlanes, frustumPlanes, sizeof(frustumPlanes));
        cullData.m_View = view;
        cullData.m_EnableFrustumCulling = renderer->m_EnableFrustumCulling ? 1 : 0;
        commandList->writeBuffer(cullCB, &cullData, sizeof(cullData), 0);

        commandList->setComputeState(cullState);

        uint32_t dispatchX = (numPrimitives + 63) / 64;
        commandList->dispatch(dispatchX, 1, 1);
    }

    // ============================================================================
    // Framebuffer Setup
    // ============================================================================
    nvrhi::TextureHandle rt = renderer->GetCurrentBackBufferTexture();
    nvrhi::TextureHandle depth = renderer->m_DepthTexture;
    nvrhi::FramebufferHandle framebuffer = renderer->m_NvrhiDevice->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(rt).setDepthAttachment(depth));

    // ============================================================================
    // Graphics Pipeline Setup
    // ============================================================================
    nvrhi::GraphicsState state;
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.VS = renderer->GetShaderHandle("ForwardLighting_VSMain");
    pipelineDesc.PS = renderer->GetShaderHandle("ForwardLighting_PSMain");
    pipelineDesc.inputLayout = m_InputLayout;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.renderState.rasterState = CommonResources::GetInstance().RasterCullBack;
    pipelineDesc.renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetOpaque;
    pipelineDesc.renderState.depthStencilState = CommonResources::GetInstance().DepthReadWrite;

    nvrhi::FramebufferInfoEx fbInfo;
    fbInfo.colorFormats = { renderer->m_RHI.VkFormatToNvrhiFormat(renderer->m_RHI.m_SwapchainFormat) };
    fbInfo.setDepthFormat(nvrhi::Format::D32);
    state.framebuffer = framebuffer;

    // ============================================================================
    // Vertex/Index Buffer and Viewport Setup
    // ============================================================================
    state.vertexBuffers = { nvrhi::VertexBufferBinding{ renderer->m_Scene.m_VertexBuffer, 0, 0 } };
    state.indexBuffer = nvrhi::IndexBufferBinding{
        renderer->m_Scene.m_IndexBuffer, nvrhi::Format::R32_UINT, 0 };

    uint32_t w = renderer->m_RHI.m_SwapchainExtent.width;
    uint32_t h = renderer->m_RHI.m_SwapchainExtent.height;
    state.viewport.viewports.push_back(nvrhi::Viewport(0.0f, (float)w, (float)h, 0.0f, 0.0f, 1.0f));
    state.viewport.scissorRects.resize(1);
    state.viewport.scissorRects[0].minX = 0;
    state.viewport.scissorRects[0].minY = 0;
    state.viewport.scissorRects[0].maxX = (int)w;
    state.viewport.scissorRects[0].maxY = (int)h;

    // ============================================================================
    // Constant Buffer Setup
    // ============================================================================
    nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(
        (uint32_t)sizeof(ForwardLightingPerFrameData), "PerFrameCB", 1);
    nvrhi::BufferHandle perFrameCB = renderer->m_NvrhiDevice->createBuffer(cbd);
    renderer->m_RHI.SetDebugName(perFrameCB, "PerFrameCB_frame");

    // ============================================================================
    // Binding Set Setup
    // ============================================================================
    nvrhi::BindingSetDesc bset;
    bset.bindings =
    {
        nvrhi::BindingSetItem::ConstantBuffer(0, perFrameCB),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, renderer->m_Scene.m_InstanceDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, renderer->m_Scene.m_MaterialConstantsBuffer),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().AnisotropicClamp),
        nvrhi::BindingSetItem::Sampler(1, CommonResources::GetInstance().AnisotropicWrap)
    };
    nvrhi::BindingLayoutHandle layout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bset, nvrhi::ShaderType::All);
    pipelineDesc.bindingLayouts = { renderer->GetGlobalTextureBindingLayout(), layout };

    nvrhi::BindingSetHandle bindingSet = renderer->m_NvrhiDevice->createBindingSet(bset, layout);
    state.bindings = { renderer->GetGlobalTextureDescriptorTable(), bindingSet };

    nvrhi::GraphicsPipelineHandle pipeline = renderer->GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
    state.pipeline = pipeline;

    // ============================================================================
    // Per-Frame Constants and Rendering
    // ============================================================================
    ForwardLightingPerFrameData cb{};
    cb.m_ViewProj = viewProj;
    cb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 0.0f };
    cb.m_LightDirection = renderer->m_Scene.GetDirectionalLightDirection();
    cb.m_LightIntensity = renderer->m_Scene.m_DirectionalLight.intensity / 10000.0f;
    commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

    state.indirectParams = indirectBuffer;
    state.indirectCountParams = countBuffer;
    commandList->setGraphicsState(state);

    commandList->drawIndexedIndirect(0, (uint32_t)renderer->m_Scene.m_InstanceData.size());

    commandList->endPipelineStatisticsQuery(m_PipelineQueries[writeIndex]);
}
