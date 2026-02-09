#pragma once

#include "pch.h"

struct BasePassResources
{
    nvrhi::BufferHandle m_MeshletJobBuffer;
    nvrhi::BufferHandle m_MeshletJobCountBuffer;
    nvrhi::BufferHandle m_MeshletIndirectBuffer;

    nvrhi::BufferHandle m_VisibleIndirectBuffer;
    nvrhi::BufferHandle m_VisibleCountBuffer;
    nvrhi::BufferHandle m_OccludedIndicesBuffer;
    nvrhi::BufferHandle m_OccludedCountBuffer;
    nvrhi::BufferHandle m_OccludedIndirectBuffer;

    nvrhi::PipelineStatisticsQueryHandle m_PipelineQueries[2];

    void Initialize();
    void PostSceneLoad();
};
