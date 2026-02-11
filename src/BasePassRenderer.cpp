#include "Renderer.h"
#include "CommonResources.h"
#include "BasePassCommon.h"
#include "Camera.h"
#include "Utilities.h"

#include "shaders/ShaderShared.h"

class BasePassRendererBase : public IRenderer
{
public:
    void Initialize() override { }
    void PostSceneLoad() override { }
    virtual void Render(nvrhi::CommandListHandle commandList) override = 0;

protected:
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
        bool m_BackFaceCull = false;
    };

    void GenerateHZBMips(nvrhi::CommandListHandle commandList);
    void ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[5]);
    void PerformOcclusionCulling(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args);
    void RenderInstances(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args);
    void PrepareRenderingData(Matrix& outView, Matrix& outViewProjForCulling, Vector4 outFrustumPlanes[5]);

    void ClearVisibleCounters(nvrhi::CommandListHandle commandList)
    {
        Renderer* renderer = Renderer::GetInstance();
        BasePassResources& res = renderer->m_BasePassResources;
        nvrhi::utils::ScopedMarker clearMarker{ commandList, "Clear Visible Counters" };
        commandList->clearBufferUInt(res.m_VisibleCountBuffer, 0);
        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(res.m_MeshletJobCountBuffer, 0);
        }
    }

    void ClearAllCounters(nvrhi::CommandListHandle commandList)
    {
        Renderer* renderer = Renderer::GetInstance();
        BasePassResources& res = renderer->m_BasePassResources;
        nvrhi::utils::ScopedMarker clearMarker{ commandList, "Clear All Counters" };
        commandList->clearBufferUInt(res.m_VisibleCountBuffer, 0);
        commandList->clearBufferUInt(res.m_OccludedCountBuffer, 0);
        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(res.m_MeshletJobCountBuffer, 0);
        }
    }
};

void BasePassRendererBase::PrepareRenderingData(Matrix& outView, Matrix& outViewProjForCulling, Vector4 outFrustumPlanes[5])
{
    Renderer* renderer = Renderer::GetInstance();
    Camera* cam = &renderer->m_Camera;
    const Matrix viewProj = cam->GetViewProjMatrix();
    outView = cam->GetViewMatrix();
    outViewProjForCulling = viewProj;
    if (renderer->m_FreezeCullingCamera)
    {
        outView = renderer->m_FrozenCullingViewMatrix;
        const Matrix proj = cam->GetProjMatrix();
        const DirectX::XMMATRIX v = DirectX::XMLoadFloat4x4(&renderer->m_FrozenCullingViewMatrix);
        const DirectX::XMMATRIX p = DirectX::XMLoadFloat4x4(&proj);
        DirectX::XMStoreFloat4x4(&outViewProjForCulling, v * p);
    }
    ComputeFrustumPlanes(cam->GetProjMatrix(), outFrustumPlanes);
}

void BasePassRendererBase::PerformOcclusionCulling(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args)
{
    PROFILE_FUNCTION();

    Renderer* renderer = Renderer::GetInstance();
    BasePassResources& res = renderer->m_BasePassResources;

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
        commandList->clearBufferUInt(res.m_VisibleCountBuffer, 0);

        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(res.m_MeshletJobCountBuffer, 0);
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
    commandList->writeBuffer(cullCB, &cullData, sizeof(cullData), 0);

    nvrhi::BindingSetDesc cullBset;
    cullBset.bindings =
    {
        nvrhi::BindingSetItem::ConstantBuffer(0, cullCB),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, renderer->m_Scene.m_InstanceDataBuffer),
        nvrhi::BindingSetItem::Texture_SRV(1, renderer->m_HZBTexture),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, renderer->m_Scene.m_MeshDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(0, res.m_VisibleIndirectBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(1, res.m_VisibleCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(2, res.m_OccludedIndicesBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(3, res.m_OccludedCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(4, args.m_CullingPhase == 0 ? res.m_OccludedIndirectBuffer : CommonResources::GetInstance().DummyUAVBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(5, res.m_MeshletJobBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(6, res.m_MeshletJobCountBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(7, res.m_MeshletIndirectBuffer),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().MinReductionClamp)
    };

    if (args.m_CullingPhase == 0)
    {
        const uint32_t dispatchX = DivideAndRoundUp(args.m_NumInstances, kThreadsPerGroup);
        Renderer::RenderPassParams params;
        params.commandList = commandList;
        params.shaderName = "GPUCulling_Culling_CSMain";
        params.bindingSetDesc = cullBset;
        params.dispatchParams = { .x = dispatchX, .y = 1, .z = 1 };
        renderer->AddComputePass(params);
    }
    else
    {
        Renderer::RenderPassParams params;
        params.commandList = commandList;
        params.shaderName = "GPUCulling_Culling_CSMain";
        params.bindingSetDesc = cullBset;
        params.dispatchParams = { .indirectBuffer = res.m_OccludedIndirectBuffer, .indirectOffsetBytes = 0 };
        renderer->AddComputePass(params);
    }

    nvrhi::utils::ScopedMarker buildIndirectMarker{ commandList, "Build Indirect Arguments" };

    // Build indirect for Phase 2 culling and/or meshlet rendering
    if (args.m_CullingPhase == 0)
    {
        Renderer::RenderPassParams params;
        params.commandList = commandList;
        params.shaderName = "GPUCulling_BuildIndirect_CSMain";
        params.bindingSetDesc = cullBset;
        params.dispatchParams = { .x = 1, .y = 1, .z = 1 };
        renderer->AddComputePass(params);
    }
}

void BasePassRendererBase::ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[5])
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

void BasePassRendererBase::RenderInstances(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args)
{
    PROFILE_FUNCTION();

    Renderer* renderer = Renderer::GetInstance();
    BasePassResources& res = renderer->m_BasePassResources;

    char marker[256];
    sprintf(marker, "Base Pass Render (Phase %d) - %s", args.m_CullingPhase + 1, args.m_BucketName);
    nvrhi::utils::ScopedMarker commandListMarker(commandList, marker);

    const bool bUseAlphaTest = (args.m_AlphaMode == ALPHA_MODE_MASK);
    const bool bUseAlphaBlend = (args.m_AlphaMode == ALPHA_MODE_BLEND);

    const nvrhi::FramebufferHandle framebuffer = bUseAlphaBlend ? 
        renderer->m_RHI->m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(renderer->m_HDRColorTexture).setDepthAttachment(renderer->m_DepthTexture)) :
        renderer->m_RHI->m_NvrhiDevice->createFramebuffer(
        nvrhi::FramebufferDesc()
        .addColorAttachment(renderer->m_GBufferAlbedo)
        .addColorAttachment(renderer->m_GBufferNormals)
        .addColorAttachment(renderer->m_GBufferORM)
        .addColorAttachment(renderer->m_GBufferEmissive)
        .setDepthAttachment(renderer->m_DepthTexture));

    nvrhi::FramebufferInfoEx fbInfo;
    if (bUseAlphaBlend)
    {
        fbInfo.colorFormats = { Renderer::HDR_COLOR_FORMAT };
    }
    else
    {
        fbInfo.colorFormats = { 
            nvrhi::Format::RGBA8_UNORM, 
            nvrhi::Format::RG16_FLOAT, 
            nvrhi::Format::RGBA8_UNORM, 
            nvrhi::Format::RGBA8_UNORM 
        };
    }
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
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, renderer->m_Scene.m_VertexBufferQuantized),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, renderer->m_Scene.m_MeshletBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, renderer->m_Scene.m_MeshletVerticesBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, renderer->m_Scene.m_MeshletTrianglesBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, res.m_MeshletJobBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(7, renderer->m_Scene.m_MeshDataBuffer),
        nvrhi::BindingSetItem::Texture_SRV(8, renderer->m_HZBTexture),
        nvrhi::BindingSetItem::RayTracingAccelStruct(9, renderer->m_Scene.m_TLAS),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(10, renderer->m_Scene.m_IndexBuffer),
        nvrhi::BindingSetItem::Texture_SRV(11, renderer->m_OpaqueColorTexture),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().AnisotropicClamp),
        nvrhi::BindingSetItem::Sampler(1, CommonResources::GetInstance().AnisotropicWrap),
        nvrhi::BindingSetItem::Sampler(2, CommonResources::GetInstance().MinReductionClamp)
    };

    // in Vulkan, space 0 is reserved for bindless, so we use space 1 for other bindings
    const uint32_t registerSpace = renderer->m_RHI->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? 0 : 1;

    const nvrhi::BindingLayoutHandle layout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bset, registerSpace);
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
    cb.m_EnableRTShadows = renderer->m_EnableRTShadows ? 1 : 0;
    cb.m_DebugMode = (uint32_t)renderer->m_DebugMode;
    cb.m_EnableFrustumCulling = renderer->m_EnableFrustumCulling ? 1 : 0;
    cb.m_EnableConeCulling = (renderer->m_EnableConeCulling && args.m_AlphaMode == ALPHA_MODE_OPAQUE) ? 1 : 0;
    cb.m_EnableOcclusionCulling = renderer->m_EnableOcclusionCulling ? 1 : 0;
    cb.m_HZBWidth = (uint32_t)renderer->m_HZBTexture->getDesc().width;
    cb.m_HZBHeight = (uint32_t)renderer->m_HZBTexture->getDesc().height;
    cb.m_P00 = projectionMatrix.m[0][0];
    cb.m_P11 = projectionMatrix.m[1][1];
    cb.m_EnableIBL = renderer->m_EnableIBL ? 1 : 0;
    cb.m_IBLIntensity = renderer->m_IBLIntensity;
    cb.m_RadianceMipCount = CommonResources::GetInstance().RadianceTexture->getDesc().mipLevels;
    cb.m_OpaqueColorDimensions = Vector2{ (float)renderer->m_OpaqueColorTexture->getDesc().width, (float)renderer->m_OpaqueColorTexture->getDesc().height };
    commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

    nvrhi::RenderState renderState;
    renderState.rasterState = args.m_BackFaceCull ? CommonResources::GetInstance().RasterCullBack : CommonResources::GetInstance().RasterCullNone;
    
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

    const char* psName = bUseAlphaTest ? "BasePass_GBuffer_PSMain_AlphaTest_AlphaTest" : 
                         bUseAlphaBlend ? "BasePass_Forward_PSMain_Forward_Transparent" : 
                         "BasePass_GBuffer_PSMain";

    if (renderer->m_UseMeshletRendering)
    {
        nvrhi::MeshletPipelineDesc meshPipelineDesc;
        meshPipelineDesc.AS = renderer->GetShaderHandle("BasePass_ASMain");
        meshPipelineDesc.MS = renderer->GetShaderHandle("BasePass_MSMain");
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
        meshState.indirectParams = res.m_MeshletIndirectBuffer;
        meshState.indirectCountBuffer = res.m_MeshletJobCountBuffer;

        commandList->setMeshletState(meshState);
        commandList->dispatchMeshIndirectCount(0, 0, args.m_NumInstances);
    }
    else
    {
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.VS = renderer->GetShaderHandle("BasePass_VSMain");
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
        state.indirectParams = res.m_VisibleIndirectBuffer;
        state.indirectCountBuffer = res.m_VisibleCountBuffer;
        commandList->setGraphicsState(state);

        commandList->drawIndexedIndirectCount(0, 0, args.m_NumInstances);
    }
}

void BasePassRendererBase::GenerateHZBMips(nvrhi::CommandListHandle commandList)
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

        const uint32_t dispatchX = DivideAndRoundUp(hzbFromDepthData.m_Width, 8);
        const uint32_t dispatchY = DivideAndRoundUp(hzbFromDepthData.m_Height, 8);

        Renderer::RenderPassParams params;
        params.commandList = commandList;
        params.shaderName = "HZBFromDepth_HZBFromDepth_CSMain";
        params.bindingSetDesc = hzbFromDepthBset;
        params.dispatchParams = { .x = dispatchX, .y = dispatchY, .z = 1 };
        params.pushConstants = &hzbFromDepthData;
        params.pushConstantsSize = sizeof(hzbFromDepthData);
        renderer->AddComputePass(params);
    }

    renderer->GenerateMipsUsingSPD(renderer->m_HZBTexture, commandList, "Generate HZB Mips", SPD_REDUCTION_MIN);
}

class OpaquePhase1Renderer : public BasePassRendererBase
{
public:
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "OpaquePhase1"; }
};

void OpaquePhase1Renderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();
    const uint32_t numOpaque = renderer->m_Scene.m_OpaqueBucket.m_Count;
    if (numOpaque == 0) return;

    Matrix view, viewProjForCulling;
    Vector4 frustumPlanes[5];
    PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

    ClearAllCounters(commandList);

    BasePassRenderingArgs args;
    args.m_FrustumPlanes = frustumPlanes;
    args.m_View = view;
    args.m_ViewProj = viewProjForCulling;
    args.m_NumInstances = numOpaque;
    args.m_InstanceBaseIndex = renderer->m_Scene.m_OpaqueBucket.m_BaseIndex;
    args.m_BucketName = "Opaque";
    args.m_CullingPhase = 0;
    args.m_AlphaMode = ALPHA_MODE_OPAQUE;

    PerformOcclusionCulling(commandList, args);
    RenderInstances(commandList, args);
}

class HZBGenerator : public BasePassRendererBase
{
public:
    void Render(nvrhi::CommandListHandle commandList) override
    {
        GenerateHZBMips(commandList);
    }
    const char* GetName() const override { return "HZBGenerator"; }
};

class OpaquePhase2Renderer : public BasePassRendererBase
{
public:
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "OpaquePhase2"; }
};

void OpaquePhase2Renderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();
    if (!renderer->m_EnableOcclusionCulling) return;

    const uint32_t numOpaque = renderer->m_Scene.m_OpaqueBucket.m_Count;
    if (numOpaque == 0) return;

    Matrix view, viewProjForCulling;
    Vector4 frustumPlanes[5];
    PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

    BasePassRenderingArgs args;
    args.m_FrustumPlanes = frustumPlanes;
    args.m_View = view;
    args.m_ViewProj = viewProjForCulling;
    args.m_NumInstances = numOpaque;
    args.m_InstanceBaseIndex = renderer->m_Scene.m_OpaqueBucket.m_BaseIndex;
    args.m_BucketName = "Opaque (Occluded)";
    args.m_CullingPhase = 1;
    args.m_AlphaMode = ALPHA_MODE_OPAQUE;

    PerformOcclusionCulling(commandList, args);
    RenderInstances(commandList, args);
}

class MaskedPassRenderer : public BasePassRendererBase
{
public:
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "MaskedPass"; }
};

void MaskedPassRenderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();
    const uint32_t numMasked = renderer->m_Scene.m_MaskedBucket.m_Count;
    if (numMasked == 0) return;

    Matrix view, viewProjForCulling;
    Vector4 frustumPlanes[5];
    PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

    ClearVisibleCounters(commandList);

    BasePassRenderingArgs args;
    args.m_FrustumPlanes = frustumPlanes;
    args.m_View = view;
    args.m_ViewProj = viewProjForCulling;
    args.m_NumInstances = numMasked;
    args.m_InstanceBaseIndex = renderer->m_Scene.m_MaskedBucket.m_BaseIndex;
    args.m_BucketName = renderer->m_EnableOcclusionCulling ? "Masked" : "Masked (No Occlusion)";
    args.m_CullingPhase = 0;
    args.m_AlphaMode = ALPHA_MODE_MASK;

    PerformOcclusionCulling(commandList, args);
    RenderInstances(commandList, args);
}

class HZBGeneratorPhase2 : public BasePassRendererBase
{
public:
    void Render(nvrhi::CommandListHandle commandList) override
    {
        GenerateHZBMips(commandList);
    }
    const char* GetName() const override { return "HZBGeneratorPhase2"; }
};

class TransparentPassRenderer : public BasePassRendererBase
{
public:
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "TransparentPass"; }
};

void TransparentPassRenderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();
    const uint32_t numTransparent = renderer->m_Scene.m_TransparentBucket.m_Count;
    if (numTransparent == 0) return;

    // Capture the opaque scene for refraction
    commandList->copyTexture(renderer->m_OpaqueColorTexture, nvrhi::TextureSlice(), renderer->m_HDRColorTexture, nvrhi::TextureSlice());

    renderer->GenerateMipsUsingSPD(renderer->m_OpaqueColorTexture, commandList, "Generate Mips for Opaque Color", SPD_REDUCTION_AVERAGE);

    Matrix view, viewProjForCulling;
    Vector4 frustumPlanes[5];
    PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

    ClearVisibleCounters(commandList);

    BasePassRenderingArgs args;
    args.m_FrustumPlanes = frustumPlanes;
    args.m_View = view;
    args.m_ViewProj = viewProjForCulling;
    args.m_NumInstances = numTransparent;
    args.m_InstanceBaseIndex = renderer->m_Scene.m_TransparentBucket.m_BaseIndex;
    args.m_BucketName = "Transparent";
    args.m_AlphaMode = ALPHA_MODE_BLEND;
    args.m_CullingPhase = 0;
    args.m_BackFaceCull = true;

    PerformOcclusionCulling(commandList, args);
    RenderInstances(commandList, args);
}

REGISTER_RENDERER(OpaquePhase1Renderer);
REGISTER_RENDERER(HZBGenerator);
REGISTER_RENDERER(OpaquePhase2Renderer);
REGISTER_RENDERER(MaskedPassRenderer);
REGISTER_RENDERER(HZBGeneratorPhase2);
REGISTER_RENDERER(TransparentPassRenderer);
