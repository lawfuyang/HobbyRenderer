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
  typedef uint32_t uint;
  typedef uint2 Vector2U;
  typedef float2 Vector2;
  typedef float3 Vector3;
  typedef float4 Vector4;
  typedef row_major float4x4 Matrix; // Ensure HLSL uses row-major layout to match DirectX::XMFLOAT4X4 on CPU
  typedef float4 Color;
#endif

struct ImGuiPushConstants
{
	Vector2 uScale;
	Vector2 uTranslate;
};

// Forward lighting related shared types

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
  Vector4 m_CameraPos; // xyz: camera world-space position, w: unused
  Vector3 m_LightDirection;
  float m_LightIntensity;
};

// Material constants (persistent, per-material data)
struct MaterialConstants
{
  Vector4 m_BaseColor;
  Vector2 m_RoughnessMetallic; // x: roughness, y: metallic
  uint32_t m_TextureFlags;
  uint32_t m_AlbedoTextureIndex;
  uint32_t m_NormalTextureIndex;
  uint32_t m_RoughnessMetallicTextureIndex;
  uint32_t m_AlbedoSamplerIndex;
  uint32_t m_NormalSamplerIndex;
  uint32_t m_RoughnessSamplerIndex;
  Vector3 m_Padding;
};

struct MeshData
{
  uint32_t m_IndexOffset;
  uint32_t m_IndexCount;
  uint32_t m_MeshletOffset;
  uint32_t m_MeshletCount;
};

struct Meshlet
{
  Vector3 m_Center;
  float m_Radius;
  uint32_t m_VertexOffset;
  uint32_t m_TriangleOffset;
  uint32_t m_ConeAxisAndCutoff;
  uint32_t pad0;
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
  float m_P00;
  float m_P11;
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
constexpr float PI = 3.14159265358979323846f;
#else
static const float PI = 3.14159265359f;
#endif

#define TEXFLAG_ALBEDO (1u << 0)
#define TEXFLAG_NORMAL (1u << 1)
#define TEXFLAG_ROUGHNESS_METALLIC (1u << 2)

// Default texture indices for bindless access
#define DEFAULT_TEXTURE_BLACK 0
#define DEFAULT_TEXTURE_WHITE 1
#define DEFAULT_TEXTURE_GRAY 2
#define DEFAULT_TEXTURE_NORMAL 3
#define DEFAULT_TEXTURE_PBR 4

#define SAMPLER_CLAMP_INDEX 0
#define SAMPLER_WRAP_INDEX 1

#ifndef __cplusplus
#define DEPTH_NEAR 1.0f
#define DEPTH_FAR 0.0f
#endif