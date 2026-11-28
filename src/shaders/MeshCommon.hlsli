#ifndef MESH_COMMON_HLSLI
#define MESH_COMMON_HLSLI

#include "Common.hlsli"
#include "srrhi/hlsl/Common.hlsli"
#include "srrhi/hlsl/Mesh.hlsli"
#include "srrhi/hlsl/Instance.hlsli"

srrhi::Vertex UnpackVertex(srrhi::VertexQuantized vq)
{
  srrhi::Vertex v;
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

// Unpack a packed triangle index triple.
uint3 UnpackTriangle(uint packed)
{
    return uint3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF);
}

#endif // MESH_COMMON_HLSLI
