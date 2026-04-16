#pragma once


#include "RenderGraph.h"

struct BasePassResources
{
    RGBufferHandle m_MeshletJobBuffer;
    RGBufferHandle m_MeshletJobCountBuffer;
    RGBufferHandle m_MeshletIndirectBuffer;

    RGBufferHandle m_VisibleIndirectBuffer;
    RGBufferHandle m_VisibleCountBuffer;
    RGBufferHandle m_OccludedIndicesBuffer;
    RGBufferHandle m_OccludedCountBuffer;
    RGBufferHandle m_OccludedIndirectBuffer;

    nvrhi::PipelineStatisticsQueryHandle m_PipelineQueries[2];

    void Initialize();
    void DeclareResources(RenderGraph& rg, std::string_view rendererName);
};
