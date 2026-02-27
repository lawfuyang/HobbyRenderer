#pragma once

#include "ShaderShared.h"

bool FrustumSphereTest(
    float3 centerVS,
    float radius,
    float4 planes[5]   // view-space frustum planes
)
{
    // Test against each frustum plane
    [unroll]
    for (int i = 0; i < 5; i++)
    {
        float3 n = planes[i].xyz;
        float d  = planes[i].w;

        // Signed distance from sphere center to plane
        float dist = dot(n, centerVS) + d;

        // If sphere is completely outside this plane
        if (dist < -radius)
            return false;
    }

    return true;
}

// 2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere. Michael Mara, Morgan McGuire. 2013
void ProjectSphereView(
    float3 c,          // sphere center in view space
    float  r,          // sphere radius
    float  zNear,      // near plane distance (> 0)
    float  P00,        // projection matrix [0][0]
    float  P11,        // projection matrix [1][1]
    out float4 aabb    // xy = min, zw = max (UV space)
)
{
    float3 cr = c * r;
    float  czr2 = c.z * c.z - r * r;

    // X bounds
    float vx = sqrt(c.x * c.x + czr2);
    float minx = (vx * c.x - cr.z) / (vx * c.z + cr.x);
    float maxx = (vx * c.x + cr.z) / (vx * c.z - cr.x);

    // Y bounds
    float vy = sqrt(c.y * c.y + czr2);
    float miny = (vy * c.y - cr.z) / (vy * c.z + cr.y);
    float maxy = (vy * c.y + cr.z) / (vy * c.z - cr.y);

    // NDC → clip-scaled
    aabb = float4(minx * P00, miny * P11, maxx * P00, maxy * P11);

    aabb.xy = clamp(aabb.xy, -1, 1);
    aabb.zw = clamp(aabb.zw, -1, 1);

    // Clip space [-1,1] → UV space [0,1]
    aabb = aabb.xwzy * float4(0.5f, -0.5f, 0.5f, -0.5f) + float4(0.5f, 0.5f, 0.5f, 0.5f);
}

bool OcclusionSphereTest(float3 center, float radius, uint2 HZBDims, float P00, float P11, Texture2D<float> hzb, SamplerState hzbSampler)
{
    // trivially accept if sphere intersects camera near plane
    if ((center.z - 0.1f) < radius)
        return true;
        
    float4 aabb;
    ProjectSphereView(center, radius, 0.1f, P00, P11, aabb);
    
	float width = (aabb.z - aabb.x) * (float)HZBDims.x;
	float height = (aabb.w - aabb.y) * (float)HZBDims.y;

    // Because we only consider 2x2 pixels, we need to make sure we are sampling from a mip that reduces the rectangle to 1x1 texel or smaller.
    // Due to the rectangle being arbitrarily offset, a 1x1 rectangle may cover 2x2 texel area. Using floor() here would require sampling 4 corners
    // of AABB (using bilinear fetch), which is a little slower.
	float level = ceil(log2(max(width, height)));

    // Sampler is set up to do min reduction, so this computes the minimum depth of a 2x2 texel quad
    float depthHZB = hzb.SampleLevel(hzbSampler, (aabb.xy + aabb.zw) * 0.5, level).r;

    float depthSphere = 0.1f / (center.z - radius); // reversed-Z, infinite far

    // visible if sphere is closer than occluder
    return depthSphere >= depthHZB;
}
