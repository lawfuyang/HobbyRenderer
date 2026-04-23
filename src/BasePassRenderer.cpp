#include "Renderer.h"
#include "CommonResources.h"
#include "BasePassCommon.h"
#include "Camera.h"
#include "Utilities.h"

#include "shaders/srrhi/cpp/BasePass.h"
#include "shaders/srrhi/cpp/GPUCulling.h"
#include "shaders/srrhi/cpp/ResizeToNextLowestPowerOfTwo.h"

extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_HZBTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferGeoNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_HDRColor;
RGTextureHandle g_RG_OpaqueColor;

struct ScopedBasePassPipelineQuery
{
    nvrhi::CommandListHandle m_CommandList;
    BasePassResources& m_Res;
    bool m_IsSelected;
    uint32_t m_WriteIndex;

    ScopedBasePassPipelineQuery(nvrhi::CommandListHandle commandList, BasePassResources& res, IRenderer* rendererPtr)
        : m_CommandList(commandList)
        , m_Res(res)
    {
        
        m_IsSelected = (g_Renderer.m_SelectedRendererIndexForPipelineStatistics != -1 && g_Renderer.m_Renderers[g_Renderer.m_SelectedRendererIndexForPipelineStatistics].get() == rendererPtr);
        if (m_IsSelected)
        {
            nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;

            m_WriteIndex = g_Renderer.m_FrameNumber % 2;
            const uint32_t readIndex = 1 - m_WriteIndex;
            g_Renderer.m_SelectedBasePassPipelineStatistics = device->getPipelineStatistics(m_Res.m_PipelineQueries[readIndex]);

            device->resetPipelineStatisticsQuery(m_Res.m_PipelineQueries[m_WriteIndex]);
            m_CommandList->beginPipelineStatisticsQuery(m_Res.m_PipelineQueries[m_WriteIndex]);
        }
    }

    ~ScopedBasePassPipelineQuery()
    {
        if (m_IsSelected)
        {
            m_CommandList->endPipelineStatisticsQuery(m_Res.m_PipelineQueries[m_WriteIndex]);
        }
    }
};

// TODO: move this to Renderer?
static void DownsampleTextureToPow2(nvrhi::CommandListHandle commandList, nvrhi::TextureHandle inputTexture, nvrhi::TextureHandle outputTexture, uint32_t samplerIdx)
{
    PROFILE_FUNCTION();
    PROFILE_GPU_SCOPED("DownsampleTextureToPow2", commandList);

    srrhi::ResizeToNextLowestPowerOfTwoConstants consts;
    consts.SetWidth(outputTexture->getDesc().width);
    consts.SetHeight(outputTexture->getDesc().height);
    consts.SetSamplerIdx(samplerIdx);

    srrhi::DownsampleTextureToPow2Inputs inputs;
    inputs.SetConstants(&consts);
    inputs.SetInputTexture(inputTexture);
    inputs.SetOutputTexture(outputTexture, 0);
    nvrhi::BindingSetDesc bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);

    const uint32_t dispatchX = DivideAndRoundUp(outputTexture->getDesc().width, 8);
    const uint32_t dispatchY = DivideAndRoundUp(outputTexture->getDesc().height, 8);
    
    const uint32_t resizeShaderID = nvrhi::getFormatInfo(outputTexture->getDesc().format).hasBlue
        ? ShaderID::RESIZETONEXTLOWESTPOWEROFTWO_CS_RESIZETONEXTLOWESTPOWEROFTWO_NUM_CHANNELS_3
        : ShaderID::RESIZETONEXTLOWESTPOWEROFTWO_CS_RESIZETONEXTLOWESTPOWEROFTWO_NUM_CHANNELS_1;

    Renderer::RenderPassParams params;
    params.commandList = commandList;
    params.shaderID = resizeShaderID;
    params.bindingSetDesc = bindingSetDesc;
    params.dispatchParams = { .x = dispatchX, .y = dispatchY, .z = 1 };
    params.pushConstants = &consts;
    params.pushConstantsSize = srrhi::DownsampleTextureToPow2Inputs::PushConstantBytes;
    g_Renderer.AddComputePass(params);
}

static void GenerateHZBMips(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph, nvrhi::BufferHandle spdAtomicCounter)
{
    PROFILE_FUNCTION();

    if (!g_Renderer.m_EnableOcclusionCulling || g_Renderer.m_FreezeCullingCamera)
    {
        return;
    }

    nvrhi::TextureHandle depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
    nvrhi::TextureHandle hzb = renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Write);

    // First, build HZB mip 0 from depth texture
    DownsampleTextureToPow2(commandList, depth, hzb, srrhi::CommonConsts::SAMPLER_MIN_REDUCTION_INDEX);

    // Then generate the rest of the mip chain using SPD
    g_Renderer.GenerateMipsUsingSPD(hzb, spdAtomicCounter, commandList, "Generate HZB Mips", srrhi::CommonConsts::SPD_REDUCTION_MIN);
}

class BasePassRendererBase : public IRenderer
{
public:
    struct ResourceHandles
    {
        nvrhi::BufferHandle visibleCount;
        nvrhi::BufferHandle visibleIndirect;
        nvrhi::BufferHandle occludedCount;
        nvrhi::BufferHandle occludedIndices;
        nvrhi::BufferHandle occludedIndirect;
        nvrhi::BufferHandle meshletJob;
        nvrhi::BufferHandle meshletJobCount;
        nvrhi::BufferHandle meshletIndirect;

        nvrhi::TextureHandle depth;
        nvrhi::TextureHandle hzb;
        nvrhi::TextureHandle albedo;
        nvrhi::TextureHandle normals;
        nvrhi::TextureHandle geoNormals;
        nvrhi::TextureHandle orm;
        nvrhi::TextureHandle emissive;
        nvrhi::TextureHandle motion;
        nvrhi::TextureHandle hdr;
        nvrhi::TextureHandle opaque;
    };
    
    virtual void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override = 0;

    void Initialize() override
    {
        m_BasePassResources.Initialize();
    }

    bool IsBasePassRenderer() const override { return true; }

protected:
    BasePassResources m_BasePassResources;
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

    void ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[5])
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

    void PerformOcclusionCulling(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args, const ResourceHandles& handles)
    {
        PROFILE_FUNCTION();

        char marker[256]{};
        sprintf(marker, "Occlusion Culling Phase %d - %s", args.m_CullingPhase + 1, args.m_BucketName);
        PROFILE_GPU_SCOPED(marker, commandList);

        if (args.m_CullingPhase == 0)
        {
            // No-op clearing, done in Render()
        }
        else if (args.m_CullingPhase == 1)
        {
            // Clear visible count buffer for Phase 2
            commandList->clearBufferUInt(handles.visibleCount, 0);

            if (g_Renderer.m_UseMeshletRendering)
            {
                commandList->clearBufferUInt(handles.meshletJobCount, 0);
            }
        }

        const nvrhi::BufferDesc cullCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::CullingConstants), args.m_CullingPhase == 0 ? "CullingCB" : "CullingCB_Phase2", 1);
        const nvrhi::BufferHandle cullCB = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(cullCBD);

        const Matrix& projectionMatrix = g_Renderer.m_Scene.m_Camera.GetProjMatrix();

        srrhi::CullingConstants cullData;
        cullData.SetNumPrimitives(args.m_NumInstances);
        cullData.SetFrustumPlanes(reinterpret_cast<const DirectX::XMFLOAT4*>(args.m_FrustumPlanes));
        cullData.SetView(args.m_View);
        cullData.SetViewProj(args.m_ViewProj);
        cullData.SetEnableFrustumCulling(g_Renderer.m_EnableFrustumCulling ? 1 : 0);
        cullData.SetEnableOcclusionCulling((g_Renderer.m_EnableOcclusionCulling && handles.hzb) ? 1 : 0);
        cullData.SetHZBWidth(handles.hzb ? handles.hzb->getDesc().width : 0);
        cullData.SetHZBHeight(handles.hzb ? handles.hzb->getDesc().height : 0);
        cullData.SetPhase(args.m_CullingPhase);
        cullData.SetUseMeshletRendering(g_Renderer.m_UseMeshletRendering ? 1 : 0);
        cullData.SetP00(projectionMatrix.m[0][0]);
        cullData.SetP11(projectionMatrix.m[1][1]);
        cullData.SetForcedLOD(g_Renderer.m_ForcedLOD);
        cullData.SetInstanceBaseIndex(args.m_InstanceBaseIndex);
        commandList->writeBuffer(cullCB, &cullData, sizeof(cullData), 0);

        srrhi::GPUCullingInputs inputs;
        inputs.SetCullingCB(cullCB);
        inputs.SetInstanceData(g_Renderer.m_Scene.m_InstanceDataBuffer);
        inputs.SetHZB(handles.hzb ? handles.hzb : CommonResources::GetInstance().DefaultTextureBlack);
        inputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
        inputs.SetVisibleArgs(handles.visibleIndirect);
        inputs.SetVisibleCount(handles.visibleCount);
        inputs.SetOccludedIndices(handles.occludedIndices ? handles.occludedIndices : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetOccludedCount(handles.occludedCount ? handles.occludedCount : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetDispatchIndirectArgs((args.m_CullingPhase == 0 && handles.occludedIndirect) ? handles.occludedIndirect : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletJobs(handles.meshletJob ? handles.meshletJob : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletJobCount(handles.meshletJobCount ? handles.meshletJobCount : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetMeshletIndirectArgs(handles.meshletIndirect ? handles.meshletIndirect : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetInstanceLOD(g_Renderer.m_Scene.m_InstanceLODBuffer);

        nvrhi::BindingSetDesc cullBset = Renderer::CreateBindingSetDesc(inputs);

        if (args.m_CullingPhase == 0)
        {
            const uint32_t dispatchX = DivideAndRoundUp(args.m_NumInstances, srrhi::CommonConsts::kThreadsPerGroup);
            Renderer::RenderPassParams params;
            params.commandList = commandList;
            params.shaderID = ShaderID::GPUCULLING_CULLING_CSMAIN;
            params.bindingSetDesc = cullBset;
            params.dispatchParams = { .x = dispatchX, .y = 1, .z = 1 };
            g_Renderer.AddComputePass(params);
        }
        else if (handles.occludedIndirect)
        {
            Renderer::RenderPassParams params;
            params.commandList = commandList;
            params.shaderID = ShaderID::GPUCULLING_CULLING_CSMAIN;
            params.bindingSetDesc = cullBset;
            params.dispatchParams = { .indirectBuffer = handles.occludedIndirect, .indirectOffsetBytes = 0 };
            g_Renderer.AddComputePass(params);
        }

        PROFILE_GPU_SCOPED("Build Indirect Arguments", commandList);

        // Build indirect for Phase 2 culling and/or meshlet rendering
        if (args.m_CullingPhase == 0)
        {
            Renderer::RenderPassParams params;
            params.commandList = commandList;
            params.shaderID = ShaderID::GPUCULLING_BUILDINDIRECT_CSMAIN;
            params.bindingSetDesc = cullBset;
            params.dispatchParams = { .x = 1, .y = 1, .z = 1 };
            g_Renderer.AddComputePass(params);
        }
    }

    void RenderInstances(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args, const ResourceHandles& handles)
    {
        PROFILE_FUNCTION();

        nvrhi::BufferHandle visibleIndirect = handles.visibleIndirect;
        nvrhi::BufferHandle meshletIndirect = handles.meshletIndirect;
        nvrhi::BufferHandle meshletJob = handles.meshletJob;
        nvrhi::BufferHandle meshletJobCount = handles.meshletJobCount;
        nvrhi::TextureHandle gbufferAlbedo = handles.albedo;
        nvrhi::TextureHandle gbufferNormals = handles.normals;
        nvrhi::TextureHandle gbufferGeoNormals = handles.geoNormals;
        nvrhi::TextureHandle gbufferORM = handles.orm;
        nvrhi::TextureHandle gbufferEmissive = handles.emissive;
        nvrhi::TextureHandle gbufferMotionVectors = handles.motion;
        nvrhi::TextureHandle depthTexture = handles.depth;
        nvrhi::TextureHandle hdrColor = handles.hdr;
        nvrhi::TextureHandle opaqueColor = args.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_BLEND ? handles.opaque : CommonResources::GetInstance().DefaultTextureBlack;

        char marker[256]{};
        sprintf(marker, "Base Pass Render (Phase %d) - %s", args.m_CullingPhase + 1, args.m_BucketName);
        PROFILE_GPU_SCOPED(marker, commandList);

        const bool bUseAlphaTest = (args.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_MASK);
        const bool bUseAlphaBlend = (args.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_BLEND);

        const nvrhi::FramebufferHandle framebuffer = bUseAlphaBlend ? 
            g_Renderer.m_RHI->m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(hdrColor).setDepthAttachment(depthTexture)) :
            g_Renderer.m_RHI->m_NvrhiDevice->createFramebuffer(
            nvrhi::FramebufferDesc()
            .addColorAttachment(gbufferAlbedo)
            .addColorAttachment(gbufferNormals)
            .addColorAttachment(gbufferORM)
            .addColorAttachment(gbufferEmissive)
            .addColorAttachment(gbufferMotionVectors)
            .addColorAttachment(gbufferGeoNormals)
            .setDepthAttachment(depthTexture));

        nvrhi::FramebufferInfoEx fbInfo;
        if (bUseAlphaBlend)
        {
            fbInfo.colorFormats = { Renderer::HDR_COLOR_FORMAT };
        }
        else
        {
            fbInfo.colorFormats = { 
                Renderer::GBUFFER_ALBEDO_FORMAT,
                Renderer::GBUFFER_NORMALS_FORMAT,
                Renderer::GBUFFER_ORM_FORMAT,
                Renderer::GBUFFER_EMISSIVE_FORMAT,
                Renderer::GBUFFER_MOTION_FORMAT,
                Renderer::GBUFFER_NORMALS_FORMAT  // GeoNormals: same format as Normals
            };
        }
        fbInfo.setDepthFormat(Renderer::DEPTH_FORMAT);

        const uint32_t w = g_Renderer.m_RHI->m_SwapchainExtent.x;
        const uint32_t h = g_Renderer.m_RHI->m_SwapchainExtent.y;

        nvrhi::ViewportState viewportState;
        viewportState.viewports.push_back(nvrhi::Viewport(0.0f, (float)w, 0.0f, (float)h, 0.0f, 1.0f));
        viewportState.scissorRects.resize(1);
        viewportState.scissorRects[0].minX = 0;
        viewportState.scissorRects[0].minY = 0;
        viewportState.scissorRects[0].maxX = (int)w;
        viewportState.scissorRects[0].maxY = (int)h;

        const nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc((uint32_t)sizeof(srrhi::BasePassConstants), "PerFrameCB", 1);
        const nvrhi::BufferHandle perFrameCB = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(cbd);

        Vector3 camPos = g_Renderer.m_Scene.m_Camera.GetPosition();
        Vector3 cullingCamPos = camPos;
        if (g_Renderer.m_FreezeCullingCamera)
        {
            cullingCamPos = g_Renderer.m_Scene.m_FrozenCullingCameraPos;
        }

        srrhi::BasePassConstants cb;
        cb.SetView(g_Renderer.m_Scene.m_View);
        cb.SetPrevView(g_Renderer.m_Scene.m_ViewPrev);
        cb.SetFrustumPlanes(reinterpret_cast<const DirectX::XMFLOAT4*>(args.m_FrustumPlanes));
        cb.SetCameraPos(DirectX::XMFLOAT4{ camPos.x, camPos.y, camPos.z, 0.0f });
        cb.SetCullingCameraPos(DirectX::XMFLOAT4{ cullingCamPos.x, cullingCamPos.y, cullingCamPos.z, 0.0f });
        cb.SetLightCount(g_Renderer.m_Scene.m_LightCount);
        cb.SetEnableRTShadows(g_Renderer.m_EnableRTShadows ? 1 : 0);
        cb.SetDebugMode((uint32_t)g_Renderer.m_DebugMode);
        cb.SetEnableFrustumCulling(g_Renderer.m_EnableFrustumCulling ? 1 : 0);
        cb.SetEnableConeCulling((g_Renderer.m_EnableConeCulling && args.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE) ? 1 : 0);
        cb.SetEnableOcclusionCulling((g_Renderer.m_EnableOcclusionCulling && handles.hzb) ? 1 : 0);
        cb.SetHZBWidth(handles.hzb ? (uint32_t)handles.hzb->getDesc().width : 0);
        cb.SetHZBHeight(handles.hzb ? (uint32_t)handles.hzb->getDesc().height : 0);
        cb.SetP00(g_Renderer.m_Scene.m_View.m_MatViewToClip._11);
        cb.SetP11(g_Renderer.m_Scene.m_View.m_MatViewToClip._22);
        cb.SetOpaqueColorDimensions(DirectX::XMFLOAT2{ (float)opaqueColor->getDesc().width, (float)opaqueColor->getDesc().height });
        cb.SetOpaqueColorMipCount(opaqueColor->getDesc().mipLevels);
        cb.SetEnableSky(g_Renderer.m_EnableSky ? 1 : 0);
        cb.SetSunDirection(g_Renderer.m_Scene.GetSunDirection());
        cb.SetRenderingMode((uint32_t)g_Renderer.m_Mode);
        cb.SetRadianceMipCount(CommonResources::GetInstance().m_RadianceMipCount);

        commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

        srrhi::BasePassInputs inputs;
        inputs.SetPerFrameCB(perFrameCB);
        inputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
        inputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
        inputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
        inputs.SetMeshlets(g_Renderer.m_Scene.m_MeshletBuffer);
        inputs.SetMeshletVertices(g_Renderer.m_Scene.m_MeshletVerticesBuffer);
        inputs.SetMeshletTriangles(g_Renderer.m_Scene.m_MeshletTrianglesBuffer);
        inputs.SetMeshletJobs(meshletJob ? meshletJob : CommonResources::GetInstance().DummySRVStructuredBuffer);
        inputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
        inputs.SetHZB(handles.hzb ? handles.hzb : CommonResources::GetInstance().DefaultTextureBlack);
        inputs.SetSceneAS(g_Renderer.m_Scene.m_TLAS);
        inputs.SetIndices(g_Renderer.m_Scene.m_IndexBuffer);
        inputs.SetOpaqueColor(opaqueColor);
        inputs.SetLights(g_Renderer.m_Scene.m_LightBuffer);

        nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);

        const nvrhi::BindingLayoutHandle layout = g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(bset);
        const nvrhi::BindingSetHandle bindingSet = g_Renderer.m_RHI->m_NvrhiDevice->createBindingSet(bset, layout);

        nvrhi::RenderState renderState;
        renderState.rasterState = args.m_BackFaceCull ? CommonResources::GetInstance().RasterCullBack : CommonResources::GetInstance().RasterCullNone;
        
        if (args.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_BLEND)
        {
            renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetAlpha;
            renderState.depthStencilState = CommonResources::GetInstance().DepthRead;
        }
        else
        {
            renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetOpaque;
            renderState.depthStencilState = CommonResources::GetInstance().DepthReadWrite;
            renderState.depthStencilState.stencilEnable = true;
            renderState.depthStencilState.frontFaceStencil.passOp = nvrhi::StencilOp::Replace;
            renderState.depthStencilState.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Always;
            renderState.depthStencilState.backFaceStencil = renderState.depthStencilState.frontFaceStencil;
            renderState.depthStencilState.stencilRefValue = 1;
        }

        const uint32_t psID = bUseAlphaTest ? ShaderID::BASEPASS_GBUFFER_PSMAIN_ALPHATEST_ALPHATEST_ALPHA_TEST_1 : 
                            bUseAlphaBlend ? ShaderID::BASEPASS_FORWARD_PSMAIN_FORWARD_TRANSPARENT_FORWARD_TRANSPARENT_1 : 
                            ShaderID::BASEPASS_GBUFFER_PSMAIN;

        if (g_Renderer.m_UseMeshletRendering)
        {
            nvrhi::MeshletPipelineDesc meshPipelineDesc;
            meshPipelineDesc.AS = g_Renderer.GetShaderHandle(ShaderID::BASEPASS_ASMAIN);
            meshPipelineDesc.MS = g_Renderer.GetShaderHandle(ShaderID::BASEPASS_MSMAIN);
            meshPipelineDesc.PS = g_Renderer.GetShaderHandle(psID);
            meshPipelineDesc.renderState = renderState;
            meshPipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };
            meshPipelineDesc.useDrawIndex = true;

            const nvrhi::MeshletPipelineHandle meshPipeline = g_Renderer.GetOrCreateMeshletPipeline(meshPipelineDesc, fbInfo);

            nvrhi::MeshletState meshState;
            meshState.framebuffer = framebuffer;
            meshState.pipeline = meshPipeline;
            meshState.bindings = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
            meshState.viewport = viewportState;
            meshState.indirectParams = handles.meshletIndirect;
            meshState.indirectCountBuffer = handles.meshletJobCount;

            commandList->setMeshletState(meshState);
            commandList->dispatchMeshIndirectCount(0, 0, args.m_NumInstances);
        }
        else
        {
            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.VS = g_Renderer.GetShaderHandle(ShaderID::BASEPASS_VSMAIN);
            pipelineDesc.PS = g_Renderer.GetShaderHandle(psID);
            pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
            pipelineDesc.renderState = renderState;
            pipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };
            pipelineDesc.useDrawIndex = true;

            nvrhi::GraphicsState state;
            state.framebuffer = framebuffer;
            state.viewport = viewportState;
            state.indexBuffer = nvrhi::IndexBufferBinding{ g_Renderer.m_Scene.m_IndexBuffer, nvrhi::Format::R32_UINT, 0 };
            state.bindings = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
            state.pipeline = g_Renderer.GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
            state.indirectParams = handles.visibleIndirect;
            state.indirectCountBuffer = handles.visibleCount;
            commandList->setGraphicsState(state);

            commandList->drawIndexedIndirectCount(0, 0, args.m_NumInstances);
        }
    }

    void PrepareRenderingData(Matrix& outView, Matrix& outViewProjForCulling, Vector4 outFrustumPlanes[5])
    {
        
        Camera* cam = &g_Renderer.m_Scene.m_Camera;
        const Matrix viewProj = cam->GetViewProjMatrix();
        outView = cam->GetViewMatrix();
        outViewProjForCulling = viewProj;
        if (g_Renderer.m_FreezeCullingCamera)
        {
            outView = g_Renderer.m_Scene.m_FrozenCullingViewMatrix;
            const Matrix proj = cam->GetProjMatrix();
            const DirectX::XMMATRIX v = DirectX::XMLoadFloat4x4(&g_Renderer.m_Scene.m_FrozenCullingViewMatrix);
            const DirectX::XMMATRIX p = DirectX::XMLoadFloat4x4(&proj);
            DirectX::XMStoreFloat4x4(&outViewProjForCulling, v * p);
        }
        ComputeFrustumPlanes(cam->GetProjMatrix(), outFrustumPlanes);
    }

    void ClearAllCounters(nvrhi::CommandListHandle commandList, const ResourceHandles& handles)
    {
        
        PROFILE_GPU_SCOPED("Clear All Counters", commandList);
        commandList->clearBufferUInt(handles.visibleCount, 0);
        if (g_Renderer.m_EnableOcclusionCulling)
        {
            commandList->clearBufferUInt(handles.occludedCount, 0);
        }
        if (g_Renderer.m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(handles.meshletJobCount, 0);
        }
    }
};

class OpaqueRenderer : public BasePassRendererBase
{
    RGBufferHandle m_RG_SPDAtomicCounter;

public:
    bool Setup(RenderGraph& renderGraph) override
    {
        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph, GetName());

        renderGraph.WriteBuffer(res.m_VisibleCountBuffer);
        renderGraph.WriteBuffer(res.m_VisibleIndirectBuffer);

        if (g_Renderer.m_EnableOcclusionCulling)
        {
            renderGraph.WriteTexture(g_RG_HZBTexture);
            renderGraph.WriteBuffer(res.m_OccludedCountBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndicesBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndirectBuffer);
        }

        if (g_Renderer.m_UseMeshletRendering)
        {
            renderGraph.WriteBuffer(res.m_MeshletJobBuffer);
            renderGraph.WriteBuffer(res.m_MeshletJobCountBuffer);
            renderGraph.WriteBuffer(res.m_MeshletIndirectBuffer);
        }

        renderGraph.WriteTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        renderGraph.WriteTexture(g_RG_GBufferNormals);
        renderGraph.WriteTexture(g_RG_GBufferGeoNormals);
        renderGraph.WriteTexture(g_RG_GBufferORM);
        renderGraph.WriteTexture(g_RG_GBufferEmissive);
        renderGraph.WriteTexture(g_RG_GBufferMotionVectors);

        const std::string counterName = std::string{ GetName() } + " HZB SPD Atomic Counter";
        renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc(counterName.c_str()), m_RG_SPDAtomicCounter);
        renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        

        const uint32_t numOpaque = g_Renderer.m_Scene.m_OpaqueBucket.m_Count;
        if (numOpaque == 0) return;

        Matrix view, viewProjForCulling;
        Vector4 frustumPlanes[5];
        PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

        ResourceHandles handles;
        BasePassResources& res = m_BasePassResources;
        handles.visibleCount = renderGraph.GetBuffer(res.m_VisibleCountBuffer, RGResourceAccessMode::Write);
        handles.visibleIndirect = renderGraph.GetBuffer(res.m_VisibleIndirectBuffer, RGResourceAccessMode::Write);
        handles.occludedCount = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndices = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndicesBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndirect = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndirectBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJob = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJobCount = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletIndirect = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletIndirectBuffer, RGResourceAccessMode::Write) : nullptr;

        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
        handles.hzb = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Write) : nullptr;
        handles.albedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
        handles.normals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
        handles.geoNormals = renderGraph.GetTexture(g_RG_GBufferGeoNormals, RGResourceAccessMode::Write);
        handles.orm = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
        handles.emissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
        handles.motion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

        ClearAllCounters(commandList, handles);

        BasePassRenderingArgs args;
        args.m_FrustumPlanes = frustumPlanes;
        args.m_View = view;
        args.m_ViewProj = viewProjForCulling;
        args.m_NumInstances = numOpaque;
        args.m_InstanceBaseIndex = g_Renderer.m_Scene.m_OpaqueBucket.m_BaseIndex;
        args.m_BucketName = "Opaque";
        args.m_CullingPhase = 0;
        args.m_AlphaMode = srrhi::CommonConsts::ALPHA_MODE_OPAQUE;

        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);

        args.m_BucketName = "Opaque (Occluded)";
        args.m_CullingPhase = 1;
        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);

        nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
        GenerateHZBMips(commandList, renderGraph, spdAtomicCounter);
    }

    const char* GetName() const override { return "Opaque Renderer"; }
};

class MaskedPassRenderer : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        

        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph, GetName());

        renderGraph.WriteBuffer(res.m_VisibleCountBuffer);
        renderGraph.WriteBuffer(res.m_VisibleIndirectBuffer);

        if (g_Renderer.m_EnableOcclusionCulling)
        {
            renderGraph.ReadTexture(g_RG_HZBTexture);
            renderGraph.WriteBuffer(res.m_OccludedCountBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndicesBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndirectBuffer);
        }
        if (g_Renderer.m_UseMeshletRendering)
        {
            renderGraph.WriteBuffer(res.m_MeshletJobBuffer);
            renderGraph.WriteBuffer(res.m_MeshletJobCountBuffer);
            renderGraph.WriteBuffer(res.m_MeshletIndirectBuffer);
        }

        renderGraph.WriteTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        renderGraph.WriteTexture(g_RG_GBufferNormals);
        renderGraph.WriteTexture(g_RG_GBufferGeoNormals);
        renderGraph.WriteTexture(g_RG_GBufferORM);
        renderGraph.WriteTexture(g_RG_GBufferEmissive);
        renderGraph.WriteTexture(g_RG_GBufferMotionVectors);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        
        const uint32_t numMasked = g_Renderer.m_Scene.m_MaskedBucket.m_Count;
        if (numMasked == 0) return;

        Matrix view, viewProjForCulling;
        Vector4 frustumPlanes[5];
        PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

        ResourceHandles handles;
        BasePassResources& res = m_BasePassResources;
        handles.visibleCount = renderGraph.GetBuffer(res.m_VisibleCountBuffer, RGResourceAccessMode::Write);
        handles.visibleIndirect = renderGraph.GetBuffer(res.m_VisibleIndirectBuffer, RGResourceAccessMode::Write);
        handles.occludedCount = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndices = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndicesBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndirect = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndirectBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJob =  g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJobCount = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletIndirect = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletIndirectBuffer, RGResourceAccessMode::Write) : nullptr;

        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
        handles.hzb = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Read) : nullptr;
        handles.albedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
        handles.normals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
        handles.geoNormals = renderGraph.GetTexture(g_RG_GBufferGeoNormals, RGResourceAccessMode::Write);
        handles.orm = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
        handles.emissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
        handles.motion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

        ClearAllCounters(commandList, handles);

        BasePassRenderingArgs args;
        args.m_FrustumPlanes = frustumPlanes;
        args.m_View = view;
        args.m_ViewProj = viewProjForCulling;
        args.m_NumInstances = numMasked;
        args.m_InstanceBaseIndex = g_Renderer.m_Scene.m_MaskedBucket.m_BaseIndex;
        args.m_BucketName = g_Renderer.m_EnableOcclusionCulling ? "Masked" : "Masked (No Occlusion)";
        args.m_CullingPhase = 0;
        args.m_AlphaMode = srrhi::CommonConsts::ALPHA_MODE_MASK;

        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);
    }

    const char* GetName() const override { return "MaskedPass"; }
};

class TransparentPassRenderer : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        

        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph, GetName());

        const uint32_t width = g_Renderer.m_RHI->m_SwapchainExtent.x;
        const uint32_t height = g_Renderer.m_RHI->m_SwapchainExtent.y;

        // Opaque Color Texture (used for transmission/refraction)
        // Resize to next lowest power of 2 for efficient mip generation
        {
            RGTextureDesc desc;
            const uint32_t pow2Width = NextLowerPow2(width);
            const uint32_t pow2Height = NextLowerPow2(height);
            desc.m_NvrhiDesc.width = pow2Width;
            desc.m_NvrhiDesc.height = pow2Height;
            desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT;
            desc.m_NvrhiDesc.debugName = "OpaqueColorTexture_RG";
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.isRenderTarget = false;
            desc.m_NvrhiDesc.useClearValue = false;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;

            // Calculate mip levels for transmission LOD sampling
            uint32_t maxDim = std::max(pow2Width, pow2Height);
            uint32_t mipLevels = 0;
            while (maxDim > 0) {
                mipLevels++;
                maxDim >>= 1;
            }
            desc.m_NvrhiDesc.mipLevels = mipLevels;
            renderGraph.DeclareTexture(desc, g_RG_OpaqueColor);
        }

        renderGraph.WriteTexture(g_RG_HDRColor);
        renderGraph.WriteTexture(g_RG_DepthTexture);

        renderGraph.WriteBuffer(res.m_VisibleCountBuffer);
        renderGraph.WriteBuffer(res.m_VisibleIndirectBuffer);

        if (g_Renderer.m_UseMeshletRendering)
        {
            renderGraph.WriteBuffer(res.m_MeshletJobBuffer);
            renderGraph.WriteBuffer(res.m_MeshletJobCountBuffer);
            renderGraph.WriteBuffer(res.m_MeshletIndirectBuffer);
        }
        if (g_Renderer.m_EnableOcclusionCulling)
        {
            renderGraph.ReadTexture(g_RG_HZBTexture);
            renderGraph.WriteBuffer(res.m_OccludedCountBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndicesBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndirectBuffer);
        }

        renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc("Transparent SPD Atomic Counter"), m_RG_SPDAtomicCounter);
        renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        
        const uint32_t numTransparent = g_Renderer.m_Scene.m_TransparentBucket.m_Count;
        if (numTransparent == 0) return;

        BasePassResources& res = m_BasePassResources;
        ResourceHandles handles;
        handles.visibleCount = renderGraph.GetBuffer(res.m_VisibleCountBuffer, RGResourceAccessMode::Write);
        handles.visibleIndirect = renderGraph.GetBuffer(res.m_VisibleIndirectBuffer, RGResourceAccessMode::Write);
        handles.occludedCount = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndices = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndicesBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndirect = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndirectBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJob = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJobCount = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletIndirect = g_Renderer.m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletIndirectBuffer, RGResourceAccessMode::Write) : nullptr;

        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
        handles.hzb = g_Renderer.m_EnableOcclusionCulling ? renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Read) : nullptr;
        handles.hdr = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Read);
        handles.opaque = renderGraph.GetTexture(g_RG_OpaqueColor, RGResourceAccessMode::Write);

        // Downsample HDR to pow2 opaque texture via linear interpolation shader
        DownsampleTextureToPow2(commandList, handles.hdr, handles.opaque, srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX);

        // Generate mips for opaque color using SPD
        nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
        g_Renderer.GenerateMipsUsingSPD(handles.opaque, spdAtomicCounter, commandList, "Generate Mips for Opaque Color", srrhi::CommonConsts::SPD_REDUCTION_AVERAGE);

        Matrix view, viewProjForCulling;
        Vector4 frustumPlanes[5];
        PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

        ClearAllCounters(commandList, handles);

        BasePassRenderingArgs args;
        args.m_FrustumPlanes = frustumPlanes;
        args.m_View = view;
        args.m_ViewProj = viewProjForCulling;
        args.m_NumInstances = numTransparent;
        args.m_InstanceBaseIndex = g_Renderer.m_Scene.m_TransparentBucket.m_BaseIndex;
        args.m_BucketName = "Transparent";
        args.m_AlphaMode = srrhi::CommonConsts::ALPHA_MODE_BLEND;
        args.m_CullingPhase = 0;
        args.m_BackFaceCull = true;

        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);
    }

    const char* GetName() const override { return "TransparentPass"; }

private:
    RGBufferHandle m_RG_SPDAtomicCounter;
};

class HZBGeneratorPhase2 : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        
        if (!g_Renderer.m_EnableOcclusionCulling) return false;

        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_HZBTexture);

        const std::string counterName = std::string{ GetName() } + " HZB SPD Atomic Counter";

        renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc(counterName.c_str()), m_RG_SPDAtomicCounter);
        renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
        GenerateHZBMips(commandList, renderGraph, spdAtomicCounter);
    }

private:
    RGBufferHandle m_RG_SPDAtomicCounter;

    const char* GetName() const override { return "HZBGeneratorPhase2"; }
};

REGISTER_RENDERER(OpaqueRenderer);
REGISTER_RENDERER(HZBGeneratorPhase2);
REGISTER_RENDERER(MaskedPassRenderer);
REGISTER_RENDERER(TransparentPassRenderer);
