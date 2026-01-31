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

    // GPU buffers for meshlet rendering
    nvrhi::BufferHandle m_MeshletJobBuffer;
    nvrhi::BufferHandle m_MeshletJobCountBuffer;
    nvrhi::BufferHandle m_MeshletIndirectBuffer;

    // GPU buffers for instance culling
    nvrhi::BufferHandle m_VisibleIndirectBuffer;
    nvrhi::BufferHandle m_VisibleCountBuffer;
    nvrhi::BufferHandle m_OccludedIndicesBuffer;
    nvrhi::BufferHandle m_OccludedCountBuffer;
    nvrhi::BufferHandle m_OccludedIndirectBuffer;

    struct BasePassRenderingArgs
    {
        uint32_t m_InstanceBaseIndex;
        const Vector4* m_FrustumPlanes;
        Matrix m_View;
        Matrix m_ViewProj;
        uint32_t m_NumInstances;
        int m_CullingPhase;
        uint32_t m_AlphaMode;
        const char* m_BucketName;
    };

    void GenerateHZBMips(nvrhi::CommandListHandle commandList);
    void ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[5]);
    void PerformOcclusionCulling(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args);
    void RenderInstances(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args);
};

REGISTER_RENDERER(BasePassRenderer);

void BasePassRenderer::PerformOcclusionCulling(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args)
{
    PROFILE_FUNCTION();

    Renderer* renderer = Renderer::GetInstance();

    char marker[256];
    sprintf(marker, "Occlusion Culling Phase %d - %s", args.m_CullingPhase + 1, args.m_BucketName);
    nvrhi::utils::ScopedMarker commandListMarker{ commandList, marker };

    if (args.m_CullingPhase == 0)
    {
        // No-op clearing, done in Render()
    }
    else if (args.m_CullingPhase == 1)
    {
        // Clear visible count buffer for Phase 2
        commandList->clearBufferUInt(m_VisibleCountBuffer, 0);

        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(m_MeshletJobCountBuffer, 0);
        }
    }

    const nvrhi::BufferDesc cullCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(CullingConstants), args.m_CullingPhase == 0 ? "CullingCB" : "CullingCB_Phase2", 1);
    const nvrhi::BufferHandle cullCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(cullCBD);

    const Matrix& projectionMatrix = renderer->m_Camera.GetProjMatrix();

    CullingConstants cullData;
    cullData.m_NumPrimitives = args.m_NumInstances;
    memcpy(cullData.m_FrustumPlanes, args.m_FrustumPlanes, 5 * sizeof(Vector4));
    cullData.m_View = args.m_View;
    cullData.m_ViewProj = args.m_ViewProj;
    cullData.m_EnableFrustumCulling = renderer->m_EnableFrustumCulling ? 1 : 0;
    cullData.m_EnableOcclusionCulling = renderer->m_EnableOcclusionCulling ? 1 : 0;
    cullData.m_HZBWidth = renderer->m_HZBTexture->getDesc().width;
    cullData.m_HZBHeight = renderer->m_HZBTexture->getDesc().height;
    cullData.m_Phase = args.m_CullingPhase;
    cullData.m_UseMeshletRendering = renderer->m_UseMeshletRendering ? 1 : 0;
    cullData.m_P00 = projectionMatrix.m[0][0];
    cullData.m_P11 = projectionMatrix.m[1][1];
    cullData.m_ForcedLOD = renderer->m_ForcedLOD;
    cullData.m_InstanceBaseIndex = args.m_InstanceBaseIndex;
    cullData.m_BucketIndex = 0; // Fixed at 0
    cullData.m_BucketVisibleOffset = 0; // Fixed at 0
    commandList->writeBuffer(cullCB, &cullData, sizeof(cullData), 0);

    nvrhi::BindingSetDesc cullBset;
    cullBset.bindings =
    {
        nvrhi::BindingSetItem::ConstantBuffer(0, cullCB),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, renderer->m_Scene.m_InstanceDataBuffer),
        nvrhi::BindingSetItem::Texture_SRV(1, renderer->m_HZBTexture),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, renderer->m_Scene.m_MeshDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_VisibleIndirectBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_VisibleCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_OccludedIndicesBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_OccludedCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_OccludedIndirectBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_MeshletJobBuffer ? m_MeshletJobBuffer : m_VisibleIndirectBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(6, m_MeshletJobCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(7, m_MeshletIndirectBuffer),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().MinReductionClamp)
    };
    const nvrhi::BindingLayoutHandle cullLayout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(cullBset, nvrhi::ShaderType::Compute);
    const nvrhi::BindingSetHandle cullBindingSet = renderer->m_RHI->m_NvrhiDevice->createBindingSet(cullBset, cullLayout);

    nvrhi::ComputeState cullState;
    cullState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("GPUCulling_Culling_CSMain"), cullLayout);
    cullState.bindings = { cullBindingSet };

    commandList->setComputeState(cullState);
    if (args.m_CullingPhase == 0)
    {
        const uint32_t dispatchX = DivideAndRoundUp(args.m_NumInstances, kThreadsPerGroup);
        commandList->dispatch(dispatchX, 1, 1);
    }
    else
    {
        cullState.indirectParams = m_OccludedIndirectBuffer;
        commandList->setComputeState(cullState);
        commandList->dispatchIndirect(0);
    }

    nvrhi::utils::ScopedMarker buildIndirectMarker{ commandList, "Build Indirect Arguments" };

    // Build indirect for Phase 2 culling and/or meshlet rendering
    nvrhi::ComputeState buildIndirectState;
    buildIndirectState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("GPUCulling_BuildIndirect_CSMain"), cullLayout);
    buildIndirectState.bindings = { cullBindingSet };
    commandList->setComputeState(buildIndirectState);
    commandList->dispatch(1, 1, 1);
}

void BasePassRenderer::ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[5])
{
    const float xScale = fabs(proj._11);
    const float yScale = fabs(proj._22);
    const float nearZ = proj._43;

    const DirectX::XMVECTOR planes[5] = {
        DirectX::XMPlaneNormalize(DirectX::XMVectorSet(-1.0f, 0.0f, 1.0f / xScale, 0.0f)),
        DirectX::XMPlaneNormalize(DirectX::XMVectorSet(1.0f, 0.0f, 1.0f / xScale, 0.0f)),
        DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, -1.0f, 1.0f / yScale, 0.0f)),
        DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, 1.0f, 1.0f / yScale, 0.0f)),
        DirectX::XMPlaneNormalize(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, nearZ))
    };

    for (int i = 0; i < 5; ++i) {
        DirectX::XMStoreFloat4(&frustumPlanes[i], planes[i]);
    }
}

void BasePassRenderer::RenderInstances(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args)
{
    PROFILE_FUNCTION();

    Renderer* renderer = Renderer::GetInstance();

    char marker[256];
    sprintf(marker, "Base Pass Render (Phase %d) - %s", args.m_CullingPhase + 1, args.m_BucketName);
    nvrhi::utils::ScopedMarker commandListMarker(commandList, marker);

    const nvrhi::FramebufferHandle framebuffer = renderer->m_RHI->m_NvrhiDevice->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(renderer->GetCurrentBackBufferTexture()).setDepthAttachment(renderer->m_DepthTexture));

    nvrhi::FramebufferInfoEx fbInfo;
    fbInfo.colorFormats = { renderer->m_RHI->m_SwapchainFormat };
    fbInfo.setDepthFormat(nvrhi::Format::D32);

    const uint32_t w = renderer->m_RHI->m_SwapchainExtent.x;
    const uint32_t h = renderer->m_RHI->m_SwapchainExtent.y;

    nvrhi::ViewportState viewportState;
    viewportState.viewports.push_back(nvrhi::Viewport(0.0f, (float)w, 0.0f, (float)h, 0.0f, 1.0f));
    viewportState.scissorRects.resize(1);
    viewportState.scissorRects[0].minX = 0;
    viewportState.scissorRects[0].minY = 0;
    viewportState.scissorRects[0].maxX = (int)w;
    viewportState.scissorRects[0].maxY = (int)h;

    const nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(
        (uint32_t)sizeof(ForwardLightingPerFrameData), "PerFrameCB", 1);
    const nvrhi::BufferHandle perFrameCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(cbd);

    nvrhi::BindingSetDesc bset;
    bset.bindings =
    {
        nvrhi::BindingSetItem::ConstantBuffer(0, perFrameCB),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, renderer->m_Scene.m_InstanceDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, renderer->m_Scene.m_MaterialConstantsBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, renderer->m_Scene.m_VertexBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, renderer->m_Scene.m_MeshletBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, renderer->m_Scene.m_MeshletVerticesBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, renderer->m_Scene.m_MeshletTrianglesBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_MeshletJobBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(7, renderer->m_Scene.m_MeshDataBuffer),
        nvrhi::BindingSetItem::Texture_SRV(8, renderer->m_HZBTexture),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().AnisotropicClamp),
        nvrhi::BindingSetItem::Sampler(1, CommonResources::GetInstance().AnisotropicWrap),
        nvrhi::BindingSetItem::Sampler(2, CommonResources::GetInstance().MinReductionClamp)
    };
    const nvrhi::BindingLayoutHandle layout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bset, nvrhi::ShaderType::All);
    const nvrhi::BindingSetHandle bindingSet = renderer->m_RHI->m_NvrhiDevice->createBindingSet(bset, layout);

    Vector3 camPos = renderer->m_Camera.GetPosition();
    const Matrix& projectionMatrix = renderer->m_Camera.GetProjMatrix();

    ForwardLightingPerFrameData cb{};
    cb.m_ViewProj = args.m_ViewProj;
    cb.m_View = args.m_View;
    memcpy(cb.m_FrustumPlanes, args.m_FrustumPlanes, sizeof(Vector4) * 5);
    cb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 0.0f };

    Vector3 cullingCamPos = camPos;
    if (renderer->m_FreezeCullingCamera)
    {
        cullingCamPos = renderer->m_FrozenCullingCameraPos;
    }
    cb.m_CullingCameraPos = Vector4{ cullingCamPos.x, cullingCamPos.y, cullingCamPos.z, 0.0f };

    cb.m_LightDirection = renderer->m_Scene.GetDirectionalLightDirection();
    cb.m_LightIntensity = renderer->m_Scene.m_DirectionalLight.intensity / 10000.0f;
    cb.m_DebugMode = (uint32_t)renderer->m_DebugMode;
    cb.m_EnableFrustumCulling = renderer->m_EnableFrustumCulling ? 1 : 0;
    cb.m_EnableConeCulling = (renderer->m_EnableConeCulling && args.m_AlphaMode == ALPHA_MODE_OPAQUE) ? 1 : 0;
    cb.m_EnableOcclusionCulling = renderer->m_EnableOcclusionCulling ? 1 : 0;
    cb.m_HZBWidth = (uint32_t)renderer->m_HZBTexture->getDesc().width;
    cb.m_HZBHeight = (uint32_t)renderer->m_HZBTexture->getDesc().height;
    cb.m_P00 = projectionMatrix.m[0][0];
    cb.m_P11 = projectionMatrix.m[1][1];
    commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

    nvrhi::RenderState renderState;
    renderState.rasterState = CommonResources::GetInstance().RasterCullBack;
    
    if (args.m_AlphaMode == ALPHA_MODE_BLEND)
    {
        renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetAlpha;
        renderState.depthStencilState = CommonResources::GetInstance().DepthRead;
    }
    else
    {
        renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetOpaque;
        renderState.depthStencilState = CommonResources::GetInstance().DepthReadWrite;
    }

    if (args.m_AlphaMode != ALPHA_MODE_OPAQUE)
    {
        renderState.rasterState = CommonResources::GetInstance().RasterCullNone;
    }

    const bool bUseAlphaTest = (args.m_AlphaMode == ALPHA_MODE_MASK);
    const char* psName = bUseAlphaTest ? "ForwardLighting_PSMain_AlphaTest" : "ForwardLighting_PSMain";

    if (renderer->m_UseMeshletRendering)
    {
        nvrhi::MeshletPipelineDesc meshPipelineDesc;
        meshPipelineDesc.AS = renderer->GetShaderHandle("ForwardLighting_ASMain");
        meshPipelineDesc.MS = renderer->GetShaderHandle("ForwardLighting_MSMain");
        meshPipelineDesc.PS = renderer->GetShaderHandle(psName);
        meshPipelineDesc.renderState = renderState;
        meshPipelineDesc.bindingLayouts = { renderer->GetGlobalTextureBindingLayout(), layout };
        meshPipelineDesc.useDrawIndex = true;

        const nvrhi::MeshletPipelineHandle meshPipeline = renderer->GetOrCreateMeshletPipeline(meshPipelineDesc, fbInfo);

        nvrhi::MeshletState meshState;
        meshState.framebuffer = framebuffer;
        meshState.pipeline = meshPipeline;
        meshState.bindings = { renderer->GetGlobalTextureDescriptorTable(), bindingSet };
        meshState.viewport = viewportState;
        meshState.indirectParams = m_MeshletIndirectBuffer;
        meshState.indirectCountBuffer = m_MeshletJobCountBuffer;

        commandList->setMeshletState(meshState);
        commandList->dispatchMeshIndirectCount(0, 0, args.m_NumInstances);
    }
    else
    {
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.VS = renderer->GetShaderHandle("ForwardLighting_VSMain");
        pipelineDesc.PS = renderer->GetShaderHandle(psName);
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.renderState = renderState;
        pipelineDesc.bindingLayouts = { renderer->GetGlobalTextureBindingLayout(), layout };
        pipelineDesc.useDrawIndex = true;

        nvrhi::GraphicsState state;
        state.framebuffer = framebuffer;
        state.viewport = viewportState;
        state.indexBuffer = nvrhi::IndexBufferBinding{ renderer->m_Scene.m_IndexBuffer, nvrhi::Format::R32_UINT, 0 };
        state.bindings = { renderer->GetGlobalTextureDescriptorTable(), bindingSet };
        state.pipeline = renderer->GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
        state.indirectParams = m_VisibleIndirectBuffer;
        state.indirectCountBuffer = m_VisibleCountBuffer;
        commandList->setGraphicsState(state);

        commandList->drawIndexedIndirectCount(0, 0, args.m_NumInstances);
    }
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

        HZBFromDepthConstants hzbFromDepthData;
        hzbFromDepthData.m_Width = renderer->m_HZBTexture->getDesc().width;
        hzbFromDepthData.m_Height = renderer->m_HZBTexture->getDesc().height;

        nvrhi::BindingSetDesc hzbFromDepthBset;
        hzbFromDepthBset.bindings =
        {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(HZBFromDepthConstants)),
            nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_DepthTexture),
            nvrhi::BindingSetItem::Texture_UAV(0, renderer->m_HZBTexture,  nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
            nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().MinReductionClamp)
        };
        const nvrhi::BindingLayoutHandle hzbFromDepthLayout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(hzbFromDepthBset, nvrhi::ShaderType::Compute);
        const nvrhi::BindingSetHandle hzbFromDepthBindingSet = renderer->m_RHI->m_NvrhiDevice->createBindingSet(hzbFromDepthBset, hzbFromDepthLayout);

        nvrhi::ComputeState hzbFromDepthState;
        hzbFromDepthState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("HZBFromDepth_HZBFromDepth_CSMain"), hzbFromDepthLayout);
        hzbFromDepthState.bindings = { hzbFromDepthBindingSet };

        commandList->setComputeState(hzbFromDepthState);
        commandList->setPushConstants(&hzbFromDepthData, sizeof(hzbFromDepthData));
        const uint32_t dispatchX = DivideAndRoundUp(hzbFromDepthData.m_Width, 8);
        const uint32_t dispatchY = DivideAndRoundUp(hzbFromDepthData.m_Height, 8);
        commandList->dispatch(dispatchX, dispatchY, 1);
    }

    // Generate HZB mips using SPD downsample
    nvrhi::utils::ScopedMarker spdMarker{ commandList, "HZB Downsample SPD" };

    const uint32_t numMips = renderer->m_HZBTexture->getDesc().mipLevels;

    // We generate mips 1..N. SPD will be configured to take mip 0 as source.
    // So SPD "mips" count is numMips - 1. 
    // Note: SPD refers to how many downsample steps to take.
    const uint32_t spdmips = numMips - 1;

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

    // Clear atomic counter
    commandList->clearBufferUInt(renderer->m_SPDAtomicCounter, 0);

    nvrhi::BindingSetDesc spdBset;
    spdBset.bindings.push_back(nvrhi::BindingSetItem::PushConstants(0, sizeof(SpdConstants)));
    spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_HZBTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{ 0, 1, 0, 1 }));

    // Bind mips 1..N to UAV slots 0..N-1
    for (uint32_t i = 1; i < numMips; ++i)
    {
        spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(i - 1, renderer->m_HZBTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{ i, 1, 0, 1 }));
    }
    for (uint32_t i = numMips; i <= 12; ++i)
    {
        // Fill remaining UAV slots with a dummy to satisfy binding layout
        spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(i - 1, CommonResources::GetInstance().DummyUAVTexture));
    }

    // Atomic counter always at slot 12
    spdBset.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(12, renderer->m_SPDAtomicCounter));

    const nvrhi::BindingLayoutHandle spdLayout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(spdBset, nvrhi::ShaderType::Compute);
    const nvrhi::BindingSetHandle spdBindingSet = renderer->m_RHI->m_NvrhiDevice->createBindingSet(spdBset, spdLayout);

    nvrhi::ComputeState spdState;
    spdState.pipeline = renderer->GetOrCreateComputePipeline(renderer->GetShaderHandle("HZBDownsampleSPD_HZBDownsampleSPD_CSMain"), spdLayout);
    spdState.bindings = { spdBindingSet };

    commandList->setComputeState(spdState);
    commandList->setPushConstants(&spdData, sizeof(spdData));
    commandList->dispatch(dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1], 1);
}

bool BasePassRenderer::Initialize()
{
    Renderer* renderer = Renderer::GetInstance();

    // Create pipeline statistics queries for double buffering
    m_PipelineQueries[0] = renderer->m_RHI->m_NvrhiDevice->createPipelineStatisticsQuery();
    m_PipelineQueries[1] = renderer->m_RHI->m_NvrhiDevice->createPipelineStatisticsQuery();

    // Create constant-sized buffers
    const nvrhi::BufferDesc visibleCountBufDesc = nvrhi::BufferDesc()
        .setByteSize(sizeof(uint32_t))
        .setStructStride(sizeof(uint32_t))
        .setCanHaveUAVs(true)
        .setIsDrawIndirectArgs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDebugName("VisibleCount");
    m_VisibleCountBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(visibleCountBufDesc);

    const nvrhi::BufferDesc occludedCountBufDesc = nvrhi::BufferDesc()
        .setByteSize(sizeof(uint32_t))
        .setStructStride(sizeof(uint32_t))
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDebugName("OccludedCount");
    m_OccludedCountBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(occludedCountBufDesc);

    const nvrhi::BufferDesc occludedIndirectBufDesc = nvrhi::BufferDesc()
        .setByteSize(sizeof(DispatchIndirectArguments))
        .setStructStride(sizeof(DispatchIndirectArguments))
        .setIsDrawIndirectArgs(true)
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDebugName("OccludedIndirectBuffer");
    m_OccludedIndirectBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(occludedIndirectBufDesc);

    const nvrhi::BufferDesc meshletJobCountBufDesc = nvrhi::BufferDesc()
        .setByteSize(sizeof(uint32_t))
        .setStructStride(sizeof(uint32_t))
        .setIsDrawIndirectArgs(true)
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDebugName("MeshletJobCount");
    m_MeshletJobCountBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(meshletJobCountBufDesc);

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

    const int readIndex = renderer->m_FrameNumber % 2;
    const int writeIndex = (renderer->m_FrameNumber + 1) % 2;
    if (renderer->m_RHI->m_NvrhiDevice->pollPipelineStatisticsQuery(m_PipelineQueries[readIndex]))
    {
        renderer->m_MainViewPipelineStatistics = renderer->m_RHI->m_NvrhiDevice->getPipelineStatistics(m_PipelineQueries[readIndex]);
        renderer->m_RHI->m_NvrhiDevice->resetPipelineStatisticsQuery(m_PipelineQueries[readIndex]);
    }
    commandList->beginPipelineStatisticsQuery(m_PipelineQueries[writeIndex]);

    Camera* cam = &renderer->m_Camera;
    const Matrix viewProj = cam->GetViewProjMatrix();
    Matrix viewProjForCulling = viewProj;
    const Matrix origView = cam->GetViewMatrix();
    Matrix view = origView;
    if (renderer->m_FreezeCullingCamera)
    {
        view = renderer->m_FrozenCullingViewMatrix;
    }
    const Vector3 camPos = renderer->m_Camera.GetPosition();

    const uint32_t numOpaque = renderer->m_Scene.m_OpaqueBucket.m_Count;
    const uint32_t numMasked = renderer->m_Scene.m_MaskedBucket.m_Count;
    const uint32_t numTransparent = renderer->m_Scene.m_TransparentBucket.m_Count;

    // Compute frustum planes
    Vector4 frustumPlanes[5];
    ComputeFrustumPlanes(cam->GetProjMatrix(), frustumPlanes);

    if (renderer->m_FreezeCullingCamera)
    {
        const Matrix proj = cam->GetProjMatrix();

        // viewProj = view * proj
        const DirectX::XMMATRIX v = DirectX::XMLoadFloat4x4(&renderer->m_FrozenCullingViewMatrix);
        const DirectX::XMMATRIX p = DirectX::XMLoadFloat4x4(&proj);
        DirectX::XMStoreFloat4x4(&viewProjForCulling, v * p);
    }

    // Allocate buffers once. Primitives/meshlet counts are static for this scene.
    if (!m_VisibleIndirectBuffer)
    {
        const nvrhi::BufferDesc visibleIndirectBufDesc = nvrhi::BufferDesc()
            .setByteSize(numPrimitives * sizeof(nvrhi::DrawIndexedIndirectArguments))
            .setStructStride(sizeof(nvrhi::DrawIndexedIndirectArguments))
            .setIsDrawIndirectArgs(true)
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName("VisibleIndirectBuffer");
        m_VisibleIndirectBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(visibleIndirectBufDesc);

        const nvrhi::BufferDesc occludedIndicesBufDesc = nvrhi::BufferDesc()
            .setByteSize(numPrimitives * sizeof(uint32_t))
            .setStructStride(sizeof(uint32_t))
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName("OccludedIndices");
        m_OccludedIndicesBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(occludedIndicesBufDesc);
    }

    if (!m_MeshletIndirectBuffer)
    {
        const nvrhi::BufferDesc meshletIndirectBufDesc = nvrhi::BufferDesc()
            .setByteSize(numPrimitives * sizeof(DispatchIndirectArguments)) // One per potential instance
            .setStructStride(sizeof(DispatchIndirectArguments))
            .setIsDrawIndirectArgs(true)
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName("MeshletIndirectBuffer");
        m_MeshletIndirectBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(meshletIndirectBufDesc);
    }

    if (!m_MeshletJobBuffer)
    {
        const nvrhi::BufferDesc meshletJobBufDesc = nvrhi::BufferDesc()
            .setByteSize(numPrimitives * sizeof(MeshletJob))
            .setStructStride(sizeof(MeshletJob))
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName("MeshletJobBuffer");
        m_MeshletJobBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(meshletJobBufDesc);
    }

    auto ClearVisibleCounters = [&]
    {
        nvrhi::utils::ScopedMarker clearMarker{ commandList, "Clear Visible Counters" };
        commandList->clearBufferUInt(m_VisibleCountBuffer, 0);
        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(m_MeshletJobCountBuffer, 0);
        }
    };

    auto ClearAllCounters = [&]
    {
        nvrhi::utils::ScopedMarker clearMarker{ commandList, "Clear All Counters" };
        commandList->clearBufferUInt(m_VisibleCountBuffer, 0);
        commandList->clearBufferUInt(m_OccludedCountBuffer, 0);
        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(m_MeshletJobCountBuffer, 0);
        }
    };

    BasePassRenderingArgs renderingArgs;
    renderingArgs.m_FrustumPlanes = frustumPlanes;
    renderingArgs.m_View = view;
    renderingArgs.m_ViewProj = viewProjForCulling;

    auto SetupArgs = [&](uint32_t numInstances, uint32_t baseIdx, const char* name, int phase, uint32_t alphaMode)
    {
        renderingArgs.m_NumInstances = numInstances;
        renderingArgs.m_InstanceBaseIndex = baseIdx;
        renderingArgs.m_BucketName = name;
        renderingArgs.m_CullingPhase = phase;
        renderingArgs.m_AlphaMode = alphaMode;
    };

    // --- PHASE 1: Opaque rendering (test against previous frame HZB) ---
    if (numOpaque > 0)
    {
        ClearAllCounters();
        SetupArgs(numOpaque, renderer->m_Scene.m_OpaqueBucket.m_BaseIndex, "Opaque", 0, ALPHA_MODE_OPAQUE);
        PerformOcclusionCulling(commandList, renderingArgs);
        RenderInstances(commandList, renderingArgs);
    }

    if (renderer->m_EnableOcclusionCulling)
    {
        // --- Generate HZB from Phase 1 Opaque depths ---
        GenerateHZBMips(commandList);

        // --- PHASE 2: Previously occluded opaque + Masked + Transparent (test against new HZB) ---
        
        // 1. Opaque Phase 2 (Occluded instances re-test)
        if (numOpaque > 0)
        {
            SetupArgs(numOpaque, renderer->m_Scene.m_OpaqueBucket.m_BaseIndex, "Opaque (Occluded)", 1, ALPHA_MODE_OPAQUE);
            
            // PerformOcclusionCulling(Phase 1) clears visible count internally
            PerformOcclusionCulling(commandList, renderingArgs);
            RenderInstances(commandList, renderingArgs);
        }

        // 2. Masked (All instances test against new HZB)
        if (numMasked > 0)
        {
            ClearVisibleCounters();
            SetupArgs(numMasked, renderer->m_Scene.m_MaskedBucket.m_BaseIndex, "Masked", 0, ALPHA_MODE_MASK);

            PerformOcclusionCulling(commandList, renderingArgs);
            RenderInstances(commandList, renderingArgs);
        }

        // --- Generate final HZB including Opaque P2 and Masked depths ---
        GenerateHZBMips(commandList);

        // 3. Transparent (All instances test against latest HZB)
        if (numTransparent > 0)
        {
            ClearVisibleCounters();
            SetupArgs(numTransparent, renderer->m_Scene.m_TransparentBucket.m_BaseIndex, "Transparent", 0, ALPHA_MODE_BLEND);

            PerformOcclusionCulling(commandList, renderingArgs);
            RenderInstances(commandList, renderingArgs);
        }
    }
    else
    {
        // Occlusion off: Render remaining buckets
        if (numMasked > 0)
        {
            ClearVisibleCounters();
            SetupArgs(numMasked, renderer->m_Scene.m_MaskedBucket.m_BaseIndex, "Masked (No Occlusion)", 0, ALPHA_MODE_MASK);

            PerformOcclusionCulling(commandList, renderingArgs);
            RenderInstances(commandList, renderingArgs);
        }
        if (numTransparent > 0)
        {
            ClearVisibleCounters();
            SetupArgs(numTransparent, renderer->m_Scene.m_TransparentBucket.m_BaseIndex, "Transparent (No Occlusion)", 0, ALPHA_MODE_BLEND);

            PerformOcclusionCulling(commandList, renderingArgs);
            RenderInstances(commandList, renderingArgs);
        }
    }

    commandList->endPipelineStatisticsQuery(m_PipelineQueries[writeIndex]);
}
