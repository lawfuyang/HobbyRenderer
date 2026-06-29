/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "../RtxdiApplicationBridge/RtxdiApplicationBridge.hlsli"
#include "../ShadingHelpers.hlsli"

// Bring srrhi-defined types into global scope as bare names.
typedef srrhi::SecondaryGBufferData SecondaryGBufferData;

#include <Rtxdi/GI/Reservoir.hlsli>
#include <Rtxdi/Utils/ReservoirAddressing.hlsli>

#ifdef WITH_NRD
#define NRD_HEADER_ONLY
#include <NRD.hlsli>
#endif

#include "../ShadingHelpers.hlsli"

static const float kMaxBrdfValue = 1e4;
static const float kMISRoughness = 0.3;

float GetMISWeight(const SplitBrdf roughBrdf, const SplitBrdf trueBrdf, const float3 diffuseAlbedo)
{
    float3 combinedRoughBrdf = roughBrdf.demodulatedDiffuse * diffuseAlbedo + roughBrdf.specular;
    float3 combinedTrueBrdf = trueBrdf.demodulatedDiffuse * diffuseAlbedo + trueBrdf.specular;

    combinedRoughBrdf = clamp(combinedRoughBrdf, 1e-4, kMaxBrdfValue);
    combinedTrueBrdf = clamp(combinedTrueBrdf, 0, kMaxBrdfValue);

    const float initWeight = saturate(calcLuminance(combinedTrueBrdf) / calcLuminance(combinedTrueBrdf + combinedRoughBrdf));
    return initWeight * initWeight * initWeight;
}

RTXDI_GIReservoir LoadInitialSampleReservoir(int2 reservoirPosition, RAB_Surface primarySurface)
{
    const uint gbufferIndex = RTXDI_ReservoirPositionToPointer(g_Const.restirGI.reservoirBufferParams, reservoirPosition, 0);
    const SecondaryGBufferData secondaryGBufferData = u_SecondaryGBuffer[gbufferIndex];

    const float3 normal = octToNdirUnorm32(secondaryGBufferData.normal);
    const float3 throughput = Unpack_R16G16B16A16_FLOAT(secondaryGBufferData.throughputAndFlags).rgb;

    // Note: the secondaryGBufferData.emission field contains the sampled radiance saved in ShadeSecondarySurfaces
    return RTXDI_MakeGIReservoir(secondaryGBufferData.worldPos,
        normal, secondaryGBufferData.emission * throughput, secondaryGBufferData.pdf);
}

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(GlobalIndex, g_Const.runtimeParams.activeCheckerboardField);

    if (any(pixelPosition > int2(g_Const.view.m_ViewportSize)))
        return;

    const RAB_Surface primarySurface = RAB_GetGBufferSurface(pixelPosition, false);

    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(pixelPosition, g_Const.runtimeParams.activeCheckerboardField);
    const RTXDI_GIReservoir reservoir = RTXDI_LoadGIReservoir(g_Const.restirGI.reservoirBufferParams, reservoirPosition, g_Const.restirGI.bufferIndices.secondarySurfaceReSTIRDIOutputBufferIndex);

    float3 diffuse = 0;
    float3 specular = 0;

    // Selectable visualization modes via debugVisualizeGIEmission:
    //   1 = raw secondary emission * throughput (post-NEE)
    //   2 = reservoir.radiance * weightSum (pre-BRDF estimator value)
    //   3 = full diffuse contribution = brdf.demodulatedDiffuse * radiance
    //       (this is what gets multiplied by albedo at compositing)
    float3 debugSecondaryEmission = 0;
    float3 debugReservoirEstimator = 0;
    if (g_Const.debugVisualizeGIEmission != 0)
    {
        const uint dbgIndex = RTXDI_ReservoirPositionToPointer(g_Const.restirGI.reservoirBufferParams, reservoirPosition, 0);
        const SecondaryGBufferData dbgGB = u_SecondaryGBuffer[dbgIndex];
        const float3 dbgThroughput = Unpack_R16G16B16A16_FLOAT(dbgGB.throughputAndFlags).rgb;
        debugSecondaryEmission = dbgGB.emission * dbgThroughput;

        if (RTXDI_IsValidGIReservoir(reservoir))
        {
            debugReservoirEstimator = reservoir.radiance * reservoir.weightSum;
        }
    }

    if (RTXDI_IsValidGIReservoir(reservoir))
    {
        float3 radiance = reservoir.radiance * reservoir.weightSum;

        float3 visibility = 1.0;
        if (g_Const.restirGI.finalShadingParams.enableFinalVisibility)
        {
            visibility = GetFinalVisibility(SceneBVH, primarySurface, reservoir.position);
        }

        radiance *= visibility;

        const SplitBrdf brdf = EvaluateBrdf(primarySurface, reservoir.position);

        if (g_Const.restirGI.finalShadingParams.enableFinalMIS)
        {
            const RTXDI_GIReservoir initialReservoir = LoadInitialSampleReservoir(reservoirPosition, primarySurface);
            const SplitBrdf brdf0 = EvaluateBrdf(primarySurface, initialReservoir.position);

            RAB_Surface roughenedSurface = primarySurface;
            roughenedSurface.material.roughness = max(roughenedSurface.material.roughness, kMISRoughness);

            const SplitBrdf roughBrdf = EvaluateBrdf(roughenedSurface, reservoir.position);
            const SplitBrdf roughBrdf0 = EvaluateBrdf(roughenedSurface, initialReservoir.position);

            const float finalWeight = 1.0 - GetMISWeight(roughBrdf, brdf, primarySurface.material.diffuseAlbedo);
            const float initialWeight = GetMISWeight(roughBrdf0, brdf0, primarySurface.material.diffuseAlbedo);

            const float3 initialRadiance = initialReservoir.radiance * initialReservoir.weightSum;

            diffuse = brdf.demodulatedDiffuse * radiance * finalWeight
                    + brdf0.demodulatedDiffuse * initialRadiance * initialWeight;

            specular = brdf.specular * radiance * finalWeight
                     + brdf0.specular * initialRadiance * initialWeight;
        }
        else
        {
            diffuse = brdf.demodulatedDiffuse * radiance;
            specular = brdf.specular * radiance;
        }
        specular = DemodulateSpecular(primarySurface.material.specularF0, specular);
    }

    // DEBUG: select visualization based on mode and write to debug UAV +
    // optionally override the on-screen GI output.
    //   1 = raw secondary emission*throughput (post-NEE input)
    //   2 = reservoir.radiance * weightSum (estimator value, before BRDF)
    //   3 = diffuse + DemodulateSpecular(specular) — total GI before albedo
    //   4 = ONLY specular contribution (DemodulateSpecular'd) from this reservoir
    //   5 = mask: red = pixel's BRDF ray was specular, green = was diffuse,
    //       intensity scaled by reservoir.weightSum to highlight bright specular hits
    //   6 = ONLY diffuse contribution (brdf.demodulatedDiffuse * radiance)
    //       — bypasses specular path entirely; if this looks correct but final
    //       is over-saturated, the bug is in the specular path (DemodulateSpecular,
    //       compositing F0 remod, or NRD specular history).
    //   7 = pdf debug: visualize 1/pdf as grayscale (clamped). Bright pixels =
    //       small pdf = potential specular firefly source. Useful to spot which
    //       BRDF rays have huge 1/pdf amplification.
    if (g_Const.debugVisualizeGIEmission != 0)
    {
        float3 dbg = 0;
        if (g_Const.debugVisualizeGIEmission == 1)
        {
            dbg = debugSecondaryEmission;
        }
        else if (g_Const.debugVisualizeGIEmission == 2)
        {
            dbg = debugReservoirEstimator;
        }
        else if (g_Const.debugVisualizeGIEmission == 3)
        {
            dbg = diffuse + specular;
        }
        else if (g_Const.debugVisualizeGIEmission == 4)
        {
            dbg = specular;
        }
        else if (g_Const.debugVisualizeGIEmission == 5)
        {
            // Read the BRDF ray flag from u_SecondaryGBuffer for this pixel
            const uint dbgIndex2 = RTXDI_ReservoirPositionToPointer(g_Const.restirGI.reservoirBufferParams, reservoirPosition, 0);
            const SecondaryGBufferData dbgGB2 = u_SecondaryGBuffer[dbgIndex2];
            const uint flags2 = dbgGB2.throughputAndFlags.y >> 16;
            const bool wasSpecularRay = (flags2 & kSecondaryGBuffer_IsSpecularRay) != 0;
            const float intensity = saturate(reservoir.weightSum * 0.1); // scale for visibility
            dbg = wasSpecularRay ? float3(intensity, 0, 0) : float3(0, intensity, 0);
        }
        else if (g_Const.debugVisualizeGIEmission == 6)
        {
            dbg = diffuse;
        }
        else if (g_Const.debugVisualizeGIEmission == 7)
        {
            const uint dbgIndex3 = RTXDI_ReservoirPositionToPointer(g_Const.restirGI.reservoirBufferParams, reservoirPosition, 0);
            const SecondaryGBufferData dbgGB3 = u_SecondaryGBuffer[dbgIndex3];
            const float invPdf = dbgGB3.pdf > 0.0 ? (1.0 / dbgGB3.pdf) : 0.0;
            const float v = saturate(invPdf * 0.01); // tonemap so 100 = white
            dbg = float3(v, v, v);
        }

        u_GIDebugEmission[pixelPosition] = float4(dbg, 1.0);

        diffuse  = dbg;
        specular = 0;
    }

    StoreShadingOutput(GlobalIndex, pixelPosition,
        primarySurface.viewDepth, primarySurface.material.roughness, diffuse, specular, 0, false, true);
}
