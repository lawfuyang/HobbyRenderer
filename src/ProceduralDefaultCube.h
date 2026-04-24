#pragma once

#include "shaders/srrhi/cpp/Common.h"
#include "shaders/srrhi/cpp/Mesh.h"

// Data produced by GenerateDefaultCube(). All buffer offsets are zero-based
// (local to this cube) and must be adjusted by the caller before merging into
// the scene's global vertex/index/meshlet buffers.
struct ProceduralCubeData
{
    std::vector<srrhi::VertexQuantized> m_Vertices;        // 24 verts (4 per face x 6 faces)
    std::vector<uint32_t>              m_Indices;          // 36 indices (6 per face x 6 faces), global
    srrhi::MeshData                    m_MeshData;         // LODCount=1; offsets are zero-based
    std::vector<srrhi::Meshlet>        m_Meshlets;         // 1 meshlet
    std::vector<uint32_t>              m_MeshletVertices;  // 24 local vertex indices
    std::vector<uint32_t>              m_MeshletTriangles; // 12 packed triangle entries
};

// Generate a unit cube (side length 1.0) centered at the origin in LH coordinate
// space, using the same vertex quantization convention as SceneLoader::ProcessMeshes.
ProceduralCubeData GenerateDefaultCube();
