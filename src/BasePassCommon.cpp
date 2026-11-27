#include "BasePassCommon.h"
#include "Renderer.h"
#include "Utilities.h"

// The cluster traversal's buffer sizes are sizeof() their srrhi types, which src/shaders/Cluster.sr defines.
#include "shaders/srrhi/cpp/Cluster.h"

// The base pass uses nvrhi's stock command signatures, so these structs have to match the D3D12
// argument layouts those signatures are built from
// (d3d12-device.cpp: 16 for DRAW, 20 for DRAW_INDEXED, 12 for DISPATCH / DISPATCH_MESH).
static_assert(sizeof(srrhi::DrawIndexedIndirectArguments) == 20,
              "must match D3D12_DRAW_INDEXED_ARGUMENTS (the stock drawIndexedIndirectSignature stride)");
static_assert(sizeof(srrhi::DispatchIndirectArguments) == 12,
              "must match D3D12_DISPATCH_MESH_ARGUMENTS (the stock dispatchMeshIndirectSignature stride)");

// ClusterTraversal.hlsl owns four grids in that buffer: the first pass' mesh dispatch, the second pass' INIT
// grid, its mesh dispatch, and the grid of the pass that re-tests the per-cluster rejections.
constexpr uint32_t kClusterIndirectSlotCount = 4;

RGBufferHandle g_RG_VisClusterJobLists[kVisIdListCount];

void BasePassResources::Initialize()
{
    // Create pipeline statistics queries for double buffering
    m_PipelineQueries[0] = g_Renderer.m_RHI->m_NvrhiDevice->createPipelineStatisticsQuery();
    m_PipelineQueries[1] = g_Renderer.m_RHI->m_NvrhiDevice->createPipelineStatisticsQuery();
}

void BasePassResources::DeclareResources(RenderGraph& rg, std::string_view rendererName, int clusterBucket)
{
    uint32_t numPrimitives = (uint32_t)g_Renderer.m_Scene.m_InstanceData.size();
    numPrimitives = std::max(numPrimitives, 1u); // avoid zero-sized buffers

    {
        RGBufferDesc desc;
        desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
            .setStructStride(sizeof(uint32_t))
            .setCanHaveUAVs(true)
            .setIsDrawIndirectArgs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName((std::string(rendererName) + "_VisibleCount").c_str());
        rg.DeclareBuffer(desc, m_VisibleCountBuffer);
    }

    {
        RGBufferDesc desc;
        desc.m_NvrhiDesc.setByteSize(numPrimitives * sizeof(srrhi::DrawIndexedIndirectArguments))
            .setStructStride(sizeof(srrhi::DrawIndexedIndirectArguments))
            .setIsDrawIndirectArgs(true)
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName((std::string(rendererName) + "_VisibleIndirectBuffer").c_str());
        rg.DeclareBuffer(desc, m_VisibleIndirectBuffer);
    }

    if (g_Renderer.m_EnableOcclusionCulling)
    {
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_OccludedCount").c_str());

            rg.DeclareBuffer(desc, m_OccludedCountBuffer);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(srrhi::DispatchIndirectArguments))
                .setStructStride(sizeof(srrhi::DispatchIndirectArguments))
                .setIsDrawIndirectArgs(true)
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_OccludedIndirectBuffer").c_str());

            rg.DeclareBuffer(desc, m_OccludedIndirectBuffer);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(numPrimitives * sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_OccludedIndices").c_str());

            rg.DeclareBuffer(desc, m_OccludedIndicesBuffer);
        }
    }

    if (g_Renderer.m_UseMeshletRendering)
    {
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setIsDrawIndirectArgs(true)
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_MeshletJobCount").c_str());

            rg.DeclareBuffer(desc, m_MeshletJobCountBuffer);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(srrhi::DispatchIndirectArguments))
                .setStructStride(sizeof(srrhi::DispatchIndirectArguments))
                .setIsDrawIndirectArgs(true)
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_MeshletIndirectBuffer").c_str());

            rg.DeclareBuffer(desc, m_MeshletIndirectBuffer);
        }

        {
            // The job list holds one entry per meshlet group, not per instance, so size it by the
            // worst case. That is computed once per scene in Scene::FinalizeLoadedScene().
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(g_Renderer.m_Scene.m_MaxMeshletJobCount * sizeof(srrhi::MeshletJob))
                .setStructStride(sizeof(srrhi::MeshletJob))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_MeshletJobBuffer").c_str());

            rg.DeclareBuffer(desc, m_MeshletJobBuffer);
        }
    }

    if (g_Renderer.m_EnableClusterRendering && clusterBucket >= 0)
    {
        const Scene& scene = g_Renderer.m_Scene;
        SDL_assert(clusterBucket < int(Scene::kClusterBucketCount));

        // The traversal's transient buffers are per bucket, so they are sized by that bucket's own worst case
        // (Scene::FinalizeLoadedScene). Sizing them from the scene-wide bound instead would reserve the opaque
        // bucket's capacity a second time for the masked bucket, which is typically a small fraction of it: the
        // trade-off is that a bucket cannot borrow another's headroom, which is what a per-bucket
        // m_ClusterMaxClusters in the traversal constants already assumes.
        const uint32_t bucket = uint32_t(clusterBucket);

        // The queues are sized by the worst case computed in Scene::FinalizeLoadedScene(): every instance
        // visiting every node / group of its primitive.
        for (uint32_t i = 0; i < 2; ++i)
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(scene.m_MaxClusterNodeCount[bucket] * sizeof(srrhi::ClusterNodeRef))
                .setStructStride(sizeof(srrhi::ClusterNodeRef))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterNodeQueue" + std::to_string(i)).c_str());

            rg.DeclareBuffer(desc, m_ClusterNodeQueue[i]);
        }

        for (uint32_t i = 0; i < 2; ++i)
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterNodeQueueCount" + std::to_string(i)).c_str());

            rg.DeclareBuffer(desc, m_ClusterNodeQueueCount[i]);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(scene.m_MaxClusterGroupCount[bucket] * sizeof(srrhi::ClusterGroupRef))
                .setStructStride(sizeof(srrhi::ClusterGroupRef))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterGroupQueue").c_str());

            rg.DeclareBuffer(desc, m_ClusterGroupQueue);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterGroupQueueCount").c_str());

            rg.DeclareBuffer(desc, m_ClusterGroupQueueCount);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(scene.m_MaxClusterJobCount[bucket] * sizeof(srrhi::ClusterJob))
                .setStructStride(sizeof(srrhi::ClusterJob))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterJobs").c_str());

            rg.DeclareBuffer(desc, m_ClusterJobs);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterJobCount").c_str());

            rg.DeclareBuffer(desc, m_ClusterJobCount);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(kClusterIndirectSlotCount * sizeof(srrhi::DispatchIndirectArguments))
                .setStructStride(sizeof(srrhi::DispatchIndirectArguments))
                .setIsDrawIndirectArgs(true)
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterIndirectBuffer").c_str());

            rg.DeclareBuffer(desc, m_ClusterIndirectBuffer);
        }

        {
            // Read back for the ImGui counters panel and --cluster-stats-frame.
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(srrhi::ClusterCounters))
                .setStructStride(sizeof(srrhi::ClusterCounters))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterCounters").c_str());

            rg.DeclareBuffer(desc, m_ClusterCountersBuffer);
        }

        // The reject lists hold what the first pass dropped on occlusion: at most one entry per node it visited,
        // or one per group, so the same worst cases the traversal queues are sized by cover them.
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(scene.m_MaxClusterNodeCount[bucket] * sizeof(srrhi::ClusterNodeRef))
                .setStructStride(sizeof(srrhi::ClusterNodeRef))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterRejectNodes").c_str());

            rg.DeclareBuffer(desc, m_ClusterRejectNodes);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterRejectNodeCount").c_str());

            rg.DeclareBuffer(desc, m_ClusterRejectNodeCount);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(scene.m_MaxClusterGroupCount[bucket] * sizeof(srrhi::ClusterGroupRef))
                .setStructStride(sizeof(srrhi::ClusterGroupRef))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterRejectGroups").c_str());

            rg.DeclareBuffer(desc, m_ClusterRejectGroups);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterRejectGroupCount").c_str());

            rg.DeclareBuffer(desc, m_ClusterRejectGroupCount);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(scene.m_MaxClusterJobCount[bucket] * sizeof(srrhi::ClusterJob))
                .setStructStride(sizeof(srrhi::ClusterJob))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterJobsSecond").c_str());

            rg.DeclareBuffer(desc, m_ClusterJobsSecond);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterJobCountSecond").c_str());

            rg.DeclareBuffer(desc, m_ClusterJobCountSecond);
        }

        {
            // One bit per cluster of this bucket, set when the per-cluster occlusion test defers it. A bitmask
            // rather than a list: a group has to be able to leave several scattered rejections behind, and pass 1
            // already knows which groups to look at.
            const uint32_t wordCount = (scene.m_MaxClusterJobCount[bucket] + 31u) / 32u;

            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(wordCount * sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_ClusterRejectMasks").c_str());

            rg.DeclareBuffer(desc, m_ClusterRejectMasks);
        }
    }}

