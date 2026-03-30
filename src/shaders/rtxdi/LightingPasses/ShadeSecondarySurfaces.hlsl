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

// Disable specular MIS on direct lighting of the secondary surfaces,
// because we do not trace the BRDF rays further.
#define RAB_ENABLE_SPECULAR_MIS 0

#include "RtxdiApplicationBridge/RtxdiApplicationBridge.hlsli"

// Bring srrhi-defined types into global scope as bare names.
typedef srrhi::SecondaryGBufferData SecondaryGBufferData;

#include <Rtxdi/DI/InitialSampling.hlsli>
#include <Rtxdi/DI/SpatialResampling.hlsli>
#include <Rtxdi/GI/Reservoir.hlsli>

#ifdef WITH_NRD
#define NRD_HEADER_ONLY
#include <NRD.hlsli>
#endif

#include "ShadingHelpers.hlsli"

static const float c_MaxIndirectRadiance = 10;

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(GlobalIndex, g_Const.runtimeParams.activeCheckerboardField);

    if (any(pixelPosition > int2(g_Const.view.m_ViewportSize)))
        return;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(GlobalIndex, g_Const.runtimeParams.frameIndex, RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
    RTXDI_RandomSamplerState tileRng = RTXDI_InitRandomSampler(GlobalIndex / RTXDI_TILE_SIZE_IN_PIXELS, g_Const.runtimeParams.frameIndex, RTXDI_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);

    const RTXDI_RuntimeParameters params = g_Const.runtimeParams;
    const uint gbufferIndex = RTXDI_ReservoirPositionToPointer(g_Const.restirDI.reservoirBufferParams, GlobalIndex, 0);

    RAB_Surface primarySurface = RAB_GetGBufferSurface(pixelPosition, false);

    SecondaryGBufferData secondaryGBufferData = u_SecondaryGBuffer[gbufferIndex];

    const float3 throughput = Unpack_R16G16B16A16_FLOAT(secondaryGBufferData.throughputAndFlags).rgb;
    const uint secondaryFlags = secondaryGBufferData.throughputAndFlags.y >> 16;
    const bool isValidSecondarySurface = any(throughput != 0);
    const bool isSpecularRay = (secondaryFlags & kSecondaryGBuffer_IsSpecularRay) != 0;
    const bool isDeltaSurface = (secondaryFlags & kSecondaryGBuffer_IsDeltaSurface) != 0;
    const bool isEnvironmentMap = (secondaryFlags & kSecondaryGBuffer_IsEnvironmentMap) != 0;

    RAB_Surface secondarySurface = RAB_EmptySurface();
    float3 radiance = secondaryGBufferData.emission;

    // Unpack the G-buffer data
    secondarySurface.worldPos = secondaryGBufferData.worldPos;
    secondarySurface.viewDepth = 1.0; // doesn't matter
    secondarySurface.normal = octToNdirUnorm32(secondaryGBufferData.normal);
    secondarySurface.geoNormal = secondarySurface.normal;
    secondarySurface.material.diffuseAlbedo = Unpack_R11G11B10_UFLOAT(secondaryGBufferData.diffuseAlbedo);
    float4 specularRough = Unpack_R8G8B8A8_Gamma_UFLOAT(secondaryGBufferData.specularAndRoughness);
    secondarySurface.material.specularF0 = specularRough.rgb;
    secondarySurface.material.roughness = specularRough.a;
    secondarySurface.diffuseProbability = getSurfaceDiffuseProbability(secondarySurface);
    secondarySurface.viewDir = normalize(primarySurface.worldPos - secondarySurface.worldPos);

    // Shade the secondary surface.
    if (isValidSecondarySurface && !isEnvironmentMap)
    {
        RAB_LightSample lightSample;
        RTXDI_DISpatialResamplingParameters dummySpatialParams = (RTXDI_DISpatialResamplingParameters)0;
        RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(rng, tileRng, secondarySurface,
            g_Const.restirDI.initialSamplingParams, g_Const.lightBufferParams,
#if RTXDI_ENABLE_PRESAMPLING
        g_Const.localLightsRISBufferSegmentParams, g_Const.environmentLightRISBufferSegmentParams,
#if RTXDI_REGIR_MODE != RTXDI_REGIR_MODE_DISABLED
        g_Const.regir,
#endif
#endif
        lightSample);
        bool valid = reservoir.weightSum > 0;

        float3 indirectDiffuse = 0;
        float3 indirectSpecular = 0;
        float lightDistance = 0;
        ShadeSurfaceWithLightSample(reservoir, secondarySurface, g_Const.restirDI.shadingParams, lightSample, /* previousFrameTLAS = */ false,
            /* enableVisibilityReuse = */ false, /* enableVisibilityShortcut */ false, indirectDiffuse, indirectSpecular, lightDistance);
        radiance += indirectDiffuse * secondarySurface.material.diffuseAlbedo + indirectSpecular;
        // Firefly suppression
        float indirectLuminance = calcLuminance(radiance);
        if (indirectLuminance > c_MaxIndirectRadiance)
            radiance *= c_MaxIndirectRadiance / indirectLuminance;
    }

    bool outputShadingResult = true;
    if (g_Const.enableBrdfIndirect)
    {
        RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();

        // For delta reflection rays, just output the shading result in this shader
        // and don't include it into ReSTIR GI reservoirs.
        outputShadingResult = isSpecularRay && isDeltaSurface;

        if (isValidSecondarySurface && !outputShadingResult)
        {
            // This pixel has a valid indirect sample so it stores information as an initial GI reservoir.
            reservoir = RTXDI_MakeGIReservoir(secondarySurface.worldPos,
                secondarySurface.normal, radiance, secondaryGBufferData.pdf);
        }
        uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(pixelPosition, g_Const.runtimeParams.activeCheckerboardField);
        RTXDI_StoreGIReservoir(reservoir, g_Const.restirGI.reservoirBufferParams, reservoirPosition, g_Const.restirGI.bufferIndices.secondarySurfaceReSTIRDIOutputBufferIndex);

        // Save the initial sample radiance for MIS in the final shading pass
        secondaryGBufferData.emission = outputShadingResult ? 0 : radiance;
        u_SecondaryGBuffer[gbufferIndex] = secondaryGBufferData;
    }

    if (outputShadingResult)
    {
        float3 diffuse = 0;
        float3 specular = 0;
        if (!isDeltaSurface)
        {
            diffuse = isSpecularRay ? 0.0 : radiance * throughput.rgb;
            specular = isSpecularRay ? radiance * throughput.rgb : 0.0;
            specular = DemodulateSpecular(primarySurface.material.specularF0, specular);
        }

        StoreShadingOutput(GlobalIndex, pixelPosition, 
            primarySurface.viewDepth, primarySurface.material.roughness, diffuse, specular, 0, false, true);
    }
}
