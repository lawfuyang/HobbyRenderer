#pragma once

#include "Scene.h"
#include "Renderer.h"

#include "cgltf.h"

// Converts a Scene::Material to a GPU-ready MaterialConstants struct.
// Declared here so both SceneLoader and Renderer can use it without duplication.
srrhi::MaterialConstants MaterialConstantsFromMaterial(const Scene::Material& mat, const std::vector<Scene::Texture>& textures);

class SceneLoader
{
public:
    struct SceneOffsets {
        int materialOffset = 0;
        int textureOffset = 0;
        int meshOffset = 0;
        int nodeOffset = 0;
        int cameraOffset = 0;
        int lightOffset = 0;
    };

    // Main GLTF loading function
    static bool LoadGLTFScene(Scene& scene, const std::string& scenePath, std::vector<srrhi::VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices, bool bFromJSONScene);
    // Load a glTF scene from an in-memory JSON string (embedded data URIs are supported;
    // external file references are not resolved).  sceneDir is used only for texture
    // path resolution — pass an empty path when all data is embedded.
    static bool LoadGLTFSceneFromMemory(Scene& scene, const char* jsonData, size_t jsonSize, const std::filesystem::path& sceneDir, std::vector<srrhi::VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices);
    static bool LoadJSONScene(Scene& scene, const std::string& scenePath, std::vector<srrhi::VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices);

    // Helper functions for processing GLTF data
    static void ProcessMaterialsAndImages(const cgltf_data* data, Scene& scene, const std::filesystem::path& sceneDir, const SceneOffsets& offsets);
    static void ProcessCameras(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);
    static void ProcessLights(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);
    static void ProcessAnimations(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);
    static void ProcessMeshes(const cgltf_data* data, Scene& scene, std::vector<srrhi::VertexQuantized>& outVerticesQuantized, std::vector<uint32_t>& outIndices, const SceneOffsets& offsets, const std::string& gltfFilePath);
    static void ProcessNodesAndHierarchy(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);

    // Texture and GPU buffer functions
    static void LoadTexturesFromImages(Scene& scene, const std::filesystem::path& sceneDir);
    static void UpdateMaterialsAndCreateConstants(Scene& scene);
    static void CreateAndUploadGpuBuffers(Scene& scene, const std::vector<srrhi::VertexQuantized>& allVerticesQuantized, const std::vector<uint32_t>& allIndices);
    static void CreateAndUploadLightBuffer(Scene& scene);

    // Decompress any meshopt-compressed buffer views in a parsed cgltf_data.
    // Public so AsyncMeshQueue can call it from background threads.
    static cgltf_result decompressMeshopt(cgltf_data* data);
    static const char* cgltf_result_tostring(cgltf_result result);

private:
    // Shared post-parse pipeline used by both LoadGLTFScene and LoadGLTFSceneFromMemory.
    // Takes ownership of `data` (always calls cgltf_free before returning).
    // gltfFilePath: the actual .glb/.gltf file path for async mesh loading;
    //               pass an empty string for in-memory / test invocations.
    static bool ProcessParsedGLTF(cgltf_data* data, Scene& scene, const std::string& bufferBasePath, const std::filesystem::path& sceneDir, std::vector<srrhi::VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices, bool ensureDirectionalLight, const std::string& gltfFilePath);

    // Utility functions
    static void SetTextureAndSampler(const cgltf_texture* tex, int& textureIndex, const cgltf_data* data, int textureOffset);
    static void ComputeWorldTransforms(Scene& scene, int nodeIndex, const Matrix& parent);
};
