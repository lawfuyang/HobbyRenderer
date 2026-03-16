/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 */

#pragma pack_matrix(row_major)

#include "../Packing.hlsli"
#include "../Bindless.hlsli"
#include "../RaytracingCommon.hlsli"
#include <Rtxdi/Utils/Math.hlsli>
#include "SharedShaderInclude/ShaderParameters.h"

// ---- Resource bindings (must match RTXDIRenderer.cpp PrepareLights binding set) ----
ConstantBuffer<PrepareLightsConstants>      g_Const             : register(b0);
RWStructuredBuffer<PolymorphicLightInfo>    u_LightDataBuffer   : register(u0);
RWBuffer<uint>                              u_LightIndexMappingBuffer : register(u1);
RWBuffer<uint>                              u_GeometryInstanceToLight : register(u2);
RWTexture2D<float>                          u_LocalLightPdfTexture : register(u4);

// t0 = task buffer: one entry per task.  TASK_PRIMITIVE_LIGHT_BIT set → analytical light;
//      bit clear → emissive triangle mesh.
StructuredBuffer<PrepareLightsTask>         t_TaskBuffer        : register(t0);
// t1 = CPU-converted analytical lights (directional/point/spot)
StructuredBuffer<PolymorphicLightInfo>      t_PrimitiveLightBuffer : register(t1);

StructuredBuffer<PerInstanceData>           t_InstanceData      : register(t26);
StructuredBuffer<MeshData>                  t_GeometryData      : register(t27);
StructuredBuffer<MaterialConstants>         t_MaterialConstants : register(t28);

// Bindless scene geometry buffers (bound via bIncludeBindlessResources = true)
StructuredBuffer<uint>                      t_SceneIndices      : register(t29);
StructuredBuffer<VertexQuantized>           t_SceneVertices     : register(t30);

#define ENVIRONMENT_SAMPLER SamplerDescriptorHeap[SAMPLER_LINEAR_CLAMP_INDEX]
#define IES_SAMPLER         SamplerDescriptorHeap[SAMPLER_LINEAR_CLAMP_INDEX]
#include "PolymorphicLight.hlsli"

bool FindTask(uint dispatchThreadId, out PrepareLightsTask task)
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
    PrepareLightsTask task = (PrepareLightsTask)0;

    if (!FindTask(dispatchThreadId, task))
        return;

    uint triangleIdx = dispatchThreadId - task.lightBufferOffset;
    bool isPrimitiveLight = (task.instanceAndGeometryIndex & TASK_PRIMITIVE_LIGHT_BIT) != 0;

    PolymorphicLightInfo lightInfo = (PolymorphicLightInfo)0;

    if (!isPrimitiveLight)
    {
        // Decode instance index (high 19 bits) and geometry sub-index (low 12 bits)
        uint instanceIndex  = (task.instanceAndGeometryIndex >> 12) & 0x7FFFFu;
        uint geometrySubIdx = task.instanceAndGeometryIndex & 0xFFFu;

        PerInstanceData instance = t_InstanceData[instanceIndex];
        MeshData        geometry = t_GeometryData[instance.m_MeshDataIndex];
        MaterialConstants material = t_MaterialConstants[instance.m_MaterialIndex];

        // Get triangle vertex positions using the shared helper
        uint lodIndex = instance.m_LODIndex;
        TriangleVertices tv = GetTriangleVertices(triangleIdx, lodIndex, geometry, t_SceneIndices, t_SceneVertices);

        float3 positions[3];
        positions[0] = MatrixMultiply(float4(tv.v0.m_Pos, 1.0f), instance.m_World).xyz;
        positions[1] = MatrixMultiply(float4(tv.v1.m_Pos, 1.0f), instance.m_World).xyz;
        positions[2] = MatrixMultiply(float4(tv.v2.m_Pos, 1.0f), instance.m_World).xyz;

        // Emissive radiance
        float3 radiance = material.m_EmissiveFactor.rgb;

        // Apply emissive texture if present
        if ((material.m_TextureFlags & TEXFLAG_EMISSIVE) != 0)
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
        // instanceAndGeometryIndex with TASK_PRIMITIVE_LIGHT_BIT set; the actual
        // primitive light index is stored in task.lightBufferOffset.
        lightInfo = t_PrimitiveLightBuffer[task.lightBufferOffset];
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
    u_LocalLightPdfTexture[pdfTexturePosition] = emissiveFlux;

    // Write the geometry-instance-to-light mapping for emissive triangle lights.
    if (!isPrimitiveLight)
    {
        uint instanceIndex = (task.instanceAndGeometryIndex >> 12) & 0x7FFFFu;
        PerInstanceData instance = t_InstanceData[instanceIndex];
        uint geometryInstanceIndex = instance.m_FirstGeometryInstanceIndex + (task.instanceAndGeometryIndex & 0xFFFu);
        u_GeometryInstanceToLight[geometryInstanceIndex] = lightBufferPtr;
    }
}
