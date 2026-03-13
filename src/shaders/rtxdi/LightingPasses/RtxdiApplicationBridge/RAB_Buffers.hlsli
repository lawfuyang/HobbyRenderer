/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RAB_BUFFER_HLSLI
#define RAB_BUFFER_HLSLI

#include <SharedShaderInclude/ShaderParameters.h>
#include <SharedShaderInclude/ShaderDebug/ShaderDebugPrintShared.h>
#include <SharedShaderInclude/ShaderDebug/PTPathViz/PTPathVertexRecord.h>
#include <SharedShaderInclude/ShaderDebug/PTPathViz/PTPathSetRecord.h>

// ---- Constant buffers ----
ConstantBuffer<ResamplingConstants> g_Const : register(b0);

// ---- G-buffer SRVs (match RTXDIRenderer.cpp binding layout) ----
// t0  = t_NeighborOffsets
// t1  = t_GBufferDepth
// t2  = (dummy — GeoNormals removed, use shading normals directly)
// t3  = t_GBufferDiffuseAlbedo
// t4  = t_GBufferSpecularRough  (ORM)
// t5  = t_GBufferNormals
// t6  = t_PrevGBufferNormals    (= m_GbufferNormalsHistory)
// t7  = (dummy — PrevGeoNormals removed)
// t8  = t_PrevGBufferDiffuseAlbedo  (= m_GBufferAlbedoHistory)
// t9  = t_PrevGBufferSpecularRough  (= m_GBufferORMHistory)
// t10 = (dummy — was PrevRestirLuminance, no Gradient pass)
// t11 = t_MotionVectors
// t12 = t_DenoiserNormalRoughness
// t13 = t_PrevDepth
// t14 = t_LocalLightPdfTexture
// t15 = t_EnvironmentPdfTexture
// t16 = t_RisBuffer
// t17 = t_RisLightDataBuffer
// t18 = SceneBVH
// t19 = PrevSceneBVH
// t20 = t_LightDataBuffer
// t21 = (dummy — was LightIndexMapping, lights don't stream in/out)
// t22 = t_GBufferEmissive
// t25 = t_GeometryInstanceToLight
// t26 = t_InstanceData  (PerInstanceData)
// t27 = t_GeometryData  (MeshData)
// t28 = t_MaterialConstants

Buffer<float2>                              t_NeighborOffsets           : register(t0);
Texture2D<float>                            t_GBufferDepth              : register(t1);
// t2: dummy (GeoNormals removed)
Texture2D<uint>                             t_GBufferDiffuseAlbedo      : register(t3);
Texture2D<uint>                             t_GBufferSpecularRough      : register(t4);
Texture2D<uint>                             t_GBufferNormals            : register(t5);
Texture2D<uint>                             t_PrevGBufferNormals        : register(t6);  // = m_GbufferNormalsHistory
// t7: dummy (PrevGeoNormals removed)
Texture2D<uint>                             t_PrevGBufferDiffuseAlbedo  : register(t8);  // = m_GBufferAlbedoHistory
Texture2D<uint>                             t_PrevGBufferSpecularRough  : register(t9);  // = m_GBufferORMHistory
// t10: dummy (no Gradient pass — PrevRestirLuminance removed)
Texture2D<float4>                           t_MotionVectors             : register(t11);
Texture2D<float4>                           t_DenoiserNormalRoughness   : register(t12);
Texture2D<float>                            t_PrevGBufferDepth          : register(t13);
Texture2D                                   t_LocalLightPdfTexture      : register(t14);
Texture2D                                   t_EnvironmentPdfTexture     : register(t15);
StructuredBuffer<uint2>                     t_RisBuffer                 : register(t16);
StructuredBuffer<uint4>                     t_RisLightDataBuffer        : register(t17);
RaytracingAccelerationStructure             SceneBVH                    : register(t18);
RaytracingAccelerationStructure             PrevSceneBVH                : register(t19);
StructuredBuffer<PolymorphicLightInfo>      t_LightDataBuffer           : register(t20);
// t21: dummy (no LightIndexMapping — lights don't stream in/out)
Texture2D<float4>                           t_GBufferEmissive           : register(t22);
StructuredBuffer<uint>                      t_GeometryInstanceToLight   : register(t25);
StructuredBuffer<PerInstanceData>           t_InstanceData              : register(t26);
StructuredBuffer<MeshData>                  t_GeometryData              : register(t27);
StructuredBuffer<MaterialConstants>         t_MaterialConstants         : register(t28);
StructuredBuffer<uint>                      t_SceneIndices              : register(t29);
StructuredBuffer<VertexQuantized>           t_SceneVertices             : register(t30);

// ---- UAVs (match RTXDIRenderer.cpp binding layout) ----
// u0  = u_LightReservoirs
// u1  = u_RisBuffer
// u2  = u_RisLightDataBuffer
// u3  = u_TemporalSamplePositions (dummy)
// u4  = u_Gradients (dummy)
// u5  = u_RestirLuminance (dummy)
// u6  = u_GIReservoirs (stub)
// u7  = u_PTReservoirs (stub)
// u8  = u_DiffuseLighting
// u9  = u_SpecularLighting
// u10 = u_DiffuseConfidence (stub)
// u11 = u_SpecularConfidence (stub)
// u12 = u_RayCountBuffer
// u13 = u_SecondaryGBuffer
// u14 = u_SecondarySurfaces (stub / PT path viz)
// u15 = u_DebugColor (stub / PT path viz)
// u16 = u_DebugPrintBuffer
// u17 = u_DirectLightingRaw
// u18 = u_IndirectLightingRaw
// u20 = u_PSRDepth
// u21 = u_PSRNormalRoughness (= DenoiserNormalRoughness)
// u22 = u_PSRMotionVectors
// u23 = u_PSRHitT
// u24 = u_PSRDiffuseAlbedo
// u25 = u_PSRSpecularF0
// u26 = u_PSRLightDir
// u27 = u_PTSampleIDTexture
// u28 = u_PTDuplicationMap

RWStructuredBuffer<RTXDI_PackedDIReservoir> u_LightReservoirs           : register(u0);
RWBuffer<uint2>                             u_RisBuffer                 : register(u1);
RWBuffer<uint4>                             u_RisLightDataBuffer        : register(u2);
RWTexture2D<int2>                           u_TemporalSamplePositions   : register(u3);
RWTexture2DArray<float4>                    u_Gradients                 : register(u4);
RWTexture2D<float2>                         u_RestirLuminance           : register(u5);
RWStructuredBuffer<RTXDI_PackedGIReservoir> u_GIReservoirs              : register(u6);
RWStructuredBuffer<RTXDI_PackedPTReservoir> u_PTReservoirs              : register(u7);
RWTexture2D<float4>                         u_DiffuseLighting           : register(u8);
RWTexture2D<float4>                         u_SpecularLighting          : register(u9);
RWBuffer<uint>                              u_RayCountBuffer            : register(u12);
RWStructuredBuffer<SecondaryGBufferData>    u_SecondaryGBuffer          : register(u13);

// PT Path Viz
RWStructuredBuffer<PTPathVertexRecord>      u_debugPathRecord           : register(u14);
RWStructuredBuffer<PTPathSet>               u_debugPathSet              : register(u15);
#define DEBUG_PT_VERTEX_RECORD_BUFFER u_debugPathRecord
#define DEBUG_PT_PATH_SET_BUFFER u_debugPathSet

// Debug Print
ConstantBuffer<ShaderPrintCBData>           g_debugPrintCB              : register(b2);
RWByteAddressBuffer                         u_DebugPrintBuffer          : register(u16);
#define RTXDI_SHADER_DEBUG_PRINT_CB g_debugPrintCB
#define RTXDI_SHADER_DEBUG_PRINT_OUTPUT_BUFFER u_DebugPrintBuffer

// Debug Lighting Buffers
RWTexture2D<float4>                         u_DirectLightingRaw         : register(u17);
RWTexture2D<float4>                         u_IndirectLightingRaw       : register(u18);

// PSR UAVs
RWTexture2D<float>                          u_PSRDepth                  : register(u20);
RWTexture2D<float4>                         u_PSRNormalRoughness        : register(u21);
RWTexture2D<float4>                         u_PSRMotionVectors          : register(u22);
RWTexture2D<float>                          u_PSRHitT                   : register(u23);
RWTexture2D<uint>                           u_PSRDiffuseAlbedo          : register(u24);
RWTexture2D<uint>                           u_PSRSpecularF0             : register(u25);
RWTexture2D<uint>                           u_PSRLightDir               : register(u26);
RWTexture2D<uint>                           u_PTSampleIDTexture         : register(u27);
RWTexture2D<uint>                           u_PTDuplicationMap          : register(u28);

// RTXDI macro aliases
#define RTXDI_RIS_BUFFER                u_RisBuffer
#define RTXDI_LIGHT_RESERVOIR_BUFFER    u_LightReservoirs
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER   t_NeighborOffsets
#define RTXDI_GI_RESERVOIR_BUFFER       u_GIReservoirs
#define RTXDI_PT_RESERVOIR_BUFFER       u_PTReservoirs
#define IES_SAMPLER                     SamplerDescriptorHeap[SAMPLER_LINEAR_CLAMP_INDEX]

// Converts a motion vector from NDC-space XY to pixel-space XY offset.
float3 convertMotionVectorToPixelSpace(
    PlanarViewConstants view,
    PlanarViewConstants prevView,
    int2 pixelPosition,
    float3 motionVector)
{
    float2 pixelMotion = motionVector.xy * view.m_ClipToWindowScale;
    return float3(pixelMotion, motionVector.z);
}

// Translates a light index between frames.
// Since lights don't stream in/out, the index is always stable — return it unchanged.
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex);
}

// Duplication map: count of pixels sharing the same sample ID (for MCap).
uint RAB_GetDuplicationMapCount(int2 prevPixelPos)
{
    int2 dim = int2(g_Const.view.m_ViewportSize);
    if (any(prevPixelPos < 0) || any(prevPixelPos >= dim))
        return 0u;
    return u_PTDuplicationMap[prevPixelPos];
}

#endif // RAB_BUFFER_HLSLI