#pragma once

#include "pch.h"
#include "Camera.h"

#include "shaders/srrhi/cpp/Common.h"
#include "shaders/srrhi/cpp/Mesh.h"
#include "shaders/srrhi/cpp/Instance.h"

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
        // One BLAS per LOD level; m_BLAS[lod] corresponds to meshData.m_IndexOffsets[lod].
        std::vector<nvrhi::rt::AccelStructHandle> m_BLAS;
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
            CubicSpline,
            Slerp,      // quaternion spherical linear interpolation (JSON animations)
            CatmullRom, // Catmull-Rom spline (JSON animations)
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
            Weights,
            EmissiveIntensity, // JSON animations: material emissive scale (scalar)
        };
        Path m_Path = Path::Translation;
        int m_SamplerIndex = -1;

        // Node targets (single glTF node stored as one-element vector, or multiple JSON targets)
        std::vector<int> m_NodeIndices;
        std::vector<int> m_MaterialIndices; // Material targets (EmissiveIntensity path)
        // Base emissive factor per material, captured at load time so the animated
        // scalar multiplies the authored colour rather than replacing it.
        std::vector<Vector3> m_BaseEmissiveFactor;
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
        uint32_t m_AlbedoTextureIndex = srrhi::CommonConsts::DEFAULT_TEXTURE_WHITE;
        uint32_t m_NormalTextureIndex = srrhi::CommonConsts::DEFAULT_TEXTURE_NORMAL;
        uint32_t m_RoughnessMetallicTextureIndex = srrhi::CommonConsts::DEFAULT_TEXTURE_PBR;
        uint32_t m_EmissiveTextureIndex = srrhi::CommonConsts::DEFAULT_TEXTURE_BLACK;
        uint32_t m_AlphaMode = srrhi::CommonConsts::ALPHA_MODE_OPAQUE;
        float m_AlphaCutoff = 0.5f;

        // KHR_materials_ior
        float m_IOR = 1.5f;
        // KHR_materials_transmission
        float m_TransmissionFactor = 0.0f;

        // KHR_materials_volume
        float m_ThicknessFactor = 0.0f;
        float m_AttenuationDistance = FLT_MAX;
        Vector3 m_AttenuationColor = Vector3{ 1.0f, 1.0f, 1.0f };

        // KHR_materials_volume — converted to physical extinction coefficients
        Vector3 m_SigmaA = Vector3{ 0.0f, 0.0f, 0.0f };  // absorption coefficient (from attenuationColor + attenuationDistance)
        Vector3 m_SigmaS = Vector3{ 0.0f, 0.0f, 0.0f };  // scattering coefficient (reserved for future use)
        bool m_IsThinSurface = false;                      // true = thin-walled (thicknessFactor == 0)
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
        int m_NodeIndex = -1; // Must be valid - references the Node containing this light's transform
    };

    // Public scene storage (instance members)
    std::vector<Mesh> m_Meshes;
    std::vector<Node> m_Nodes;
    std::vector<Material> m_Materials;
    std::vector<Texture> m_Textures;
    std::vector<Camera> m_Cameras;
    std::vector<Light> m_Lights;
    std::vector<Animation> m_Animations;
    std::vector<int> m_DynamicMaterialIndices; // Material indices targeted by emissive animations
    std::pair<uint32_t, uint32_t> m_MaterialDirtyRange = { UINT32_MAX, 0 }; // Dirty range for material GPU upload
    std::vector<int> m_DynamicNodeIndices; // Topologically sorted

    ::Camera m_Camera;
    Matrix m_FrozenCullingViewMatrix;
    Vector3 m_FrozenCullingCameraPos;
    srrhi::PlanarViewConstants m_View;
    srrhi::PlanarViewConstants m_ViewPrev;
    int m_SelectedCameraIndex = -1;

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

    std::vector<srrhi::PerInstanceData> m_InstanceData;
    std::vector<srrhi::MeshData> m_MeshData;
    std::vector<srrhi::Meshlet> m_Meshlets;
    std::vector<uint32_t> m_MeshletVertices;
    std::vector<uint32_t> m_MeshletTriangles;
    nvrhi::BufferHandle m_InstanceDataBuffer;
    nvrhi::BufferHandle m_RTInstanceDescBuffer;
    // Flat GPU buffer: blasAddresses[instanceIndex * srrhi::CommonConsts::MAX_LOD_COUNT + lodIndex]
    // Uploaded once at scene load; read by TLASPatch_CS to look up per-LOD device addresses.
    nvrhi::BufferHandle m_BLASAddressBuffer;
    // Per-instance LOD index buffer: instanceLOD[instanceIndex] = lodIndex.
    // Written each frame by the GPU culling passes (opaque, masked, transparent).
    // Read by TLASRenderer to patch BLAS addresses before the TLAS build.
    nvrhi::BufferHandle m_InstanceLODBuffer;
    nvrhi::rt::AccelStructHandle m_TLAS;
    std::vector<nvrhi::rt::InstanceDesc> m_RTInstanceDescs;

    DirectX::BoundingSphere m_SceneBoundingSphere;

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

    // Sun direction derived from the first directional light's node world transform.
    // After RH->LH conversion, +Z = light shine direction, -Z = toward the light source.
    // Convention: returns the direction TOWARD the sun (local -Z of the light node).
    Vector3 GetSunDirection() const
    {
        const Light& dirLight = m_Lights.back();
        SDL_assert(dirLight.m_Type == Light::Directional && dirLight.m_NodeIndex >= 0);
        const Node& node = m_Nodes.at(dirLight.m_NodeIndex);
        DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&node.m_WorldTransform);
        DirectX::XMVECTOR fwd = DirectX::XMVector3Normalize(
            DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), world));
        Vector3 f; DirectX::XMStoreFloat3(&f, fwd);
        return Vector3{ f.x, f.y, f.z };
    }

    // Sun pitch (elevation angle in radians, 0 = horizon, PI/2 = zenith)
    // Derived from the shine direction (+Z of light node); pitch = elevation of the sun.
    float GetSunPitch() const
    {
        // Shine direction = -GetSunDirection() (toward-sun negated).
        // The sun elevation equals asin of the Y component of the toward-sun direction,
        // which is the same as asin(-shineDir.y). But since GetSunDirection() already
        // returns toward-sun, we can use it directly.
        Vector3 d = GetSunDirection();
        return std::asin(std::clamp(d.y, -1.0f, 1.0f));
    }

    // Sun yaw (azimuth angle in radians)
    // Derived from the shine direction (+Z of light node) for roundtrip consistency
    // with SetSunPitchYaw which applies yaw to the shine direction.
    float GetSunYaw() const
    {
        // Shine direction = negated toward-sun direction
        Vector3 d = GetSunDirection();
        // Negate to get shine direction, then compute yaw from that
        return std::atan2(-d.x, -d.z);
    }

    // Check if any instance transforms have changed this frame
    bool AreInstanceTransformsDirty() const
    {
        return m_InstanceDirtyRange.first <= m_InstanceDirtyRange.second;
    }

    // Update the directional light node's transform from pitch/yaw angles.
    // pitch: elevation (radians), yaw: azimuth (radians).
    void SetSunPitchYaw(float pitch, float yaw)
    {
        Light& dirLight = m_Lights.at(0);
        SDL_assert(dirLight.m_Type == Light::Directional && dirLight.m_NodeIndex >= 0);
        Node& node = m_Nodes.at(dirLight.m_NodeIndex);

        using namespace DirectX;
        // After RH->LH conversion, +Z = light shine direction, -Z = toward sun.
        // Positive pitch rotates +Z downward (light shines down), so -Z points
        // upward at the given elevation = toward the sun.
        XMVECTOR quat = XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.0f);
        XMStoreFloat4(&node.m_Rotation, quat);

        XMMATRIX localM = XMMatrixRotationQuaternion(quat);
        XMStoreFloat4x4(&node.m_LocalTransform, localM);

        // Directional light nodes are root nodes (no parent), so world = local.
        node.m_WorldTransform = node.m_LocalTransform;
    }

    float GetSunIntensity() const { return m_Lights.at(0).m_Intensity; } // 1st light is always the sun

    float GetSceneBoundingRadius() const { return m_SceneBoundingSphere.Radius; }

    // Binary Scene Cache
    bool LoadFromCache(const std::string& cachePath, std::vector<uint32_t>& allIndices, std::vector<srrhi::VertexQuantized>& allVerticesQuantized);
    void SaveToCache(const std::string& cachePath, const std::vector<uint32_t>& allIndices, const std::vector<srrhi::VertexQuantized>& allVerticesQuantized);

    void UpdateNodeBoundingSphere(int nodeIndex);
};
