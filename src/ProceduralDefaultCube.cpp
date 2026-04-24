#include "pch.h"
#include "ProceduralDefaultCube.h"
#include "meshoptimizer.h"

// ─── Cube face data ───────────────────────────────────────────────────────────
// Unit cube centred at origin, LH coordinate space (Y-up, Z-forward).
// Winding: CCW from the outside (same as the post-RH->LH-converted scene geometry).
// Each face has 4 vertices and 2 triangles (indices 0,1,2 and 0,2,3).

struct RawFace
{
    float pos[4][3];
    float normal[3];
    float tangent[3];
    float tangentW;
    float uv[4][2];
};

// clang-format off
static const RawFace g_CubeFaces[6] =
{
    // +X face (right, normal = (1,0,0), tangent U-dir = (0,0,-1))
    {
        {{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f}},
        {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f,-1.0f}, 1.0f,
        {{0.0f,1.0f},{1.0f,1.0f},{1.0f,0.0f},{0.0f,0.0f}}
    },
    // -X face (left, normal = (-1,0,0), tangent U-dir = (0,0,1))
    {
        {{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f}},
        {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 1.0f,
        {{0.0f,1.0f},{1.0f,1.0f},{1.0f,0.0f},{0.0f,0.0f}}
    },
    // +Y face (top, normal = (0,1,0), tangent U-dir = (1,0,0))
    {
        {{-0.5f, 0.5f,-0.5f},{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f}},
        {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1.0f,
        {{0.0f,0.0f},{0.0f,1.0f},{1.0f,1.0f},{1.0f,0.0f}}
    },
    // -Y face (bottom, normal = (0,-1,0), tangent U-dir = (1,0,0))
    {
        {{-0.5f,-0.5f, 0.5f},{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f}},
        {0.0f,-1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1.0f,
        {{0.0f,0.0f},{0.0f,1.0f},{1.0f,1.0f},{1.0f,0.0f}}
    },
    // +Z face (front, normal = (0,0,1), tangent U-dir = (1,0,0))
    {
        {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}},
        {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 1.0f,
        {{0.0f,1.0f},{1.0f,1.0f},{1.0f,0.0f},{0.0f,0.0f}}
    },
    // -Z face (back, normal = (0,0,-1), tangent U-dir = (-1,0,0))
    {
        {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}},
        {0.0f, 0.0f,-1.0f}, {-1.0f, 0.0f, 0.0f}, 1.0f,
        {{0.0f,1.0f},{1.0f,1.0f},{1.0f,0.0f},{0.0f,0.0f}}
    },
};
// clang-format on

static srrhi::VertexQuantized QuantizeVertex(const float pos[3], const float normal[3],
                                              const float uv[2],  const float tangent[3],
                                              float tangentW)
{
    srrhi::VertexQuantized vq{};
    vq.m_Pos = DirectX::XMFLOAT3(pos[0], pos[1], pos[2]);

    vq.m_Normal  = (uint32_t)(meshopt_quantizeSnorm(normal[0], 10) + 511)
                 | ((uint32_t)(meshopt_quantizeSnorm(normal[1], 10) + 511) << 10)
                 | ((uint32_t)(meshopt_quantizeSnorm(normal[2], 10) + 511) << 20);
    vq.m_Normal |= (tangentW >= 0.0f ? 0u : 1u) << 30;

    vq.m_Uv = (uint32_t)meshopt_quantizeHalf(uv[0])
            | ((uint32_t)meshopt_quantizeHalf(uv[1]) << 16);

    float tx = tangent[0], ty = tangent[1], tz = tangent[2];
    float tsum = fabsf(tx) + fabsf(ty) + fabsf(tz);
    if (tsum > 1e-6f)
    {
        float tu = tz >= 0.0f ? tx / tsum : (1.0f - fabsf(ty / tsum)) * (tx >= 0.0f ? 1.0f : -1.0f);
        float tv = tz >= 0.0f ? ty / tsum : (1.0f - fabsf(tx / tsum)) * (ty >= 0.0f ? 1.0f : -1.0f);
        vq.m_Tangent = (uint32_t)(meshopt_quantizeSnorm(tu, 8) + 127)
                     | ((uint32_t)(meshopt_quantizeSnorm(tv, 8) + 127) << 8);
    }

    return vq;
}

ProceduralCubeData GenerateDefaultCube()
{
    ProceduralCubeData out;
    out.m_Vertices.reserve(24);
    out.m_Indices.reserve(36);

    for (int f = 0; f < 6; ++f)
    {
        const RawFace& face = g_CubeFaces[f];
        const uint32_t base = (uint32_t)out.m_Vertices.size();

        for (int v = 0; v < 4; ++v)
        {
            out.m_Vertices.push_back(QuantizeVertex(
                face.pos[v], face.normal, face.uv[v], face.tangent, face.tangentW));
        }

        // Two CCW triangles: (base+0, base+1, base+2) and (base+0, base+2, base+3)
        out.m_Indices.push_back(base + 0);
        out.m_Indices.push_back(base + 1);
        out.m_Indices.push_back(base + 2);
        out.m_Indices.push_back(base + 0);
        out.m_Indices.push_back(base + 2);
        out.m_Indices.push_back(base + 3);
    }

    // ── Build meshlet data ────────────────────────────────────────────────────
    // All 12 triangles fit comfortably in one meshlet
    // (kMaxMeshletVertices=64, kMaxMeshletTriangles=96)
    const size_t maxVerts     = srrhi::CommonConsts::kMaxMeshletVertices;
    const size_t maxTriangles = srrhi::CommonConsts::kMaxMeshletTriangles;
    const float  coneWeight   = 0.25f;

    // Local 0-based indices for meshopt (no global vertex offset applied here)
    std::vector<uint32_t> localIndices(out.m_Indices);

    size_t maxMeshlets = meshopt_buildMeshletsBound(localIndices.size(), maxVerts, maxTriangles);
    std::vector<meshopt_Meshlet> rawMeshlets(maxMeshlets);
    std::vector<unsigned int>    meshletVerts(maxMeshlets * maxVerts);
    std::vector<unsigned char>   meshletTris(maxMeshlets * maxTriangles * 3);

    size_t meshletCount = meshopt_buildMeshlets(
        rawMeshlets.data(), meshletVerts.data(), meshletTris.data(),
        localIndices.data(), localIndices.size(),
        &out.m_Vertices[0].m_Pos.x, out.m_Vertices.size(), sizeof(srrhi::VertexQuantized),
        maxVerts, maxTriangles, coneWeight);

    rawMeshlets.resize(meshletCount);

    for (size_t i = 0; i < meshletCount; ++i)
    {
        const meshopt_Meshlet& m = rawMeshlets[i];
        meshopt_optimizeMeshlet(&meshletVerts[m.vertex_offset], &meshletTris[m.triangle_offset],
                                m.triangle_count, m.vertex_count);

        meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &meshletVerts[m.vertex_offset], &meshletTris[m.triangle_offset],
            m.triangle_count, &out.m_Vertices[0].m_Pos.x,
            out.m_Vertices.size(), sizeof(srrhi::VertexQuantized));

        srrhi::Meshlet gpuMeshlet{};
        gpuMeshlet.m_VertexOffset   = (uint32_t)out.m_MeshletVertices.size();
        gpuMeshlet.m_TriangleOffset = (uint32_t)out.m_MeshletTriangles.size();
        gpuMeshlet.m_VertexCount    = m.vertex_count;
        gpuMeshlet.m_TriangleCount  = m.triangle_count;

        gpuMeshlet.m_CenterRadius[0] = (uint32_t)meshopt_quantizeHalf(bounds.center[0])
                                     | ((uint32_t)meshopt_quantizeHalf(bounds.center[1]) << 16);
        gpuMeshlet.m_CenterRadius[1] = (uint32_t)meshopt_quantizeHalf(bounds.center[2])
                                     | ((uint32_t)meshopt_quantizeHalf(bounds.radius) << 16);

        const uint32_t axisX    = (uint32_t)((bounds.cone_axis[0] + 1.0f) * 0.5f * UINT8_MAX);
        const uint32_t axisY    = (uint32_t)((bounds.cone_axis[1] + 1.0f) * 0.5f * UINT8_MAX);
        const uint32_t axisZ    = (uint32_t)((bounds.cone_axis[2] + 1.0f) * 0.5f * UINT8_MAX);
        const uint32_t cutoff   = (uint32_t)(bounds.cone_cutoff_s8 * 2);
        gpuMeshlet.m_ConeAxisAndCutoff = axisX | (axisY << 8) | (axisZ << 16) | (cutoff << 24);

        for (uint32_t v = 0; v < m.vertex_count; ++v)
            out.m_MeshletVertices.push_back(meshletVerts[m.vertex_offset + v]);

        for (uint32_t t = 0; t < m.triangle_count; ++t)
        {
            uint32_t i0 = meshletTris[m.triangle_offset + t * 3 + 0];
            uint32_t i1 = meshletTris[m.triangle_offset + t * 3 + 1];
            uint32_t i2 = meshletTris[m.triangle_offset + t * 3 + 2];
            out.m_MeshletTriangles.push_back(i0 | (i1 << 8) | (i2 << 16));
        }

        out.m_Meshlets.push_back(gpuMeshlet);
    }

    // ── MeshData (zero-based local offsets) ───────────────────────────────────
    out.m_MeshData = {};
    out.m_MeshData.m_LODCount         = 1;
    out.m_MeshData.m_IndexOffsets[0]  = 0;
    out.m_MeshData.m_IndexCounts[0]   = (uint32_t)out.m_Indices.size();
    out.m_MeshData.m_MeshletOffsets[0] = 0;
    out.m_MeshData.m_MeshletCounts[0] = (uint32_t)out.m_Meshlets.size();
    out.m_MeshData.m_LODErrors[0]     = 0.0f;

    return out;
}
