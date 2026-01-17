////////////////////////////////////////////////////////////////////////////////
// NOTE TO FUTURE AI: This file is shared between C++ and HLSL. It uses #ifdef __cplusplus
// to conditionally include C++ headers and define types. When modifying, ensure compatibility
// for both languages. Structs are defined with the same layout for CPU/GPU data sharing.
// Always test compilation in both C++ and HLSL contexts after changes.
////////////////////////////////////////////////////////////////////////////////

#pragma once

// Minimal language-specific macro layer. The rest of this file uses the macros below so both C++ and HLSL follow the exact same code path.

// Include Math aliases on C++ side so we can map shared types to them.
#ifdef __cplusplus
typedef uint32_t uint;
#endif

// Map shared scalar/vector/matrix declarations to language-specific forms.
#ifdef __cplusplus
  // In C++, prefer simple POD aliases (types are provided via pch.h)
  typedef uint32_t uint;
#else
  // HLSL path
  typedef float2 Vector2;
  typedef float3 Vector3;
  typedef float4 Vector4;
  // Ensure HLSL uses row-major layout to match DirectX::XMFLOAT4X4 on CPU
  typedef row_major float4x4 Matrix;
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
#ifdef __cplusplus
struct VertexInput
{
  Vector3 m_Pos;
  Vector3 m_Normal;
  Vector2 m_Uv;
};
#else
struct VertexInput
{
  float3 m_Pos : POSITION;
  float3 m_Normal : NORMAL;
  float2 m_Uv : TEXCOORD0;
};
#endif

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
  uint m_TextureFlags;
  uint m_AlbedoTextureIndex;
  uint m_NormalTextureIndex;
  uint m_RoughnessMetallicTextureIndex;
  uint m_AlbedoSamplerIndex;
  uint m_NormalSamplerIndex;
  uint m_RoughnessSamplerIndex;
  Vector3 m_Padding;
};

// Per-instance data for instanced rendering
struct PerInstanceData
{
  Matrix m_World;
  uint m_MaterialIndex;
  uint m_IndexOffset;
  uint m_IndexCount;
  uint padding0;
  Vector3 m_Min;
  uint padding1;
  Vector3 m_Max;
  uint padding2;
};

struct DrawIndexedIndirectArguments
{
  uint m_IndexCount;
  uint m_InstanceCount;
  uint m_StartIndexLocation;
  int m_BaseVertexLocation;
  uint m_StartInstanceLocation;
};

struct CullingConstants
{
  Vector4 m_FrustumPlanes[5];
  Matrix m_View;
  uint m_NumPrimitives;
  uint m_EnableFrustumCulling;
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