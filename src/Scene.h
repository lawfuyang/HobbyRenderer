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
        int m_MaterialIndex = -1;
        uint32_t m_MeshDataIndex = 0;
        nvrhi::rt::AccelStructHandle m_BLAS;
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

        Matrix m_LocalTransform{};
        Matrix m_WorldTransform{};

        Vector3 m_Translation = Vector3{ 0.0f, 0.0f, 0.0f };
        Quaternion m_Rotation = Quaternion{ 0.0f, 0.0f, 0.0f, 1.0f };
        Vector3 m_Scale = Vector3{ 1.0f, 1.0f, 1.0f };
        bool m_HasTRS = false;
        bool m_IsAnimated = false; // Directly targeted by an animation channel
        bool m_IsDynamic = false;  // Animated or child of dynamic
        bool m_IsDirty = false;    // Transform changed this frame

        Vector3 m_Center{};
        float m_Radius{};
        int m_CameraIndex = -1;
        int m_LightIndex = -1;

        // Indices into m_InstanceData for all instances using this node
        std::vector<uint32_t> m_InstanceIndices;
    };

    struct AnimationSampler
    {
        enum class Interpolation
        {
            Linear,
            Step,
            CubicSpline
        };
        Interpolation m_Interpolation = Interpolation::Linear;
        std::vector<float> m_Inputs; // Time points
        std::vector<Vector4> m_Outputs; // Keyframe values
    };

    struct AnimationChannel
    {
        enum class Path
        {
            Translation,
            Rotation,
            Scale,
            Weights
        };
        Path m_Path;
        int m_NodeIndex = -1;
        int m_SamplerIndex = -1;
    };

    struct Animation
    {
        std::string m_Name;
        std::vector<AnimationChannel> m_Channels;
        std::vector<AnimationSampler> m_Samplers;
        float m_Duration = 0.0f;
        float m_CurrentTime = 0.0f;
    };

    struct Material
    {
        std::string m_Name;
        Vector4 m_BaseColorFactor = Vector4{1.0f, 1.0f, 1.0f, 1.0f};
        Vector3 m_EmissiveFactor = Vector3{0.0f, 0.0f, 0.0f};
        int m_BaseColorTexture = -1; // index into m_Textures
        int m_NormalTexture = -1;
        int m_MetallicRoughnessTexture = -1;
        int m_EmissiveTexture = -1;
        float m_RoughnessFactor = 1.0f;
        float m_MetallicFactor = 0.0f;
        uint32_t m_AlbedoTextureIndex = DEFAULT_TEXTURE_WHITE;
        uint32_t m_NormalTextureIndex = DEFAULT_TEXTURE_NORMAL;
        uint32_t m_RoughnessMetallicTextureIndex = DEFAULT_TEXTURE_PBR;
        uint32_t m_EmissiveTextureIndex = DEFAULT_TEXTURE_BLACK;
        uint32_t m_AlphaMode = ALPHA_MODE_OPAQUE;
        float m_AlphaCutoff = 0.5f;

        // KHR_materials_ior
        float m_IOR = 1.5f;
        // KHR_materials_transmission
        float m_TransmissionFactor = 0.0f;
        // KHR_materials_volume
        float m_ThicknessFactor = 0.0f;
        float m_AttenuationDistance = FLT_MAX;
        Vector3 m_AttenuationColor = Vector3{ 1.0f, 1.0f, 1.0f };
    };

    struct Texture
    {
        std::string m_Uri;
        nvrhi::TextureHandle m_Handle;
        uint32_t m_BindlessIndex = UINT32_MAX;
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

        // Exposure settings (photographic EV100)
        float m_ExposureValue           = 10.0f; // EV100
        float m_ExposureCompensation    = 0.0f;  // in stops
        float m_ExposureValueMin        = -7.0f;  // clamp for auto exposure
        float m_ExposureValueMax        = 23.0f; // clamp for auto exposure
    };

    struct Light
    {
        std::string m_Name;
        enum Type { Directional, Point, Spot };
        Type m_Type;
        Vector3 m_Color = Vector3{1.0f, 1.0f, 1.0f};
        float m_Intensity = 1.0f;
        float m_Range = 0.0f; // 0 = infinite
        float m_Radius = 0.0f;
        // Spot
        float m_SpotInnerConeAngle = 0.0f;
        float m_SpotOuterConeAngle = DirectX::XM_PIDIV4; // 45deg
        float m_AngularSize = 0.533f; // For directional lights (degree)
        int m_NodeIndex = -1;
    };

    struct DirectionalLight
    {
        float yaw       = 0.0f;
        float pitch     = -DirectX::XM_PI / 3.0f; // -60 degrees, like Unreal Engine default sunlight
        float intensity = 20000.0f;  // Default to 20,000 lux (bright daylight)
        float angularSize = 0.533f; // Sun angular size in degrees (default to real sun)
    };

    // Public scene storage (instance members)
    std::vector<Mesh> m_Meshes;
    std::vector<Node> m_Nodes;
    std::vector<Material> m_Materials;
    std::vector<Texture> m_Textures;
    std::vector<Camera> m_Cameras;
    std::vector<Light> m_Lights;
    std::vector<Animation> m_Animations;
    std::vector<int> m_DynamicNodeIndices; // Topologically sorted

    std::string m_RadianceTexturePath;
    std::string m_IrradianceTexturePath;

    nvrhi::TextureHandle m_RadianceTexture;
    nvrhi::TextureHandle m_IrradianceTexture;

    std::pair<uint32_t, uint32_t> m_InstanceDirtyRange = { UINT32_MAX, 0 };

    struct BucketInfo
    {
        uint32_t m_BaseIndex;
        uint32_t m_Count;
    };
    BucketInfo m_OpaqueBucket;
    BucketInfo m_MaskedBucket;
    BucketInfo m_TransparentBucket;

    // GPU buffers created for the scene
    nvrhi::BufferHandle m_VertexBufferQuantized;
    nvrhi::BufferHandle m_IndexBuffer;
    nvrhi::BufferHandle m_MaterialConstantsBuffer;
    nvrhi::BufferHandle m_MeshDataBuffer;
    nvrhi::BufferHandle m_MeshletBuffer;
    nvrhi::BufferHandle m_MeshletVerticesBuffer;
    nvrhi::BufferHandle m_MeshletTrianglesBuffer;
    nvrhi::BufferHandle m_LightBuffer;
    uint32_t m_LightCount = 0;
    bool m_LightsDirty = true;

    std::vector<PerInstanceData> m_InstanceData;
    std::vector<MeshData> m_MeshData;
    std::vector<Meshlet> m_Meshlets;
    std::vector<uint32_t> m_MeshletVertices;
    std::vector<uint32_t> m_MeshletTriangles;
    nvrhi::BufferHandle m_InstanceDataBuffer;
    nvrhi::BufferHandle m_RTInstanceDescBuffer;
    nvrhi::rt::AccelStructHandle m_TLAS;
    std::vector<nvrhi::rt::InstanceDesc> m_RTInstanceDescs;

    // Load the scene from the path configured in `Config::Get().m_ScenePath`.
    // Only mesh vertex/index data and node hierarchy are loaded for now.
    void LoadScene();

    // Rebuilds instance data, buckets, and dynamic node indices.
    // Called after loading from glTF or Cache.
    void FinalizeLoadedScene();

    void BuildAccelerationStructures();

    // Per-frame update for animations
    void Update(float deltaTime);

    // Release GPU resources and clear scene data
    void Shutdown();

    // Environment / Atmosphere settings
    float m_SunPitch = DirectX::XMConvertToRadians(45.0f);
    float m_SunYaw = 0.0f;
    Vector3 m_SunDirection = Vector3(0.0f, 1.0f, 0.0f);

    float GetSunIntensity() const { return m_Lights.at(0).m_Intensity; } // 1st light is always the sun

    // Binary Scene Cache
    bool LoadFromCache(const std::string& cachePath, std::vector<uint32_t>& allIndices, std::vector<VertexQuantized>& allVerticesQuantized);
    void SaveToCache(const std::string& cachePath, const std::vector<uint32_t>& allIndices, const std::vector<VertexQuantized>& allVerticesQuantized);

    void UpdateNodeBoundingSphere(int nodeIndex);
};
