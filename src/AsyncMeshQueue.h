#pragma once

#include "AsyncQueueBase.h"
#include "PendingInstanceUpdate.h"

// ─── AsyncMeshQueue ──────────────────────────────────────────────────────────
// Loads and processes mesh data (vertex quantization, LOD generation, meshlet
// building) on a background thread — CPU only, no GPU work.
//
// The caller-provided callback is invoked on the background thread with a
// MeshUpdateCommand containing fully-processed, ready-to-upload data.
// Scene::ApplyPendingUpdates() should collect these commands and perform the
// GPU buffer re-creation on the main thread.
class AsyncMeshQueue : public AsyncQueueBase
{
public:
    using OnLoadedCallback = std::function<void(MeshUpdateCommand)>;

    // Enqueue a glTF file to be loaded and processed asynchronously.
    // gltfPath:           absolute path to the .glb / .gltf file.
    // affectedPrimitives: list of (meshIndex, primitiveIndex) pairs in the
    //                     Scene that should be updated once data is ready.
    // glTFMeshIdx:        index of the mesh within the glTF file to process.
    // glTFPrimIdx:        index of the primitive within that mesh to process.
    // materialOffset:     scene-global material offset for this model.
    // textureOffset:      scene-global texture offset for this model.
    PendingLoadID EnqueueLoad(std::string gltfPath,
                               std::vector<std::pair<int, int>> affectedPrimitives,
                               int glTFMeshIdx,
                               int glTFPrimIdx,
                               int materialOffset,
                               int textureOffset,
                               OnLoadedCallback callback);

    // Request cancellation of a pending load (same semantics as AsyncTextureQueue).
    void CancelLoad(PendingLoadID id);

private:
    std::atomic<PendingLoadID> m_NextID{ 1 };
    std::mutex                 m_CancelMutex;
    std::unordered_set<PendingLoadID> m_CancelledIDs;
};
