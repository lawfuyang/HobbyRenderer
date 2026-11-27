#include "GBufferMaterial.hlsli"
#include "srrhi/hlsl/VisibilityResolve.hlsli"

/*
	The visibility resolve (docs/visibility_buffer_implementation_plan.md §7).

	One invocation per pixel: decode the id the cluster raster wrote, fetch the triangle it names, rebuild the
	interpolant the raster path would have handed its pixel shader, and run the same material evaluation into the six
	G-buffer targets.

	The rebuild is geometric. The three vertices come out of the cluster's blob, get the same per-vertex transform and
	tangent-frame treatment PrepareVSOut gives them, and are interpolated with perspective-correct barycentrics
	derived in screen space - so what reaches the material code is the raster path's values, to within the rounding
	of a different association.

	Why not the hardware's own interpolation: a full-screen pass has no triangle. That reconstruction is the price of
	the deferred shift; what it replaces is a per-fragment material evaluation inside the raster.

	Depth is not written here, and must not be: the raster pass still owns it, which is what keeps the HZB and the
	occlusion decisions identical to the raster path's.
*/

static const Texture2D<uint> g_VisibilityBuffer = srrhi::VisibilityResolveInputs::GetVisibilityBuffer();
static const StructuredBuffer<srrhi::ClusterJob> g_ClusterJobsCompute =
    srrhi::VisibilityResolveInputs::GetClusterJobsCompute();

static const StructuredBuffer<srrhi::ClusterJob>    g_ClusterJobs       = srrhi::BasePassInputs::GetClusterJobs();
static const StructuredBuffer<srrhi::ClusterJob>    g_ClusterJobsSecond = srrhi::BasePassInputs::GetClusterJobsSecond();
static const StructuredBuffer<srrhi::ClusterJob>    g_ClusterJobsMasked = srrhi::BasePassInputs::GetClusterJobsMasked();
static const StructuredBuffer<srrhi::ClusterRecord> g_ClusterRecords    = srrhi::BasePassInputs::GetClusterRecords();

// One attribute of the triangle, weighted by the perspective-correct barycentrics.
float4 Blend(float4 a, float4 b, float4 c, float3 w)
{
    return a * w.x + b * w.y + c * w.z;
}

float3 Blend(float3 a, float3 b, float3 c, float3 w)
{
    return a * w.x + b * w.y + c * w.z;
}

float2 Blend(float2 a, float2 b, float2 c, float3 w)
{
    return a * w.x + b * w.y + c * w.z;
}

// A clip position to the pixel-centre space SV_Position reports, matching ClipXYToUV's half-pixel convention.
float2 ClipToPixel(float4 clip, float2 targetSize)
{
    return (clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f) * targetSize;
}

[earlydepthstencil]
GBufferOut PSMain(FullScreenVertexOut input)
{
    // A pixel the raster did not draw keeps the clear value, so an empty id returns that rather than shading it. It
    // could be a discard instead, but the streamed sampler needs [earlydepthstencil] and the two do not mix.
    GBufferOut empty = (GBufferOut)0;

    const float2 targetSize = float2(g_PerFrame.m_Width, g_PerFrame.m_Height);
    const uint2 pixel = min(uint2(input.pos.xy), uint2(g_PerFrame.m_Width - 1u, g_PerFrame.m_Height - 1u));
    const uint id = g_VisibilityBuffer.Load(int3(pixel, 0));
    if (id == 0u)
        return empty;

    const uint idList = id >> kVisListShift;
    const uint jobIndex = ((id >> kVisJobShift) & kVisJobMask) - 1u;
    const uint triangleIndex = id & kVisTriangleMask;

    // The id names a slot in the list its bucket drew from, and one frame can hold ids from all four.
    srrhi::ClusterJob job = g_ClusterJobs[jobIndex];
    if (idList == 1u)
        job = g_ClusterJobsSecond[jobIndex];
    else if (idList == 2u)
        job = g_ClusterJobsMasked[jobIndex];
    else if (idList == 3u)
        job = g_ClusterJobsCompute[jobIndex];

    // The per-triangle view is native here, and cheaper than the raster path's version: the id already carries the
    // triangle's index inside its cluster, so this needs no second draw and no re-emitted corners. The colour is
    // the one that view uses, so the two paths show the same picture.
    if (g_PerFrame.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_CLUSTER_TRIANGLES)
    {
        GBufferOut triangleView = (GBufferOut)0;
        triangleView.Albedo = float4(HashColor(triangleIndex * 0x9E3779B1u + job.m_ClusterIndex * 0x85EBCA77u), 1.0f);
        triangleView.Normal = float2(0.5f, 0.5f);
        triangleView.ORM = float2(0.5f, 0.0f);
        triangleView.Emissive = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return triangleView;
    }

    const srrhi::ClusterRecord cluster = g_ClusterRecords[job.m_ClusterIndex];
    const srrhi::PerInstanceData instance = g_Instances[job.m_InstanceIndex];

    const uint streamBase = ClusterStreamBase(cluster);
    const uint3 corners = UnpackTriangle(g_ClusterStreamData[streamBase + cluster.m_TriangleOffset + triangleIndex]);

    const srrhi::Vertex v0 = UnpackVertex(ReadClusterVertex(streamBase, cluster, corners.x));
    const srrhi::Vertex v1 = UnpackVertex(ReadClusterVertex(streamBase, cluster, corners.y));
    const srrhi::Vertex v2 = UnpackVertex(ReadClusterVertex(streamBase, cluster, corners.z));

    // The raster path's per-vertex work, verbatim: world and previous-world positions, and world-space normals and
    // tangents. Transforming before the interpolation is what makes the blended normal equal the raster path's -
    // normalising three vertices and then blending is not the same as blending and then normalising.
    const float4 w0 = MatrixMultiply(float4(v0.m_Pos, 1.0f), instance.m_World);
    const float4 w1 = MatrixMultiply(float4(v1.m_Pos, 1.0f), instance.m_World);
    const float4 w2 = MatrixMultiply(float4(v2.m_Pos, 1.0f), instance.m_World);

    const float4 c0 = MatrixMultiply(w0, g_PerFrame.m_View.m_MatWorldToClip);
    const float4 c1 = MatrixMultiply(w1, g_PerFrame.m_View.m_MatWorldToClip);
    const float4 c2 = MatrixMultiply(w2, g_PerFrame.m_View.m_MatWorldToClip);

    // Screen-space barycentrics from the signed areas, then the perspective-correct weights. The shared denominator
    // is twice the triangle's signed pixel area, which is zero for a triangle that covers nothing; such a triangle
    // cannot have written this id, but the guard keeps a degenerate record from dividing by zero.
    const float2 p0 = ClipToPixel(c0, targetSize);
    const float2 p1 = ClipToPixel(c1, targetSize);
    const float2 p2 = ClipToPixel(c2, targetSize);

    const float2 edge0 = p1 - p0;
    const float2 edge1 = p2 - p0;
    const float2 offset = input.pos.xy - p0;
    const float doubleArea = edge0.x * edge1.y - edge0.y * edge1.x;
    if (abs(doubleArea) < 1e-12f)
        return empty;

    const float b1 = (offset.x * edge1.y - offset.y * edge1.x) / doubleArea;
    const float b2 = (edge0.x * offset.y - edge0.y * offset.x) / doubleArea;
    const float b0 = 1.0f - b1 - b2;

    float3 weights = float3(b0, b1, b2) * rcp(float3(c0.w, c1.w, c2.w));
    const float weightSum = weights.x + weights.y + weights.z;
    if (weightSum <= 0.0f)
        return empty;
    weights /= weightSum;

    // The interpolant the raster path's pixel shader reads, rebuilt.
    VSOut o;
    o.Position = 0.0f.xxxx;   // Its projection is the interpolation above; the G-buffer path never reads it.
    o.uv = Blend(v0.m_Uv, v1.m_Uv, v2.m_Uv, weights);
    o.normal = Blend(TransformNormal(v0.m_Normal, instance.m_World),
                     TransformNormal(v1.m_Normal, instance.m_World),
                     TransformNormal(v2.m_Normal, instance.m_World), weights);
    o.tangent = Blend(float4(TransformNormal(v0.m_Tangent.xyz, instance.m_World), v0.m_Tangent.w),
                      float4(TransformNormal(v1.m_Tangent.xyz, instance.m_World), v1.m_Tangent.w),
                      float4(TransformNormal(v2.m_Tangent.xyz, instance.m_World), v2.m_Tangent.w), weights);
    o.worldPos = Blend(w0.xyz, w1.xyz, w2.xyz, weights);
    // The previous world position as the current one plus the blended per-vertex delta: a vertex whose transform has
    // not changed contributes an exact zero, so a static object's motion is exactly zero, the same guarantee the
    // raster path gets from interpolating two identical sets of vertex positions.
    o.prevWorldPos = o.worldPos + Blend(
        w0.xyz - MatrixMultiply(float4(v0.m_Pos, 1.0f), instance.m_PrevWorld).xyz,
        w1.xyz - MatrixMultiply(float4(v1.m_Pos, 1.0f), instance.m_PrevWorld).xyz,
        w2.xyz - MatrixMultiply(float4(v2.m_Pos, 1.0f), instance.m_PrevWorld).xyz, weights);

    // The values the raster path carried through its interpolant, so the instance, cluster and LOD debug views keep
    // working. A cluster has no meshlet, so that id stays invalid, exactly as the cluster mesh shader leaves it.
    o.instanceID = job.m_InstanceIndex;
    o.meshletID = 0xFFFFFFFFu;
    o.lodIndex = cluster.m_LODLevel;
    o.clusterID = job.m_ClusterIndex;

    return EvaluateGBufferMaterial(o);
}
