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

#ifndef RAB_RAY_PAYLOAD_HLSLI
#define RAB_RAY_PAYLOAD_HLSLI

struct RAB_RayPayload
{
    float3 throughput;
    float committedRayT;
    uint instanceID;
    uint geometryIndex;
    uint primitiveIndex;
    bool frontFace;
    float2 barycentrics;
};

bool RAB_IsValidHit(RAB_RayPayload rayPayload)
{
    return (rayPayload.committedRayT > 0) &&
           (rayPayload.instanceID != ~0u);
}

float RAB_RayPayloadGetCommittedHitT(RAB_RayPayload rayPayload)
{
    return rayPayload.committedRayT;
}

bool RAB_RayPayloadIsFrontFace(RAB_RayPayload rayPayload)
{
	return rayPayload.frontFace;
}

bool RAB_RayPayloadIsTwoSided(RAB_RayPayload rayPayload)
{
	return false;
}

float3 RAB_RayPayloadGetEmittedRadiance(RAB_RayPayload rayPayload)
{
    PerInstanceData instance = t_InstanceData[rayPayload.instanceID];
    MaterialConstants mat    = t_MaterialConstants[instance.m_MaterialIndex];

    // Return emissive color directly from material constants
    return mat.m_EmissiveFactor.rgb;
}

#endif // RAB_RAY_PAYLOAD_HLSLI