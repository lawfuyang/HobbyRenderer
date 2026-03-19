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

#include "SharedShaderInclude/ShaderParameters.h"
#include "../CommonLighting.hlsli"

#ifdef WITH_NRD
#include <NRD.hlsli>
#endif

ConstantBuffer<ResamplingConstants> g_Const : register(b0);

// G-buffer inputs — slots match RTXDIRenderer.cpp CompositingPass binding set
// t3  = GBufferAlbedo   (RGBA8_UNORM: baseColor.rgb + alpha)
// t4  = GBufferORM      (RG8_UNORM:  roughness=.r, metallic=.g)
// t22 = GBufferEmissive (RGBA8_UNORM)
// t23 = DI diffuse illumination  (denoised or raw)
// t24 = DI specular illumination (denoised or raw)
Texture2D<float4> t_GBufferAlbedo        : register(t3);
Texture2D<float2> t_GBufferORM           : register(t4);
Texture2D<float4> t_GBufferEmissive      : register(t22);
Texture2D<float4> t_DiffuseIllumination  : register(t23);
Texture2D<float4> t_SpecularIllumination : register(t24);

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
