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

    RGBufferHandle m_VisibleIndirectBuffer;
    RGBufferHandle m_VisibleCountBuffer;
    RGBufferHandle m_OccludedIndicesBuffer;
    RGBufferHandle m_OccludedCountBuffer;
    RGBufferHandle m_OccludedIndirectBuffer;

    nvrhi::PipelineStatisticsQueryHandle m_PipelineQueries[2];

    void Initialize();
    // bTraversesClusters: only the buckets that run a cluster traversal declare its buffers, so the transparent
    // pass does not pay for a visible-cluster list it never builds.
    void DeclareResources(RenderGraph& rg, std::string_view rendererName, bool bTraversesClusters = false);
};
