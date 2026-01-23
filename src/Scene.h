#pragma once

#include "pch.h"
#include "Camera.h"

// Include shared structs
#include "shaders/ShaderShared.h"

// Minimal scene representation for glTF meshes/nodes/materials/textures
class Scene
{
public:
    struct Primitive
    {
        uint32_t m_VertexOffset = 0;
        uint32_t m_VertexCount = 0;
        uint32_t m_IndexOffset = 0;
        uint32_t m_IndexCount = 0;
        uint32_t m_MeshletOffset = 0;
        uint32_t m_MeshletCount = 0;
        int m_MaterialIndex = -1;
        uint32_t m_MeshDataIndex = 0;
    };

    struct Mesh
    {
        std::vector<Primitive> m_Primitives;
        // local sphere
        Vector3 m_Center{};
        float m_Radius{};
    };

    struct Node
    {
        std::string m_Name;
        int m_MeshIndex = -1;
        int m_Parent = -1;
        std::vector<int> m_Children;
        // local transform
        Matrix m_LocalTransform{};
        // world transform (computed)
        Matrix m_WorldTransform{};
        // world sphere
        Vector3 m_Center{};
        float m_Radius{};
        // Camera/Light indices
        int m_CameraIndex = -1;
        int m_LightIndex = -1;
    };

    struct Material
    {
        std::string m_Name;
        Vector4 m_BaseColorFactor = Vector4{1.0f, 1.0f, 1.0f, 1.0f};
        int m_BaseColorTexture = -1; // index into m_Textures
        int m_NormalTexture = -1;
        int m_MetallicRoughnessTexture = -1;
        float m_RoughnessFactor = 1.0f;
        float m_MetallicFactor = 0.0f;
        // Bindless indices
        uint32_t m_AlbedoTextureIndex = 1; // DEFAULT_TEXTURE_WHITE
        uint32_t m_NormalTextureIndex = 3; // DEFAULT_TEXTURE_NORMAL
        uint32_t m_RoughnessMetallicTextureIndex = 4; // DEFAULT_TEXTURE_PBR
    };

    struct Texture
    {
        std::string m_Uri;
        nvrhi::TextureHandle m_Handle;
        uint32_t m_BindlessIndex = UINT32_MAX;
        // Sampler preference mapped from glTF: 0 = Clamp, 1 = Wrap
        enum SamplerType
        {
            Clamp = 0,
            Wrap = 1
        };
        SamplerType m_Sampler = Wrap;
    };

    struct Camera
    {
        std::string m_Name;
        ProjectionParams m_Projection;
        // Associated node index for transform
        int m_NodeIndex = -1;
    };

    struct Light
    {
        std::string m_Name;
        enum Type { Directional, Point, Spot };
        Type m_Type;
        Vector3 m_Color = Vector3{1.0f, 1.0f, 1.0f};
        float m_Intensity = 1.0f;
        float m_Range = 0.0f; // 0 = infinite
        // Spot
        float m_SpotInnerConeAngle = 0.0f;
        float m_SpotOuterConeAngle = DirectX::XM_PIDIV4; // 45deg
        // Associated node index for transform
        int m_NodeIndex = -1;
    };

    struct DirectionalLight
    {
        float yaw       = 0.0f;
        float pitch     = -DirectX::XM_PI / 3.0f; // -60 degrees, like Unreal Engine default sunlight
        float intensity = 10000.0f;  // Default to 10,000 lux (bright daylight)
    };

    // Public scene storage (instance members)
    std::vector<Mesh> m_Meshes;
    std::vector<Node> m_Nodes;
    std::vector<Material> m_Materials;
    std::vector<Texture> m_Textures;
    std::vector<Camera> m_Cameras;
    std::vector<Light> m_Lights;

    DirectionalLight m_DirectionalLight;

    // GPU buffers created for the scene
    nvrhi::BufferHandle m_VertexBuffer;
    nvrhi::BufferHandle m_IndexBuffer;
    nvrhi::BufferHandle m_MaterialConstantsBuffer;
    nvrhi::BufferHandle m_MeshDataBuffer;
    nvrhi::BufferHandle m_MeshletBuffer;
    nvrhi::BufferHandle m_MeshletVerticesBuffer;
    nvrhi::BufferHandle m_MeshletTrianglesBuffer;

    // GPU buffers for instances
    std::vector<PerInstanceData> m_InstanceData;
    std::vector<MeshData> m_MeshData;
    std::vector<Meshlet> m_Meshlets;
    std::vector<uint32_t> m_MeshletVertices;
    std::vector<uint8_t> m_MeshletTriangles;
    nvrhi::BufferHandle m_InstanceDataBuffer;

    // Load the scene from the path configured in `Config::Get().m_GltfScene`.
    // Only mesh vertex/index data and node hierarchy are loaded for now.
    bool LoadScene();
    // Release GPU resources and clear scene data
    void Shutdown();

    // Get directional light direction in world space
    Vector3 GetDirectionalLightDirection() const;
};
