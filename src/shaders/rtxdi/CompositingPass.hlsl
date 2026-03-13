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
#include "../Packing.hlsli"

#ifdef WITH_NRD
#include <NRD.hlsli>
#endif

ConstantBuffer<ResamplingConstants> g_Const : register(b0);
ConstantBuffer<PerPassConstants>    g_PerPass : register(b1);

// G-buffer inputs (same slots as RAB_Buffers.hlsli)
Texture2D<uint>  t_GBufferDiffuseAlbedo  : register(t3);
Texture2D<uint>  t_GBufferSpecularRough  : register(t4);
Texture2D<float4> t_GBufferEmissive      : register(t22);

// DI illumination inputs — denoised or raw depending on WITH_NRD
Texture2D<float4> t_DiffuseIllumination  : register(t23);
Texture2D<float4> t_SpecularIllumination : register(t24);

float4 CompositingPass_PSMain(FullScreenVertexOut input) : SV_Target
{
    int2 pixelPos = int2(input.pos.xy);

    // Unpack material from packed G-buffer
    float3 diffuseAlbedo = Unpack_R11G11B10_UFLOAT(t_GBufferDiffuseAlbedo[pixelPos]);
    float4 specularRough = Unpack_R8G8B8A8_Gamma_UFLOAT(t_GBufferSpecularRough[pixelPos]);
    float3 specularF0    = specularRough.rgb;
    float3 emissive      = t_GBufferEmissive[pixelPos].rgb;

    // Load DI illumination (demodulated — multiply by albedo to reconstruct radiance)
    float4 diffuse_illumination  = t_DiffuseIllumination[pixelPos];
    float4 specular_illumination = t_SpecularIllumination[pixelPos];

#ifdef WITH_NRD
    if (g_Const.denoiserMode == DENOISER_MODE_RELAX)
    {
        diffuse_illumination  = RELAX_BackEnd_UnpackRadiance(diffuse_illumination);
        specular_illumination = RELAX_BackEnd_UnpackRadiance(specular_illumination);
    }
#endif

    // Remodulate: multiply demodulated illumination by albedo
    float3 color = diffuse_illumination.rgb  * diffuseAlbedo
                 + specular_illumination.rgb * max(float3(0.01f, 0.01f, 0.01f), specularF0)
                 + emissive;

    if (any(isnan(color)))
        color = float3(0.0f, 0.0f, 1.0f);

    return float4(color, 1.0f);
}
