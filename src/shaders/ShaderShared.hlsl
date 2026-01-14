#pragma once

// Minimal language-specific macro layer. The rest of this file uses the macros below so both C++ and HLSL follow the exact same code path.

// Include Math aliases on C++ side so we can map shared types to them.
#ifdef __cplusplus
#include "pch.h"
#endif

// Map shared scalar/vector/matrix declarations to language-specific forms.
#ifdef __cplusplus
  // In C++, prefer simple POD aliases (types are provided via pch.h)
#else
  // HLSL path
  typedef float2 Vector2;
  typedef float3 Vector3;
  typedef float4 Vector4;
  // Ensure HLSL uses row-major layout to match DirectX::XMFLOAT4X4 on CPU
  typedef row_major float4x4 Matrix;
  typedef float4 Color;
#endif

#ifdef IMGUI_DEFINE_PUSH_CONSTANTS
struct PushConstants
{
	Vector2 uScale;
	Vector2 uTranslate;
};
#endif

// Forward lighting related shared types

// Forward-lighting specific shared types. Guarded so only the
// forward lighting shader + renderer include these definitions.
#ifdef FORWARD_LIGHTING_DEFINE
// Vertex input: provide simple C++ and HLSL variants
#ifdef __cplusplus
struct VertexInput
{
  Vector3 pos;
  Vector3 normal;
  Vector2 uv;
};
#else
struct VertexInput
{
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
};
#endif

// Shared per-frame data structure (one definition used by both C++ and HLSL).
struct PerFrameData
{
  Matrix ViewProj;
  Vector4 CameraPos; // xyz: camera world-space position, w: unused
  Vector3 LightDirection;
  float LightIntensity;
};

// Per-instance data for instanced rendering
struct PerInstanceData
{
  Matrix World;
  Vector4 BaseColor;
  Vector2 RoughnessMetallic; // x: roughness, y: metallic
  Vector2 Padding;
};
#endif // FORWARD_LIGHTING_DEFINE

#ifdef __cplusplus
constexpr float PI = 3.14159265358979323846f;
#else
static const float PI = 3.14159265359f;
#endif

// Default texture indices for bindless access
#define DEFAULT_TEXTURE_BLACK 0
#define DEFAULT_TEXTURE_WHITE 1
#define DEFAULT_TEXTURE_GRAY 2
#define DEFAULT_TEXTURE_NORMAL 3
#define DEFAULT_TEXTURE_PBR 4
