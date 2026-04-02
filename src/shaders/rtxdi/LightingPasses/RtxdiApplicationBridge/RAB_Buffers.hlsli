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

// All resource declarations come from the srrhi-generated header.
// The ResamplingPassInputs srinput owns all bindings for the main resampling passes.
//
// RTXDI SDK parameter headers are included BEFORE srrhi/hlsl/RTXDI.hlsli.
// The SDK types (RTXDI_LightBufferRegion, ReGIR_*, etc.) are declared as
// "extern" in RTXDI.sr, so srrhi does NOT redefine them in namespace srrhi.
//
// Note: DXC does not allow accessing cbuffer members via namespace::member
// syntax (e.g. srrhi::ResamplingPassInputs_Const is invalid for a cbuffer
// member). Resources are accessed exclusively through the getter functions
// in the srrhi::ResamplingPassInputs namespace.
#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/ReGIR/ReGIRParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>
#include <Rtxdi/Utils/ReservoirAddressing.hlsli>

#include "srrhi/hlsl/RTXDI.hlsli"

// ---------------------------------------------------------------------------
// Constant buffer
// ---------------------------------------------------------------------------
#define g_Const                     srrhi::ResamplingPassInputs::GetConst()

// ---------------------------------------------------------------------------
// SRVs — accessed via getter functions (no direct _* variable access)
// ---------------------------------------------------------------------------
#define t_NeighborOffsets           srrhi::ResamplingPassInputs::GetNeighborOffsets()
#define t_GBufferDepth              srrhi::ResamplingPassInputs::GetGBufferDepth()
#define t_GBufferGeoNormals         srrhi::ResamplingPassInputs::GetGBufferGeoNormals()
#define t_GBufferAlbedo             srrhi::ResamplingPassInputs::GetGBufferAlbedo()
#define t_GBufferORM                srrhi::ResamplingPassInputs::GetGBufferORM()
#define t_GBufferNormals            srrhi::ResamplingPassInputs::GetGBufferNormals()
#define t_PrevGBufferNormals        srrhi::ResamplingPassInputs::GetPrevGBufferNormals()
#define t_PrevGBufferGeoNormals     srrhi::ResamplingPassInputs::GetPrevGBufferGeoNormals()
#define t_PrevGBufferAlbedo         srrhi::ResamplingPassInputs::GetPrevGBufferAlbedo()
#define t_PrevGBufferORM            srrhi::ResamplingPassInputs::GetPrevGBufferORM()
#define t_MotionVectors             srrhi::ResamplingPassInputs::GetMotionVectors()
#define t_DenoiserNormalRoughness   srrhi::ResamplingPassInputs::GetDenoiserNormalRoughness()
#define t_PrevGBufferDepth          srrhi::ResamplingPassInputs::GetPrevGBufferDepth()
#define t_LocalLightPdfTexture      srrhi::ResamplingPassInputs::GetLocalLightPdfTexture()
#define t_EnvironmentPdfTexture     srrhi::ResamplingPassInputs::GetEnvironmentPdfTexture()
#define SceneBVH                    srrhi::ResamplingPassInputs::GetSceneBVH()
#define PrevSceneBVH                srrhi::ResamplingPassInputs::GetPrevSceneBVH()
#define t_LightDataBuffer           srrhi::ResamplingPassInputs::GetLightDataBuffer()
#define t_GBufferEmissive           srrhi::ResamplingPassInputs::GetGBufferEmissive()
#define t_GeometryInstanceToLight   srrhi::ResamplingPassInputs::GetGeometryInstanceToLight()
#define t_InstanceData              srrhi::ResamplingPassInputs::GetInstanceData()
#define t_GeometryData              srrhi::ResamplingPassInputs::GetGeometryData()
#define t_MaterialConstants         srrhi::ResamplingPassInputs::GetMaterialConstants()
#define t_SceneIndices              srrhi::ResamplingPassInputs::GetSceneIndices()
#define t_SceneVertices             srrhi::ResamplingPassInputs::GetSceneVertices()

// UAVs
#define u_LightReservoirs           srrhi::ResamplingPassInputs::GetLightReservoirs()
#define u_RisBuffer                 srrhi::ResamplingPassInputs::GetRisBuffer()
#define u_RisLightDataBuffer        srrhi::ResamplingPassInputs::GetRisLightDataBuffer()
#define u_GIReservoirs              srrhi::ResamplingPassInputs::GetGIReservoirs()
#define u_SecondaryGBuffer          srrhi::ResamplingPassInputs::GetSecondaryGBuffer()
#define u_DiffuseLighting           srrhi::ResamplingPassInputs::GetDiffuseLighting()
#define u_SpecularLighting          srrhi::ResamplingPassInputs::GetSpecularLighting()

// RTXDI macro aliases (SDK expects these names)
#define RTXDI_RIS_BUFFER                u_RisBuffer
#define RTXDI_LIGHT_RESERVOIR_BUFFER    u_LightReservoirs
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER   t_NeighborOffsets
#define RTXDI_GI_RESERVOIR_BUFFER       u_GIReservoirs
#define IES_SAMPLER                     SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX]

// RTXDIConstants aliases — expose srrhi::RTXDIConstants values as bare names
// used throughout the bridge and lighting pass shaders.
#define INSTANCE_MASK_OPAQUE             srrhi::RTXDIConstants::INSTANCE_MASK_OPAQUE
#define INSTANCE_MASK_ALPHA_TESTED       srrhi::RTXDIConstants::INSTANCE_MASK_ALPHA_TESTED
#define INSTANCE_MASK_TRANSPARENT        srrhi::RTXDIConstants::INSTANCE_MASK_TRANSPARENT
#define INSTANCE_MASK_ALL                srrhi::RTXDIConstants::INSTANCE_MASK_ALL
#define DENOISER_MODE_OFF                srrhi::RTXDIConstants::DENOISER_MODE_OFF
#define DENOISER_MODE_REBLUR             srrhi::RTXDIConstants::DENOISER_MODE_REBLUR
#define DENOISER_MODE_RELAX              srrhi::RTXDIConstants::DENOISER_MODE_RELAX
#define BACKGROUND_DEPTH                 srrhi::RTXDIConstants::BACKGROUND_DEPTH
#define INDIRECT_LIGHTING_MODE_NONE      srrhi::RTXDIConstants::INDIRECT_LIGHTING_MODE_NONE
#define INDIRECT_LIGHTING_MODE_BRDF      srrhi::RTXDIConstants::INDIRECT_LIGHTING_MODE_BRDF
#define INDIRECT_LIGHTING_MODE_RESTIRGI  srrhi::RTXDIConstants::INDIRECT_LIGHTING_MODE_RESTIRGI
#define kSecondaryGBuffer_IsSpecularRay    srrhi::RTXDIConstants::kSecondaryGBuffer_IsSpecularRay
#define kSecondaryGBuffer_IsDeltaSurface   srrhi::RTXDIConstants::kSecondaryGBuffer_IsDeltaSurface
#define kSecondaryGBuffer_IsEnvironmentMap srrhi::RTXDIConstants::kSecondaryGBuffer_IsEnvironmentMap
#define RTXDI_SCREEN_SPACE_GROUP_SIZE    srrhi::RTXDIConstants::RTXDI_SCREEN_SPACE_GROUP_SIZE
#define RTXDI_PRESAMPLING_GROUP_SIZE     srrhi::RTXDIConstants::RTXDI_PRESAMPLING_GROUP_SIZE
#define RTXDI_GRAD_FACTOR                srrhi::RTXDIConstants::RTXDI_GRAD_FACTOR
#define RTXDI_GRAD_STORAGE_SCALE         srrhi::RTXDIConstants::RTXDI_GRAD_STORAGE_SCALE
#define RTXDI_GRAD_MAX_VALUE             srrhi::RTXDIConstants::RTXDI_GRAD_MAX_VALUE

// Translates a light index between frames.
// Since lights don't stream in/out, the index is always stable — return it unchanged.
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex);
}

#endif // RAB_BUFFER_HLSLI