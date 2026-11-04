#include "OmmRuntime.h"

#include "Scene.h"
#include "Renderer.h"

namespace
{
    // A micromap array build reads both input buffers in the build-input state, and the build only barriers its
    // own destination, so each buffer starts (and stays) in that state.
    nvrhi::BufferDesc MakeOmmBuildInputDesc(uint32_t byteSize, const std::string& debugName)
    {
        SDL_assert(byteSize > 0);

        nvrhi::BufferDesc desc;
        desc.byteSize = byteSize;
        desc.initialState = nvrhi::ResourceStates::OpacityMicromapBuildInput;
        desc.keepInitialState = true;
        desc.debugName = debugName;
        return desc;
    }
}

void BuildSceneOmm(Scene& scene)
{
    SCOPED_TIMER("[OMM] BuildSceneOmm");

    nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice;
    nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
    ScopedCommandList scopedCmd{ cmd, "Build Scene OMMs" };

    size_t builtLods = 0;
    uint64_t totalMasks = 0;
    uint64_t totalDataBytes = 0;
    uint64_t totalIndexBytes = 0;

    for (Scene::Mesh& mesh : scene.m_Meshes)
    {
        for (Scene::Primitive& primitive : mesh.m_Primitives)
        {
            for (uint32_t lodIndex = 0; lodIndex < primitive.m_Omm.size(); ++lodIndex)
            {
                OmmLodData& lod = primitive.m_Omm[lodIndex];
                if (lod.m_Data.empty())
                    continue;

                SDL_assert(lod.m_PerOmmDescs.size() == lod.m_Stats.m_MaskCount);
                SDL_assert(!lod.m_Counts.empty());

                // The three buffers are per LOD because every LOD owns its own array: one primitive's masks differ
                // per LOD, and a BLAS geometry links exactly one array.
                const std::string suffix = "_LOD" + std::to_string(lodIndex);

                nvrhi::BufferDesc dataDesc = MakeOmmBuildInputDesc(uint32_t(lod.m_Data.size()), "OMM_Data" + suffix);
                lod.m_DataBuffer = device->createBuffer(dataDesc);
                cmd->writeBuffer(lod.m_DataBuffer, lod.m_Data.data(), lod.m_Data.size());

                const size_t descBytes = lod.m_PerOmmDescs.size() * sizeof(OmmPerOmmDesc);
                nvrhi::BufferDesc perOmmDesc = MakeOmmBuildInputDesc(uint32_t(descBytes), "OMM_Descs" + suffix);
                lod.m_DescBuffer = device->createBuffer(perOmmDesc);
                cmd->writeBuffer(lod.m_DescBuffer, lod.m_PerOmmDescs.data(), descBytes);

                // One index per triangle, in the same triangle order as the BLAS geometry's index slice, so the
                // geometry reads it from offset 0 with the LOD's own stride.
                const bool bSixteenBit = lod.m_Stats.m_bSixteenBitIndices;
                const size_t indexBytes = bSixteenBit ? lod.m_Indices16.size() * sizeof(uint16_t)
                                                      : lod.m_Indices.size() * sizeof(uint32_t);

                nvrhi::BufferDesc indexDesc;
                indexDesc.byteSize = uint32_t(indexBytes);
                indexDesc.structStride = bSixteenBit ? uint32_t(sizeof(uint16_t)) : uint32_t(sizeof(uint32_t));
                indexDesc.isAccelStructBuildInput = true;
                indexDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
                indexDesc.keepInitialState = true;
                indexDesc.debugName = "OMM_Indices" + suffix;
                lod.m_IndexBuffer = device->createBuffer(indexDesc);

                if (bSixteenBit)
                    cmd->writeBuffer(lod.m_IndexBuffer, lod.m_Indices16.data(), indexBytes);
                else
                    cmd->writeBuffer(lod.m_IndexBuffer, lod.m_Indices.data(), indexBytes);

                nvrhi::rt::OpacityMicromapDesc ommDesc;
                ommDesc.debugName = "OMM" + suffix;
                ommDesc.flags = nvrhi::rt::OpacityMicromapBuildFlags::FastTrace;
                ommDesc.counts = lod.m_Counts;
                ommDesc.inputBuffer = lod.m_DataBuffer;
                ommDesc.perOmmDescs = lod.m_DescBuffer;

                lod.m_Array = device->createOpacityMicromap(ommDesc);
                SDL_assert(lod.m_Array != nullptr);
                cmd->buildOpacityMicromap(lod.m_Array, ommDesc);

                ++builtLods;
                totalMasks += lod.m_Stats.m_MaskCount;
                totalDataBytes += lod.m_Data.size();
                totalIndexBytes += indexBytes;
            }
        }
    }

    SDL_Log("[OMM] Built %zu micromap arrays (%llu masks, %.1f MiB mask data, %.1f MiB index data)",
            builtLods, static_cast<unsigned long long>(totalMasks), double(totalDataBytes) / (1024.0 * 1024.0),
            double(totalIndexBytes) / (1024.0 * 1024.0));
}
