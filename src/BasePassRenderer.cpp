#include "Renderer.h"
#include "CommonResources.h"
#include "Camera.h"
#include "Utilities.h"

#include "shaders/ShaderShared.h"

#define FFX_CPU
#define FFX_STATIC static
using FfxUInt32 = uint32_t;
using FfxInt32 = int32_t;
using FfxFloat32 = float;
using FfxUInt32x2 = uint32_t[2];
using FfxUInt32x4 = uint32_t[4];
#define ffxMax(a, b) std::max(a, b)
#define ffxMin(a, b) std::min(a, b)
#include "shaders/ffx_spd.h"

class BasePassRenderer : public IRenderer
{
public:
    bool Initialize() override;
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "BasePass"; }

private:
    nvrhi::PipelineStatisticsQueryHandle m_PipelineQueries[2];

    void GenerateHZBMips(nvrhi::CommandListHandle commandList);
    void ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[5]);
    void PerformOcclusionCulling(nvrhi::CommandListHandle commandList, const Vector4 frustumPlanes[5], const Matrix& view, const Matrix& viewProj, uint32_t numPrimitives,
                                 nvrhi::BufferHandle visibleIndirectBuffer, nvrhi::BufferHandle visibleCountBuffer,
                                 nvrhi::BufferHandle occludedIndicesBuffer, nvrhi::BufferHandle occludedCountBuffer, 
                                 nvrhi::BufferHandle occludedIndirectBuffer, int phase);
    void RenderInstances(nvrhi::CommandListHandle commandList, int phase, nvrhi::BufferHandle indirectBuffer, nvrhi::BufferHandle countBuffer, const Matrix& viewProj, const Vector3& camPos);
};

REGISTER_RENDERER(BasePassRenderer);

void BasePassRenderer::PerformOcclusionCulling(nvrhi::CommandListHandle commandList, const Vector4 frustumPlanes[5], const Matrix& view, const Matrix& viewProj, uint32_t numPrimitives,
                                                nvrhi::BufferHandle visibleIndirectBuffer, nvrhi::BufferHandle visibleCountBuffer,
                                                nvrhi::BufferHandle occludedIndicesBuffer, nvrhi::BufferHandle occludedCountBuffer,
                                                nvrhi::BufferHandle occludedIndirectBuffer, int phase)
{
    PROFILE_FUNCTION();

    Renderer* renderer = Renderer::GetInstance();

    nvrhi::utils::ScopedMarker commandListMarker{ commandList, phase == 0 ? "Occlusion Culling Phase 1" : "Occlusion Culling Phase 2" };

    if (phase == 0)
    {
        // Clear visible and occluded count buffers for Phase 1
        commandList->clearBufferUInt(visibleCountBuffer, 0);
        commandList->clearBufferUInt(occludedCountBuffer, 0);
    }
    else if (phase == 1)
    {
        // generate HZB mips for Phase 2 testing
        GenerateHZBMips(commandList);

        // Clear visible count buffer for Phase 2
        commandList->clearBufferUInt(visibleCountBuffer, 0);
    }

    nvrhi::BufferDesc cullCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(CullingConstants), phase == 0 ? "CullingCB" : "CullingCB_Phase2", 1);
    nvrhi::BufferHandle cullCB = renderer->m_NvrhiDevice->createBuffer(cullCBD);
    renderer->m_RHI.SetDebugName(cullCB, phase == 0 ? "CullingCB" : "CullingCB_Phase2");

    CullingConstants cullData;
    cullData.m_NumPrimitives = numPrimitives;
    memcpy(cullData.m_FrustumPlanes, frustumPlanes, std::size(cullData.m_FrustumPlanes) * sizeof(Vector4));
    cullData.m_View = view;
    cullData.m_ViewProj = viewProj;
    cullData.m_EnableFrustumCulling = renderer->m_EnableFrustumCulling ? 1 : 0;
    cullData.m_EnableOcclusionCulling = renderer->m_EnableOcclusionCulling ? 1 : 0;
    cullData.m_HZBWidth = renderer->m_HZBTexture->getDesc().width;
    cullData.m_HZBHeight = renderer->m_HZBTexture->getDesc().height;
    cullData.m_Phase = phase;
    commandList->writeBuffer(cullCB, &cullData, sizeof(cullData), 0);

    nvrhi::BindingSetDesc cullBset;
    cullBset.bindings =
    {
        nvrhi::BindingSetItem::ConstantBuffer(0, cullCB),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, renderer->m_Scene.m_InstanceDataBuffer),
        nvrhi::BindingSetItem::Texture_SRV(1, renderer->m_HZBTexture),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, renderer->m_Scene.m_MeshDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visibleIndirectBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(1, visibleCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(2, occludedIndicesBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(3, occludedCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(4, occludedIndirectBuffer),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().MinReductionClamp)
    };
    nvrhi::BindingLayoutHandle cullLayout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(cullBset, nvrhi::ShaderType::Compute);
    nvrhi::BindingSetHandle cullBindingSet = renderer->m_NvrhiDevice->createBindingSet(cullBset, cullLayout);

    nvrhi::ComputeState cullState;
    cullState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("GPUCulling_Culling_CSMain"), cullLayout);
    cullState.bindings = { cullBindingSet };

    commandList->setComputeState(cullState);
    if (phase == 0)
    {
        uint32_t dispatchX = DivideAndRoundUp(numPrimitives, 64);
        commandList->dispatch(dispatchX, 1, 1);

        nvrhi::utils::ScopedMarker phase2Marker{ commandList, "Occlusion Culling Phase 2 - Build Indirect" };

        // Build indirect for Phase 2
        nvrhi::ComputeState buildIndirectState;
        buildIndirectState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("GPUCulling_BuildIndirect_CSMain"), cullLayout);
        buildIndirectState.bindings = { cullBindingSet };
        commandList->setComputeState(buildIndirectState);
        commandList->dispatch(1, 1, 1);
    }
    else
    {
        cullState.indirectParams = occludedIndirectBuffer;
        commandList->setComputeState(cullState);
        commandList->dispatchIndirect(0);
    }
}

void BasePassRenderer::ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[5])
{
    float xScale = fabs(proj._11);
    float yScale = fabs(proj._22);
    float nearZ = proj._43;

    DirectX::XMVECTOR planes[5];
    planes[0] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(-1.0f, 0.0f, 1.0f / xScale, 0.0f));
    planes[1] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(1.0f, 0.0f, 1.0f / xScale, 0.0f));
    planes[2] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, -1.0f, 1.0f / yScale, 0.0f));
    planes[3] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, 1.0f, 1.0f / yScale, 0.0f));
    planes[4] = DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, nearZ));

    for (int i = 0; i < 5; i++) {
        DirectX::XMStoreFloat4(&frustumPlanes[i], planes[i]);
    }
}

void BasePassRenderer::RenderInstances(nvrhi::CommandListHandle commandList, int phase, nvrhi::BufferHandle indirectBuffer, nvrhi::BufferHandle countBuffer, const Matrix& viewProj, const Vector3& camPos)
{
    PROFILE_FUNCTION();

    Renderer* renderer = Renderer::GetInstance();

    const char* markerName = (phase == 0) ? "Base Pass Render - Visible Instances" : "Base Pass Render - Occlusion Tested Instances";
    nvrhi::utils::ScopedMarker commandListMarker(commandList, markerName);

    nvrhi::FramebufferHandle framebuffer = renderer->m_NvrhiDevice->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(renderer->GetCurrentBackBufferTexture()).setDepthAttachment(renderer->m_DepthTexture));

    nvrhi::GraphicsState state;
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.VS = renderer->GetShaderHandle("ForwardLighting_VSMain");
    pipelineDesc.PS = renderer->GetShaderHandle("ForwardLighting_PSMain");
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.renderState.rasterState = CommonResources::GetInstance().RasterCullBack;
    pipelineDesc.renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetOpaque;
    pipelineDesc.renderState.depthStencilState = CommonResources::GetInstance().DepthReadWrite;

    nvrhi::FramebufferInfoEx fbInfo;
    fbInfo.colorFormats = { renderer->m_RHI.VkFormatToNvrhiFormat(renderer->m_RHI.m_SwapchainFormat) };
    fbInfo.setDepthFormat(nvrhi::Format::D32);
    state.framebuffer = framebuffer;

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

    nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(
        (uint32_t)sizeof(ForwardLightingPerFrameData), "PerFrameCB", 1);
    nvrhi::BufferHandle perFrameCB = renderer->m_NvrhiDevice->createBuffer(cbd);
    renderer->m_RHI.SetDebugName(perFrameCB, "PerFrameCB_frame");

    nvrhi::BindingSetDesc bset;
    bset.bindings =
    {
        nvrhi::BindingSetItem::ConstantBuffer(0, perFrameCB),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, renderer->m_Scene.m_InstanceDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, renderer->m_Scene.m_MaterialConstantsBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, renderer->m_Scene.m_VertexBuffer),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().AnisotropicClamp),
        nvrhi::BindingSetItem::Sampler(1, CommonResources::GetInstance().AnisotropicWrap)
    };
    nvrhi::BindingLayoutHandle layout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bset, nvrhi::ShaderType::All);
    pipelineDesc.bindingLayouts = { renderer->GetGlobalTextureBindingLayout(), layout };

    nvrhi::BindingSetHandle bindingSet = renderer->m_NvrhiDevice->createBindingSet(bset, layout);
    state.bindings = { renderer->GetGlobalTextureDescriptorTable(), bindingSet };

    nvrhi::GraphicsPipelineHandle pipeline = renderer->GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
    state.pipeline = pipeline;

    ForwardLightingPerFrameData cb{};
    cb.m_ViewProj = viewProj;
    cb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 0.0f };
    cb.m_LightDirection = renderer->m_Scene.GetDirectionalLightDirection();
    cb.m_LightIntensity = renderer->m_Scene.m_DirectionalLight.intensity / 10000.0f;
    commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

    state.indirectParams = indirectBuffer;
    state.indirectCountBuffer = countBuffer;
    commandList->setGraphicsState(state);

    commandList->drawIndexedIndirectCount(0, 0, (uint32_t)renderer->m_Scene.m_InstanceData.size());
}

bool BasePassRenderer::Initialize()
{
    Renderer* renderer = Renderer::GetInstance();

    // Create pipeline statistics queries for double buffering
    m_PipelineQueries[0] = renderer->m_NvrhiDevice->createPipelineStatisticsQuery();
    m_PipelineQueries[1] = renderer->m_NvrhiDevice->createPipelineStatisticsQuery();

    return true;
}

void BasePassRenderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();

    const uint32_t numPrimitives = (uint32_t)renderer->m_Scene.m_InstanceData.size();
    if (numPrimitives == 0)
    {
        return;
    }

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
    // 2-Phase Occlusion Culling
    // ============================================================================
    Camera* cam = &renderer->m_Camera;
    Matrix viewProj = cam->GetViewProjMatrix();
    Matrix viewProjForCulling = viewProj;
    Matrix view = cam->GetViewMatrix();
    if (renderer->m_FreezeCullingCamera)
    {
        view = renderer->m_FrozenCullingViewMatrix;
    }
    Matrix proj = cam->GetProjMatrix();
    Vector3 camPos = renderer->m_Camera.GetPosition();

    // Compute frustum planes in LH view space
    Vector4 frustumPlanes[5];
    if (renderer->m_FreezeCullingCamera)
    {
        // viewProj = view * proj
        DirectX::XMMATRIX v = DirectX::XMLoadFloat4x4(&renderer->m_FrozenCullingViewMatrix);
        DirectX::XMMATRIX p = DirectX::XMLoadFloat4x4(&proj);
        DirectX::XMStoreFloat4x4(&viewProjForCulling, v * p);

        // Compute frustum planes in LH view space from frozen projection matrix
        ComputeFrustumPlanes(proj, frustumPlanes);
    }
    else
    {
        ComputeFrustumPlanes(proj, frustumPlanes);
    }

    nvrhi::BufferHandle visibleIndirectBuffer;
    nvrhi::BufferHandle visibleCountBuffer;
    nvrhi::BufferHandle occludedIndicesBuffer;
    nvrhi::BufferHandle occludedCountBuffer;
    nvrhi::BufferHandle occludedIndirectBuffer;

    // Create buffers for visible instances (Phase 1 + Phase 2 results)
    nvrhi::BufferDesc visibleIndirectBufDesc = nvrhi::BufferDesc()
        .setByteSize(numPrimitives * sizeof(nvrhi::DrawIndexedIndirectArguments))
        .setStructStride(sizeof(nvrhi::DrawIndexedIndirectArguments))
        .setIsDrawIndirectArgs(true)
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true);
    visibleIndirectBuffer = renderer->m_NvrhiDevice->createBuffer(visibleIndirectBufDesc);
    renderer->m_RHI.SetDebugName(visibleIndirectBuffer, "VisibleIndirectBuffer");

    nvrhi::BufferDesc visibleCountBufDesc = nvrhi::BufferDesc()
        .setByteSize(sizeof(uint32_t))
        .setStructStride(sizeof(uint32_t))
        .setCanHaveUAVs(true)
        .setIsDrawIndirectArgs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true);
    visibleCountBuffer = renderer->m_NvrhiDevice->createBuffer(visibleCountBufDesc);
    renderer->m_RHI.SetDebugName(visibleCountBuffer, "VisibleCount");

    // Create buffers for occluded instances (Phase 1 results for Phase 2 input)
    nvrhi::BufferDesc occludedIndicesBufDesc = nvrhi::BufferDesc()
        .setByteSize(numPrimitives * sizeof(uint32_t))
        .setStructStride(sizeof(uint32_t))
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true);
    occludedIndicesBuffer = renderer->m_NvrhiDevice->createBuffer(occludedIndicesBufDesc);
    renderer->m_RHI.SetDebugName(occludedIndicesBuffer, "OccludedIndices");

    nvrhi::BufferDesc occludedCountBufDesc = nvrhi::BufferDesc()
        .setByteSize(sizeof(uint32_t))
        .setStructStride(sizeof(uint32_t))
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true);
    occludedCountBuffer = renderer->m_NvrhiDevice->createBuffer(occludedCountBufDesc);
    renderer->m_RHI.SetDebugName(occludedCountBuffer, "OccludedCount");

    nvrhi::BufferDesc occludedIndirectBufDesc = nvrhi::BufferDesc()
        .setByteSize(sizeof(DispatchIndirectArguments))
        .setStructStride(sizeof(DispatchIndirectArguments))
        .setIsDrawIndirectArgs(true)
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true);
    occludedIndirectBuffer = renderer->m_NvrhiDevice->createBuffer(occludedIndirectBufDesc);
    renderer->m_RHI.SetDebugName(occludedIndirectBuffer, "OccludedIndirectBuffer");

    // Clear count buffers
    commandList->clearBufferUInt(visibleCountBuffer, 0);
    commandList->clearBufferUInt(occludedCountBuffer, 0);

    // ===== PHASE 1: Coarse culling against previous frame HZB =====
    PerformOcclusionCulling(commandList, frustumPlanes, view, viewProjForCulling, numPrimitives, visibleIndirectBuffer, visibleCountBuffer, occludedIndicesBuffer, occludedCountBuffer, occludedIndirectBuffer, 0);

    if (renderer->m_EnableOcclusionCulling && !renderer->m_FreezeCullingCamera)
    {
        // ===== PHASE 1 RENDER: Full render for visible instances =====
        RenderInstances(commandList, 0, visibleIndirectBuffer, visibleCountBuffer, viewProj, camPos);

        // ===== PHASE 2: Test occluded instances against new HZB =====
        PerformOcclusionCulling(commandList, frustumPlanes, view, viewProjForCulling, numPrimitives, visibleIndirectBuffer, visibleCountBuffer, occludedIndicesBuffer, occludedCountBuffer, occludedIndirectBuffer, 1);
    }

    // ===== PHASE 2 RENDER: Full render for remaining visible instances =====
    RenderInstances(commandList, 1, visibleIndirectBuffer, visibleCountBuffer, viewProj, camPos);

    // generate HZB mips for next frame
    GenerateHZBMips(commandList);

    commandList->endPipelineStatisticsQuery(m_PipelineQueries[writeIndex]);
}

void BasePassRenderer::GenerateHZBMips(nvrhi::CommandListHandle commandList)
{
    PROFILE_FUNCTION();

    Renderer* renderer = Renderer::GetInstance();

    if (!renderer->m_EnableOcclusionCulling || renderer->m_FreezeCullingCamera)
    {
        return;
    }

    nvrhi::utils::ScopedMarker commandListMarker{ commandList, "Generate HZB Mips" };

    // First, build HZB mip 0 from depth texture
    {
        nvrhi::utils::ScopedMarker hzbFromDepthMarker{ commandList, "HZB From Depth" };

        nvrhi::BufferDesc hzbFromDepthCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(HZBFromDepthConstants), "HZBFromDepthCB", 1);
        nvrhi::BufferHandle hzbFromDepthCB = renderer->m_NvrhiDevice->createBuffer(hzbFromDepthCBD);
        renderer->m_RHI.SetDebugName(hzbFromDepthCB, "HZBFromDepthCB");

        HZBFromDepthConstants hzbFromDepthData;
        hzbFromDepthData.m_Width = renderer->m_HZBTexture->getDesc().width;
        hzbFromDepthData.m_Height = renderer->m_HZBTexture->getDesc().height;
        commandList->writeBuffer(hzbFromDepthCB, &hzbFromDepthData, sizeof(hzbFromDepthData), 0);

        nvrhi::BindingSetDesc hzbFromDepthBset;
        hzbFromDepthBset.bindings =
        {
            nvrhi::BindingSetItem::ConstantBuffer(0, hzbFromDepthCB),
            nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_DepthTexture),
            nvrhi::BindingSetItem::Texture_UAV(0, renderer->m_HZBTexture,  nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
            nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().MinReductionClamp)
        };
        nvrhi::BindingLayoutHandle hzbFromDepthLayout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(hzbFromDepthBset, nvrhi::ShaderType::Compute);
        nvrhi::BindingSetHandle hzbFromDepthBindingSet = renderer->m_NvrhiDevice->createBindingSet(hzbFromDepthBset, hzbFromDepthLayout);

        nvrhi::ComputeState hzbFromDepthState;
        hzbFromDepthState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("HZBFromDepth_HZBFromDepth_CSMain"), hzbFromDepthLayout);
        hzbFromDepthState.bindings = { hzbFromDepthBindingSet };

        commandList->setComputeState(hzbFromDepthState);
        uint32_t dispatchX = DivideAndRoundUp(hzbFromDepthData.m_Width, 8);
        uint32_t dispatchY = DivideAndRoundUp(hzbFromDepthData.m_Height, 8);
        commandList->dispatch(dispatchX, dispatchY, 1);
    }

    // Generate HZB mips using SPD downsample
    nvrhi::utils::ScopedMarker spdMarker{ commandList, "HZB Downsample SPD" };

    uint32_t numMips = renderer->m_HZBTexture->getDesc().mipLevels;

    // We generate mips 1..N. SPD will be configured to take mip 0 as source.
    // So SPD "mips" count is numMips - 1. 
    // Note: SPD refers to how many downsample steps to take.
    uint32_t spdmips = numMips - 1;

    FfxUInt32x2 dispatchThreadGroupCountXY;
    FfxUInt32x2 workGroupOffset;
    FfxUInt32x2 numWorkGroupsAndMips;
    FfxUInt32x4 rectInfo = { 0, 0, renderer->m_HZBTexture->getDesc().width, renderer->m_HZBTexture->getDesc().height };

    ffxSpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo, spdmips);

    // Constant buffer matching HZBDownsampleSPD.hlsl
    SpdConstants spdData;
    spdData.m_Mips = numWorkGroupsAndMips[1];
    spdData.m_NumWorkGroups = numWorkGroupsAndMips[0];
    spdData.m_WorkGroupOffset.x = workGroupOffset[0];
    spdData.m_WorkGroupOffset.y = workGroupOffset[1];

    nvrhi::BufferDesc spdCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(spdData), "SpdCB", 1);
    nvrhi::BufferHandle spdCB = renderer->m_NvrhiDevice->createBuffer(spdCBD);
    commandList->writeBuffer(spdCB, &spdData, sizeof(spdData), 0);

    // Clear atomic counter
    commandList->clearBufferUInt(renderer->m_SPDAtomicCounter, 0);

    nvrhi::BindingSetDesc spdBset;
    spdBset.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(0, spdCB));
    spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_HZBTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{ 0, 1, 0, 1 }));

    // Bind mips 1..N to UAV slots 0..N-1
    uint32_t i = 1;
    for (; i < numMips; ++i)
    {
        spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(i - 1, renderer->m_HZBTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{ i, 1, 0, 1 }));
    }
    for (; i <= 12; ++i)
    {
        // Fill remaining UAV slots with a dummy to satisfy binding layout
        spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(i - 1, CommonResources::GetInstance().DummyUAVTexture));
    }

    // Atomic counter always at slot 12
    spdBset.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(12, renderer->m_SPDAtomicCounter));

    nvrhi::BindingLayoutHandle spdLayout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(spdBset, nvrhi::ShaderType::Compute);
    nvrhi::BindingSetHandle spdBindingSet = renderer->m_NvrhiDevice->createBindingSet(spdBset, spdLayout);

    nvrhi::ComputeState spdState;
    spdState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("HZBDownsampleSPD_HZBDownsampleSPD_CSMain"), spdLayout);
    spdState.bindings = { spdBindingSet };

    commandList->setComputeState(spdState);
    commandList->dispatch(dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1], 1);
}
