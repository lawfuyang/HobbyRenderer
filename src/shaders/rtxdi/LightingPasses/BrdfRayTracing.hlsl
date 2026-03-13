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

#include "RtxdiApplicationBridge/RtxdiApplicationBridge.hlsli"
#include "../../RaytracingCommon.hlsli"
#include "../ShaderDebug/ShaderDebugPrint/ShaderDebugPrint.hlsli"
#include "../ShaderDebug/PTPathViz/PTPathVizRecording.hlsli"

#include <Rtxdi/DI/Reservoir.hlsli>
#include <Rtxdi/Utils/ReservoirAddressing.hlsli>

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

    RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);

    if (!RAB_IsSurfaceValid(surface))
        return;

	ShaderDebug::SetDebugShaderPrintCurrentThreadCursorXY(pixelPosition);

    if(all(pixelPosition == g_Const.debug.mouseSelectedPixel))
    {
        Debug_EnablePTPathRecording();
    }
    Debug_RecordPTCameraPosition(g_Const.view.m_CameraDirectionOrPosition.xyz);
    Debug_SetPTVertexIndex(1);
    Debug_RecordPTIntersectionPosition(RAB_GetSurfaceWorldPos(surface));
    Debug_RecordPTIntersectionNormal(RAB_GetSurfaceNormal(surface));
    Debug_RecordPTNEELightPosition(RAB_GetSurfaceWorldPos(surface));

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(GlobalIndex, g_Const.runtimeParams.frameIndex, RTXDI_GI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);

    float3 tangent, bitangent;
    branchlessONB(surface.normal, tangent, bitangent);

    float distance = max(1, 0.1 * length(surface.worldPos - g_Const.view.m_CameraDirectionOrPosition.xyz));

    RayDesc ray;
    ray.TMin = 0.001f * distance;
    ray.TMax = 1000.0;

    float2 Rand;
    Rand.x = RTXDI_GetNextRandom(rng);
    Rand.y = RTXDI_GetNextRandom(rng);

    float3 V = normalize(g_Const.view.m_CameraDirectionOrPosition.xyz - surface.worldPos);

    bool isSpecularRay = false;
    bool isDeltaSurface = surface.material.roughness < kMinRoughness;
    float specular_PDF;
    float3 BRDF_over_PDF;
    float overall_PDF;

    {
        float3 specularDirection;
        float3 specular_BRDF_over_PDF;
        {
            float3 Ve = float3(dot(V, tangent), dot(V, bitangent), dot(V, surface.normal));
            float3 He = sampleGGX_VNDF(Ve, surface.material.roughness, Rand);
            float3 H = isDeltaSurface ? surface.normal : normalize(He.x * tangent + He.y * bitangent + He.z * surface.normal);
            specularDirection = reflect(-V, H);

            float HoV = saturate(dot(H, V));
            float NoV = saturate(dot(surface.normal, V));
            float3 F = Schlick_Fresnel(surface.material.specularF0, HoV);
            float G1 = isDeltaSurface ? 1.0 : (NoV > 0) ? G1_Smith(surface.material.roughness, NoV) : 0;
            specular_BRDF_over_PDF = F * G1;
        }

        float3 diffuseDirection;
        float diffuse_BRDF_over_PDF;
        {
            float solidAnglePdf;
            float3 localDirection = SampleCosHemisphere(Rand, solidAnglePdf);
            diffuseDirection = tangent * localDirection.x + bitangent * localDirection.y + surface.normal * localDirection.z;
            diffuse_BRDF_over_PDF = 1.0;
        }

		// Ignores PDF of specular or diffuse
		// Chooses PDF based on relative luminance
        specular_PDF = saturate(calcLuminance(specular_BRDF_over_PDF) /
            calcLuminance(specular_BRDF_over_PDF + diffuse_BRDF_over_PDF * surface.material.diffuseAlbedo));
        isSpecularRay = RTXDI_GetNextRandom(rng) < specular_PDF;

        if (isSpecularRay)
        {
            ray.Direction = specularDirection;
            BRDF_over_PDF = specular_BRDF_over_PDF / specular_PDF;
        }
        else
        {
            ray.Direction = diffuseDirection;
            BRDF_over_PDF = diffuse_BRDF_over_PDF / (1.0 - specular_PDF);
        }

		// Calculates PDF of individual respective lobes.
        const float specularLobe_PDF = SampleGGX_VNDF_PDF(surface.material.roughness, surface.normal, V, ray.Direction);
    const float diffuseLobe_PDF = saturate(dot(ray.Direction, surface.normal)) / PI;

        // For delta surfaces, we only pass the diffuse lobe to ReSTIR GI, and this pdf is for that.
        overall_PDF = isDeltaSurface ? diffuseLobe_PDF : lerp(diffuseLobe_PDF, specularLobe_PDF, specular_PDF);
    }

    if (dot(surface.geoNormal, ray.Direction) <= 0.0)
    {
        BRDF_over_PDF = 0.0;
        ray.TMax = 0;
    }

    ray.Origin = surface.worldPos;

    float3 radiance = 0;

    RAB_RayPayload payload = (RAB_RayPayload)0;
    payload.instanceID = ~0u;
    payload.throughput = 1.0;

    uint instanceMask = INSTANCE_MASK_OPAQUE;

    if (g_Const.sceneConstants.enableAlphaTestedGeometry)
        instanceMask |= INSTANCE_MASK_ALPHA_TESTED;

    if (g_Const.sceneConstants.enableTransparentGeometry)
        instanceMask |= INSTANCE_MASK_TRANSPARENT;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;

    rayQuery.TraceRayInline(SceneBVH, RAY_FLAG_NONE, instanceMask, ray);

    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if (considerTransparentMaterial(
                rayQuery.CandidateInstanceID(),
                rayQuery.CandidateGeometryIndex(),
                rayQuery.CandidatePrimitiveIndex(),
                rayQuery.CandidateTriangleBarycentrics(),
                payload.throughput))
            {
                rayQuery.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        payload.instanceID = rayQuery.CommittedInstanceID();
        payload.geometryIndex = rayQuery.CommittedGeometryIndex();
        payload.primitiveIndex = rayQuery.CommittedPrimitiveIndex();
        payload.barycentrics = rayQuery.CommittedTriangleBarycentrics();
        payload.committedRayT = rayQuery.CommittedRayT();
    }

    uint gbufferIndex = RTXDI_ReservoirPositionToPointer(g_Const.restirGI.reservoirBufferParams, GlobalIndex, 0);

    struct
    {
        float3 position;
        float3 normal;
        float3 diffuseAlbedo;
        float3 specularF0;
        float roughness;
        bool isEnvironmentMap;
    } secondarySurface;

    // Include the emissive component of surfaces seen with BRDF rays if requested (i.e. when Direct Lighting mode
    // is set to BRDF) or on delta reflection rays because those bypass ReSTIR GI and direct specular lighting,
    // and we need to see reflections of lamps and the sky in mirrors.
    const bool includeEmissiveComponent = g_Const.brdfPT.enableIndirectEmissiveSurfaces || (isSpecularRay && isDeltaSurface);

    if (payload.instanceID != ~0u)
    {
        PerInstanceData instance = t_InstanceData[payload.instanceID];
        MeshData        geometry = t_GeometryData[instance.m_MeshDataIndex];
        MaterialConstants mat    = t_MaterialConstants[instance.m_MaterialIndex];

        // Build RayHitInfo for GetFullHitAttributes
        RayHitInfo hit;
        hit.m_InstanceIndex  = payload.instanceID;
        hit.m_PrimitiveIndex = payload.primitiveIndex;
        hit.m_Barycentrics   = payload.barycentrics;
        hit.m_RayT           = payload.committedRayT;

        FullHitAttributes attr = GetFullHitAttributes(hit, ray, instance, geometry, t_SceneIndices, t_SceneVertices);
        PBRAttributes pbr      = GetPBRAttributes(attr, mat);

        // Bent normal to avoid self-shadowing
        float3 flatNormal = attr.m_WorldNormal; // GetFullHitAttributes already normalizes
        pbr.normal = getBentNormal(flatNormal, pbr.normal, ray.Direction);

        // Metallic workflow split
        float3 diffuseAlbedo, specularF0;
        getReflectivity(pbr.metallic, pbr.baseColor, diffuseAlbedo, specularF0);

        // Material overrides
        if (g_Const.brdfPT.materialOverrideParams.roughnessOverride >= 0)
            pbr.roughness = g_Const.brdfPT.materialOverrideParams.roughnessOverride;

        if (g_Const.brdfPT.materialOverrideParams.metalnessOverride >= 0)
        {
            pbr.metallic = g_Const.brdfPT.materialOverrideParams.metalnessOverride;
            getReflectivity(pbr.metallic, pbr.baseColor, diffuseAlbedo, specularF0);
        }

        pbr.roughness = max(pbr.roughness, g_Const.brdfPT.materialOverrideParams.minSecondaryRoughness);

        if (includeEmissiveComponent)
            radiance += pbr.emissive;

        // Geometry normal: face outward relative to ray
        float3 geometryNormal = (dot(attr.m_WorldNormal, ray.Direction) < 0) ? attr.m_WorldNormal : -attr.m_WorldNormal;

        secondarySurface.position = ray.Origin + ray.Direction * payload.committedRayT;
        secondarySurface.normal = geometryNormal;
        secondarySurface.diffuseAlbedo = diffuseAlbedo;
        secondarySurface.specularF0 = specularF0;
        secondarySurface.roughness = pbr.roughness;
        secondarySurface.isEnvironmentMap = false;
    }
    else
    {
        if (g_Const.sceneConstants.enableEnvironmentMap && includeEmissiveComponent)
        {
            float3 environmentRadiance = GetEnvironmentRadiance(ray.Direction);
            radiance += environmentRadiance;
        }

        secondarySurface.position = ray.Origin + ray.Direction * DISTANT_LIGHT_DISTANCE;
        secondarySurface.normal = -ray.Direction;
        secondarySurface.diffuseAlbedo = 0;
        secondarySurface.specularF0 = 0;
        secondarySurface.roughness = 0;
        secondarySurface.isEnvironmentMap = true;
    }

    if (g_Const.enableBrdfIndirect)
    {
        SecondaryGBufferData secondaryGBufferData = (SecondaryGBufferData)0;
        secondaryGBufferData.worldPos = secondarySurface.position;
        secondaryGBufferData.normal = ndirToOctUnorm32(secondarySurface.normal);
        secondaryGBufferData.throughputAndFlags = Pack_R16G16B16A16_FLOAT(float4(payload.throughput * BRDF_over_PDF, 0));
        secondaryGBufferData.diffuseAlbedo = Pack_R11G11B10_UFLOAT(secondarySurface.diffuseAlbedo);
        secondaryGBufferData.specularAndRoughness = Pack_R8G8B8A8_Gamma_UFLOAT(float4(secondarySurface.specularF0, secondarySurface.roughness));

        if (g_Const.brdfPT.enableReSTIRGI)
        {
            if (isSpecularRay && isDeltaSurface)
            {
                // Special case for specular rays on delta surfaces: they bypass ReSTIR GI and are shaded
                // entirely in the ShadeSecondarySurfaces pass, so they need the right throughput here.
            }
            else
            {
                // BRDF_over_PDF will be multiplied after resampling GI reservoirs.
                secondaryGBufferData.throughputAndFlags = Pack_R16G16B16A16_FLOAT(float4(payload.throughput, 0));
            }

            // The emission from the secondary surface needs to be added when creating the initial
            // GI reservoir sample in ShadeSecondarySurface.hlsl. It need to be stored separately.
            secondaryGBufferData.emission = radiance;
            radiance = 0;

            secondaryGBufferData.pdf = overall_PDF;
        }

        uint flags = 0;
        if (isSpecularRay) flags |= kSecondaryGBuffer_IsSpecularRay;
        if (isDeltaSurface) flags |= kSecondaryGBuffer_IsDeltaSurface;
        if (secondarySurface.isEnvironmentMap) flags |= kSecondaryGBuffer_IsEnvironmentMap;
        secondaryGBufferData.throughputAndFlags.y |= flags << 16;

        u_SecondaryGBuffer[gbufferIndex] = secondaryGBufferData;
    }

    Debug_SetPTVertexIndex(2);
    Debug_RecordPTIntersectionPosition(secondarySurface.position);
    Debug_RecordPTIntersectionNormal(secondarySurface.normal);

    if ((any(radiance > 0) || !g_Const.enableBrdfAdditiveBlend))
    {
        radiance *= payload.throughput;

        float3 diffuse = isSpecularRay ? 0.0 : radiance * BRDF_over_PDF;
        float3 specular = isSpecularRay ? radiance * BRDF_over_PDF : 0.0;
        float diffuseHitT = payload.committedRayT;
        float specularHitT = payload.committedRayT;

        specular = DemodulateSpecular(surface.material.specularF0, specular);

        if(!isDeltaSurface)
            StoreShadingOutput(GlobalIndex, pixelPosition,
                surface.viewDepth, surface.material.roughness, diffuse, specular, payload.committedRayT, !g_Const.enableBrdfAdditiveBlend, !g_Const.enableBrdfIndirect);
        else
        {
            StoreShadingOutput(GlobalIndex, pixelPosition,
                surface.viewDepth, surface.material.roughness, 0, 0, 0, !g_Const.enableBrdfAdditiveBlend, !g_Const.enableBrdfIndirect);
        }
    }
}
