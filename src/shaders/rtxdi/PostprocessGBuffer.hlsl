/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 */

#pragma pack_matrix(row_major)

#include "SharedShaderInclude/ShaderParameters.h"
#include "../Packing.hlsli"

// Inputs — registers match RTXDIRenderer.cpp PostprocessGBuffer binding set:
//   t1  = LinearZ (R32_FLOAT linear view-space depth, written by GenerateViewZ)
//   t4  = ORM (packed R8G8B8A8: R=occlusion, G=roughness, B=metalness)
//   t5  = normals (packed oct R32_UINT)
Texture2D<float>  t_ViewDepth      : register(t1);  // LinearZ
Texture2D<uint>   t_ORM            : register(t4);
Texture2D<uint>   t_Normals        : register(t5);

ConstantBuffer<ResamplingConstants> g_Const : register(b0);

#define NRD_BILATERAL_WEIGHT_VIEWZ_SENSITIVITY 100.0
#define NRD_BILATERAL_WEIGHT_CUTOFF            0.03

float GetBilateralWeight(float z, float zc)
{
    z = abs(z - zc) * rcp(min(abs(z), abs(zc)) + 0.001);
    z = rcp(1.0 + NRD_BILATERAL_WEIGHT_VIEWZ_SENSITIVITY * z) * step(z, NRD_BILATERAL_WEIGHT_CUTOFF);
    return z;
}

float GetModifiedRoughnessFromNormalVariance(float linearRoughness, float3 nonNormalizedAverageNormal)
{
    // https://blog.selfshadow.com/publications/s2013-shading-course/rad/s2013_pbs_rad_notes.pdf (page 20)
    float l = length(nonNormalizedAverageNormal);
    float kappa = saturate(1.0 - l * l) * rcp(max(l * (3.0 - l * l), 1e-15));
    return sqrt(saturate(linearRoughness * linearRoughness + kappa));
}

// Fullscreen pixel shader — output goes to the denoiser normal+roughness render target.
float4 main(FullScreenVertexOut input) : SV_Target
{
    int2 pixelPosition = int2(input.pos.xy);

    // Decode shading normal from packed oct-encoded uint
    float3 currentNormal = octToNdirUnorm32(t_Normals[pixelPosition]);

    // Unpack ORM: R=occlusion, G=roughness, B=metalness, A=unused
    float4 orm = Unpack_R8G8B8A8_Gamma_UFLOAT(t_ORM[pixelPosition]);
    float currentRoughness = orm.g;

    float currentLinearZ = t_ViewDepth[pixelPosition];

    // Bilateral normal averaging for roughness variance estimation (NRD requirement)
    float3 averageNormal = currentNormal;
    float  sumW = 1.0;

    for (int i = -1; i <= 1; i++)
    {
        for (int j = -1; j <= 1; j++)
        {
            if ((i == 0) && (j == 0)) continue;

            int2   p       = pixelPosition + int2(i, j);
            float3 pNormal = octToNdirUnorm32(t_Normals[p]);
            float  pZ      = t_ViewDepth[p];

            float w = GetBilateralWeight(currentLinearZ, pZ);
            averageNormal += pNormal * w;
            sumW += w;
        }
    }
    averageNormal *= rcp(sumW);

    // Roughness modified by normal variance (improves NRD specular denoising)
    static const float kMirrorRoughness = 0.01f;
    float modifiedRoughness = (currentRoughness <= kMirrorRoughness)
        ? 0.0f
        : GetModifiedRoughnessFromNormalVariance(currentRoughness, averageNormal);

    // Write denoiser normal+roughness (NRD IN_NORMAL_ROUGHNESS format)
    return float4(currentNormal * 0.5f + 0.5f, modifiedRoughness);
}