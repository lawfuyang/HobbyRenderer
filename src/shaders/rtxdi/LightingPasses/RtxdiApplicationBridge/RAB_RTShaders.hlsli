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
    MaterialConstants mat    = t_MaterialConstants[instance.m_MaterialIndex];

    // Alpha-tested: sample base color alpha
    if (mat.m_AlphaMode == ALPHA_MODE_MASK)
    {
        // Sample base color texture for alpha
        float alpha = mat.m_BaseColor.a;
        if (mat.m_AlbedoTextureIndex >= 0)
        {
            // Barycentrics → UV requires vertex data; use a simple approximation here
            // (full UV interpolation would need vertex buffer access)
            alpha = mat.m_BaseColor.a;
        }
        return alpha >= mat.m_AlphaCutoff;
    }

    // Alpha-blended: attenuate throughput
    if (mat.m_AlphaMode == ALPHA_MODE_BLEND)
    {
        throughput *= (1.0 - mat.m_BaseColor.a);
        return false;
    }

    // Transmissive
    if (mat.m_TransmissionFactor > 0.0f)
    {
        float3 transmission = float3(mat.m_TransmissionFactor, mat.m_TransmissionFactor, mat.m_TransmissionFactor);
        throughput *= transmission;
        return all(throughput == 0);
    }

    // Opaque
    return false;
}



#endif // RAB_RT_SHADERS_HLSLI