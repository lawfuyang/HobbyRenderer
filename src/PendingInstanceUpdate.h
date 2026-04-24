#pragma once

#include "shaders/srrhi/cpp/Mesh.h"
#include "TextureLoader.h"

using PendingLoadID = uint64_t;
static constexpr PendingLoadID INVALID_PENDING_LOAD_ID = 0;

// ─── TextureUpdateCommand ────────────────────────────────────────────────────
// Produced by AsyncTextureQueue on the background thread; consumed by
// Scene::ApplyPendingUpdates() on the main thread.
struct TextureUpdateCommand
{
    PendingLoadID                       m_LoadID       = INVALID_PENDING_LOAD_ID;
    uint32_t                            m_TextureIndex = 0;  // into Scene::m_Textures
    nvrhi::TextureDesc                  m_Desc;
    std::unique_ptr<ITextureDataReader> m_Data;
    bool                                m_bCancelled   = false;
};

// ─── MeshUpdateCommand ───────────────────────────────────────────────────────
// Produced by AsyncMeshQueue on the background thread; consumed by
// Scene::ApplyPendingUpdates() on the main thread.
// All mesh/meshlet data is in local (zero-based) coordinate space.
// The caller must add globalVertexOffset / globalIndexOffset before merging.
struct MeshUpdateCommand
{
    PendingLoadID                       m_LoadID = INVALID_PENDING_LOAD_ID;

    // Processed geometry output (local offsets, 0-based)
    std::vector<srrhi::VertexQuantized> m_Vertices;
    std::vector<uint32_t>               m_Indices;
    srrhi::MeshData                     m_MeshData{};
    std::vector<srrhi::Meshlet>         m_Meshlets;
    std::vector<uint32_t>               m_MeshletVertices;
    std::vector<uint32_t>               m_MeshletTriangles;

    // Which scene (meshIndex, primitiveIndex) pairs should be patched to point
    // at the newly uploaded mesh data once it is merged into the GPU buffers.
    std::vector<std::pair<int, int>> m_AffectedPrimitives;

    bool m_bCancelled = false;
};
