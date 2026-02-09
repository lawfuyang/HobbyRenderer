#include "BasePassCommon.h"
#include "Renderer.h"
#include "Utilities.h"
#include "shaders/ShaderShared.h"

void BasePassResources::Initialize()
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
}

void BasePassResources::PostSceneLoad()
{
    Renderer* renderer = Renderer::GetInstance();

    uint32_t numPrimitives = (uint32_t)renderer->m_Scene.m_InstanceData.size();
    numPrimitives = std::max(numPrimitives, 1u); // avoid zero-sized buffers

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

    const nvrhi::BufferDesc meshletIndirectBufDesc = nvrhi::BufferDesc()
        .setByteSize(numPrimitives * sizeof(DispatchIndirectArguments))
        .setStructStride(sizeof(DispatchIndirectArguments))
        .setIsDrawIndirectArgs(true)
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDebugName("MeshletIndirectBuffer");
    m_MeshletIndirectBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(meshletIndirectBufDesc);

    const nvrhi::BufferDesc meshletJobBufDesc = nvrhi::BufferDesc()
        .setByteSize(numPrimitives * sizeof(MeshletJob))
        .setStructStride(sizeof(MeshletJob))
        .setCanHaveUAVs(true)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setDebugName("MeshletJobBuffer");
    m_MeshletJobBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(meshletJobBufDesc);
}
