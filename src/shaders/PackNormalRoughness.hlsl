// DenoiserHelper pre-pass: converts GBuffer normals + roughness into the
// R10G10B10A2_UNORM format expected by NRD (NRD_NORMAL_ENCODING = R10G10B10A2_UNORM,
// NRD_ROUGHNESS_ENCODING = LINEAR).
//
// Inputs:
//   t0  - g_NormalsTexture       RG16_FLOAT, octahedron-encoded world-space normals
//   t1  - g_ORMTexture           RG8_UNORM,  .x = roughness (linear), .y = metallic
//   u0  - g_PackedNormalRoughness R10G10B10A2_UNORM output
//   b0  - push constants         PackNormalRoughnessConsts

#include "ShaderShared.h"
#include "NRD.hlsli"

// ============================================================================
// Bindings
// ============================================================================

struct PackNormalRoughnessConsts
{
    Vector2U m_OutputResolution;
};

PUSH_CONSTANT
PackNormalRoughnessConsts g_Consts;

Texture2D<float2>   g_NormalsTexture        : register(t0);  // octahedron RG16_FLOAT
Texture2D<float2>   g_ORMTexture            : register(t1);  // RG8_UNORM: .x=roughness

VK_IMAGE_FORMAT_UNKNOWN
RWTexture2D<float4> g_PackedNormalRoughness : register(u0);  // R10G10B10A2_UNORM output

// ============================================================================
// Compute shader
// ============================================================================

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchThreadId.xy;
    if (coord.x >= g_Consts.m_OutputResolution.x || coord.y >= g_Consts.m_OutputResolution.y)
        return;

    // Decode world-space normal (octahedron RG16_FLOAT)
    const float2 encodedNormal = g_NormalsTexture.Load(int3(coord, 0));
    const float3 worldNormal   = DecodeOct(encodedNormal);

    // Read linear roughness from GBuffer ORM (.x)
    const float roughness = g_ORMTexture.Load(int3(coord, 0)).x;

    // Pack into NRD R10G10B10A2_UNORM layout (materialID = 0 → alpha = 0)
    const float3 packed = _NRD_EncodeNormalRoughness101010(worldNormal, roughness);
    g_PackedNormalRoughness[coord] = float4(packed, 0.0);
}
