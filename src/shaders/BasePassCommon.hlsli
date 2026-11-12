#ifndef BASEPASS_COMMON_HLSLI
#define BASEPASS_COMMON_HLSLI

#include "Common.hlsli"
#include "MeshCommon.hlsli"
#include "srrhi/hlsl/BasePass.hlsli"

// The parts of the base pass that every geometry source shares: the per-frame constants, the instance/vertex
// buffers they decode with, and the vertex output the G-buffer pixel shader reads. The vertex-shader path
// (transparent), the meshlet amplification/mesh path and the phase-2 cluster mesh path all go through them, so
// a debug view or a new interpolant lands in one place.

static const srrhi::BasePassConstants g_PerFrame = srrhi::BasePassInputs::GetPerFrameCB();
static const StructuredBuffer<srrhi::PerInstanceData> g_Instances = srrhi::BasePassInputs::GetInstances();
static const StructuredBuffer<srrhi::VertexQuantized> g_Vertices = srrhi::BasePassInputs::GetVertices();

float3 HashColor(uint id)
{
    uint h = id * 0x27D4EB2Du;
    h = h ^ (h >> 15);
    h = h * 0x85EBCA6Bu;
    h = h ^ (h >> 13);
    h = h * 0xC2B2AE35u;
    h = h ^ (h >> 16);
    return float3((h & 0xFF) / 255.0f, ((h >> 8) & 0xFF) / 255.0f, ((h >> 16) & 0xFF) / 255.0f);
}

struct VSOut
{
    float4 Position : SV_POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 prevWorldPos : TEXCOORD5;
    nointerpolation uint instanceID : TEXCOORD2;
    nointerpolation uint meshletID : TEXCOORD3;
    nointerpolation uint lodIndex : TEXCOORD4;
    nointerpolation uint clusterID : TEXCOORD6;   // 0xFFFFFFFF outside the cluster mesh path
};

VSOut PrepareVSOut(srrhi::Vertex v, srrhi::PerInstanceData inst, uint instanceID, uint meshletID, uint lodIndex)
{
    VSOut o;
    float4 worldPos = MatrixMultiply(float4(v.m_Pos, 1.0f), inst.m_World);
    o.Position = MatrixMultiply(worldPos, g_PerFrame.m_View.m_MatWorldToClip);

    o.normal = TransformNormal(v.m_Normal, inst.m_World);
    o.tangent.xyz = TransformNormal(v.m_Tangent.xyz, inst.m_World);
    o.tangent.w = v.m_Tangent.w;
    o.uv = v.m_Uv;
    o.worldPos = worldPos.xyz;
    o.prevWorldPos = MatrixMultiply(float4(v.m_Pos, 1.0f), inst.m_PrevWorld).xyz;
    o.instanceID = instanceID;
    o.meshletID = meshletID;
    o.lodIndex = lodIndex;
    // The cluster mesh shader overwrites this for every vertex it emits.
    o.clusterID = 0xFFFFFFFFu;
    return o;
}

#endif // BASEPASS_COMMON_HLSLI
