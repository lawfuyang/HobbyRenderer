#include "Renderer.h"
#include "CommonResources.h"
#include "BasePassCommon.h"
#include "Camera.h"
#include "Utilities.h"

#include "shaders/ShaderShared.h"

extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_HZBTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_SkyVisibility;
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
        Renderer* renderer = Renderer::GetInstance();
        m_IsSelected = (renderer->m_SelectedRendererIndexForPipelineStatistics != -1 && renderer->m_Renderers[renderer->m_SelectedRendererIndexForPipelineStatistics].get() == rendererPtr);
        if (m_IsSelected)
        {
            nvrhi::DeviceHandle device = renderer->m_RHI->m_NvrhiDevice;

            m_WriteIndex = renderer->m_FrameNumber % 2;
            const uint32_t readIndex = 1 - m_WriteIndex;
            renderer->m_SelectedBasePassPipelineStatistics = device->getPipelineStatistics(m_Res.m_PipelineQueries[readIndex]);

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
        nvrhi::TextureHandle orm;
        nvrhi::TextureHandle emissive;
        nvrhi::TextureHandle motion;
        nvrhi::TextureHandle hdr;
        nvrhi::TextureHandle opaque;
        nvrhi::TextureHandle skyVis;
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

    void GenerateHZBMips(nvrhi::CommandListHandle commandList, const ResourceHandles& handles, nvrhi::BufferHandle spdAtomicCounter)
    {
        PROFILE_FUNCTION();

        Renderer* renderer = Renderer::GetInstance();

        if (!renderer->m_EnableOcclusionCulling || renderer->m_FreezeCullingCamera)
        {
            return;
        }

        // Get transient depth texture from resource handles
        nvrhi::TextureHandle depthTexture = handles.depth;

        nvrhi::utils::ScopedMarker commandListMarker{ commandList, "Generate HZB Mips" };

        // First, build HZB mip 0 from depth texture
        {
            nvrhi::utils::ScopedMarker hzbFromDepthMarker{ commandList, "HZB From Depth" };

            HZBFromDepthConstants hzbFromDepthData;
            hzbFromDepthData.m_Width = handles.hzb->getDesc().width;
            hzbFromDepthData.m_Height = handles.hzb->getDesc().height;

            nvrhi::BindingSetDesc hzbFromDepthBset;
            hzbFromDepthBset.bindings =
            {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(HZBFromDepthConstants)),
                nvrhi::BindingSetItem::Texture_SRV(0, depthTexture),
                nvrhi::BindingSetItem::Texture_UAV(0, handles.hzb,  nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
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

        renderer->GenerateMipsUsingSPD(handles.hzb, spdAtomicCounter, commandList, "Generate HZB Mips", SPD_REDUCTION_MIN);
    }

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
            commandList->clearBufferUInt(handles.visibleCount, 0);

            if (renderer->m_UseMeshletRendering)
            {
                commandList->clearBufferUInt(handles.meshletJobCount, 0);
            }
        }

        const nvrhi::BufferDesc cullCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(CullingConstants), args.m_CullingPhase == 0 ? "CullingCB" : "CullingCB_Phase2", 1);
        const nvrhi::BufferHandle cullCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(cullCBD);

        const Matrix& projectionMatrix = renderer->m_Scene.m_Camera.GetProjMatrix();

        CullingConstants cullData;
        cullData.m_NumPrimitives = args.m_NumInstances;
        memcpy(cullData.m_FrustumPlanes, args.m_FrustumPlanes, 5 * sizeof(Vector4));
        cullData.m_View = args.m_View;
        cullData.m_ViewProj = args.m_ViewProj;
        cullData.m_EnableFrustumCulling = renderer->m_EnableFrustumCulling ? 1 : 0;
        cullData.m_EnableOcclusionCulling = (renderer->m_EnableOcclusionCulling && handles.hzb) ? 1 : 0;
        cullData.m_HZBWidth = handles.hzb ? handles.hzb->getDesc().width : 0;
        cullData.m_HZBHeight = handles.hzb ? handles.hzb->getDesc().height : 0;
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
            nvrhi::BindingSetItem::Texture_SRV(1, handles.hzb ? handles.hzb : CommonResources::GetInstance().DefaultTextureWhite),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(2, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, handles.visibleIndirect),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, handles.visibleCount),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(2, handles.occludedIndices ? handles.occludedIndices : CommonResources::GetInstance().DummyUAVBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(3, handles.occludedCount ? handles.occludedCount : CommonResources::GetInstance().DummyUAVBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(4, (args.m_CullingPhase == 0 && handles.occludedIndirect) ? handles.occludedIndirect : CommonResources::GetInstance().DummyUAVBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(5, handles.meshletJob ? handles.meshletJob : CommonResources::GetInstance().DummyUAVBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(6, handles.meshletJobCount ? handles.meshletJobCount : CommonResources::GetInstance().DummyUAVBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(7, handles.meshletIndirect ? handles.meshletIndirect : CommonResources::GetInstance().DummyUAVBuffer),
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
            params.dispatchParams = { .indirectBuffer = handles.occludedIndirect, .indirectOffsetBytes = 0 };
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

    void RenderInstances(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args, const ResourceHandles& handles)
    {
        PROFILE_FUNCTION();

        Renderer* renderer = Renderer::GetInstance();
        
        nvrhi::BufferHandle visibleIndirect = handles.visibleIndirect;
        nvrhi::BufferHandle meshletIndirect = handles.meshletIndirect;
        nvrhi::BufferHandle meshletJob = handles.meshletJob;
        nvrhi::BufferHandle meshletJobCount = handles.meshletJobCount;
        nvrhi::TextureHandle gbufferAlbedo = handles.albedo;
        nvrhi::TextureHandle gbufferNormals = handles.normals;
        nvrhi::TextureHandle gbufferORM = handles.orm;
        nvrhi::TextureHandle gbufferEmissive = handles.emissive;
        nvrhi::TextureHandle gbufferMotionVectors = handles.motion;
        nvrhi::TextureHandle depthTexture = handles.depth;
        nvrhi::TextureHandle hdrColor = handles.hdr;
        nvrhi::TextureHandle opaqueColor = args.m_AlphaMode == ALPHA_MODE_BLEND ? handles.opaque : CommonResources::GetInstance().DefaultTextureBlack;
        nvrhi::TextureHandle skyVis = handles.skyVis;

        char marker[256];
        sprintf(marker, "Base Pass Render (Phase %d) - %s", args.m_CullingPhase + 1, args.m_BucketName);
        nvrhi::utils::ScopedMarker commandListMarker(commandList, marker);

        const bool bUseAlphaTest = (args.m_AlphaMode == ALPHA_MODE_MASK);
        const bool bUseAlphaBlend = (args.m_AlphaMode == ALPHA_MODE_BLEND);

        const nvrhi::FramebufferHandle framebuffer = bUseAlphaBlend ? 
            renderer->m_RHI->m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(hdrColor).setDepthAttachment(depthTexture)) :
            renderer->m_RHI->m_NvrhiDevice->createFramebuffer(
            nvrhi::FramebufferDesc()
            .addColorAttachment(gbufferAlbedo)
            .addColorAttachment(gbufferNormals)
            .addColorAttachment(gbufferORM)
            .addColorAttachment(gbufferEmissive)
            .addColorAttachment(gbufferMotionVectors)
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
                Renderer::GBUFFER_MOTION_FORMAT
            };
        }
        fbInfo.setDepthFormat(Renderer::DEPTH_FORMAT);

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
            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, handles.meshletJob ? handles.meshletJob : CommonResources::GetInstance().DummyUAVBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(7, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::Texture_SRV(8, handles.hzb ? handles.hzb : CommonResources::GetInstance().DefaultTextureWhite),
            nvrhi::BindingSetItem::RayTracingAccelStruct(9, renderer->m_Scene.m_TLAS),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(10, renderer->m_Scene.m_IndexBuffer),
            nvrhi::BindingSetItem::Texture_SRV(11, opaqueColor),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, renderer->m_Scene.m_LightBuffer),
            nvrhi::BindingSetItem::Texture_SRV(13, handles.skyVis ? handles.skyVis : CommonResources::GetInstance().DefaultTexture3DWhite),
        };

        const nvrhi::BindingLayoutHandle layout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bset);
        const nvrhi::BindingSetHandle bindingSet = renderer->m_RHI->m_NvrhiDevice->createBindingSet(bset, layout);

        float skyVisFarPlane = renderer->m_Scene.GetSceneBoundingRadius() * 0.5f;

        ForwardLightingPerFrameData cb{};
        cb.m_View = renderer->m_Scene.m_View;
        cb.m_PrevView = renderer->m_Scene.m_ViewPrev;
        memcpy(cb.m_FrustumPlanes, args.m_FrustumPlanes, sizeof(Vector4) * 5);

        Vector3 camPos = renderer->m_Scene.m_Camera.GetPosition();
        cb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 0.0f };

        Vector3 cullingCamPos = camPos;
        if (renderer->m_FreezeCullingCamera)
        {
            cullingCamPos = renderer->m_FrozenCullingCameraPos;
        }
        cb.m_CullingCameraPos = Vector4{ cullingCamPos.x, cullingCamPos.y, cullingCamPos.z, 0.0f };

        cb.m_LightCount = renderer->m_Scene.m_LightCount;
        cb.m_EnableRTShadows = renderer->m_EnableRTShadows ? 1 : 0;
        cb.m_DebugMode = (uint32_t)renderer->m_DebugMode;
        cb.m_EnableFrustumCulling = renderer->m_EnableFrustumCulling ? 1 : 0;
        cb.m_EnableConeCulling = (renderer->m_EnableConeCulling && args.m_AlphaMode == ALPHA_MODE_OPAQUE) ? 1 : 0;
        cb.m_EnableOcclusionCulling = (renderer->m_EnableOcclusionCulling && handles.hzb) ? 1 : 0;
        cb.m_HZBWidth = handles.hzb ? (uint32_t)handles.hzb->getDesc().width : 0;
        cb.m_HZBHeight = handles.hzb ? (uint32_t)handles.hzb->getDesc().height : 0;
        // FIXME: Switch to m_MatViewToClip (jittered) once TAA is implemented
        cb.m_P00 = cb.m_View.m_MatViewToClipNoOffset._11;
        cb.m_P11 = cb.m_View.m_MatViewToClipNoOffset._22;
        cb.m_OpaqueColorDimensions = Vector2{ (float)opaqueColor->getDesc().width, (float)opaqueColor->getDesc().height };
        cb.m_EnableSky = renderer->m_EnableSky ? 1 : 0;
        cb.m_SunDirection = renderer->m_Scene.m_SunDirection;
        cb.m_RenderingMode = (uint32_t)renderer->m_Mode;
        cb.m_RadianceMipCount = CommonResources::GetInstance().m_RadianceMipCount;
        cb.m_SkyVisibilityZCount = (uint32_t)renderer->m_SkyVisibilityZCount;
        cb.m_SkyVisibilityFar = skyVisFarPlane;
        cb.m_SkyVisibilityGridZParams = CalculateGridZParams(0.1f, skyVisFarPlane, 1.0f, cb.m_SkyVisibilityZCount);

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
            renderState.depthStencilState.stencilEnable = true;
            renderState.depthStencilState.frontFaceStencil.passOp = nvrhi::StencilOp::Replace;
            renderState.depthStencilState.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Always;
            renderState.depthStencilState.backFaceStencil = renderState.depthStencilState.frontFaceStencil;
            renderState.depthStencilState.stencilRefValue = 1;
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
            meshPipelineDesc.bindingLayouts = { layout, renderer->GetGlobalTextureBindingLayout(), renderer->GetGlobalSamplerBindingLayout() };
            meshPipelineDesc.useDrawIndex = true;

            const nvrhi::MeshletPipelineHandle meshPipeline = renderer->GetOrCreateMeshletPipeline(meshPipelineDesc, fbInfo);

            nvrhi::MeshletState meshState;
            meshState.framebuffer = framebuffer;
            meshState.pipeline = meshPipeline;
            meshState.bindings = { bindingSet, renderer->GetGlobalTextureDescriptorTable(), renderer->GetGlobalSamplerDescriptorTable() };
            meshState.viewport = viewportState;
            meshState.indirectParams = handles.meshletIndirect;
            meshState.indirectCountBuffer = handles.meshletJobCount;

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
            pipelineDesc.bindingLayouts = { layout, renderer->GetGlobalTextureBindingLayout(), renderer->GetGlobalSamplerBindingLayout() };
            pipelineDesc.useDrawIndex = true;

            nvrhi::GraphicsState state;
            state.framebuffer = framebuffer;
            state.viewport = viewportState;
            state.indexBuffer = nvrhi::IndexBufferBinding{ renderer->m_Scene.m_IndexBuffer, nvrhi::Format::R32_UINT, 0 };
            state.bindings = { bindingSet, renderer->GetGlobalTextureDescriptorTable(), renderer->GetGlobalSamplerDescriptorTable() };
            state.pipeline = renderer->GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
            state.indirectParams = handles.visibleIndirect;
            state.indirectCountBuffer = handles.visibleCount;
            commandList->setGraphicsState(state);

            commandList->drawIndexedIndirectCount(0, 0, args.m_NumInstances);
        }
    }

    void PrepareRenderingData(Matrix& outView, Matrix& outViewProjForCulling, Vector4 outFrustumPlanes[5])
    {
        Renderer* renderer = Renderer::GetInstance();
        Camera* cam = &renderer->m_Scene.m_Camera;
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

    void ClearVisibleCounters(nvrhi::CommandListHandle commandList, const ResourceHandles& handles)
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::utils::ScopedMarker clearMarker{ commandList, "Clear Visible Counters" };
        commandList->clearBufferUInt(handles.visibleCount, 0);
        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(handles.meshletJobCount, 0);
        }
    }

    void ClearAllCounters(nvrhi::CommandListHandle commandList, const ResourceHandles& handles)
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::utils::ScopedMarker clearMarker{ commandList, "Clear All Counters" };
        commandList->clearBufferUInt(handles.visibleCount, 0);
        if (renderer->m_EnableOcclusionCulling)
        {
            commandList->clearBufferUInt(handles.occludedCount, 0);
        }
        if (renderer->m_UseMeshletRendering)
        {
            commandList->clearBufferUInt(handles.meshletJobCount, 0);
        }
    }
};

class OpaquePhase1Renderer : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph);

        renderGraph.WriteBuffer(res.m_VisibleCountBuffer);
        renderGraph.WriteBuffer(res.m_VisibleIndirectBuffer);
        if (renderer->m_EnableOcclusionCulling)
        {
            renderGraph.ReadTexture(g_RG_HZBTexture);
            renderGraph.WriteBuffer(res.m_OccludedCountBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndicesBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndirectBuffer);
        }
        if (renderer->m_UseMeshletRendering)
        {
            renderGraph.WriteBuffer(res.m_MeshletJobBuffer);
            renderGraph.WriteBuffer(res.m_MeshletJobCountBuffer);
            renderGraph.WriteBuffer(res.m_MeshletIndirectBuffer);
        }

        renderGraph.WriteTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        renderGraph.WriteTexture(g_RG_GBufferNormals);
        renderGraph.WriteTexture(g_RG_GBufferORM);
        renderGraph.WriteTexture(g_RG_GBufferEmissive);
        renderGraph.WriteTexture(g_RG_GBufferMotionVectors);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        Renderer* renderer = Renderer::GetInstance();
        const uint32_t numOpaque = renderer->m_Scene.m_OpaqueBucket.m_Count;
        if (numOpaque == 0) return;

        Matrix view, viewProjForCulling;
        Vector4 frustumPlanes[5];
        PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

        ResourceHandles handles;
        BasePassResources& res = m_BasePassResources;
        handles.visibleCount = renderGraph.GetBuffer(res.m_VisibleCountBuffer, RGResourceAccessMode::Write);
        handles.visibleIndirect = renderGraph.GetBuffer(res.m_VisibleIndirectBuffer, RGResourceAccessMode::Write);
        handles.occludedCount = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndices = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndicesBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndirect = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndirectBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJob = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJobCount = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletIndirect = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletIndirectBuffer, RGResourceAccessMode::Write) : nullptr;

        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
        handles.hzb = renderer->m_EnableOcclusionCulling ? renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Read) : nullptr;
        handles.albedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
        handles.normals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
        handles.orm = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
        handles.emissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
        handles.motion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

        ClearAllCounters(commandList, handles);

        BasePassRenderingArgs args;
        args.m_FrustumPlanes = frustumPlanes;
        args.m_View = view;
        args.m_ViewProj = viewProjForCulling;
        args.m_NumInstances = numOpaque;
        args.m_InstanceBaseIndex = renderer->m_Scene.m_OpaqueBucket.m_BaseIndex;
        args.m_BucketName = "Opaque";
        args.m_CullingPhase = 0;
        args.m_AlphaMode = ALPHA_MODE_OPAQUE;

        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);
    }
    const char* GetName() const override { return "OpaquePhase1"; }
};

class HZBGenerator : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (!renderer->m_EnableOcclusionCulling || renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_HZBTexture);

        renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc("HZB SPD Atomic Counter"), m_RG_SPDAtomicCounter);
        renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        Renderer* renderer = Renderer::GetInstance();
        
        ResourceHandles handles;
        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
        handles.hzb = renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Write);
        nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
        GenerateHZBMips(commandList, handles, spdAtomicCounter);
    }
    const char* GetName() const override { return "HZBGenerator"; }

private:
    RGBufferHandle m_RG_SPDAtomicCounter;
};

class OpaquePhase2Renderer : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (!renderer->m_EnableOcclusionCulling || renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph);

        renderGraph.ReadTexture(g_RG_HZBTexture);
        renderGraph.ReadBuffer(res.m_OccludedIndirectBuffer);
        
        renderGraph.WriteBuffer(res.m_VisibleCountBuffer);
        renderGraph.WriteBuffer(res.m_VisibleIndirectBuffer);
        if (renderer->m_UseMeshletRendering)
        {
            renderGraph.WriteBuffer(res.m_MeshletJobBuffer);
            renderGraph.WriteBuffer(res.m_MeshletJobCountBuffer);
            renderGraph.WriteBuffer(res.m_MeshletIndirectBuffer);
        }

        renderGraph.WriteTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        renderGraph.WriteTexture(g_RG_GBufferNormals);
        renderGraph.WriteTexture(g_RG_GBufferORM);
        renderGraph.WriteTexture(g_RG_GBufferEmissive);
        renderGraph.WriteTexture(g_RG_GBufferMotionVectors);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        Renderer* renderer = Renderer::GetInstance();
        
        const uint32_t numOpaque = renderer->m_Scene.m_OpaqueBucket.m_Count;
        if (numOpaque == 0) return;

        Matrix view, viewProjForCulling;
        Vector4 frustumPlanes[5];
        PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

        ResourceHandles handles;
        BasePassResources& res = m_BasePassResources;
        handles.visibleCount = renderGraph.GetBuffer(res.m_VisibleCountBuffer, RGResourceAccessMode::Write);
        handles.visibleIndirect = renderGraph.GetBuffer(res.m_VisibleIndirectBuffer, RGResourceAccessMode::Write);
        handles.occludedIndirect = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndirectBuffer, RGResourceAccessMode::Read) : nullptr;
        handles.meshletJob = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJobCount = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletIndirect = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletIndirectBuffer, RGResourceAccessMode::Write) : nullptr;

        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
        handles.hzb = renderer->m_EnableOcclusionCulling ? renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Read) : nullptr;
        handles.albedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
        handles.normals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
        handles.orm = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
        handles.emissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
        handles.motion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

        BasePassRenderingArgs args;
        args.m_FrustumPlanes = frustumPlanes;
        args.m_View = view;
        args.m_ViewProj = viewProjForCulling;
        args.m_NumInstances = numOpaque;
        args.m_InstanceBaseIndex = renderer->m_Scene.m_OpaqueBucket.m_BaseIndex;
        args.m_BucketName = "Opaque (Occluded)";
        args.m_CullingPhase = 1;
        args.m_AlphaMode = ALPHA_MODE_OPAQUE;

        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);
    }
    const char* GetName() const override { return "OpaquePhase2"; }
};

class MaskedPassRenderer : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph);

        renderGraph.WriteBuffer(res.m_VisibleCountBuffer);
        renderGraph.WriteBuffer(res.m_VisibleIndirectBuffer);
        if (renderer->m_EnableOcclusionCulling)
        {
            renderGraph.ReadTexture(g_RG_HZBTexture);
            renderGraph.WriteBuffer(res.m_OccludedCountBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndicesBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndirectBuffer);
        }
        if (renderer->m_UseMeshletRendering)
        {
            renderGraph.WriteBuffer(res.m_MeshletJobBuffer);
            renderGraph.WriteBuffer(res.m_MeshletJobCountBuffer);
            renderGraph.WriteBuffer(res.m_MeshletIndirectBuffer);
        }

        renderGraph.WriteTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        renderGraph.WriteTexture(g_RG_GBufferNormals);
        renderGraph.WriteTexture(g_RG_GBufferORM);
        renderGraph.WriteTexture(g_RG_GBufferEmissive);
        renderGraph.WriteTexture(g_RG_GBufferMotionVectors);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        Renderer* renderer = Renderer::GetInstance();
        const uint32_t numMasked = renderer->m_Scene.m_MaskedBucket.m_Count;
        if (numMasked == 0) return;

        Matrix view, viewProjForCulling;
        Vector4 frustumPlanes[5];
        PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

        ResourceHandles handles;
        BasePassResources& res = m_BasePassResources;
        handles.visibleCount = renderGraph.GetBuffer(res.m_VisibleCountBuffer, RGResourceAccessMode::Write);
        handles.visibleIndirect = renderGraph.GetBuffer(res.m_VisibleIndirectBuffer, RGResourceAccessMode::Write);
        handles.occludedCount = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndices = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndicesBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndirect = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndirectBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJob =  renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJobCount = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletIndirect = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletIndirectBuffer, RGResourceAccessMode::Write) : nullptr;

        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
        handles.hzb = renderer->m_EnableOcclusionCulling ? renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Read) : nullptr;
        handles.albedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
        handles.normals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
        handles.orm = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
        handles.emissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
        handles.motion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

        ClearAllCounters(commandList, handles);

        BasePassRenderingArgs args;
        args.m_FrustumPlanes = frustumPlanes;
        args.m_View = view;
        args.m_ViewProj = viewProjForCulling;
        args.m_NumInstances = numMasked;
        args.m_InstanceBaseIndex = renderer->m_Scene.m_MaskedBucket.m_BaseIndex;
        args.m_BucketName = renderer->m_EnableOcclusionCulling ? "Masked" : "Masked (No Occlusion)";
        args.m_CullingPhase = 0;
        args.m_AlphaMode = ALPHA_MODE_MASK;

        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);
    }
    const char* GetName() const override { return "MaskedPass"; }
};

class HZBGeneratorPhase2 : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (!renderer->m_EnableOcclusionCulling || renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_HZBTexture);

        renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc("HZB Phase 2 SPD Atomic Counter"), m_RG_SPDAtomicCounter);
        renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        Renderer* renderer = Renderer::GetInstance();
        
        ResourceHandles handles;
        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
        handles.hzb = renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Write);
        nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
        GenerateHZBMips(commandList, handles, spdAtomicCounter);
    }
    const char* GetName() const override { return "HZBGeneratorPhase2"; }

private:
    RGBufferHandle m_RG_SPDAtomicCounter;
};

class TransparentPassRenderer : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph);

        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Opaque Color Texture (used for transmission/refraction)
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT;
            desc.m_NvrhiDesc.debugName = "OpaqueColorTexture_RG";
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.isRenderTarget = false;
            desc.m_NvrhiDesc.useClearValue = false;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::CopyDest;

            // Calculate mip levels for transmission LOD sampling
            uint32_t maxDim = std::max(width, height);
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
        if (renderer->m_UseMeshletRendering)
        {
            renderGraph.WriteBuffer(res.m_MeshletJobBuffer);
            renderGraph.WriteBuffer(res.m_MeshletJobCountBuffer);
            renderGraph.WriteBuffer(res.m_MeshletIndirectBuffer);
        }
        if (renderer->m_EnableOcclusionCulling)
        {
            renderGraph.ReadTexture(g_RG_HZBTexture);
            renderGraph.WriteBuffer(res.m_OccludedCountBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndicesBuffer);
            renderGraph.WriteBuffer(res.m_OccludedIndirectBuffer);
        }

        if (renderer->m_EnableSky)
        {
            renderGraph.ReadTexture(g_RG_SkyVisibility);
        }

        renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc("Transparent SPD Atomic Counter"), m_RG_SPDAtomicCounter);
        renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        ScopedBasePassPipelineQuery query{ commandList, m_BasePassResources, this };
        Renderer* renderer = Renderer::GetInstance();
        const uint32_t numTransparent = renderer->m_Scene.m_TransparentBucket.m_Count;
        if (numTransparent == 0) return;

        BasePassResources& res = m_BasePassResources;
        ResourceHandles handles;
        handles.visibleCount = renderGraph.GetBuffer(res.m_VisibleCountBuffer, RGResourceAccessMode::Write);
        handles.visibleIndirect = renderGraph.GetBuffer(res.m_VisibleIndirectBuffer, RGResourceAccessMode::Write);
        handles.occludedCount = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndices = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndicesBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.occludedIndirect = renderer->m_EnableOcclusionCulling ? renderGraph.GetBuffer(res.m_OccludedIndirectBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJob = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletJobCount = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletJobCountBuffer, RGResourceAccessMode::Write) : nullptr;
        handles.meshletIndirect = renderer->m_UseMeshletRendering ? renderGraph.GetBuffer(res.m_MeshletIndirectBuffer, RGResourceAccessMode::Write) : nullptr;

        handles.depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
        handles.hzb = renderer->m_EnableOcclusionCulling ? renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Read) : nullptr;
        handles.hdr = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);
        handles.opaque = renderGraph.GetTexture(g_RG_OpaqueColor, RGResourceAccessMode::Write);
        handles.skyVis = renderer->m_EnableSky ? renderGraph.GetTexture(g_RG_SkyVisibility, RGResourceAccessMode::Read) : nullptr;

        // Capture the opaque scene for refraction
        commandList->copyTexture(handles.opaque, nvrhi::TextureSlice(), handles.hdr, nvrhi::TextureSlice());

        nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
        renderer->GenerateMipsUsingSPD(handles.opaque, spdAtomicCounter, commandList, "Generate Mips for Opaque Color", SPD_REDUCTION_AVERAGE);

        Matrix view, viewProjForCulling;
        Vector4 frustumPlanes[5];
        PrepareRenderingData(view, viewProjForCulling, frustumPlanes);

        ClearVisibleCounters(commandList, handles);

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

        PerformOcclusionCulling(commandList, args, handles);
        RenderInstances(commandList, args, handles);
    }

    const char* GetName() const override { return "TransparentPass"; }

private:
    RGBufferHandle m_RG_SPDAtomicCounter;
};

REGISTER_RENDERER(OpaquePhase1Renderer);
REGISTER_RENDERER(HZBGenerator);
REGISTER_RENDERER(OpaquePhase2Renderer);
REGISTER_RENDERER(MaskedPassRenderer);
REGISTER_RENDERER(HZBGeneratorPhase2);
REGISTER_RENDERER(TransparentPassRenderer);
