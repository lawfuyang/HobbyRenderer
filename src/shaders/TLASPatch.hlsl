// TLASPatch.hlsl
// GPU-side TLAS patching compute shader.
//
// Reads the per-instance LOD index buffer written by the GPU culling passes and:
//   1. Writes the correct per-LOD BLAS device address into the RT instance desc buffer
//      via nvrhi::rt::IndirectInstanceDesc.blasDeviceAddress.
//   2. Writes the LOD index into g_InstanceData[instanceIndex].m_LODIndex so that
//      RT shaders can use m_IndexOffsets[m_LODIndex] in GetTriangleVertices.
//
// Dispatch: ceil(instanceCount / 64) thread groups of 64 threads each.
// One thread per instance — no job-count indirection needed.

#include "nvrhi/nvrhiHLSL.h"

#include "srrhi/hlsl/Common.hlsli"
#include "srrhi/hlsl/Instance.hlsli"

// ---- Inputs ----------------------------------------------------------------

// Flat BLAS address table: blasAddresses[instanceIndex * srrhi::CommonConsts::MAX_LOD_COUNT + lodIndex]
// Uploaded once at scene load by Scene::BuildAccelerationStructures.
StructuredBuffer<uint64_t> g_BLASAddresses : register(t0);

// Per-instance LOD index: g_InstanceLOD[instanceIndex] = lodIndex.
// Written each frame by GPUCulling_Culling_CSMain for every visible instance.
StructuredBuffer<uint> g_InstanceLOD : register(t1);

// ---- Outputs ---------------------------------------------------------------

// RT instance desc buffer as typed structs — no manual byte arithmetic needed.
RWStructuredBuffer<nvrhi::rt::IndirectInstanceDesc> g_RTInstanceDescs : register(u0);

// Per-instance data buffer (PerInstanceData structs).
// We write m_LODIndex so RT shaders use the correct index offset.
RWStructuredBuffer<srrhi::PerInstanceData> g_InstanceData : register(u1);

// ---- Push constant ---------------------------------------------------------

// Total number of instances to process.
struct
{
    uint m_InstanceCount;
} g_PC;

[numthreads(64, 1, 1)]
void TLASPatch_CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint instanceIndex = dispatchThreadId.x;
    if (instanceIndex >= g_PC.m_InstanceCount)
        return;

    uint lodIndex = g_InstanceLOD[instanceIndex];

    // Clamp lodIndex to [0, srrhi::CommonConsts::MAX_LOD_COUNT-1] for safety
    lodIndex = min(lodIndex, srrhi::CommonConsts::MAX_LOD_COUNT - 1);

    // Look up the BLAS device address for this instance + LOD and write it directly
    // into the typed struct field — no manual byte offsets required.
    g_RTInstanceDescs[instanceIndex].blasDeviceAddress =
        g_BLASAddresses[instanceIndex * srrhi::CommonConsts::MAX_LOD_COUNT + lodIndex];

    // Write m_LODIndex into the per-instance data buffer
    g_InstanceData[instanceIndex].m_LODIndex = lodIndex;
}
