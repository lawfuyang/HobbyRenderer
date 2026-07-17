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

// ---------------------------------------------------------------------------
// Amplification Shader shared helpers
// ---------------------------------------------------------------------------

// Unpack the float16-encoded bounding sphere from a Meshlet.
void UnpackMeshletBV(srrhi::Meshlet m, out float3 center, out float radius)
{
    center.x = f16tof32(m.m_CenterRadius[0] & 0xFFFF);
    center.y = f16tof32(m.m_CenterRadius[0] >> 16);
    center.z = f16tof32(m.m_CenterRadius[1] & 0xFFFF);
    radius   = f16tof32(m.m_CenterRadius[1] >> 16);
}

// Decode a MeshletJob and resolve the per-thread meshlet.
// Returns true if this thread's meshlet index is within bounds for the LOD.
// Outputs: instanceIndex, lodIndex, meshletIndex (local), absoluteMeshletIndex, inst (PerInstanceData), meshlet (Meshlet).
bool AS_DecodeMeshletIndex(
    srrhi::MeshletJob                        job,
    StructuredBuffer<srrhi::PerInstanceData> instances,
    StructuredBuffer<srrhi::MeshData>        meshData,
    uint3                                    groupThreadID,
    uint3                                    groupId,
    out uint                                 instanceIndex,
    out uint                                 lodIndex,
    out uint                                 meshletIndex,
    out uint                                 absoluteMeshletIndex,
    out srrhi::PerInstanceData               inst,
    out srrhi::Meshlet                       meshlet,
    StructuredBuffer<srrhi::Meshlet>         meshlets)
{
    instanceIndex = job.m_InstanceIndex;
    lodIndex      = job.m_LODIndex;

    uint meshletOffset   = groupId.x * srrhi::CommonConsts::kThreadsPerGroup;
    meshletIndex         = meshletOffset + groupThreadID.x;

    inst = instances[instanceIndex];
    srrhi::MeshData mesh = meshData[inst.m_MeshDataIndex];

    absoluteMeshletIndex = 0;
    meshlet              = (srrhi::Meshlet)0;

    if (meshletIndex >= mesh.m_MeshletCounts[lodIndex])
        return false;

    absoluteMeshletIndex = mesh.m_MeshletOffsets[lodIndex] + meshletIndex;
    meshlet              = meshlets[absoluteMeshletIndex];
    return true;
}

// Write the instance/LOD header into the groupshared payload (thread 0 only).
// PayloadType must have m_InstanceIndex, m_LODIndex, m_MeshletIndices[] members.
#define AS_WritePayloadHeader(payload, instanceIdx, lodIdx, groupThreadID) \
    if ((groupThreadID).x == 0)                                            \
    {                                                                      \
        (payload).m_InstanceIndex = (instanceIdx);                         \
        (payload).m_LODIndex      = (lodIdx);                              \
    }

// Wave-compact visible meshlets into the payload and call DispatchMesh.
// Must be called after all visibility tests; bVisible and absoluteMeshletIndex
// are per-thread inputs.
#define AS_CompactAndDispatch(payload, bVisible, absoluteMeshletIdx)           \
    if (bVisible)                                                               \
    {                                                                           \
        uint payloadIdx = WavePrefixCountBits(bVisible);                       \
        (payload).m_MeshletIndices[payloadIdx] = (absoluteMeshletIdx);         \
    }                                                                           \
    DispatchMesh(WaveActiveCountBits(bVisible), 1, 1, payload);

// Unpack a packed triangle index triple.
uint3 UnpackTriangle(uint packed)
{
    return uint3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF);
}

#endif // MESH_COMMON_HLSLI
