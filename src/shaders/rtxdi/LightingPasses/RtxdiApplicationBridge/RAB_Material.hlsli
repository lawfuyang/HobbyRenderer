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

#ifndef RAB_MATERIAL_HLSLI
#define RAB_MATERIAL_HLSLI

// brdf functions (Lambert, GGX_times_NdotL, Schlick_Fresnel, ConstructONB,
// SampleCosHemisphere, ImportanceSampleGGX_VNDF, M_PI) are all in CommonLighting.hlsli
// which is included transitively via ShaderParameters.h -> ShaderShared.h -> CommonLighting
// (or directly by the top-level shader). No separate include needed here.
#include "Rtxdi/Utils/RandomSamplerstate.hlsli"
#include "../../CommonLighting.hlsli"
#include "../../Packing.hlsli"

static const float kMinRoughness = 0.03f;

/*
* The RTXDI SDK RAB_Material implementation for the Full Sample uses a
* material model of two BRDFs:
* Diffuse is Lambertian
* Specular is GGX/Trowbridge-Reitz
*/

struct RAB_Material
{
    float3 diffuseAlbedo;
    float3 specularF0;
    float roughness;
	float3 emissiveColor;
};

RAB_Material RAB_EmptyMaterial()
{
    RAB_Material material = (RAB_Material)0;

    return material;
}

float3 GetDiffuseAlbedo(RAB_Material material)
{
    return material.diffuseAlbedo;
}

float3 GetSpecularF0(RAB_Material material)
{
    return material.specularF0;
}

float GetRoughness(RAB_Material material)
{
    return material.roughness;
}

float RAB_GetRoughness(RAB_Material material)
{
    return GetRoughness(material);
}

float3 RAB_GetEmissiveColor(RAB_Material material)
{
    return material.emissiveColor;
}

RAB_Material GetGBufferMaterial(
    int2 pixelPosition,
    srrhi::PlanarViewConstants view,
    Texture2D<float4> albedoTexture,
    Texture2D<float2> ormTexture)
{
    RAB_Material material = RAB_EmptyMaterial();

    if (any(pixelPosition >= view.m_ViewportSize))
        return material;

    // Albedo is RGBA8_UNORM — read directly, no unpacking needed.
    float3 baseColor = albedoTexture[pixelPosition].rgb;

    // ORM is RG8_UNORM — .r = roughness, .g = metallic.  No unpacking needed.
    float2 orm     = ormTexture[pixelPosition];
    float roughness = orm.r;
    float metallic  = orm.g;

    // Derive diffuseAlbedo and specularF0 from the metallic workflow.
    GetReflectivityFromMetallic(metallic, baseColor, material.diffuseAlbedo, material.specularF0);
    material.roughness = roughness;

    return material;
}

RAB_Material RAB_GetGBufferMaterial(
    int2 pixelPosition,
    bool previousFrame)
{
    if(previousFrame)
    {
        return GetGBufferMaterial(
            pixelPosition,
            g_Const.prevView,
            t_PrevGBufferAlbedo,
            t_PrevGBufferORM);
    }
    else
    {
        return GetGBufferMaterial(
            pixelPosition,
            g_Const.view,
            t_GBufferAlbedo,
            t_GBufferORM);
    }
}

// Compares the materials of two surfaces, returns true if the surfaces
// are similar enough that we can share the light reservoirs between them.
// If unsure, just return true.
bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
    const float roughnessThreshold = 0.5;
    const float reflectivityThreshold = 0.25;
    const float albedoThreshold = 0.25;

    if (!RTXDI_CompareRelativeDifference(a.roughness, b.roughness, roughnessThreshold))
        return false;

    if (abs(calcLuminance(a.specularF0) - calcLuminance(b.specularF0)) > reflectivityThreshold)
        return false;
    
    if (abs(calcLuminance(a.diffuseAlbedo) - calcLuminance(b.diffuseAlbedo)) > albedoThreshold)
        return false;

    return true;
}

#endif // RAB_MATERIAL_HLSLI