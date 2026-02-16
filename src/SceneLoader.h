#pragma once

#include "Scene.h"
#include "Renderer.h"

#include "cgltf.h"

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
    static bool LoadGLTFScene(Scene& scene, const std::string& scenePath, std::vector<VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices);
    static bool LoadJSONScene(Scene& scene, const std::string& scenePath, std::vector<VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices);
    static void ApplyEnvironmentLights(Scene& scene);

    // Helper functions for processing GLTF data
    static void ProcessMaterialsAndImages(const cgltf_data* data, Scene& scene, const std::filesystem::path& sceneDir, const SceneOffsets& offsets);
    static void ProcessCameras(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);
    static void ProcessLights(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);
    static void ProcessAnimations(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);
    static void ProcessMeshes(const cgltf_data* data, Scene& scene, std::vector<VertexQuantized>& outVerticesQuantized, std::vector<uint32_t>& outIndices, const SceneOffsets& offsets);
    static void ProcessNodesAndHierarchy(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets);

    // Texture and GPU buffer functions
    static void LoadTexturesFromImages(Scene& scene, const std::filesystem::path& sceneDir, Renderer* renderer);
    static void UpdateMaterialsAndCreateConstants(Scene& scene, Renderer* renderer);
    static void CreateAndUploadGpuBuffers(Scene& scene, Renderer* renderer, const std::vector<VertexQuantized>& allVerticesQuantized, const std::vector<uint32_t>& allIndices);
    static void CreateAndUploadLightBuffer(Scene& scene, Renderer* renderer);

private:
    // Utility functions
    static void SetTextureAndSampler(const cgltf_texture* tex, int& textureIndex, const cgltf_data* data, int textureOffset);
    static void ComputeWorldTransforms(Scene& scene, int nodeIndex, const Matrix& parent);
    static cgltf_result decompressMeshopt(cgltf_data* data);
    static const char* cgltf_result_tostring(cgltf_result result);
};
