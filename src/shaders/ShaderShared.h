////////////////////////////////////////////////////////////////////////////////
// This file is shared between C++ and HLSL. It uses #ifdef __cplusplus
// to conditionally include C++ headers and define types. When modifying, ensure compatibility
// for both languages. Structs are defined with the same layout for CPU/GPU data sharing.
// Always test compilation in both C++ and HLSL contexts after changes.
////////////////////////////////////////////////////////////////////////////////

#ifndef SHADER_SHARED_H
#define SHADER_SHARED_H

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

static const float3 kEarthCenter = float3(0.0f, -6360000.0f, 0.0f);

#include "srrhi/hlsl/Common.hlsli" // TODO: remove this when everything gets converted to srrhi
#endif

#ifdef __cplusplus
// C++ aliases for HLSL scalar/vector types used in shared structs (ShaderParameters.h etc.)
// Note: cstdint and DirectXMath.h must already be included by the including C++ translation unit.
typedef uint32_t uint;
typedef DirectX::XMUINT2 uint2;
typedef DirectX::XMINT2  int2;
typedef DirectX::XMFLOAT2 float2;
typedef DirectX::XMFLOAT3 float3;
typedef DirectX::XMFLOAT4 float4;

#include "srrhi/cpp/Common.h" // TODO: remove this when everything gets converted to srrhi

#endif // __cplusplus

// Forward-lighting specific shared types.
// Vertex input: provide simple C++ and HLSL variants
struct Vertex
{
  Vector3 m_Pos;
  Vector3 m_Normal;
  Vector2 m_Uv;
  Vector4 m_Tangent;
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
  srrhi::PlanarViewConstants m_View;
  srrhi::PlanarViewConstants m_PrevView;
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
  uint32_t m_OpaqueColorMipCount;  // Mip count for g_OpaqueColor texture (for LOD clamping)
  uint32_t pad1;
};

struct DeferredLightingConstants
{
  srrhi::PlanarViewConstants m_View;
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
  uint32_t m_UseReSTIRDI;         // 1 = read ReSTIR DI illumination textures instead of computing direct lighting
  uint32_t m_UseReSTIRDIDenoised; // 1 = t8/t9 are denoised diffuse/specular illumination
  uint32_t pad0;
};

struct PathTracerConstants
{
    srrhi::PlanarViewConstants m_View;
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
  uint32_t m_IndexOffsets[srrhi::CommonConsts::MAX_LOD_COUNT];
  uint32_t m_IndexCounts[srrhi::CommonConsts::MAX_LOD_COUNT];
  uint32_t m_MeshletOffsets[srrhi::CommonConsts::MAX_LOD_COUNT];
  uint32_t m_MeshletCounts[srrhi::CommonConsts::MAX_LOD_COUNT];
  float m_LODErrors[srrhi::CommonConsts::MAX_LOD_COUNT];
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
  // Current LOD index for this instance.
  // Written each frame by TLASPatch_CS (meshlet path) so that RT shaders
  // can use m_IndexOffsets[m_LODIndex] in GetTriangleVertices.
  // Defaults to 0 so instances not yet processed by the culling pass use LOD 0.
  uint32_t m_LODIndex;
  Vector3 m_Center;
  // Index of the first geometry instance in the RTXDI geometry-to-light mapping table.
  // Used by RTXDI to map triangle hits to emissive light indices.
  uint32_t m_FirstGeometryInstanceIndex;
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

struct ResizeToNextLowestPowerOfTwoConstants
{
  uint32_t m_Width;
  uint32_t m_Height;
  uint32_t m_SamplerIdx;
};

// ============================================================================
// RTXDIConstants
// RTXDIConstants — GPU-shared constant buffer for ReSTIR DI passes.
//
// Layout mirrors the RTXDI SDK parameter structs so that C++ can fill them
// directly from the ReSTIRDIContext accessors. The #ifdef guards let the same
// source file compile as both a C++ header and an HLSL include.
// ============================================================================

struct RTXDIConstants
{
    srrhi::PlanarViewConstants m_View;
    srrhi::PlanarViewConstants m_PrevView;
    //
    Vector2U m_ViewportSize;             // (width, height) in pixels
    uint32_t m_FrameIndex;
    uint32_t m_LightCount;               // total lights in g_Lights[]
    //
    uint32_t m_NeighborOffsetMask;       // ReSTIRDIStaticParameters.NeighborOffsetCount - 1
    uint32_t m_ActiveCheckerboardField;  // 0 = off
    uint32_t m_LocalLightFirstIndex;
    uint32_t m_LocalLightCount;
    //
    uint32_t m_InfiniteLightFirstIndex;
    uint32_t m_InfiniteLightCount;
    uint32_t m_EnvLightPresent;
    uint32_t m_EnvLightIndex;
    //
    uint32_t m_ReservoirBlockRowPitch;
    uint32_t m_ReservoirArrayPitch;
    uint32_t m_NumLocalLightSamples;     // numPrimaryLocalLightSamples
    uint32_t m_NumInfiniteLightSamples;  // numPrimaryInfiniteLightSamples
    //
    uint32_t m_NumEnvSamples;            // numEnvironmentMapSamples drawn per pixel
    uint32_t m_NumBrdfSamples;           // numPrimaryBrdfSamples
    uint32_t m_LocalLightSamplingMode;   // ReSTIRDI_LocalLightSamplingMode_*
    float    m_BrdfCutoff;               // ReSTIRDI_InitialSamplingParameters.brdfCutoff
    //
    uint32_t m_InitialSamplingOutputBufferIndex;
    uint32_t m_TemporalResamplingInputBufferIndex;
    uint32_t m_TemporalResamplingOutputBufferIndex;
    uint32_t m_SpatialResamplingInputBufferIndex;
    //
    uint32_t m_SpatialResamplingOutputBufferIndex;
    uint32_t m_ShadingInputBufferIndex;
    uint32_t m_EnableSky;
    uint32_t m_SpatialNumSamples;        // number of spatial neighbour candidates
    //
    float    m_SpatialSamplingRadius;    // pixel-space search radius
    uint32_t m_SpatialNumDisocclusionBoostSamples;
    Vector2U m_LocalLightPDFTextureSize; // (width, height) of mip 0
    //
    uint32_t m_LocalRISBufferOffset;
    uint32_t m_LocalRISTileSize;
    uint32_t m_LocalRISTileCount;
    uint32_t m_EnvRISBufferOffset;
    //
    uint32_t m_EnvRISTileSize;
    uint32_t m_EnvRISTileCount;
    uint32_t m_EnvSamplingMode;          // 0=Off  1=BRDF(uniform)  2=ReSTIR-DI(importance)
    uint32_t m_EnableRTShadows;          // enables ray-traced shadows in visibility functions
    //
    Vector2U m_EnvPDFTextureSize;        // (width, height) of env PDF mip-0
    float    m_SunIntensity;
    uint32_t pad1;
    //
    Vector3  m_SunDirection;
    uint32_t m_TemporalMaxHistoryLength;
    //
    uint32_t m_TemporalBiasCorrectionMode;
    float    m_TemporalDepthThreshold;
    float    m_TemporalNormalThreshold;
    uint32_t m_TemporalEnableVisibilityShortcut;
    //
    uint32_t m_TemporalEnablePermutationSampling;
    float    m_TemporalPermutationSamplingThreshold;
    uint32_t m_TemporalUniformRandomNumber;
    uint32_t m_TemporalEnableBoilingFilter;
    //
    float    m_TemporalBoilingFilterStrength;
    uint32_t m_SpatialBiasCorrectionMode;
    float    m_SpatialDepthThreshold;
    float    m_SpatialNormalThreshold;
    //
    uint32_t m_SpatialDiscountNaiveSamples;
    uint32_t m_EnableInitialVisibility;
    uint32_t m_EnableFinalVisibility;
    uint32_t m_ReuseFinalVisibility;
    //
    uint32_t m_DiscardInvisibleSamples;
    uint32_t m_FinalVisibilityMaxAge;
    float    m_FinalVisibilityMaxDistance;
    uint32_t pad0;
};

#endif // SHADER_SHARED_H