#include "pch.h"
#include "AsyncMeshQueue.h"
#include "SceneLoader.h"
#include "meshoptimizer.h"
#include "cgltf.h"

// Processes a single cgltf_primitive into vertex-quantized + LOD + meshlet data.
// All offsets in the output are zero-based (local to this primitive).
// Returns false if the primitive has no position accessor.
static bool ProcessSinglePrimitive(const cgltf_primitive& prim, MeshUpdateCommand& cmd)
{
    const cgltf_accessor* posAcc  = nullptr;
    const cgltf_accessor* normAcc = nullptr;
    const cgltf_accessor* uvAcc   = nullptr;
    const cgltf_accessor* tangAcc = nullptr;

    for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
    {
        const cgltf_attribute& attr = prim.attributes[ai];
        if      (attr.type == cgltf_attribute_type_position)  posAcc  = attr.data;
        else if (attr.type == cgltf_attribute_type_normal)     normAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_texcoord)   uvAcc   = attr.data;
        else if (attr.type == cgltf_attribute_type_tangent)    tangAcc = attr.data;
    }

    if (!posAcc) return false;

    const cgltf_size vertCount = posAcc->count;
    std::vector<srrhi::Vertex> rawVertices(vertCount);

    for (cgltf_size v = 0; v < vertCount; ++v)
    {
        srrhi::Vertex vx{};
        float pos[4] = {};
        cgltf_accessor_read_float(posAcc, v, pos, cgltf_num_components(posAcc->type));
        vx.m_Pos = { pos[0], pos[1], -pos[2] }; // RH -> LH: negate Z

        float nrm[4] = {};
        if (normAcc)
            cgltf_accessor_read_float(normAcc, v, nrm, cgltf_num_components(normAcc->type));
        vx.m_Normal = { nrm[0], nrm[1], -nrm[2] };

        float uv[4] = {};
        if (uvAcc)
            cgltf_accessor_read_float(uvAcc, v, uv, cgltf_num_components(uvAcc->type));
        vx.m_Uv = { uv[0], uv[1] };

        float tang[4] = { 0,0,0,1 };
        if (tangAcc)
            cgltf_accessor_read_float(tangAcc, v, tang, cgltf_num_components(tangAcc->type));
        vx.m_Tangent = { tang[0], tang[1], -tang[2], -tang[3] }; // RH -> LH

        rawVertices[v] = vx;
    }

    std::vector<uint32_t> rawIndices;
    if (prim.indices)
    {
        rawIndices.resize(prim.indices->count);
        for (cgltf_size k = 0; k < prim.indices->count; ++k)
            rawIndices[k] = (uint32_t)cgltf_accessor_read_index(prim.indices, k);
        for (cgltf_size k = 0; k + 2 < prim.indices->count; k += 3)
            std::swap(rawIndices[k + 1], rawIndices[k + 2]); // restore CCW winding after Z-negate
    }
    else
    {
        rawIndices.resize(vertCount);
        for (uint32_t k = 0; k < (uint32_t)vertCount; ++k) rawIndices[k] = k;
        for (uint32_t k = 0; k + 2 < (uint32_t)vertCount; k += 3)
            std::swap(rawIndices[k + 1], rawIndices[k + 2]);
    }

    std::vector<uint32_t> remap(rawIndices.size());
    size_t uniqueVerts = meshopt_generateVertexRemap(remap.data(), rawIndices.data(), rawIndices.size(),
                                                      rawVertices.data(), rawVertices.size(), sizeof(srrhi::Vertex));

    std::vector<srrhi::Vertex> optVerts(uniqueVerts);
    std::vector<uint32_t>      localIdx(rawIndices.size());
    meshopt_remapVertexBuffer(optVerts.data(), rawVertices.data(), rawVertices.size(), sizeof(srrhi::Vertex), remap.data());
    meshopt_remapIndexBuffer(localIdx.data(), rawIndices.data(), rawIndices.size(), remap.data());
    meshopt_optimizeVertexCache(localIdx.data(), localIdx.data(), localIdx.size(), uniqueVerts);
    meshopt_optimizeVertexFetch(optVerts.data(), localIdx.data(), localIdx.size(), optVerts.data(), uniqueVerts, sizeof(srrhi::Vertex));

    // Quantize vertices
    cmd.m_Vertices.reserve(uniqueVerts);
    for (const srrhi::Vertex& v : optVerts)
    {
        srrhi::VertexQuantized vq{};
        vq.m_Pos = v.m_Pos;

        vq.m_Normal  = (uint32_t)(meshopt_quantizeSnorm(v.m_Normal.x, 10) + 511)
                     | ((uint32_t)(meshopt_quantizeSnorm(v.m_Normal.y, 10) + 511) << 10)
                     | ((uint32_t)(meshopt_quantizeSnorm(v.m_Normal.z, 10) + 511) << 20);
        vq.m_Normal |= (v.m_Tangent.w >= 0.0f ? 0u : 1u) << 30;

        vq.m_Uv = (uint32_t)meshopt_quantizeHalf(v.m_Uv.x)
                | ((uint32_t)meshopt_quantizeHalf(v.m_Uv.y) << 16);

        float tx = v.m_Tangent.x, ty = v.m_Tangent.y, tz = v.m_Tangent.z;
        float tsum = fabsf(tx) + fabsf(ty) + fabsf(tz);
        if (tsum > 1e-6f)
        {
            float tu = tz >= 0.0f ? tx / tsum : (1.0f - fabsf(ty / tsum)) * (tx >= 0.0f ? 1.0f : -1.0f);
            float tv = tz >= 0.0f ? ty / tsum : (1.0f - fabsf(tx / tsum)) * (ty >= 0.0f ? 1.0f : -1.0f);
            vq.m_Tangent = (uint32_t)(meshopt_quantizeSnorm(tu, 8) + 127)
                         | ((uint32_t)(meshopt_quantizeSnorm(tv, 8) + 127) << 8);
        }
        cmd.m_Vertices.push_back(vq);
    }

    // LOD generation + meshlet building
    const size_t maxVerts     = srrhi::CommonConsts::kMaxMeshletVertices;
    const size_t maxTriangles = srrhi::CommonConsts::kMaxMeshletTriangles;
    const float  coneWeight   = 0.25f;
    const uint32_t kIndexLimitForLOD = 1024;
    const float kMaxError      = 1e-1f;
    const float kMinReduction  = 0.85f;
    const float attribute_weights[3] = { 1.0f, 1.0f, 1.0f };
    const float simplifyScale = meshopt_simplifyScale(&optVerts[0].m_Pos.x, uniqueVerts, sizeof(srrhi::Vertex));
    const uint32_t baseIndexCount = (uint32_t)localIdx.size();

    std::vector<uint32_t> currentLodIndices = localIdx;
    float accumulatedError = 0.0f;

    for (uint32_t lod = 0; lod < srrhi::CommonConsts::MAX_LOD_COUNT; ++lod)
    {
        std::vector<uint32_t> lodIndices;
        float lodError = 0.0f;

        if (lod == 0)
        {
            lodIndices = currentLodIndices;
        }
        else
        {
            if (baseIndexCount < kIndexLimitForLOD) break;

            const size_t prevCount = cmd.m_MeshData.m_IndexCounts[lod - 1];
            size_t target = (size_t(double(prevCount) * 0.6) / 3) * 3;

            lodIndices.resize(prevCount);
            size_t newCount = meshopt_simplifyWithAttributes(
                lodIndices.data(), currentLodIndices.data(), prevCount,
                &optVerts[0].m_Pos.x, uniqueVerts, sizeof(srrhi::Vertex),
                &optVerts[0].m_Normal.x, sizeof(srrhi::Vertex),
                attribute_weights, 3, nullptr, target, kMaxError,
                meshopt_SimplifySparse, &lodError);
            lodIndices.resize(newCount);

            if (newCount == prevCount || newCount == 0) break;
            if (newCount >= size_t(double(prevCount) * kMinReduction)) break;
            if (newCount < kIndexLimitForLOD) break;

            accumulatedError = std::max(accumulatedError * 1.5f, lodError);
            lodError = accumulatedError;
            currentLodIndices = lodIndices;
            meshopt_optimizeVertexCache(lodIndices.data(), lodIndices.data(), lodIndices.size(), uniqueVerts);
        }

        cmd.m_MeshData.m_IndexOffsets[lod] = (uint32_t)cmd.m_Indices.size();
        cmd.m_MeshData.m_IndexCounts[lod]  = (uint32_t)lodIndices.size();
        cmd.m_MeshData.m_LODErrors[lod]    = lodError * simplifyScale;

        for (uint32_t idx : lodIndices) cmd.m_Indices.push_back(idx);

        size_t maxMeshlets = meshopt_buildMeshletsBound(lodIndices.size(), maxVerts, maxTriangles);
        std::vector<meshopt_Meshlet> lMeshlets(maxMeshlets);
        std::vector<unsigned int>    lVerts(maxMeshlets * maxVerts);
        std::vector<unsigned char>   lTris(maxMeshlets * maxTriangles * 3);

        size_t meshletCount = meshopt_buildMeshlets(lMeshlets.data(), lVerts.data(), lTris.data(),
            lodIndices.data(), lodIndices.size(),
            &optVerts[0].m_Pos.x, uniqueVerts, sizeof(srrhi::Vertex),
            maxVerts, maxTriangles, coneWeight);
        lMeshlets.resize(meshletCount);

        cmd.m_MeshData.m_MeshletOffsets[lod] = (uint32_t)cmd.m_Meshlets.size();
        cmd.m_MeshData.m_MeshletCounts[lod]  = (uint32_t)meshletCount;
        cmd.m_MeshData.m_LODCount = lod + 1;

        for (size_t i = 0; i < meshletCount; ++i)
        {
            const meshopt_Meshlet& m = lMeshlets[i];
            meshopt_optimizeMeshlet(&lVerts[m.vertex_offset], &lTris[m.triangle_offset],
                                    m.triangle_count, m.vertex_count);

            meshopt_Bounds bounds = meshopt_computeMeshletBounds(
                &lVerts[m.vertex_offset], &lTris[m.triangle_offset],
                m.triangle_count, &optVerts[0].m_Pos.x, uniqueVerts, sizeof(srrhi::Vertex));

            srrhi::Meshlet gpuMeshlet{};
            gpuMeshlet.m_VertexOffset   = (uint32_t)cmd.m_MeshletVertices.size();
            gpuMeshlet.m_TriangleOffset = (uint32_t)cmd.m_MeshletTriangles.size();
            gpuMeshlet.m_VertexCount    = m.vertex_count;
            gpuMeshlet.m_TriangleCount  = m.triangle_count;

            gpuMeshlet.m_CenterRadius[0] = (uint32_t)meshopt_quantizeHalf(bounds.center[0])
                                         | ((uint32_t)meshopt_quantizeHalf(bounds.center[1]) << 16);
            gpuMeshlet.m_CenterRadius[1] = (uint32_t)meshopt_quantizeHalf(bounds.center[2])
                                         | ((uint32_t)meshopt_quantizeHalf(bounds.radius) << 16);

            const uint32_t axisX  = (uint32_t)((bounds.cone_axis[0] + 1.0f) * 0.5f * UINT8_MAX);
            const uint32_t axisY  = (uint32_t)((bounds.cone_axis[1] + 1.0f) * 0.5f * UINT8_MAX);
            const uint32_t axisZ  = (uint32_t)((bounds.cone_axis[2] + 1.0f) * 0.5f * UINT8_MAX);
            const uint32_t cutoff = (uint32_t)(bounds.cone_cutoff_s8 * 2);
            gpuMeshlet.m_ConeAxisAndCutoff = axisX | (axisY << 8) | (axisZ << 16) | (cutoff << 24);

            for (uint32_t vi = 0; vi < m.vertex_count; ++vi)
                cmd.m_MeshletVertices.push_back(lVerts[m.vertex_offset + vi]);

            for (uint32_t ti = 0; ti < m.triangle_count; ++ti)
            {
                uint32_t i0 = lTris[m.triangle_offset + ti * 3 + 0];
                uint32_t i1 = lTris[m.triangle_offset + ti * 3 + 1];
                uint32_t i2 = lTris[m.triangle_offset + ti * 3 + 2];
                cmd.m_MeshletTriangles.push_back(i0 | (i1 << 8) | (i2 << 16));
            }

            cmd.m_Meshlets.push_back(gpuMeshlet);
        }
    }

    return true;
}

PendingLoadID AsyncMeshQueue::EnqueueLoad(std::string gltfPath,
                                           std::vector<std::pair<int, int>> affectedPrimitives,
                                           int glTFMeshIdx,
                                           int glTFPrimIdx,
                                           int /*materialOffset*/,
                                           int /*textureOffset*/,
                                           OnLoadedCallback callback)
{
    PendingLoadID id = m_NextID.fetch_add(1, std::memory_order_relaxed);

    EnqueueTask([this, id, path = std::move(gltfPath),
                 affected = std::move(affectedPrimitives),
                 glTFMeshIdx, glTFPrimIdx,
                 cb = std::move(callback)]() mutable
    {
        MeshUpdateCommand cmd;
        cmd.m_LoadID             = id;
        cmd.m_AffectedPrimitives = std::move(affected);

        {
            std::lock_guard<std::mutex> lk(m_CancelMutex);
            if (m_CancelledIDs.count(id))
            {
                m_CancelledIDs.erase(id);
                cmd.m_bCancelled = true;
                cb(std::move(cmd));
                return;
            }
        }

        // Parse the glTF file
        cgltf_options options{};
        cgltf_data*   data = nullptr;
        cgltf_result  result = cgltf_parse_file(&options, path.c_str(), &data);

        if (result != cgltf_result_success)
        {
            SDL_Log("[AsyncMeshQueue] Failed to parse '%s': %s", path.c_str(),
                    SceneLoader::cgltf_result_tostring(result));
            cmd.m_bCancelled = true;
            cb(std::move(cmd));
            return;
        }

        result = cgltf_load_buffers(&options, data, path.c_str());
        if (result != cgltf_result_success)
        {
            SDL_Log("[AsyncMeshQueue] Failed to load buffers '%s'", path.c_str());
            cgltf_free(data);
            cmd.m_bCancelled = true;
            cb(std::move(cmd));
            return;
        }

        result = SceneLoader::decompressMeshopt(data);
        if (result != cgltf_result_success)
        {
            SDL_Log("[AsyncMeshQueue] meshopt decompression failed '%s'", path.c_str());
            cgltf_free(data);
            cmd.m_bCancelled = true;
            cb(std::move(cmd));
            return;
        }

        // Process the specific primitive identified by glTFMeshIdx / glTFPrimIdx.
        bool bProcessed = false;
        if (glTFMeshIdx >= 0 && glTFMeshIdx < (int)data->meshes_count)
        {
            cgltf_mesh& gltfMesh = data->meshes[glTFMeshIdx];
            if (glTFPrimIdx >= 0 && glTFPrimIdx < (int)gltfMesh.primitives_count)
                bProcessed = ProcessSinglePrimitive(gltfMesh.primitives[glTFPrimIdx], cmd);
        }

        cgltf_free(data);

        if (!bProcessed)
        {
            SDL_Log("[AsyncMeshQueue] No processable primitive in '%s'", path.c_str());
            cmd.m_bCancelled = true;
        }

        cb(std::move(cmd));
    });

    return id;
}

void AsyncMeshQueue::CancelLoad(PendingLoadID id)
{
    std::lock_guard<std::mutex> lk(m_CancelMutex);
    m_CancelledIDs.insert(id);
}
