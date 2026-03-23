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

#ifndef RAB_VISIBILITY_TEST_HLSLI
#define RAB_VISIBILITY_TEST_HLSLI

#include "RAB_LightSample.hlsli"

// Build a shadow ray from originWorldPos toward samplePosition.
// Matches FullSample's setupVisibilityRay: NO normal bias, just a small TMin
// to avoid self-intersection. Using a large normal bias (e.g. N*0.1) causes:
//   - Decals (coplanar geometry) to appear overly bright: the bias pushes the
//     origin past the background surface, which then falls within TMin and is
//     skipped, so the decal always sees the sun unobstructed.
//   - Shadows to disappear for geometry closer than the bias distance.
// The 'offset' parameter is the TMin (and half the TMax shrink):
//   - Conservative visibility: 0.001 (default)
//   - Final shading visibility: 0.01
RayDesc SetupShadowRay(float3 originWorldPos, float3 surfaceNormal, float3 samplePosition, float offset = 0.001)
{
    float3 L    = samplePosition - originWorldPos;
    float  dist = length(L);

    RayDesc ray;
    ray.Origin    = originWorldPos;
    ray.Direction = L / max(dist, 1e-6);
    ray.TMin      = offset;
    ray.TMax      = max(offset, dist - offset * 2.0f);
    return ray;
}

// Conservative visibility — used by the RTXDI SDK during resampling passes
// (initial sampling, temporal, spatial resampling).
// RAY_FLAG_CULL_NON_OPAQUE intentionally skips alpha-tested / non-opaque
// geometry to avoid noisy contributions from surfaces
// that may or may not occlude depending on the alpha channel.
// This matches the reference sample design: conservative means "assume visible
// if unsure", so resampling keeps potentially good samples alive.
bool GetConservativeVisibility(RaytracingAccelerationStructure accelStruct, RAB_Surface surface, float3 samplePosition)
{
    RayDesc ray = SetupShadowRay(surface.worldPos, surface.normal, samplePosition);

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;

    rayQuery.TraceRayInline(accelStruct, RAY_FLAG_NONE, INSTANCE_MASK_OPAQUE, ray);

    rayQuery.Proceed();

    bool visible = (rayQuery.CommittedStatus() == COMMITTED_NOTHING);

    REPORT_RAY(!visible);

    return visible;
}

// Traces a cheap visibility ray that returns approximate, conservative visibility
// between the surface and the light sample. Conservative means if unsure, assume the light is visible.
// Significant differences between this conservative visibility and the final one will result in more noise.
// This function is used in the spatial resampling functions for ray traced bias correction.
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    return GetConservativeVisibility(SceneBVH, surface, lightSample.position);
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
    return GetConservativeVisibility(SceneBVH, surface, samplePosition);
}

// Previous-frame conservative visibility helper (uses previous-frame TLAS).
bool GetConservativeVisibilityPrevious(RAB_Surface surface, float3 samplePosition)
{
    RayDesc ray = SetupShadowRay(surface.worldPos, surface.normal, samplePosition);

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;

    rayQuery.TraceRayInline(PrevSceneBVH, RAY_FLAG_NONE, INSTANCE_MASK_OPAQUE, ray);

    rayQuery.Proceed();

    return (rayQuery.CommittedStatus() == COMMITTED_NOTHING);
}

// Same as RAB_GetConservativeVisibility but for temporal resampling.
// When the previous frame TLAS and BLAS are available, the implementation should use the previous position and the previous AS.
// When they are not available, use the current AS. That will result in transient bias.
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, RAB_LightSample lightSample)
{
    if (g_Const.enablePreviousTLAS)
        return GetConservativeVisibilityPrevious(previousSurface, lightSample.position);
    else
        return GetConservativeVisibility(SceneBVH, currentSurface, lightSample.position);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, float3 samplePosition)
{
    if (g_Const.enablePreviousTLAS)
        return GetConservativeVisibilityPrevious(previousSurface, samplePosition);
    else
        return GetConservativeVisibility(SceneBVH, currentSurface, samplePosition);
}

// Traces an expensive visibility ray that considers all alpha tested and transparent geometry along the way.
// Only used for final shading.
// Not a required bridge function.
// Returns a float3 throughput: 1.0 if fully visible, 0.0 if occluded.
// Partially transparent geometry (ALPHA_MODE_BLEND) is treated as fully transparent (throughput = 1.0).
float3 GetFinalVisibility(RaytracingAccelerationStructure accelStruct, RAB_Surface surface, float3 samplePosition)
{
    RayDesc ray = SetupShadowRay(surface.worldPos, surface.normal, samplePosition, 0.01);

    uint instanceMask = INSTANCE_MASK_OPAQUE;
    uint rayFlags = RAY_FLAG_NONE;

    if (g_Const.sceneConstants.enableAlphaTestedGeometry)
        instanceMask |= INSTANCE_MASK_ALPHA_TESTED;

    if (g_Const.sceneConstants.enableTransparentGeometry)
        instanceMask |= INSTANCE_MASK_TRANSPARENT;

    if (!g_Const.sceneConstants.enableTransparentGeometry && !g_Const.sceneConstants.enableAlphaTestedGeometry)
        rayFlags |= RAY_FLAG_CULL_NON_OPAQUE;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;

    rayQuery.TraceRayInline(accelStruct, rayFlags, instanceMask, ray);

    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instanceIndex  = rayQuery.CandidateInstanceIndex();
            uint primitiveIndex = rayQuery.CandidatePrimitiveIndex();
            float2 bary         = rayQuery.CandidateTriangleBarycentrics();

            PerInstanceData   inst = t_InstanceData[instanceIndex];
            MeshData          mesh = t_GeometryData[inst.m_MeshDataIndex];
            MaterialConstants mat  = t_MaterialConstants[inst.m_MaterialIndex];

            if (mat.m_AlphaMode == ALPHA_MODE_MASK)
            {
                TriangleVertices tv   = GetTriangleVertices(primitiveIndex, inst.m_LODIndex, mesh, t_SceneIndices, t_SceneVertices);
                RayGradients     grad = GetShadowRayGradients(tv, bary, ray.Origin, inst.m_World);

                if (AlphaTestGrad(grad.uv, grad.ddx, grad.ddy, mat))
                    rayQuery.CommitNonOpaqueTriangleHit(); // opaque texel: blocks light
                // else: transparent texel — do not commit, continue traversal
            }
            else if (mat.m_AlphaMode == ALPHA_MODE_BLEND)
            {
                // Do not commit hit; continue through translucent surface
            }
        }
    }

    REPORT_RAY(rayQuery.CommittedStatus() != COMMITTED_NOTHING);

    if (rayQuery.CommittedStatus() == COMMITTED_NOTHING)
        return float3(1.0, 1.0, 1.0);
    else
        return float3(0.0, 0.0, 0.0);
}

#endif // RAB_VISIBILITY_TEST_HLSLI