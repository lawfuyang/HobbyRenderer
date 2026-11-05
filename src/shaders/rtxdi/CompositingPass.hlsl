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
#include "SharcCommon.h"
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
#define DENOISER_MODE_REBLUR     srrhi::RTXDIConstants::DENOISER_MODE_REBLUR

// Undo the NRD packing of the lighting signal. Packed radiance is not linear, so it
// has to be unpacked before any interpolation between texels.
float4 UnpackIllumination(float4 packed)
{
#ifdef WITH_NRD
    if (g_Const.denoiserMode == DENOISER_MODE_REBLUR)
        return REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed);
    else if (g_Const.denoiserMode == DENOISER_MODE_RELAX)
        return RELAX_BackEnd_UnpackRadiance(packed);
#endif
    return packed;
}

// Reads the lighting signal at this full-resolution pixel. The screen-space passes may have run
// at a fraction of the render resolution, in which case the four surrounding texels are unpacked
// and bilinearly blended.
float4 LoadIllumination(Texture2D<float4> tex, int2 pixelPos)
{
    if (g_Const.gbufferScale <= 1)
        return UnpackIllumination(tex[pixelPos]);

    const int2 srcSize = int2(g_Const.view.m_ViewportSize) / int(g_Const.gbufferScale);

    float2 srcPos = (float2(pixelPos) + 0.5f) / float(g_Const.gbufferScale) - 0.5f;
    int2 i0 = int2(floor(srcPos));
    float2 f = srcPos - float2(i0);

    i0 = clamp(i0, int2(0, 0), srcSize - 1);
    const int2 i1 = min(i0 + 1, srcSize - 1);

    float4 v00 = UnpackIllumination(tex[int2(i0.x, i0.y)]);
    float4 v10 = UnpackIllumination(tex[int2(i1.x, i0.y)]);
    float4 v01 = UnpackIllumination(tex[int2(i0.x, i1.y)]);
    float4 v11 = UnpackIllumination(tex[i1]);

    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

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
    float4 diffuse_illumination  = LoadIllumination(t_DiffuseIllumination, pixelPos);
    float4 specular_illumination = LoadIllumination(t_SpecularIllumination, pixelPos);

    // Remodulate: multiply demodulated illumination by albedo/F0.
    float3 color = diffuse_illumination.rgb  * diffuseAlbedo
                 + specular_illumination.rgb * max(float3(0.01f, 0.01f, 0.01f), specularF0)
                 + emissive;

    if (any(isnan(color)))
        color = float3(1.0f, 0.0f, 0.0f);

    return float4(color, 1.0f);
}
