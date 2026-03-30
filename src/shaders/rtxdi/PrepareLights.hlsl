/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 */

#pragma pack_matrix(row_major)

#include "../Packing.hlsli"
#include "../Bindless.hlsli"
#include "../RaytracingCommon.hlsli"
#include <Rtxdi/Utils/Math.hlsli>
#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/ReGIR/ReGIRParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>
#include "srrhi/hlsl/RTXDI.hlsli"

// ---------------------------------------------------------------------------
// Resource accessors — obtained via srrhi getter functions.
// No 'using namespace srrhi' and no direct _* variable access.
// ---------------------------------------------------------------------------
static const srrhi::PrepareLightsConstants        g_Const                   = srrhi::PrepareLightsInputs::GetConst();
static const StructuredBuffer<srrhi::PrepareLightsTask>       t_TaskBuffer             = srrhi::PrepareLightsInputs::GetTaskBuffer();
static const StructuredBuffer<srrhi::PolymorphicLightInfo>    t_PrimitiveLightBuffer   = srrhi::PrepareLightsInputs::GetPrimitiveLightBuffer();
static const StructuredBuffer<srrhi::PerInstanceData>         t_InstanceData           = srrhi::PrepareLightsInputs::GetInstanceData();
static const StructuredBuffer<srrhi::MeshData>                t_GeometryData           = srrhi::PrepareLightsInputs::GetGeometryData();
static const StructuredBuffer<srrhi::MaterialConstants>       t_MaterialConstants      = srrhi::PrepareLightsInputs::GetMaterialConstants();
static const StructuredBuffer<uint>                           t_SceneIndices           = srrhi::PrepareLightsInputs::GetSceneIndices();
static const StructuredBuffer<srrhi::VertexQuantized>         t_SceneVertices          = srrhi::PrepareLightsInputs::GetSceneVertices();
static const RWStructuredBuffer<srrhi::PolymorphicLightInfo>  u_LightDataBuffer        = srrhi::PrepareLightsInputs::GetLightDataBuffer();
static const RWBuffer<uint>                                   u_LightIndexMappingBuffer = srrhi::PrepareLightsInputs::GetLightIndexMappingBuffer();
static const RWTexture2D<float4>                              u_LocalLightPdfTexture   = srrhi::PrepareLightsInputs::GetLocalLightPdfTexture();

static const uint TASK_PRIMITIVE_LIGHT_BIT = srrhi::RTXDIConstants::TASK_PRIMITIVE_LIGHT_BIT;

#define ENVIRONMENT_SAMPLER SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX]
#define IES_SAMPLER         SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX]
#include "PolymorphicLight.hlsli"

bool FindTask(uint dispatchThreadId, out srrhi::PrepareLightsTask task)
{
    int left = 0;
    int right = int(g_Const.numTasks) - 1;

    while (right >= left)
    {
        int middle = (left + right) / 2;
        task = t_TaskBuffer[middle];

        int tri = int(dispatchThreadId) - int(task.lightBufferOffset);

        if (tri < 0)
            right = middle - 1;
        else if (tri < int(task.triangleCount))
            return true;
        else
            left = middle + 1;
    }

    return false;
}

[numthreads(256, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
    srrhi::PrepareLightsTask task = (srrhi::PrepareLightsTask)0;

    if (!FindTask(dispatchThreadId, task))
        return;

    uint triangleIdx = dispatchThreadId - task.lightBufferOffset;
    bool isPrimitiveLight = (task.instanceAndGeometryIndex & TASK_PRIMITIVE_LIGHT_BIT) != 0;

    srrhi::PolymorphicLightInfo lightInfo = (srrhi::PolymorphicLightInfo)0;

    if (!isPrimitiveLight)
    {
        // Decode instance index (high 19 bits) and geometry sub-index (low 12 bits)
        uint instanceIndex  = (task.instanceAndGeometryIndex >> 12) & 0x7FFFFu;
        uint geometrySubIdx = task.instanceAndGeometryIndex & 0xFFFu;

        srrhi::PerInstanceData instance = t_InstanceData[instanceIndex];
        srrhi::MeshData geometry = t_GeometryData[instance.m_MeshDataIndex];
        srrhi::MaterialConstants material = t_MaterialConstants[instance.m_MaterialIndex];

        // Get triangle vertex positions using the shared helper
        // Always use LOD 0 for light preparation: the light buffer is sized
        // for LOD 0 triangle counts (set on the CPU in PostSceneLoad), so
        // triangleIdx is a LOD 0 primitive index.  Using the dynamic
        // instance.m_LODIndex here would read from a different (smaller)
        // index range, producing wrong geometry and out-of-bounds accesses.
        uint lodIndex = 0;
        TriangleVertices tv = GetTriangleVertices(triangleIdx, lodIndex, geometry, t_SceneIndices, t_SceneVertices);

        float3 positions[3];
        positions[0] = MatrixMultiply(float4(tv.v0.m_Pos, 1.0f), instance.m_World).xyz;
        positions[1] = MatrixMultiply(float4(tv.v1.m_Pos, 1.0f), instance.m_World).xyz;
        positions[2] = MatrixMultiply(float4(tv.v2.m_Pos, 1.0f), instance.m_World).xyz;

        // Emissive radiance
        float3 radiance = material.m_EmissiveFactor.rgb;

        // Apply emissive texture if present
        if ((material.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_EMISSIVE) != 0)
        {
            // Interpolate UVs for the triangle centroid
            float2 uv0 = tv.v0.m_Uv;
            float2 uv1 = tv.v1.m_Uv;
            float2 uv2 = tv.v2.m_Uv;

            // Calculate UV-space gradients for anisotropic sampling
            float2 edges[3];
            edges[0] = uv1 - uv0;
            edges[1] = uv2 - uv1;
            edges[2] = uv0 - uv2;

            float3 edgeLengths;
            edgeLengths[0] = length(edges[0]);
            edgeLengths[1] = length(edges[1]);
            edgeLengths[2] = length(edges[2]);

            float2 shortEdge, longEdge1, longEdge2;
            if (edgeLengths[0] < edgeLengths[1] && edgeLengths[0] < edgeLengths[2])
            {
                shortEdge = edges[0]; longEdge1 = edges[1]; longEdge2 = edges[2];
            }
            else if (edgeLengths[1] < edgeLengths[2])
            {
                shortEdge = edges[1]; longEdge1 = edges[2]; longEdge2 = edges[0];
            }
            else
            {
                shortEdge = edges[2]; longEdge1 = edges[0]; longEdge2 = edges[1];
            }

            float2 shortGradient = shortEdge * (2.0f / 3.0f);
            float2 longGradient  = (longEdge1 + longEdge2) / 3.0f;
            float2 centerUV      = (uv0 + uv1 + uv2) / 3.0f;

            float3 emissiveMask = SampleBindlessTextureGrad(material.m_EmissiveTextureIndex, material.m_EmissiveSamplerIndex, centerUV, shortGradient, longGradient).rgb;
            radiance *= emissiveMask;
        }

        radiance = max(0, radiance);

        TriangleLight triLight;
        triLight.base     = positions[0];
        triLight.edge1    = positions[1] - positions[0];
        triLight.edge2    = positions[2] - positions[0];
        triLight.radiance = radiance;

        float3 crossProduct = cross(triLight.edge1, triLight.edge2);
        float crossLen = length(crossProduct);
        triLight.normal      = (crossLen > 0.0f) ? (crossProduct / crossLen) : float3(0, 1, 0);
        triLight.surfaceArea = crossLen * 0.5f;

        lightInfo = triLight.Store();
    }
    else
    {
        // Primitive (analytical) light — read directly from the primitive light buffer.
        // Match FullSample semantics: primitive source index is encoded in
        // instanceAndGeometryIndex low bits, while lightBufferOffset is the destination
        // slot in u_LightDataBuffer.
        uint primitiveLightIndex = task.instanceAndGeometryIndex & ~TASK_PRIMITIVE_LIGHT_BIT;
        lightInfo = t_PrimitiveLightBuffer[primitiveLightIndex];
    }

    uint lightBufferPtr = task.lightBufferOffset + triangleIdx;
    u_LightDataBuffer[g_Const.currentFrameLightOffset + lightBufferPtr] = lightInfo;

    // Write light index mapping for temporal resampling
    if (task.previousLightBufferOffset >= 0)
    {
        uint prevBufferPtr = uint(task.previousLightBufferOffset) + triangleIdx;

        u_LightIndexMappingBuffer[g_Const.previousFrameLightOffset + prevBufferPtr] =
            g_Const.currentFrameLightOffset + lightBufferPtr + 1;

        u_LightIndexMappingBuffer[g_Const.currentFrameLightOffset + lightBufferPtr] =
            g_Const.previousFrameLightOffset + prevBufferPtr + 1;
    }

    // Write flux into the local-light PDF texture (mip 0) via Z-curve addressing.
    float emissiveFlux = PolymorphicLight::getPower(lightInfo);
    uint2 pdfTexturePosition = RTXDI_LinearIndexToZCurve(lightBufferPtr);
    u_LocalLightPdfTexture[pdfTexturePosition] = float4(emissiveFlux, 0, 0, 0);
}
