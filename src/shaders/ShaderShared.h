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
      #define VK_IMAGE_FORMAT(f) [[vk::image_format(f)]]

      // Allows RWTexture2D<float4> to map to any compatible type
      // see https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#formats-without-shader-storage-format
      // shaderStorageImageWriteWithoutFormat and shaderStorageImageReadWithoutFormat 
      #define VK_IMAGE_FORMAT_UNKNOWN [[vk::image_format("unknown")]]
  #elif defined(DXIL)
      // DrawID is bound to b255 in space0 for D3D12 when useDrawIndex is enabled in the pipeline.
      cbuffer DrawIDCB : register(b255)
      {
          uint g_DrawID;
      };
      #define DRAW_INDEX_ARG_COMMA 
      #define GET_DRAW_INDEX() g_DrawID
      #define PUSH_CONSTANT 
      #define VK_IMAGE_FORMAT(f)
      #define VK_IMAGE_FORMAT_UNKNOWN
  #else
      #error "Unknown shader compilation target. Define either SPIRV or DXIL."
  #endif

  struct FullScreenVertexOut
  {
      float4 pos : SV_Position;
      float2 uv : TEXCOORD0;
  };

  uint32_t DivideAndRoundUp(uint32_t dividend, uint32_t divisor)
  {
    return (dividend + divisor - 1) / divisor;
  }

  // Standard MatrixMultiply helper to enforce consistent multiplication order: mul(vector, matrix)
  float4 MatrixMultiply(float4 v, float4x4 m)
  {
      return mul(v, m);
  }

  float3 MatrixMultiply(float3 v, float3x3 m)
  {
      return mul(v, m);
  }

  float GetMaxScale(Matrix m)
  {
    return max(length(m[0].xyz), max(length(m[1].xyz), length(m[2].xyz)));
  }

  float3x3 MakeAdjugateMatrix(float4x4 m)
  {
      return float3x3
      (
          cross(m[1].xyz, m[2].xyz),
          cross(m[2].xyz, m[0].xyz),
          cross(m[0].xyz, m[1].xyz)
      );
  }

  float3 TransformNormal(float3 normal, float4x4 worldMatrix)
  {
      float3x3 adjugateWorldMatrix = MakeAdjugateMatrix(worldMatrix);
      return normalize(MatrixMultiply(normal, adjugateWorldMatrix));
  }

  // Convert a UV to clip space coordinates (XY: [-1, 1])
  float2 UVToClipXY(float2 uv)
  {
      return uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
  }

  // Convert a clip position after projection and perspective divide to a UV
  float2 ClipXYToUV(float2 xy)
  {
      return xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
  }

  float3 DecodeOct(float2 e)
  {
      float3 v = float3(e, 1.0f - abs(e.x) - abs(e.y));
      float t = max(-v.z, 0.0f);
      v.x += v.x >= 0.0f ? -t : t;
      v.y += v.y >= 0.0f ? -t : t;
      return normalize(v);
  }

  // Unpacks a 2 channel normal to xyz
  float3 TwoChannelNormalX2(float2 normal)
  {
      float2 xy = 2.0f * normal - 1.0f;
      float z = sqrt(saturate(1.0f - dot(xy, xy)));
      return float3(xy.x, xy.y, z);
  }

  float3 TransformNormalWithTBN(float2 nmSample, float3 normal, float3 tangent, float tangentSign)
  {
      float3 normalMap = TwoChannelNormalX2(nmSample);
      float3 n_w = normalize(normal);
      float3 t_w = normalize(tangent);
      t_w = normalize(t_w - n_w * dot(t_w, n_w));
      float3 b_w = normalize(cross(n_w, t_w) * tangentSign);
      float3x3 TBN = float3x3(t_w, b_w, n_w);
      return normalize(MatrixMultiply(normalMap, TBN));
  }

#define DEPTH_NEAR 1.0f
#define DEPTH_FAR 0.0f

static const float3 kEarthCenter = float3(0.0f, -6360000.0f, 0.0f);
#endif

struct ImGuiPushConstants
{
	Vector2 uScale;
	Vector2 uTranslate;
};

static const float PI = 3.14159265359f;
static const uint32_t kThreadsPerGroup = 32;
static const uint32_t kMaxMeshletVertices = 64;
static const uint32_t kMaxMeshletTriangles = 96;

// Bruneton / atmosphere precomputed texture dimensions
static const int TRANSMITTANCE_TEXTURE_WIDTH = 256;
static const int TRANSMITTANCE_TEXTURE_HEIGHT = 64;
static const int SCATTERING_TEXTURE_R_SIZE = 32;
static const int SCATTERING_TEXTURE_MU_SIZE = 128;
static const int SCATTERING_TEXTURE_MU_S_SIZE = 32;
static const int SCATTERING_TEXTURE_NU_SIZE = 8;
static const int SCATTERING_TEXTURE_WIDTH = SCATTERING_TEXTURE_NU_SIZE * SCATTERING_TEXTURE_MU_S_SIZE;
static const int SCATTERING_TEXTURE_HEIGHT = SCATTERING_TEXTURE_MU_SIZE;
static const int SCATTERING_TEXTURE_DEPTH = SCATTERING_TEXTURE_R_SIZE;
static const int IRRADIANCE_TEXTURE_WIDTH = 64;
static const int IRRADIANCE_TEXTURE_HEIGHT = 16;

#define DEBUG_MODE_NONE 0
#define DEBUG_MODE_INSTANCES 1
#define DEBUG_MODE_MESHLETS 2
#define DEBUG_MODE_WORLD_NORMALS 3
#define DEBUG_MODE_ALBEDO 4
#define DEBUG_MODE_ROUGHNESS 5
#define DEBUG_MODE_METALLIC 6
#define DEBUG_MODE_EMISSIVE 7
#define DEBUG_MODE_LOD 8
#define DEBUG_MODE_MOTION_VECTORS 9

#define RENDERING_MODE_NORMAL 0
#define RENDERING_MODE_IBL 1
#define RENDERING_MODE_PATH_TRACER 2

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
#define DEFAULT_TEXTURE_BRDF_LUT 5
#define DEFAULT_TEXTURE_IRRADIANCE 6
#define DEFAULT_TEXTURE_RADIANCE 7
#define BRUNETON_TRANSMITTANCE_TEXTURE 8
#define BRUNETON_SCATTERING_TEXTURE 9
#define BRUNETON_IRRADIANCE_TEXTURE 10
#define DEFAULT_TEXTURE_COUNT 11

// Global Sampler Indices
#define SAMPLER_ANISOTROPIC_CLAMP_INDEX 0
#define SAMPLER_ANISOTROPIC_WRAP_INDEX 1
#define SAMPLER_POINT_CLAMP_INDEX 2
#define SAMPLER_POINT_WRAP_INDEX 3
#define SAMPLER_LINEAR_CLAMP_INDEX 4
#define SAMPLER_LINEAR_WRAP_INDEX 5
#define SAMPLER_MIN_REDUCTION_INDEX 6
#define SAMPLER_MAX_REDUCTION_INDEX 7
#define SAMPLER_LINEAR_CLAMP_BORDER_WHITE_INDEX 8

#define MAX_LOD_COUNT 8

// Forward-lighting specific shared types.
// Vertex input: provide simple C++ and HLSL variants
struct Vertex
{
  Vector3 m_Pos;
  Vector3 m_Normal;
  Vector2 m_Uv;
  Vector4 m_Tangent;
};

struct PlanarViewConstants
{
    Matrix      m_MatWorldToView;
    Matrix      m_MatViewToClip;
    Matrix      m_MatWorldToClip;
    Matrix      m_MatClipToView;
    Matrix      m_MatViewToWorld;
    Matrix      m_MatClipToWorld;

    Matrix      m_MatViewToClipNoOffset;
    Matrix      m_MatWorldToClipNoOffset;
    Matrix      m_MatClipToViewNoOffset;
    Matrix      m_MatClipToWorldNoOffset;

    Vector2      m_ViewportOrigin;
    Vector2      m_ViewportSize;

    Vector2      m_ViewportSizeInv;
    Vector2      m_PixelOffset;

    Vector2      m_ClipToWindowScale;
    Vector2      m_ClipToWindowBias;
};

struct VertexQuantized
{
  Vector3 m_Pos;     // 12 bytes
  uint32_t m_Normal; // 10-10-10-2 snorm (4 bytes)
  uint32_t m_Uv;     // half2 (4 bytes)
  uint32_t m_Tangent; // 10-10-10-2 snorm (4 bytes)
};                   // total: 24 bytes (multiple of 4)

#if !defined(__cplusplus)
  Vertex UnpackVertex(VertexQuantized vq)
  {
    Vertex v;
    v.m_Pos = vq.m_Pos;
    v.m_Normal.x = float(vq.m_Normal & 1023) / 511.0f - 1.0f;
    v.m_Normal.y = float((vq.m_Normal >> 10) & 1023) / 511.0f - 1.0f;
    v.m_Normal.z = float((vq.m_Normal >> 20) & 1023) / 511.0f - 1.0f;
    
    float2 octTan = float2((vq.m_Tangent & 255), (vq.m_Tangent >> 8) & 255) / 127.0f - 1.0f;
    v.m_Tangent.xyz = DecodeOct(octTan);
    v.m_Tangent.w = (vq.m_Normal & (1u << 30)) != 0 ? -1.0f : 1.0f;

    v.m_Uv = f16tof32(uint2(vq.m_Uv & 0xFFFF, vq.m_Uv >> 16));
    return v;
  }
#endif // __cplusplus

struct GPULight
{
    Vector3 m_Position;
    float m_Intensity;
    Vector3 m_Direction;
    uint32_t m_Type;
    Vector3 m_Color;
    float m_Range;
    float m_SpotInnerConeAngle;
    float m_SpotOuterConeAngle;
    float m_Radius;
    float m_CosSunAngularRadius;
};

// Shared per-frame data structure (one definition used by both C++ and HLSL).
struct ForwardLightingPerFrameData
{
  PlanarViewConstants m_View;
  PlanarViewConstants m_PrevView;
  Vector4 m_FrustumPlanes[5];
  Vector4 m_CameraPos; // xyz: camera world-space position, w: unused
  Vector4 m_CullingCameraPos; // xyz: culling camera position
  //
  uint32_t m_LightCount;
  uint32_t m_EnableRTShadows;
  uint32_t m_DebugMode;
  uint32_t m_EnableFrustumCulling;
  //
  uint32_t m_EnableConeCulling;
  uint32_t m_EnableOcclusionCulling;
  uint32_t m_HZBWidth;
  uint32_t m_HZBHeight;
  //
  float m_P00;
  float m_P11;
  Vector2 m_OpaqueColorDimensions;
  //
  Vector3 m_SunDirection;
  uint32_t m_EnableSky;
  //
  uint32_t m_RenderingMode;
  uint32_t m_RadianceMipCount;
};

struct SkyConstants
{
  PlanarViewConstants m_View;
  Vector4 m_CameraPos;
  Vector3 m_SunDirection;
  float m_SunIntensity;
  uint32_t m_RenderingMode;
};

struct DeferredLightingConstants
{
  PlanarViewConstants m_View;
  Vector4 m_CameraPos;
  //
  Vector3 m_SunDirection;
  uint32_t m_LightCount;
  //
  uint32_t m_EnableRTShadows;
  uint32_t m_DebugMode;
  uint32_t m_EnableSky;
  uint32_t m_RenderingMode;
  //
  uint32_t m_RadianceMipCount;
  uint32_t m_UseReSTIRDI;    // 1 = read g_RTXDIDIOutput instead of computing direct lighting
  Vector2 pad0;
};

struct PathTracerConstants
{
    PlanarViewConstants m_View;
    Vector4 m_CameraPos;
    uint32_t m_LightCount;
    uint32_t m_AccumulationIndex;
    uint32_t m_FrameIndex;
    uint32_t m_MaxBounces;
    // 
    Vector2 m_Jitter;
    Vector2 pad2;
    //
    Vector3 m_SunDirection;
    float m_CosSunAngularRadius; // cos(half-angle of sun disc)
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
  float m_IOR;
  float m_TransmissionFactor;

  // for rasterized path
  float m_ThicknessFactor;
  float m_AttenuationDistance;
  Vector3 m_AttenuationColor;

  // for path tracer
  Vector3 m_SigmaA;          // absorption coefficient (per-channel, units: 1/m)
  uint32_t m_IsThinSurface;  // 1 = thin-walled surface (no refraction bend), 0 = thick/volumetric
  Vector3 m_SigmaS;          // scattering coefficient (per-channel, reserved for future volume scattering)
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
  uint32_t m_CenterRadius[2]; // x: center.x, y: center.y, z: center.z, w: radius (all quantized as 16-bit half-floats)
  uint32_t m_VertexOffset;
  uint32_t m_TriangleOffset;
  uint32_t m_VertexCount;
  uint32_t m_TriangleCount;
  uint32_t m_ConeAxisAndCutoff;
  uint32_t pad0[3];
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
  Matrix m_PrevWorld;
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
};

struct HZBFromDepthConstants
{
  uint32_t m_Width;
  uint32_t m_Height;
};

enum SpdReductionType
{
    SPD_REDUCTION_MIN = 0,
    SPD_REDUCTION_MAX = 1,
    SPD_REDUCTION_AVERAGE = 2
};

struct SpdConstants
{
  uint32_t m_Mips;
  uint32_t m_NumWorkGroups;
  Vector2U m_WorkGroupOffset;
  SpdReductionType m_ReductionType;
};

struct HistogramConstants
{
    uint32_t m_Width;
    uint32_t m_Height;
    float m_MinLogLuminance;
    float m_MaxLogLuminance;
};

struct AdaptationConstants
{
    float m_DeltaTime;
    float m_AdaptationSpeed;
    uint32_t m_NumPixels;
    float m_MinLogLuminance;
    float m_MaxLogLuminance;
    float m_ExposureValueMin;     // EV clamp
    float m_ExposureValueMax;     // EV clamp
    float m_ExposureCompensation; // EV bias
};

struct TonemapConstants
{
    uint32_t m_Width;
    uint32_t m_Height;
    float m_BloomIntensity;
    uint32_t m_EnableBloom;    uint32_t m_DebugBloom;
    uint32_t m_Pad;
};

struct BloomConstants
{
    float m_Knee;
    float m_Strength;
    uint32_t m_Width;
    uint32_t m_Height;
    float m_UpsampleRadius;
};
// ============================================================================
// RTXDIConstants — GPU-shared constant buffer for ReSTIR DI passes.
//
// Layout mirrors the RTXDI SDK parameter structs so that C++ can fill them
// directly from the ReSTIRDIContext accessors. The #ifdef guards let the same
// source file compile as both a C++ header and an HLSL include.
// ============================================================================
#ifdef __cplusplus
// Pull in the RTXDI C++ types so we can embed them below.
#include <Rtxdi/RtxdiParameters.h>
#include <Rtxdi/DI/ReSTIRDIParameters.h>
#endif

struct RTXDIConstants
{
    // ---- viewport & frame ----
    Vector2U m_ViewportSize;     // (width, height) in pixels
    uint32_t m_FrameIndex;
    uint32_t m_LightCount;       // total number of lights in g_Lights[]

    // ---- RTXDI_RuntimeParameters ----
    uint32_t m_NeighborOffsetMask;       // = ReSTIRDIStaticParameters.NeighborOffsetCount - 1
    uint32_t m_ActiveCheckerboardField;  // 0 = off
    uint32_t m_RuntimePad1;
    uint32_t m_RuntimePad2;

    // ---- RTXDI_LightBufferParameters: localLightBufferRegion ----
    uint32_t m_LocalLightFirstIndex;
    uint32_t m_LocalLightCount;
    uint32_t m_LocalRegionPad1;
    uint32_t m_LocalRegionPad2;

    // ---- RTXDI_LightBufferParameters: infiniteLightBufferRegion ----
    uint32_t m_InfiniteLightFirstIndex;
    uint32_t m_InfiniteLightCount;
    uint32_t m_InfiniteRegionPad1;
    uint32_t m_InfiniteRegionPad2;

    // ---- RTXDI_LightBufferParameters: environmentLightParams ----
    uint32_t m_EnvLightPresent;
    uint32_t m_EnvLightIndex;
    uint32_t m_EnvLightPad1;
    uint32_t m_EnvLightPad2;

    // ---- RTXDI_ReservoirBufferParameters ----
    uint32_t m_ReservoirBlockRowPitch;
    uint32_t m_ReservoirArrayPitch;
    uint32_t m_ReservoirPad1;
    uint32_t m_ReservoirPad2;

    // ---- ReSTIRDI_BufferIndices ----
    uint32_t m_InitialSamplingOutputBufferIndex;
    uint32_t m_TemporalResamplingInputBufferIndex;
    uint32_t m_TemporalResamplingOutputBufferIndex;
    uint32_t m_SpatialResamplingInputBufferIndex;
    uint32_t m_SpatialResamplingOutputBufferIndex;
    uint32_t m_ShadingInputBufferIndex;
    uint32_t m_EnableSky;
    uint32_t m_BufferIdxPad2;

    // ---- Camera: current & previous frame view matrices for reprojection ----
    PlanarViewConstants m_View;
    PlanarViewConstants m_PrevView;

    Vector3 m_SunDirection;
};