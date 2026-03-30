/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#pragma pack_matrix(row_major)

#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/ReGIR/ReGIRParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>
#include "srrhi/hlsl/RTXDI.hlsli"

#include "../CommonLighting.hlsli"

#ifdef WITH_NRD
#include <NRD.hlsli>
#endif

#define g_Const                  srrhi::CompositingPassInputs::GetConst()
#define t_GBufferAlbedo          srrhi::CompositingPassInputs::GetGBufferAlbedo()
#define t_GBufferORM             srrhi::CompositingPassInputs::GetGBufferORM()
#define t_GBufferEmissive        srrhi::CompositingPassInputs::GetGBufferEmissive()
#define t_DiffuseIllumination    srrhi::CompositingPassInputs::GetDiffuseIllumination()
#define t_SpecularIllumination   srrhi::CompositingPassInputs::GetSpecularIllumination()
#define DENOISER_MODE_RELAX      srrhi::RTXDIConstants::DENOISER_MODE_RELAX

float4 CompositingPass_PSMain(FullScreenVertexOut input) : SV_Target
{
    int2 pixelPos = int2(input.pos.xy);

    // Read G-buffer — no unpacking needed, hardware resolves UNORM to float.
    float3 baseColor = t_GBufferAlbedo[pixelPos].rgb;
    float2 orm       = t_GBufferORM[pixelPos];
    float  roughness = orm.r;
    float  metallic  = orm.g;
    float3 emissive  = t_GBufferEmissive[pixelPos].rgb;

    // Derive diffuseAlbedo and specularF0 from the metallic workflow.
    float3 diffuseAlbedo, specularF0;
    GetReflectivityFromMetallic(metallic, baseColor, diffuseAlbedo, specularF0);

    // Load DI illumination (demodulated — multiply by albedo/F0 to reconstruct radiance).
    float4 diffuse_illumination  = t_DiffuseIllumination[pixelPos];
    float4 specular_illumination = t_SpecularIllumination[pixelPos];

#ifdef WITH_NRD
    if (g_Const.denoiserMode == DENOISER_MODE_RELAX)
    {
        diffuse_illumination  = RELAX_BackEnd_UnpackRadiance(diffuse_illumination);
        specular_illumination = RELAX_BackEnd_UnpackRadiance(specular_illumination);
    }
#endif

    // Remodulate: multiply demodulated illumination by albedo/F0.
    float3 color = diffuse_illumination.rgb  * diffuseAlbedo
                 + specular_illumination.rgb * max(float3(0.01f, 0.01f, 0.01f), specularF0)
                 + emissive;

    if (any(isnan(color)))
        color = float3(1.0f, 0.0f, 0.0f);

    return float4(color, 1.0f);
}
