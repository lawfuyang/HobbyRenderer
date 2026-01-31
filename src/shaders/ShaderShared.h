////////////////////////////////////////////////////////////////////////////////
// NOTE TO FUTURE AI: This file is shared between C++ and HLSL. It uses #ifdef __cplusplus
// to conditionally include C++ headers and define types. When modifying, ensure compatibility
// for both languages. Structs are defined with the same layout for CPU/GPU data sharing.
// Always test compilation in both C++ and HLSL contexts after changes.
////////////////////////////////////////////////////////////////////////////////

#pragma once

// Minimal language-specific macro layer. The rest of this file uses the macros below so both C++ and HLSL follow the exact same code path.

#if !defined(__cplusplus)
  // HLSL path
  typedef uint uint32_t;
  typedef uint2 Vector2U;
  typedef float2 Vector2;
  typedef float3 Vector3;
  typedef float4 Vector4;
  typedef row_major float4x4 Matrix; // Ensure HLSL uses row-major layout to match DirectX::XMFLOAT4X4 on CPU
  typedef float4 Color;

  // Shader macros for cross-platform compatibility
  #if defined(SPIRV)
      #define DRAW_INDEX_ARG_COMMA , [[vk::builtin("DrawIndex")]] uint drawIndex : DRAW_INDEX
      #define GET_DRAW_INDEX() drawIndex
      #define PUSH_CONSTANT [[vk::push_constant]]
  #elif defined(DXIL)
      // DrawID is bound to b255 in space0 for D3D12 when useDrawIndex is enabled in the pipeline.
      cbuffer DrawIDCB : register(b255)
      {
          uint g_DrawID;
      };
      #define DRAW_INDEX_ARG_COMMA 
      #define GET_DRAW_INDEX() g_DrawID
      #define PUSH_CONSTANT 
  #else
      #define DRAW_INDEX_ARG_COMMA , uint drawIndex : DRAW_INDEX
      #define GET_DRAW_INDEX() drawIndex
      #define PUSH_CONSTANT 
  #endif
#endif

struct ImGuiPushConstants
{
	Vector2 uScale;
	Vector2 uTranslate;
};

// Forward lighting related shared types

#define DEBUG_MODE_NONE 0
#define DEBUG_MODE_INSTANCES 1
#define DEBUG_MODE_MESHLETS 2
#define DEBUG_MODE_WORLD_NORMALS 3
#define DEBUG_MODE_ALBEDO 4
#define DEBUG_MODE_ROUGHNESS 5
#define DEBUG_MODE_METALLIC 6
#define DEBUG_MODE_EMISSIVE 7
#define DEBUG_MODE_LOD 8

#define MAX_LOD_COUNT 8

// Forward-lighting specific shared types.
// Vertex input: provide simple C++ and HLSL variants
struct Vertex
{
  Vector3 m_Pos;
  Vector3 m_Normal;
  Vector2 m_Uv;
};

// Shared per-frame data structure (one definition used by both C++ and HLSL).
struct ForwardLightingPerFrameData
{
  Matrix m_ViewProj;
  Matrix m_View;
  Vector4 m_FrustumPlanes[5];
  Vector4 m_CameraPos; // xyz: camera world-space position, w: unused
  Vector4 m_CullingCameraPos; // xyz: culling camera position
  Vector3 m_LightDirection;
  float m_LightIntensity;
  uint32_t m_DebugMode;
  uint32_t m_EnableFrustumCulling;
  uint32_t m_EnableConeCulling;
  uint32_t m_EnableOcclusionCulling;
  uint32_t m_HZBWidth;
  uint32_t m_HZBHeight;
  float m_P00;
  float m_P11;
};

// Material constants (persistent, per-material data)
#define ALPHA_MODE_OPAQUE 0
#define ALPHA_MODE_MASK 1
#define ALPHA_MODE_BLEND 2

struct MaterialConstants
{
  Vector4 m_BaseColor;
  Vector4 m_EmissiveFactor; // rgb: emissive factor, a: unused
  Vector2 m_RoughnessMetallic; // x: roughness, y: metallic
  uint32_t m_TextureFlags;
  uint32_t m_AlbedoTextureIndex;
  uint32_t m_NormalTextureIndex;
  uint32_t m_RoughnessMetallicTextureIndex;
  uint32_t m_EmissiveTextureIndex;
  uint32_t m_AlbedoSamplerIndex;
  uint32_t m_NormalSamplerIndex;
  uint32_t m_RoughnessSamplerIndex;
  uint32_t m_EmissiveSamplerIndex;
  uint32_t m_AlphaMode;
  float m_AlphaCutoff;
  uint32_t pad0[2];
};

struct MeshData
{
  uint32_t m_LODCount;
  uint32_t pad0[3];
  uint32_t m_IndexOffsets[MAX_LOD_COUNT];
  uint32_t m_IndexCounts[MAX_LOD_COUNT];
  uint32_t m_MeshletOffsets[MAX_LOD_COUNT];
  uint32_t m_MeshletCounts[MAX_LOD_COUNT];
  float m_LODErrors[MAX_LOD_COUNT];
};

struct Meshlet
{
  Vector3 m_Center;
  float m_Radius;
  uint32_t m_VertexOffset;
  uint32_t m_TriangleOffset;
  uint32_t m_VertexCount;
  uint32_t m_TriangleCount;
  uint32_t m_ConeAxisAndCutoff;
  Vector3 pad0;
};

struct MeshletJob
{
  uint32_t m_InstanceIndex;
  uint32_t m_LODIndex;
};

// Per-instance data for instanced rendering
struct PerInstanceData
{
  Matrix m_World;
  uint32_t m_MaterialIndex;
  uint32_t m_MeshDataIndex;
  float m_Radius;
  uint32_t padding0;
  Vector3 m_Center;
};

struct DrawIndexedIndirectArguments
{
  uint32_t m_IndexCount;
  uint32_t m_InstanceCount;
  uint32_t m_StartIndexLocation;
  int m_BaseVertexLocation;
  uint32_t m_StartInstanceLocation;
};

struct DispatchIndirectArguments
{
  uint32_t m_ThreadGroupCountX;
  uint32_t m_ThreadGroupCountY;
  uint32_t m_ThreadGroupCountZ;
};

struct CullingConstants
{
  Vector4 m_FrustumPlanes[5];
  Matrix m_View;
  Matrix m_ViewProj;
  uint32_t m_NumPrimitives;
  uint32_t m_EnableFrustumCulling;
  uint32_t m_EnableOcclusionCulling;
  uint32_t m_HZBWidth;
  uint32_t m_HZBHeight;
  uint32_t m_Phase; // 0 = Phase 1 (test all against HZB), 1 = Phase 2 (test occluded against new HZB)
  uint32_t m_UseMeshletRendering;
  float m_P00;
  float m_P11;
  int m_ForcedLOD; // -1 for auto, 0+ for forced
  uint32_t m_InstanceBaseIndex;
  uint32_t m_BucketIndex;
  uint32_t m_BucketVisibleOffset;
  uint32_t pad0;
};

struct HZBFromDepthConstants
{
  uint32_t m_Width;
  uint32_t m_Height;
};

struct HZBConstants
{
  uint32_t m_NumMips;
  uint32_t m_BaseWidth;
  uint32_t m_BaseHeight;
};

struct SpdConstants
{
  uint32_t m_Mips;
  uint32_t m_NumWorkGroups;
  Vector2U m_WorkGroupOffset;
};

#ifdef __cplusplus
inline uint32_t DivideAndRoundUp(uint32_t dividend, uint32_t divisor)
{
    return (dividend + divisor - 1) / divisor;
}
#else
uint32_t DivideAndRoundUp(uint32_t dividend, uint32_t divisor)
{
    return (dividend + divisor - 1) / divisor;
}
#endif

static const float PI = 3.14159265359f;
static const uint32_t kThreadsPerGroup = 32;
static const uint32_t kMaxMeshletVertices = 64;
static const uint32_t kMaxMeshletTriangles = 96;

#define TEXFLAG_ALBEDO (1u << 0)
#define TEXFLAG_NORMAL (1u << 1)
#define TEXFLAG_ROUGHNESS_METALLIC (1u << 2)
#define TEXFLAG_EMISSIVE (1u << 3)

// Default texture indices for bindless access
#define DEFAULT_TEXTURE_BLACK 0
#define DEFAULT_TEXTURE_WHITE 1
#define DEFAULT_TEXTURE_GRAY 2
#define DEFAULT_TEXTURE_NORMAL 3
#define DEFAULT_TEXTURE_PBR 4

#define SAMPLER_CLAMP_INDEX 0
#define SAMPLER_WRAP_INDEX 1

#ifndef __cplusplus
float GetMaxScale(Matrix m)
{
    return max(length(m[0].xyz), max(length(m[1].xyz), length(m[2].xyz)));
}

#define DEPTH_NEAR 1.0f
#define DEPTH_FAR 0.0f
#endif