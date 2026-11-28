#ifndef BASEPASS_COMMON_HLSLI
#define BASEPASS_COMMON_HLSLI

#include "Common.hlsli"
#include "MeshCommon.hlsli"
#include "srrhi/hlsl/BasePass.hlsli"

// The parts of the base pass that every geometry source shares: the per-frame constants, the instance/vertex
// buffers they decode with, and the vertex output the G-buffer pixel shader reads. The vertex-shader path
// (transparent) and the cluster mesh path both go through them, so a debug view or a new interpolant lands in one
// place.

static const srrhi::BasePassConstants g_PerFrame = srrhi::BasePassInputs::GetPerFrameCB();
static const StructuredBuffer<srrhi::PerInstanceData> g_Instances = srrhi::BasePassInputs::GetInstances();
static const StructuredBuffer<srrhi::VertexQuantized> g_Vertices = srrhi::BasePassInputs::GetVertices();

// The cluster's triangle and vertex words live in the group's blob, so element offsets in the cluster record are
// group-local and the group's address in the streaming arena is added to them. The traversal only emits a job for
// a group whose address is valid, so the address is not tested again here.
static const StructuredBuffer<uint> g_ClusterStreamData = srrhi::BasePassInputs::GetClusterStreamData();
static const StructuredBuffer<srrhi::ClusterGroupState> g_ClusterGroupStates = srrhi::BasePassInputs::GetClusterGroupStates();

uint ClusterStreamBase(srrhi::ClusterRecord cluster)
{
    return g_ClusterGroupStates[cluster.m_GroupIndex].m_Address;
}

// A cluster vertex, straight out of the group's blob. Six words per vertex is a VertexQuantized verbatim, so the
// mesh shader never touches the scene-global vertex buffer: the cut's vertices are the only ones the arena has to
// hold (docs/cluster-lod-upstream-review-2026-11-23.md §17.3).
srrhi::VertexQuantized ReadClusterVertex(uint streamBase, srrhi::ClusterRecord cluster, uint localIndex)
{
    const uint at = streamBase + cluster.m_AttributeOffset + localIndex * 6u;
    srrhi::VertexQuantized vq;
    vq.m_Pos.x   = asfloat(g_ClusterStreamData[at + 0u]);
    vq.m_Pos.y   = asfloat(g_ClusterStreamData[at + 1u]);
    vq.m_Pos.z   = asfloat(g_ClusterStreamData[at + 2u]);
    vq.m_Normal  = g_ClusterStreamData[at + 3u];
    vq.m_Uv      = g_ClusterStreamData[at + 4u];
    vq.m_Tangent = g_ClusterStreamData[at + 5u];
    return vq;
}

// The visibility id, 32 bits, zero meaning "no surface":
//     31..30  list      0 = the opaque pass' own list, 1 = the two-pass culled list, 2 = the masked bucket
//     29..7   job + 1   1-based, so no visible job encodes as zero
//     6..0    triangle  the triangle's index inside its cluster
// The raster packs it and the resolve unpacks it, so the shifts live here, where neither side can change one
// without the other.
static const uint kVisListShift     = 30u;
static const uint kVisJobShift      = 7u;
static const uint kVisListMask      = 0x3u;
static const uint kVisJobMask       = 0x7FFFFFu;
static const uint kVisTriangleMask  = 0x7Fu;

uint PackVisId(uint jobIndex, uint triangleIndex)
{
    const uint idList = g_PerFrame.m_VisListIndex & kVisListMask;
    const uint job = (jobIndex + 1u) & kVisJobMask;
    return (idList << kVisListShift) | (job << kVisJobShift) | (triangleIndex & kVisTriangleMask);
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
    nointerpolation uint lodIndex : TEXCOORD4;
    nointerpolation uint clusterID : TEXCOORD6;   // 0xFFFFFFFF outside the cluster mesh path
};

VSOut PrepareVSOut(srrhi::Vertex v, srrhi::PerInstanceData inst, uint instanceID, uint lodIndex)
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
    o.lodIndex = lodIndex;
    // The cluster mesh shader overwrites this for every vertex it emits.
    o.clusterID = 0xFFFFFFFFu;
    return o;
}

#endif // BASEPASS_COMMON_HLSLI
