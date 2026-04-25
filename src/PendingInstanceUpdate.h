#pragma once

#include "shaders/srrhi/cpp/Mesh.h"
#include "Utilities.h"

#include <nvrhi/nvrhi.h>

using PendingLoadID = uint64_t;
static constexpr PendingLoadID INVALID_PENDING_LOAD_ID = 0;

// ─── TextureUpdateCommand ────────────────────────────────────────────────────
// Produced by AsyncTextureQueue on the background thread; consumed by
// Scene::ApplyPendingUpdates() on the main thread.
struct TextureUpdateCommand
{
    PendingLoadID                           m_LoadID       = INVALID_PENDING_LOAD_ID;
    uint32_t                                m_TextureIndex = 0;  // into Scene::m_Textures
    nvrhi::TextureDesc                      m_Desc;
    std::unique_ptr<MemoryMappedDataReader> m_Data;
    bool                                    m_bCancelled   = false;
};

// ─── PrimAccessorInfo ────────────────────────────────────────────────────────
// Describes where one glTF accessor's data lives inside a binary buffer file.
// Offsets are relative to the start of buffer[0]'s data region in the file
// (after any GLB binary-chunk header).
struct PrimAccessorInfo
{
    uint64_t byteOffset    = 0; // byte offset from start of buffer data
    uint32_t count         = 0; // number of elements
    uint32_t byteStride    = 0; // 0 = tightly packed
    int      componentType = 0; // cgltf_component_type enum: 1=BYTE, 2=UBYTE, 3=SHORT, 4=USHORT, 5=UINT, 6=FLOAT
    int      numComponents = 0; // 1, 2, 3, or 4 (derived from cgltf_type)
    bool     normalized    = false;
    bool     present       = false;
};

// ─── PendingAsyncMeshInfo ─────────────────────────────────────────────────────
// Produced by SceneLoader::ProcessMeshes for each primitive that needs async
// geometry loading.  Consumed by AsyncMeshQueue::EnqueueLoad.
//
// Fast path  : binFilePath is set; AsyncMeshQueue uses MemoryMappedDataReader to
//              read exactly the accessor byte ranges it needs (one primitive).
// Fallback   : binFilePath is empty; AsyncMeshQueue re-parses the full glTF file
//              (used for meshopt-compressed or data-URI-embedded buffers).
struct PendingAsyncMeshInfo
{
    std::string gltfPath;          // full glTF/GLB path — used as fallback if binFilePath is empty
    std::string binFilePath;       // path to binary data file (.bin or .glb)
    uint64_t    binDataOffset = 0; // byte offset within binFilePath where buffer[0] starts

    int sceneMeshIdx   = 0;
    int scenePrimIdx   = 0;
    int glTFMeshIdx    = 0;
    int glTFPrimIdx    = 0;
    int materialOffset = 0;
    int textureOffset  = 0;

    PrimAccessorInfo posAccessor;
    PrimAccessorInfo normAccessor;
    PrimAccessorInfo uvAccessor;
    PrimAccessorInfo tangAccessor;
    PrimAccessorInfo indexAccessor;
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

    // Local bounding sphere computed from vertex positions on the background thread
    // by ProcessSinglePrimitiveFromMapped.  m_LocalSphereRadius < 0 means not set.
    Vector3 m_LocalSphereCenter{};
    float   m_LocalSphereRadius = -1.0f;

    bool m_bCancelled = false;
};
