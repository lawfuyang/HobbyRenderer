#include "BasePassCommon.h"
#include "Renderer.h"
#include "Utilities.h"

void BasePassResources::Initialize()
{
    // Create pipeline statistics queries for double buffering
    m_PipelineQueries[0] = g_Renderer.m_RHI->m_NvrhiDevice->createPipelineStatisticsQuery();
    m_PipelineQueries[1] = g_Renderer.m_RHI->m_NvrhiDevice->createPipelineStatisticsQuery();
}

void BasePassResources::DeclareResources(RenderGraph& rg, std::string_view rendererName)
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
            desc.m_NvrhiDesc.setByteSize(numPrimitives * sizeof(srrhi::DispatchIndirectArguments))
                .setStructStride(sizeof(srrhi::DispatchIndirectArguments))
                .setIsDrawIndirectArgs(true)
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_MeshletIndirectBuffer").c_str());

            rg.DeclareBuffer(desc, m_MeshletIndirectBuffer);
        }

        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.setByteSize(numPrimitives * sizeof(srrhi::MeshletJob))
                .setStructStride(sizeof(srrhi::MeshletJob))
                .setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setDebugName((std::string(rendererName) + "_MeshletJobBuffer").c_str());

            rg.DeclareBuffer(desc, m_MeshletJobBuffer);
        }
    }
}

