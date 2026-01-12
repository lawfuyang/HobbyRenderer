#pragma once

#include "pch.h"
#include "MathTypes.h"

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
        int m_MaterialIndex = -1;
    };

    struct Mesh
    {
        std::vector<Primitive> m_Primitives;
        // local AABB
        Vector3 m_AabbMin{};
        Vector3 m_AabbMax{};
    };

    struct Node
    {
        std::string m_Name;
        int m_MeshIndex = -1;
        int m_Parent = -1;
        std::vector<int> m_Children;
        // local transform
        Vector4x4 m_LocalTransform{};
        // world transform (computed)
        Vector4x4 m_WorldTransform{};
        // world AABB
        Vector3 m_AabbMin{};
        Vector3 m_AabbMax{};
    };

    struct Material
    {
        std::string m_Name;
    };

    struct Texture
    {
        std::string m_Uri;
    };

    // Public scene storage (instance members)
    std::vector<Mesh> m_Meshes;
    std::vector<Node> m_Nodes;
    std::vector<Material> m_Materials;
    std::vector<Texture> m_Textures;

    // GPU buffers created for the scene
    nvrhi::BufferHandle m_VertexBuffer;
    nvrhi::BufferHandle m_IndexBuffer;

    // Load the scene from the path configured in `Config::Get().m_GltfScene`.
    // Only mesh vertex/index data and node hierarchy are loaded for now.
    bool LoadScene();
    // Release GPU resources and clear scene data
    void Shutdown();
};
