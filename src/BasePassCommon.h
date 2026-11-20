#pragma once


#include "RenderGraph.h"

struct BasePassResources
{
    RGBufferHandle m_MeshletJobBuffer;
    RGBufferHandle m_MeshletJobCountBuffer;
    RGBufferHandle m_MeshletIndirectBuffer;

    // Cluster LOD traversal: the node queue ping-pongs between depth passes, groups are queued once and
    // then turned into the visible-cluster list.
    RGBufferHandle m_ClusterNodeQueue[2];
    RGBufferHandle m_ClusterNodeQueueCount[2];
    RGBufferHandle m_ClusterGroupQueue;
    RGBufferHandle m_ClusterGroupQueueCount;
    RGBufferHandle m_ClusterJobs;
    RGBufferHandle m_ClusterJobCount;
    RGBufferHandle m_ClusterIndirectBuffer;
    RGBufferHandle m_ClusterCountersBuffer;

    // Two-pass culling: what the first pass dropped on occlusion alone, and the list the second pass draws from.
    RGBufferHandle m_ClusterRejectNodes;
    RGBufferHandle m_ClusterRejectNodeCount;
    RGBufferHandle m_ClusterRejectGroups;
    RGBufferHandle m_ClusterRejectGroupCount;
    RGBufferHandle m_ClusterJobsSecond;
    RGBufferHandle m_ClusterJobCountSecond;
    RGBufferHandle m_ClusterRejectMasks;

    RGBufferHandle m_VisibleIndirectBuffer;
    RGBufferHandle m_VisibleCountBuffer;
    RGBufferHandle m_OccludedIndicesBuffer;
    RGBufferHandle m_OccludedCountBuffer;
    RGBufferHandle m_OccludedIndirectBuffer;

    nvrhi::PipelineStatisticsQueryHandle m_PipelineQueries[2];

    void Initialize();
    // clusterBucket: which bucket's cluster traversal buffers to declare (-1, Scene::kClusterBucketNone, for a
    // bucket that does not traverse). Only the traversing buckets pay for the buffers, and each is sized by its
    // own worst case rather than the scene's.
    void DeclareResources(RenderGraph& rg, std::string_view rendererName, int clusterBucket = -1);
};
