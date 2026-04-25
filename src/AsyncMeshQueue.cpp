#include "pch.h"
#include "AsyncMeshQueue.h"
#include "SceneLoader.h"
#include "Utilities.h"
#include "meshoptimizer.h"
#include "cgltf.h"

// ── Helpers for the mmap fast path ──────────────────────────────────────────

static size_t ComponentByteSize(int componentType)
{
    switch (componentType)
    {
    case 1: case 2: return 1;  // BYTE, UBYTE
    case 3: case 4: return 2;  // SHORT, USHORT
    case 5: case 6: return 4;  // UINT, FLOAT
    default:        return 1;
    }
}

// Read up to maxComponents floats from element elemIdx of the given accessor.
// bufData points to the start of the binary buffer (after binDataOffset has been applied).
static void ReadAccessorFloat(const uint8_t* bufData, const PrimAccessorInfo& acc,
                               size_t elemIdx, float* out, int maxComponents)
{
    const size_t compSize = ComponentByteSize(acc.componentType);
    const size_t elemSize = (size_t)acc.numComponents * compSize;
    const size_t stride   = acc.byteStride != 0 ? (size_t)acc.byteStride : elemSize;
    const uint8_t* ptr    = bufData + acc.byteOffset + elemIdx * stride;

    const int n = std::min(acc.numComponents, maxComponents);
    for (int c = 0; c < n; ++c)
    {
        const uint8_t* cp = ptr + c * compSize;
        switch (acc.componentType)
        {
        case 1: out[c] = acc.normalized ? std::max(*reinterpret_cast<const int8_t*>(cp) / 127.0f, -1.0f)
                                        : (float)*reinterpret_cast<const int8_t*>(cp); break;
        case 2: out[c] = acc.normalized ? *cp / 255.0f : (float)*cp; break;
        case 3: out[c] = acc.normalized ? std::max(*reinterpret_cast<const int16_t*>(cp) / 32767.0f, -1.0f)
                                        : (float)*reinterpret_cast<const int16_t*>(cp); break;
        case 4: out[c] = acc.normalized ? *reinterpret_cast<const uint16_t*>(cp) / 65535.0f
                                        : (float)*reinterpret_cast<const uint16_t*>(cp); break;
        case 5: out[c] = (float)*reinterpret_cast<const uint32_t*>(cp); break;
        case 6: out[c] = *reinterpret_cast<const float*>(cp); break;
        default: out[c] = 0.0f; break;
        }
    }
    for (int c = n; c < maxComponents; ++c) out[c] = 0.0f;
}

static uint32_t ReadIndex(const uint8_t* bufData, const PrimAccessorInfo& acc, size_t elemIdx)
{
    const size_t stride = acc.byteStride != 0 ? (size_t)acc.byteStride : ComponentByteSize(acc.componentType);
    const uint8_t* ptr  = bufData + acc.byteOffset + elemIdx * stride;
    switch (acc.componentType)
    {
    case 4: return *reinterpret_cast<const uint16_t*>(ptr);  // USHORT
    case 5: return *reinterpret_cast<const uint32_t*>(ptr);  // UINT
    default: return *ptr;                                      // UBYTE
    }
}

// Same as ProcessSinglePrimitive but reads vertex/index data from a raw memory buffer
// using the accessor metadata stored in PendingAsyncMeshInfo.
static void ProcessSinglePrimitiveFromMapped(const uint8_t* bufData, const PendingAsyncMeshInfo& info,
                                              MeshUpdateCommand& cmd)
{
    SDL_assert(info.posAccessor.present && "Mmap fast path requires position accessor to be present");

    const uint32_t vertCount = info.posAccessor.count;
    std::vector<srrhi::Vertex> rawVertices(vertCount);

    for (uint32_t v = 0; v < vertCount; ++v)
    {
        srrhi::Vertex vx{};

        float pos[4] = {};
        ReadAccessorFloat(bufData, info.posAccessor, v, pos, 3);
        vx.m_Pos = { pos[0], pos[1], -pos[2] };  // RH -> LH: negate Z

        float nrm[3] = {};
        if (info.normAccessor.present)
            ReadAccessorFloat(bufData, info.normAccessor, v, nrm, 3);
        vx.m_Normal = { nrm[0], nrm[1], -nrm[2] };

        float uv[2] = {};
        if (info.uvAccessor.present)
            ReadAccessorFloat(bufData, info.uvAccessor, v, uv, 2);
        vx.m_Uv = { uv[0], uv[1] };

        float tang[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (info.tangAccessor.present)
            ReadAccessorFloat(bufData, info.tangAccessor, v, tang, 4);
        vx.m_Tangent = { tang[0], tang[1], -tang[2], -tang[3] };  // RH -> LH

        rawVertices[v] = vx;
    }

    std::vector<uint32_t> rawIndices;
    if (info.indexAccessor.present)
    {
        rawIndices.resize(info.indexAccessor.count);
        for (size_t k = 0; k < info.indexAccessor.count; ++k)
            rawIndices[k] = ReadIndex(bufData, info.indexAccessor, k);
        for (size_t k = 0; k + 2 < info.indexAccessor.count; k += 3)
            std::swap(rawIndices[k + 1], rawIndices[k + 2]);  // restore CCW winding after Z-negate
    }
    else
    {
        rawIndices.resize(vertCount);
        for (uint32_t k = 0; k < vertCount; ++k) rawIndices[k] = k;
        for (uint32_t k = 0; k + 2 < vertCount; k += 3)
            std::swap(rawIndices[k + 1], rawIndices[k + 2]);
    }

    // ── Vertex remapping + optimisation ────────────────────────────────────
    std::vector<uint32_t> remap(rawIndices.size());
    size_t uniqueVerts = meshopt_generateVertexRemap(remap.data(), rawIndices.data(), rawIndices.size(),
                                                      rawVertices.data(), rawVertices.size(), sizeof(srrhi::Vertex));

    std::vector<srrhi::Vertex> optVerts(uniqueVerts);
    std::vector<uint32_t>      localIdx(rawIndices.size());
    meshopt_remapVertexBuffer(optVerts.data(), rawVertices.data(), rawVertices.size(), sizeof(srrhi::Vertex), remap.data());
    meshopt_remapIndexBuffer(localIdx.data(), rawIndices.data(), rawIndices.size(), remap.data());
    meshopt_optimizeVertexCache(localIdx.data(), localIdx.data(), localIdx.size(), uniqueVerts);
    meshopt_optimizeVertexFetch(optVerts.data(), localIdx.data(), localIdx.size(), optVerts.data(), uniqueVerts, sizeof(srrhi::Vertex));

    // Quantize
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

    // Compute local bounding sphere from quantized vertex positions on the background
    // thread so ApplyPendingUpdates can use it directly without re-scanning vertices.
    {
        Sphere primSphere;
        Sphere::CreateFromPoints(primSphere,
            cmd.m_Vertices.size(),
            &cmd.m_Vertices[0].m_Pos, sizeof(srrhi::VertexQuantized));
        cmd.m_LocalSphereCenter = Vector3(primSphere.Center.x, primSphere.Center.y, primSphere.Center.z);
        cmd.m_LocalSphereRadius = primSphere.Radius;
    }

    // ── LOD generation + meshlet building (identical to ProcessSinglePrimitive) ──
    const size_t   maxVerts     = srrhi::CommonConsts::kMaxMeshletVertices;
    const size_t   maxTriangles = srrhi::CommonConsts::kMaxMeshletTriangles;
    const float    coneWeight   = 0.25f;
    const uint32_t kIndexLimitForLOD = 1024;
    const float    kMaxError         = 1e-1f;
    const float    kMinReduction     = 0.85f;
    const float    attribute_weights[3] = { 1.0f, 1.0f, 1.0f };
    const float    simplifyScale = meshopt_simplifyScale(&optVerts[0].m_Pos.x, uniqueVerts, sizeof(srrhi::Vertex));
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
            lodError         = accumulatedError;
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
        cmd.m_MeshData.m_LODCount            = lod + 1;

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
}

PendingLoadID AsyncMeshQueue::EnqueueLoad(PendingAsyncMeshInfo info, OnLoadedCallback callback)
{
    PendingLoadID id = m_NextID.fetch_add(1, std::memory_order_relaxed);

    EnqueueTask([this, id, info = std::move(info), cb = std::move(callback)]() mutable
    {
        MeshUpdateCommand cmd;
        cmd.m_LoadID             = id;
        cmd.m_AffectedPrimitives = { { info.sceneMeshIdx, info.scenePrimIdx } };

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

        // ── Fast path: mmap the binary buffer and read accessor data directly ──
        if (!info.binFilePath.empty() && info.posAccessor.present)
        {
            MemoryMappedDataReader mapped(info.binFilePath);
            if (mapped.IsValid())
            {
                mapped.SetOffset(static_cast<size_t>(info.binDataOffset));
                const uint8_t* bufData = static_cast<const uint8_t*>(mapped.GetData());
                ProcessSinglePrimitiveFromMapped(bufData, info, cmd);
            }
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
