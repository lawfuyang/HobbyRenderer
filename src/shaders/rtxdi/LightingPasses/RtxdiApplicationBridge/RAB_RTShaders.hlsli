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

#ifndef RAB_RT_SHADERS_HLSLI
#define RAB_RT_SHADERS_HLSLI

bool considerTransparentMaterial(uint instanceIndex, uint geometryIndex, uint triangleIndex, float2 rayBarycentrics, inout float3 throughput)
{
    PerInstanceData instance = t_InstanceData[instanceIndex];
    MeshData geometry        = t_GeometryData[instance.m_MeshDataIndex];
    MaterialConstants mat    = t_MaterialConstants[instance.m_MaterialIndex];
    float2 uv = GetInterpolatedUV(
        triangleIndex,
        instance.m_LODIndex,
        rayBarycentrics,
        geometry,
        t_SceneIndices,
        t_SceneVertices);

    float4 baseColorSample = float4(1.0, 1.0, 1.0, 1.0);
    if ((mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0)
    {
        baseColorSample = SampleBindlessTextureLevel(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uv, 0);
    }

    float alpha = mat.m_BaseColor.a * baseColorSample.a;
    float3 transmissionTint = mat.m_BaseColor.rgb * baseColorSample.rgb;

    if (mat.m_AlphaMode == ALPHA_MODE_MASK)
    {
        return alpha >= mat.m_AlphaCutoff;
    }

    if (mat.m_AlphaMode == ALPHA_MODE_BLEND)
    {
        float opacity = saturate(alpha * (1.0f - mat.m_TransmissionFactor));
        throughput *= (1.0f - opacity);
    }

    if (mat.m_TransmissionFactor > 0.0f)
    {
        throughput *= lerp(1.0.xxx, saturate(transmissionTint), mat.m_TransmissionFactor);
    }

    return all(throughput <= 1e-3f);
}



#endif // RAB_RT_SHADERS_HLSLI