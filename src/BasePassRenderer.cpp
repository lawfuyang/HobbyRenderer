#include "Renderer.h"
#include "CommonResources.h"
#include "BasePassCommon.h"
#include "Camera.h"
#include "Config.h"
#include "Utilities.h"

#include "shaders/srrhi/cpp/BasePass.h"
#include "shaders/srrhi/cpp/GPUCulling.h"
#include "shaders/srrhi/cpp/Cluster.h"
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

// ClusterTraversal.hlsl writes three grids into the cluster indirect buffer; these are their slots, in the same
// order the shader's kIndirectSlot* constants use. The first pass' mesh grid is slot 0, which is the offset every
// cluster draw that does not set one uses.
static constexpr uint32_t kClusterSecondPassInitSlot = 1;
static constexpr uint32_t kClusterSecondPassMeshSlot = 2;
static constexpr uint32_t kClusterRejectClustersSlot = 3;

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

        nvrhi::BufferHandle clusterNodeQueue[2];
        nvrhi::BufferHandle clusterNodeQueueCount[2];
        nvrhi::BufferHandle clusterGroupQueue;
        nvrhi::BufferHandle clusterGroupQueueCount;
        nvrhi::BufferHandle clusterJobs;
        nvrhi::BufferHandle clusterJobCount;
        nvrhi::BufferHandle clusterIndirect;
        nvrhi::BufferHandle clusterCounters;
        nvrhi::BufferHandle clusterRejectNodes;
        nvrhi::BufferHandle clusterRejectNodeCount;
        nvrhi::BufferHandle clusterRejectGroups;
        nvrhi::BufferHandle clusterRejectGroupCount;
        nvrhi::BufferHandle clusterJobsSecond;
        nvrhi::BufferHandle clusterJobCountSecond;
        nvrhi::BufferHandle clusterRejectMasks;

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
        const Vector4* m_FrustumPlanes; // [6] planes
        Matrix m_View;
        Matrix m_ViewProj;
        uint32_t m_NumInstances;
        int m_CullingPhase;
        uint32_t m_AlphaMode;
        const char* m_BucketName;
        bool m_BackFaceCull = false;
        // Byte offset into the cluster indirect buffer of the grid this draw consumes.
        uint32_t m_ClusterIndirectOffset = 0;
    };

    // True when this bucket's base pass draws through the cluster mesh shader, which is also the condition for the
    // traversal being the owner of the per-instance LOD ray tracing is patched with.
    bool DrawsClusters(uint32_t alphaMode)
    {
        return g_Renderer.m_EnableClusterRendering && alphaMode != srrhi::CommonConsts::ALPHA_MODE_BLEND;
    }

    void ComputeFrustumPlanes(const Matrix& proj, Vector4 frustumPlanes[6])
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
        // Infinite far plane: reversed-Z infinite perspective has no finite far clip.
        // Use a degenerate plane (zero normal, +INF offset) that never culls anything.
        frustumPlanes[5] = { 0.0f, 0.0f, 0.0f, std::numeric_limits<float>::infinity() };
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
            // Culled instances keep their zeroed entry, which the indexed draw relies on.
            commandList->clearBufferUInt(handles.visibleIndirect, 0);

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
        cullData.SetForcedLOD(Config::Get().m_ForcedLOD);
        cullData.SetInstanceBaseIndex(args.m_InstanceBaseIndex);
        cullData.SetUseClusterRendering(DrawsClusters(args.m_AlphaMode) ? 1u : 0u);
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

        // Build indirect args after every culling phase. The phase 2 culling grid is only
        // meaningful after phase 0, but the mesh dispatch grid has to be rebuilt for both.
        {
            Renderer::RenderPassParams params;
            params.commandList = commandList;
            params.shaderID = ShaderID::GPUCULLING_BUILDINDIRECT_CSMAIN;
            params.bindingSetDesc = cullBset;
            params.dispatchParams = { .x = 1, .y = 1, .z = 1 };
            g_Renderer.AddComputePass(params);
        }
    }

    // The traversal's buffers are written every frame and none of them outlive it, so every renderer that runs
    // a traversal declares them as writes.
    void WriteClusterBuffers(RenderGraph& renderGraph)
    {
        BasePassResources& res = m_BasePassResources;
        for (uint32_t i = 0; i < 2; ++i)
        {
            renderGraph.WriteBuffer(res.m_ClusterNodeQueue[i]);
            renderGraph.WriteBuffer(res.m_ClusterNodeQueueCount[i]);
        }
        renderGraph.WriteBuffer(res.m_ClusterGroupQueue);
        renderGraph.WriteBuffer(res.m_ClusterGroupQueueCount);
        renderGraph.WriteBuffer(res.m_ClusterJobs);
        renderGraph.WriteBuffer(res.m_ClusterJobCount);
        renderGraph.WriteBuffer(res.m_ClusterIndirectBuffer);
        renderGraph.WriteBuffer(res.m_ClusterCountersBuffer);
        renderGraph.WriteBuffer(res.m_ClusterRejectNodes);
        renderGraph.WriteBuffer(res.m_ClusterRejectNodeCount);
        renderGraph.WriteBuffer(res.m_ClusterRejectGroups);
        renderGraph.WriteBuffer(res.m_ClusterRejectGroupCount);
        renderGraph.WriteBuffer(res.m_ClusterJobsSecond);
        renderGraph.WriteBuffer(res.m_ClusterJobCountSecond);
        renderGraph.WriteBuffer(res.m_ClusterRejectMasks);
    }

    void FetchClusterHandles(const RenderGraph& renderGraph, ResourceHandles& handles)
    {
        BasePassResources& res = m_BasePassResources;
        for (uint32_t i = 0; i < 2; ++i)
        {
            handles.clusterNodeQueue[i] = renderGraph.GetBuffer(res.m_ClusterNodeQueue[i], RGResourceAccessMode::Write);
            handles.clusterNodeQueueCount[i] = renderGraph.GetBuffer(res.m_ClusterNodeQueueCount[i], RGResourceAccessMode::Write);
        }
        handles.clusterGroupQueue = renderGraph.GetBuffer(res.m_ClusterGroupQueue, RGResourceAccessMode::Write);
        handles.clusterGroupQueueCount = renderGraph.GetBuffer(res.m_ClusterGroupQueueCount, RGResourceAccessMode::Write);
        handles.clusterJobs = renderGraph.GetBuffer(res.m_ClusterJobs, RGResourceAccessMode::Write);
        handles.clusterJobCount = renderGraph.GetBuffer(res.m_ClusterJobCount, RGResourceAccessMode::Write);
        handles.clusterIndirect = renderGraph.GetBuffer(res.m_ClusterIndirectBuffer, RGResourceAccessMode::Write);
        handles.clusterCounters = renderGraph.GetBuffer(res.m_ClusterCountersBuffer, RGResourceAccessMode::Write);
        handles.clusterRejectNodes = renderGraph.GetBuffer(res.m_ClusterRejectNodes, RGResourceAccessMode::Write);
        handles.clusterRejectNodeCount = renderGraph.GetBuffer(res.m_ClusterRejectNodeCount, RGResourceAccessMode::Write);
        handles.clusterRejectGroups = renderGraph.GetBuffer(res.m_ClusterRejectGroups, RGResourceAccessMode::Write);
        handles.clusterRejectGroupCount = renderGraph.GetBuffer(res.m_ClusterRejectGroupCount, RGResourceAccessMode::Write);
        handles.clusterJobsSecond = renderGraph.GetBuffer(res.m_ClusterJobsSecond, RGResourceAccessMode::Write);
        handles.clusterJobCountSecond = renderGraph.GetBuffer(res.m_ClusterJobCountSecond, RGResourceAccessMode::Write);
        handles.clusterRejectMasks = renderGraph.GetBuffer(res.m_ClusterRejectMasks, RGResourceAccessMode::Write);
    }

    // Both passes read the same buffers and differ only in the constant buffer, except for the group queue: in the
    // second pass that IS the reject group list, which the node passes append to and Groups then walks.
    // `indirectArgs` overrides the linear grid when the grid comes from a count only the GPU knows.
    void DispatchTraversalPass(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args,
                               const ResourceHandles& handles, nvrhi::BufferHandle cb, uint32_t cullPass,
                               uint32_t shaderID, uint32_t queueIn, uint32_t queueOut, uint32_t gridX,
                               nvrhi::BufferHandle indirectArgs = nullptr, uint32_t indirectOffsetBytes = 0)
    {
        const Scene& scene = g_Renderer.m_Scene;
        const nvrhi::TextureHandle hzb = handles.hzb;
        const bool bSecondPass = cullPass != 0;

        srrhi::ClusterTraversalInputs inputs;
        inputs.SetTraversalCB(cb);
        inputs.SetInstanceData(scene.m_InstanceDataBuffer);
        inputs.SetGeometry(scene.m_ClusterGeometryBuffer);
        inputs.SetNodes(scene.m_ClusterNodeBuffer);
        inputs.SetNodeBounds(scene.m_ClusterNodeBoundsBuffer);
        inputs.SetGroups(scene.m_ClusterGroupBuffer);
        inputs.SetGeneratingGroups(scene.m_ClusterGeneratingGroupsBuffer);
        inputs.SetClusterRecords(scene.m_ClusterRecordBuffer);
        inputs.SetHZB(hzb ? hzb : CommonResources::GetInstance().DefaultTextureBlack);
        inputs.SetLevelMeshLODs(scene.m_ClusterLevelMeshLODBuffer);
        inputs.SetDiscreteLODLevels(g_Renderer.m_GeometryStreaming ?
            g_Renderer.m_GeometryStreaming->GetDiscreteLevelBuffer() : CommonResources::GetInstance().DummySRVStructuredBuffer);
        inputs.SetNodeQueueIn(handles.clusterNodeQueue[queueIn]);
        inputs.SetNodeQueueOut(handles.clusterNodeQueue[queueOut]);
        inputs.SetNodeQueueCountIn(handles.clusterNodeQueueCount[queueIn]);
        inputs.SetNodeQueueCountOut(handles.clusterNodeQueueCount[queueOut]);
        inputs.SetGroupQueue(bSecondPass ? handles.clusterRejectGroups : handles.clusterGroupQueue);
        inputs.SetGroupQueueCount(bSecondPass ? handles.clusterRejectGroupCount : handles.clusterGroupQueueCount);
        inputs.SetClusterJobs(handles.clusterJobs);
        inputs.SetClusterJobCount(handles.clusterJobCount);
        inputs.SetClusterIndirectArgs(handles.clusterIndirect);
        inputs.SetCounters(handles.clusterCounters);
        inputs.SetRejectNodes(handles.clusterRejectNodes);
        inputs.SetRejectNodeCount(handles.clusterRejectNodeCount);
        inputs.SetRejectGroups(handles.clusterRejectGroups);
        inputs.SetRejectGroupCount(handles.clusterRejectGroupCount);
        inputs.SetClusterJobsSecond(handles.clusterJobsSecond);
        inputs.SetClusterJobCountSecond(handles.clusterJobCountSecond);
        inputs.SetRejectClusterMasks(handles.clusterRejectMasks);
        // The first pass' group queue stays bound for both passes: its length is the grid the per-cluster
        // reject pass runs over, and pass 1 only repurposes the *other* queue (the reject group list).
        inputs.SetPass0Groups(handles.clusterGroupQueue);
        inputs.SetPass0GroupCount(handles.clusterGroupQueueCount);

        // Geometry streaming: the residency table the traversal tests and requests against, plus the list and the
        // counters a missing group is asked for through.
        const GeometryStreaming* streaming = g_Renderer.m_GeometryStreaming.get();
        inputs.SetGroupStates(streaming ? streaming->GetGroupStateBuffer() : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetLoadRequests(streaming ? streaming->GetLoadRequestBuffer() : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        inputs.SetStreamCounters(streaming ? streaming->GetStreamCounterBuffer() : CommonResources::GetInstance().DummyUAVStructuredBuffer);
        // The traversal writes the LOD ray tracing is patched with, so the two paths share one classification.
        inputs.SetInstanceLOD(scene.m_InstanceLODBuffer);

        Renderer::RenderPassParams params;
        params.commandList = commandList;
        params.shaderID = shaderID;
        params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
        params.dispatchParams = indirectArgs ?
            Renderer::ComputeDispatchParams{ .indirectBuffer = indirectArgs, .indirectOffsetBytes = indirectOffsetBytes } :
            Renderer::ComputeDispatchParams{ .x = gridX, .y = 1, .z = 1 };
        g_Renderer.AddComputePass(params);
    }

    nvrhi::BufferHandle WriteClusterTraversalConstants(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args,
                                                       const ResourceHandles& handles, uint32_t cullPass)
    {
        const Scene& scene = g_Renderer.m_Scene;
        const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(
            sizeof(srrhi::ClusterTraversalConstants), "ClusterTraversalCB", 1);
        const nvrhi::BufferHandle cb = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(cbDesc);

        const Matrix& projectionMatrix = scene.m_Camera.GetProjMatrix();
        const nvrhi::TextureHandle hzb = handles.hzb;

        Vector3 cullingCameraPosition = scene.m_Camera.GetPosition();
        if (Config::Get().m_ClusterFreezeCulling)
        {
            cullingCameraPosition = scene.m_FrozenCullingCameraPos;
        }

        // The metric is defined in pixels of the full-resolution view, which is also the HZB's mip 0.
        const float focalPixels = projectionMatrix.m[1][1] * float(handles.depth->getDesc().height) * 0.5f;

        srrhi::ClusterTraversalConstants data{};
        data.SetView(args.m_View);
        data.SetFrustumPlanes(args.m_FrustumPlanes);
        data.SetCameraPosition(DirectX::XMFLOAT4{ cullingCameraPosition.x, cullingCameraPosition.y, cullingCameraPosition.z, 1.0f });
        data.SetNumInstances(args.m_NumInstances);
        data.SetInstanceBaseIndex(args.m_InstanceBaseIndex);
        data.SetEnableFrustumCulling(g_Renderer.m_EnableFrustumCulling ? 1u : 0u);
        data.SetEnableOcclusionCulling((g_Renderer.m_EnableOcclusionCulling && hzb) ? 1u : 0u);
        data.SetHZBWidth(hzb ? hzb->getDesc().width : 0);
        data.SetHZBHeight(hzb ? hzb->getDesc().height : 0);
        data.SetForcedLOD(Config::Get().m_ClusterForcedLOD >= 0 ? uint32_t(Config::Get().m_ClusterForcedLOD) : 0xFFFFFFFFu);
        data.SetMaxClusters(Config::Get().m_ClusterMaxClusters > 0 ? uint32_t(Config::Get().m_ClusterMaxClusters) : 0u);
        data.SetP00(projectionMatrix.m[0][0]);
        data.SetP11(projectionMatrix.m[1][1]);
        data.SetPixelErrorThreshold(Config::Get().m_ClusterPixelError);
        data.SetFocalPixels(focalPixels);
        data.SetDisableCulling(Config::Get().m_ClusterDisableCulling ? 1u : 0u);
        data.SetCullPass(cullPass);
        data.SetEnableRejectLists(Config::Get().m_ClusterTwoPass ? 1u : 0u);

        // Streaming is always on: the traversal asks for any group the arena does not hold, and the age filter
        // hands back the ones the camera has left. Without cluster data there is no table to read, so it is off.
        const GeometryStreaming* streaming = g_Renderer.m_GeometryStreaming.get();
        data.SetStreamingEnabled(streaming != nullptr ? 1u : 0u);
        data.SetFrameIndex(streaming != nullptr ? streaming->GetFrameIndex() : 0u);
        data.SetMaxLoads(streaming != nullptr ? streaming->GetMaxLoads() : 0u);
        data.SetEnableDiscreteLOD(Config::Get().m_ClusterDiscreteLod ? 1u : 0u);
        commandList->writeBuffer(cb, &data, sizeof(data), 0);

        return cb;
    }

    uint32_t ClusterBucketIndex(const BasePassRenderingArgs& args)
    {
        // Both traversal blocks are opaque-or-masked, and each bucket's bounds come from its own instances.
        const uint32_t bucketIndex = args.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE ?
            Scene::kClusterBucketOpaque : Scene::kClusterBucketMasked;
        SDL_assert(bucketIndex < Scene::kClusterBucketCount);
        return bucketIndex;
    }

    void CopyClusterCounters(nvrhi::CommandListHandle commandList, const ResourceHandles& handles, uint32_t bucketIndex)
    {
        const Scene& scene = g_Renderer.m_Scene;
        const Matrix& projectionMatrix = scene.m_Camera.GetProjMatrix();
        const float focalPixels = projectionMatrix.m[1][1] * float(handles.depth->getDesc().height) * 0.5f;

        // One readback per bucket: both blocks traverse every frame and each has its own counters buffer.
        if (Config::Get().m_ClusterStatsFrame == int(g_Renderer.m_FrameNumber))
        {
            g_Renderer.RequestClusterCountersReadback(handles.clusterCounters, commandList, bucketIndex,
                bucketIndex == Scene::kClusterBucketOpaque ? "Opaque" : "Masked", focalPixels);
        }

        if (g_Renderer.IsClusterCountersLive())
        {
            // The panel path copies every frame and is read one frame late, so it never stalls.
            g_Renderer.CopyClusterCountersForPanel(handles.clusterCounters, commandList, bucketIndex);
        }
    }

    // The first pass builds this bucket's visible-cluster list from the roots, using the previous frame's depth.
    // Whatever the occlusion test rejects is recorded instead of dropped, so the second pass can pick it up once
    // this frame's depth exists (docs/cluster-lod-implementation-plan.md §4 phase 4).
    void PerformClusterTraversal(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args,
                                 const ResourceHandles& handles, bool bSecondPassFollows)
    {
        PROFILE_FUNCTION();

        const Scene& scene = g_Renderer.m_Scene;

        for (uint32_t i = 0; i < 2; ++i)
        {
            commandList->clearBufferUInt(handles.clusterNodeQueueCount[i], 0);
        }
        commandList->clearBufferUInt(handles.clusterGroupQueueCount, 0);
        commandList->clearBufferUInt(handles.clusterJobCount, 0);
        commandList->clearBufferUInt(handles.clusterCounters, 0);

        // The reject machinery is only written and read when a second pass follows, and every one of these is a
        // separate clear command: with the feature off they would cost more than the traversal itself.
        if (Config::Get().m_ClusterTwoPass)
        {
            commandList->clearBufferUInt(handles.clusterJobCountSecond, 0);
            commandList->clearBufferUInt(handles.clusterRejectNodeCount, 0);
            commandList->clearBufferUInt(handles.clusterRejectGroupCount, 0);
            commandList->clearBufferUInt(handles.clusterRejectMasks, 0);
        }

        const nvrhi::BufferHandle cb = WriteClusterTraversalConstants(commandList, args, handles, 0u);
        const uint32_t cullPass = 0u;
        const uint32_t bucketIndex = ClusterBucketIndex(args);

        // One scope for the whole traversal: every nested scope is two more query resolves, and the shapes this
        // pass could plausibly split into are all in the 0.02-0.3 ms range where a resolve is a real tax.
        {
            PROFILE_GPU_SCOPED("Cluster Traversal", commandList);

            // Seeds go into queue 0; queue 1 is the first pass's output.
            DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERTRAVERSALINIT_CSMAIN,
                1u, 0u, DivideAndRoundUp(args.m_NumInstances, srrhi::CommonConsts::kThreadsPerGroup));

            const uint32_t nodeGridX = DivideAndRoundUp(scene.m_MaxClusterNodeCount[bucketIndex], srrhi::CommonConsts::kThreadsPerGroup);

            // One pass per tree level. The grid is the worst case because a pass cannot know its own count
            // before it runs; every idle thread exits on the first read of the counter.
            const uint32_t passCount = scene.m_MaxClusterTreeDepth + 1;
            for (uint32_t pass = 0; pass < passCount; ++pass)
            {
                const uint32_t queueIn = pass & 1u;
                const uint32_t queueOut = 1u - queueIn;

                commandList->clearBufferUInt(handles.clusterNodeQueueCount[queueOut], 0);
                DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERTRAVERSALRUN_CSMAIN,
                    queueIn, queueOut, nodeGridX);
            }

            DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERTRAVERSALGROUPS_CSMAIN,
                0u, 1u, DivideAndRoundUp(scene.m_MaxClusterGroupCount[bucketIndex], srrhi::CommonConsts::kThreadsPerGroup));

            // Writes the first pass' mesh grid and the second pass' seed grids.
            DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERBUILDARGS_CSMAIN, 0u, 1u, 1u);
        }

        // The second pass appends to the same counters, so the readback has to wait for it when it runs.
        if (!bSecondPassFollows)
        {
            CopyClusterCounters(commandList, handles, bucketIndex);
        }
    }

    // The second pass continues from what the first pass rejected, against the depth the first pass just wrote.
    // The traversal kernels are the same ones: the queues are seeded from the reject lists and the constant
    // buffer says "cull pass 1", which stops the recording and sends the clusters to the second job list.
    void PerformClusterTraversalSecondPass(nvrhi::CommandListHandle commandList, const BasePassRenderingArgs& args,
                                           const ResourceHandles& handles)
    {
        PROFILE_FUNCTION();

        const Scene& scene = g_Renderer.m_Scene;
        const uint32_t bucketIndex = ClusterBucketIndex(args);

        commandList->clearBufferUInt(handles.clusterNodeQueueCount[0], 0);
        commandList->clearBufferUInt(handles.clusterNodeQueueCount[1], 0);
        commandList->clearBufferUInt(handles.clusterJobCountSecond, 0);
        // The group queue is NOT cleared: in this pass it is the reject group list, which the seed step below and
        // the node passes both append to.

        const nvrhi::BufferHandle cb = WriteClusterTraversalConstants(commandList, args, handles, 1u);
        const uint32_t cullPass = 1u;

        {
            PROFILE_GPU_SCOPED("Cluster Traversal Second Pass", commandList);

            // One thread per entry of the larger reject list. The grid comes from the first pass' counters
            // through the indirect slot ClusterBuildArgs filled in, so nothing can be rejected there and the
            // grid is empty, the second pass costs one indirect dispatch.
            DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERTRAVERSALINIT_CSMAIN,
                1u, 0u, 1u, handles.clusterIndirect, kClusterSecondPassInitSlot * sizeof(srrhi::DispatchIndirectArguments));

            const uint32_t nodeGridX = DivideAndRoundUp(scene.m_MaxClusterNodeCount[bucketIndex], srrhi::CommonConsts::kThreadsPerGroup);

            const uint32_t passCount = scene.m_MaxClusterTreeDepth + 1;
            for (uint32_t pass = 0; pass < passCount; ++pass)
            {
                const uint32_t queueIn = pass & 1u;
                const uint32_t queueOut = 1u - queueIn;

                commandList->clearBufferUInt(handles.clusterNodeQueueCount[queueOut], 0);
                DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERTRAVERSALRUN_CSMAIN,
                    queueIn, queueOut, nodeGridX);
            }

            DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERTRAVERSALGROUPS_CSMAIN,
                0u, 1u, DivideAndRoundUp(scene.m_MaxClusterGroupCount[bucketIndex], srrhi::CommonConsts::kThreadsPerGroup));

            // Per-cluster rejects: the first pass' group walk only tested each group's sphere, and this re-tests
            // the individual clusters it deferred against the depth the first pass has now filled in.
            DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERREJECTCLUSTERS_CSMAIN,
                0u, 1u, 1u, handles.clusterIndirect, kClusterRejectClustersSlot * sizeof(srrhi::DispatchIndirectArguments));

            DispatchTraversalPass(commandList, args, handles, cb, cullPass, ShaderID::CLUSTERTRAVERSAL_CLUSTERBUILDARGS_CSMAIN, 0u, 1u, 1u);
        }

        CopyClusterCounters(commandList, handles, bucketIndex);
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

        // The cluster path serves the opaque and masked buckets only: the transparent bucket keeps the plain
        // vertex-shader path, which can still be sorted per surface (docs/cluster-lod-implementation-plan.md §7).
        const bool bDrawClusters = g_Renderer.m_EnableClusterRendering && !bUseAlphaBlend;

        const uint32_t effectiveDebugMode = uint32_t(Config::Get().m_DebugMode);

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
        cb.SetEnableRTShadows(g_Renderer.IsRTShadowEnabled() ? 1 : 0);
        cb.SetDebugMode(effectiveDebugMode);
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
        cb.SetForcedTextureMip(g_Renderer.m_ForcedTextureMip);

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

        // The cluster draw's own inputs. Every one of them is a dummy when the cluster path is not drawing, so
        // the binding layout stays the same for the transparent and legacy paths.
        const Scene& scene = g_Renderer.m_Scene;
        const nvrhi::BufferHandle dummyStructured = CommonResources::GetInstance().DummySRVStructuredBuffer;
        inputs.SetClusterJobs(bDrawClusters ? handles.clusterJobs : dummyStructured);
        inputs.SetClusterRecords(scene.m_ClusterRecordBuffer ? scene.m_ClusterRecordBuffer : dummyStructured);
        inputs.SetClusterStreamData(g_Renderer.m_GeometryStreaming ? g_Renderer.m_GeometryStreaming->GetStreamDataBuffer() : dummyStructured);
        inputs.SetClusterGroups(scene.m_ClusterGroupBuffer ? scene.m_ClusterGroupBuffer : dummyStructured);
        inputs.SetClusterGroupStates(g_Renderer.m_GeometryStreaming ? g_Renderer.m_GeometryStreaming->GetGroupStateBuffer() : dummyStructured);

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

        if (bDrawClusters)
        {
            nvrhi::MeshletPipelineDesc clusterPipelineDesc;
            clusterPipelineDesc.AS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERBASEPASS_ASMAIN);
            clusterPipelineDesc.MS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERBASEPASS_MSMAIN);
            clusterPipelineDesc.PS = g_Renderer.GetShaderHandle(psID);
            clusterPipelineDesc.renderState = renderState;
            clusterPipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };

            nvrhi::MeshletState meshState;
            meshState.framebuffer = framebuffer;
            meshState.pipeline = g_Renderer.GetOrCreateMeshletPipeline(clusterPipelineDesc, fbInfo);
            meshState.bindings = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
            meshState.viewport = viewportState;
            meshState.indirectParams = handles.clusterIndirect;

            commandList->setMeshletState(meshState);
            // One DispatchMesh over the visible-cluster list; ClusterBuildArgs wrote that count into the args.
            commandList->dispatchMeshIndirect(args.m_ClusterIndirectOffset, 1);

            if (effectiveDebugMode == srrhi::CommonConsts::DEBUG_MODE_CLUSTER_BOUNDS)
            {
                // The bounds overlay shares this dispatch list, framebuffer and depth buffer, but it must not
                // write depth: the HZB built from it at the end of the opaque pass decides the next frame's
                // occlusion, and a wireframe in the depth buffer would cull real geometry behind it.
                nvrhi::RenderState overlayState = renderState;
                overlayState.rasterState = CommonResources::GetInstance().RasterCullNone;
                overlayState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetOpaque;
                overlayState.depthStencilState = CommonResources::GetInstance().DepthRead;

                nvrhi::MeshletPipelineDesc overlayPipelineDesc;
                overlayPipelineDesc.AS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERDEBUG_ASMAIN);
                overlayPipelineDesc.MS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERDEBUG_MSMAIN);
                overlayPipelineDesc.PS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERDEBUG_PSMAIN);
                overlayPipelineDesc.primType = nvrhi::PrimitiveType::LineList;
                overlayPipelineDesc.renderState = overlayState;
                overlayPipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };

                nvrhi::MeshletState overlayMeshState;
                overlayMeshState.framebuffer = framebuffer;
                overlayMeshState.pipeline = g_Renderer.GetOrCreateMeshletPipeline(overlayPipelineDesc, fbInfo);
                overlayMeshState.bindings = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
                overlayMeshState.viewport = viewportState;
                overlayMeshState.indirectParams = handles.clusterIndirect;

                commandList->setMeshletState(overlayMeshState);
                commandList->dispatchMeshIndirect(args.m_ClusterIndirectOffset, 1);
            }
            else if (effectiveDebugMode == srrhi::CommonConsts::DEBUG_MODE_CLUSTER_TRIANGLES)
            {
                // Per-triangle view: the same triangles again, one vertex triple each, so the pixel shader can
                // colour them by index. The depth test is read-equal (identical positions, so identical depth)
                // and the albedo target is overwritten; the deferred pass passes this mode through unlit.
                nvrhi::RenderState overlayState = renderState;
                overlayState.depthStencilState = CommonResources::GetInstance().DepthRead;

                nvrhi::MeshletPipelineDesc trianglePipelineDesc;
                trianglePipelineDesc.AS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERDEBUG_TRIANGLESASMAIN);
                trianglePipelineDesc.MS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERDEBUG_TRIANGLESMSMAIN);
                trianglePipelineDesc.PS = g_Renderer.GetShaderHandle(ShaderID::CLUSTERDEBUG_TRIANGLESPSMAIN);
                trianglePipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
                trianglePipelineDesc.renderState = overlayState;
                trianglePipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };

                nvrhi::MeshletState triangleMeshState;
                triangleMeshState.framebuffer = framebuffer;
                triangleMeshState.pipeline = g_Renderer.GetOrCreateMeshletPipeline(trianglePipelineDesc, fbInfo);
                triangleMeshState.bindings = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
                triangleMeshState.viewport = viewportState;
                triangleMeshState.indirectParams = handles.clusterIndirect;

                commandList->setMeshletState(triangleMeshState);
                commandList->dispatchMeshIndirect(args.m_ClusterIndirectOffset, 1);
            }
        }
        else if (g_Renderer.m_UseMeshletRendering)
        {
            nvrhi::MeshletPipelineDesc meshPipelineDesc;
            meshPipelineDesc.AS = g_Renderer.GetShaderHandle(ShaderID::BASEPASS_ASMAIN);
            meshPipelineDesc.MS = g_Renderer.GetShaderHandle(ShaderID::BASEPASS_MSMAIN);
            meshPipelineDesc.PS = g_Renderer.GetShaderHandle(psID);
            meshPipelineDesc.renderState = renderState;
            meshPipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };

            const nvrhi::MeshletPipelineHandle meshPipeline = g_Renderer.GetOrCreateMeshletPipeline(meshPipelineDesc, fbInfo);

            nvrhi::MeshletState meshState;
            meshState.framebuffer = framebuffer;
            meshState.pipeline = meshPipeline;
            meshState.bindings = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
            meshState.viewport = viewportState;
            meshState.indirectParams = handles.meshletIndirect;

            commandList->setMeshletState(meshState);
            // A single DispatchMesh whose grid is the flat job count built by BuildIndirect.
            commandList->dispatchMeshIndirect(0, 1);
        }
        else
        {
            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.VS = g_Renderer.GetShaderHandle(ShaderID::BASEPASS_VSMAIN);
            pipelineDesc.PS = g_Renderer.GetShaderHandle(psID);
            pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
            pipelineDesc.renderState = renderState;
            pipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };

            nvrhi::GraphicsState state;
            state.framebuffer = framebuffer;
            state.viewport = viewportState;
            state.indexBuffer = nvrhi::IndexBufferBinding{ g_Renderer.m_Scene.m_IndexBuffer, nvrhi::Format::R32_UINT, 0 };
            state.bindings = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
            state.pipeline = g_Renderer.GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
            state.indirectParams = handles.visibleIndirect;
            commandList->setGraphicsState(state);

            // One command per instance; culled instances are left as all-zero (zero-instance)
            // entries by the per-pass clear, so no count buffer is needed.
            commandList->drawIndexedIndirect(0, args.m_NumInstances);
        }
    }

    void PrepareRenderingData(Matrix& outView, Matrix& outViewProjForCulling, Vector4 outFrustumPlanes[6])
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
        // Culled instances keep their zeroed entry, which the indexed draw relies on.
        commandList->clearBufferUInt(handles.visibleIndirect, 0);
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
        res.DeclareResources(renderGraph, GetName(), Scene::kClusterBucketOpaque);

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

        if (g_Renderer.m_EnableClusterRendering)
        {
            WriteClusterBuffers(renderGraph);
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
        Vector4 frustumPlanes[6];
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

        if (g_Renderer.m_EnableClusterRendering)
        {
            FetchClusterHandles(renderGraph, handles);
        }

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

        const bool bClusterTwoPass = g_Renderer.m_EnableClusterRendering && Config::Get().m_ClusterTwoPass;

        PerformOcclusionCulling(commandList, args, handles);

        if (g_Renderer.m_EnableClusterRendering)
        {
            PerformClusterTraversal(commandList, args, handles, bClusterTwoPass);
        }

        RenderInstances(commandList, args, handles);

        // The cluster path draws the whole visible-cluster list in that one dispatch, so a second draw would
        // shade every visible cluster twice. The second culling phase still runs: it is what refreshes the
        // per-instance LOD that TLASPatch patches the ray-tracing structures with, and leaving it out makes the
        // raster and ray-traced paths disagree.
        args.m_BucketName = "Opaque (Occluded)";
        args.m_CullingPhase = 1;
        PerformOcclusionCulling(commandList, args, handles);
        if (!g_Renderer.m_EnableClusterRendering)
        {
            RenderInstances(commandList, args, handles);
        }

        nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
        GenerateHZBMips(commandList, renderGraph, spdAtomicCounter);

        // Two-pass culling: everything the first pass rejected on last frame's depth is re-tested against the
        // depth this frame just wrote, and what survives is drawn from its own list. It can only add clusters,
        // so with an unchanged camera it draws nothing and the image is the single-pass one.
        if (bClusterTwoPass)
        {
            PerformClusterTraversalSecondPass(commandList, args, handles);

            // The second pass fills its own job list and its own grid slot, so the first pass' draw was free to
            // consume what it wrote.
            ResourceHandles secondPassHandles = handles;
            secondPassHandles.clusterJobs = handles.clusterJobsSecond;
            secondPassHandles.clusterJobCount = handles.clusterJobCountSecond;

            BasePassRenderingArgs secondPassArgs = args;
            secondPassArgs.m_BucketName = "Opaque (Revived)";
            secondPassArgs.m_ClusterIndirectOffset = kClusterSecondPassMeshSlot * sizeof(srrhi::DispatchIndirectArguments);

            RenderInstances(commandList, secondPassArgs, secondPassHandles);
        }
    }

    const char* GetName() const override { return "Opaque Renderer"; }
};

class MaskedPassRenderer : public BasePassRendererBase
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        

        BasePassResources& res = m_BasePassResources;
        res.DeclareResources(renderGraph, GetName(), Scene::kClusterBucketMasked);

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

        if (g_Renderer.m_EnableClusterRendering)
        {
            WriteClusterBuffers(renderGraph);
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
        Vector4 frustumPlanes[6];
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

        if (g_Renderer.m_EnableClusterRendering)
        {
            FetchClusterHandles(renderGraph, handles);
        }

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

        if (g_Renderer.m_EnableClusterRendering)
        {
            PerformClusterTraversal(commandList, args, handles, false);
        }

        RenderInstances(commandList, args, handles);
    }

    const char* GetName() const override { return "MaskedPass"; }
};

class TransparentPassRenderer : public BasePassRendererBase
{
public:
    // The transparent bucket stays on the vertex-shader path, on purpose:
    // - Correct blending needs a back-to-front surface order, and a per-cluster mesh-shader list is unordered: the
    //   clusters would blend into each other in whatever order the traversal happened to append them.
    // - The bucket is where per-instance LOD still makes sense: one instance is one draw, so it can be sorted and
    //   LOD'd as a whole without splitting it into clusters.
    // Phase 5's geometry streaming and the OIT pass this bucket is slated for will replace it; until then it runs
    // the same code and the same per-instance LOD as the pre-cluster baseline, which is what V3.2 checks.
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
        Vector4 frustumPlanes[6];
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
