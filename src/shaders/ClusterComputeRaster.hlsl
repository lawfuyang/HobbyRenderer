#include "BasePassCommon.hlsli"
#include "srrhi/hlsl/ClusterComputeRaster.hlsli"

/*
	Compute software raster for sub-pixel clusters (docs/visibility_buffer_implementation_plan.md, phase 7).

	The cluster cull routes a cluster here when its projected half-extent is at most
	--compute-raster-pixels, because that is the size at which the hardware rasteriser spends most of its work on
	helper lanes: a triangle covering one or two pixels still occupies a 2x2 quad of pixel-shader invocations and
	still costs the fixed-function setup. This pass covers those triangles itself, one sample per pixel centre,
	and appends a stamp for each covered sample.

	The stamps are replayed by StampRaster.hlsl as one-pixel points through the hardware depth test. That split is
	what keeps depth and stencil authoritative without a depth-format change: D24S8 cannot be a UAV, so a compute
	pass cannot write it, and the depth buffer also carries the base pass' stencil coverage mask.

	The vertex decode is ClusterVis.hlsl's, verbatim - same record, same quantised expansion, same two matrix
	multiplications - so a routed triangle lands on the same samples with the same depth the mesh path would have
	produced. Winding is not tested: the opaque cluster draw runs with back-face culling off (m_BackFaceCull is
	only set for the blended bucket), so the software path must not cull either.

	Measured, and the reason it is off by default: routing 78 % of a scene's clusters here (animals, 8 px
	threshold) leaves the image one pixel short of identical but costs +15 % GPU time (+5.7 % at 2 px, a wash on
	Bistro). The pass exists to remove helper-lane waste, and the shader it replaces writes a single uint - 31 M of
	Bistro's 8,192 M instructions - so there is nearly nothing to save while the dispatch, the barrier and the
	per-sample append are all new cost. It pays only once the raster would shade materials, which here it does not.
*/

static const StructuredBuffer<srrhi::ClusterJob> g_ComputeJobs = srrhi::BasePassInputs::GetClusterJobs();
static const StructuredBuffer<srrhi::ClusterRecord> g_ClusterRecords = srrhi::BasePassInputs::GetClusterRecords();
static const RWStructuredBuffer<uint4> g_Stamps = srrhi::ClusterComputeRasterInputs::GetStamps();
static const RWStructuredBuffer<uint> g_StampArgs = srrhi::ClusterComputeRasterInputs::GetStampArgs();

// One thread group per routed cluster; the group walks the cluster's triangles. Must match the stamp buffer's
// element count on the host (ClusterComputeRasterRenderer::kMaxStamps).
static const uint kMaxStamps = 1u << 20;
static const uint kThreadsPerCluster = 64u;

// Twice the signed area of the triangle (a, b, c) in pixel space.
float EdgeFunction(float2 a, float2 b, float2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

[numthreads(kThreadsPerCluster, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThread : SV_GroupThreadID)
{
    const uint jobIndex = groupId.x;
    const srrhi::ClusterJob job = g_ComputeJobs[jobIndex];
    const srrhi::ClusterRecord cluster = g_ClusterRecords[job.m_ClusterIndex];
    const srrhi::PerInstanceData instance = g_Instances[job.m_InstanceIndex];
    const uint streamBase = ClusterStreamBase(cluster);

    const float2 targetSize = float2(g_PerFrame.m_Width, g_PerFrame.m_Height);

    for (uint triangleIndex = groupThread.x; triangleIndex < cluster.m_TriangleCount; triangleIndex += kThreadsPerCluster)
    {
        const uint3 corners = UnpackTriangle(g_ClusterStreamData[streamBase + cluster.m_TriangleOffset + triangleIndex]);

        float4 clip[3];
        float2 pixel[3];
        float depth[3];
        bool bAnyBehind = false;

        [unroll] for (uint corner = 0u; corner < 3u; ++corner)
        {
            const srrhi::Vertex vertex = UnpackVertex(ReadClusterVertex(streamBase, cluster, corners[corner]));
            const float4 worldPos = MatrixMultiply(float4(vertex.m_Pos, 1.0f), instance.m_World);
            clip[corner] = MatrixMultiply(worldPos, g_PerFrame.m_View.m_MatWorldToClip);
            bAnyBehind = bAnyBehind || clip[corner].w <= 0.0f;
        }

        // A routed cluster is small on screen, so a triangle of it reaching the near plane is a camera artefact
        // rather than a case worth clipping: dividing by a non-positive w projects to the wrong side of the screen.
        if (bAnyBehind)
            continue;

        [unroll] for (uint corner = 0u; corner < 3u; ++corner)
        {
            const float3 ndc = clip[corner].xyz / clip[corner].w;
            // The pixel-centre convention SV_Position reports: ClipToPixel in VisibilityResolve.hlsl.
            pixel[corner] = (ndc.xy * float2(0.5f, -0.5f) + 0.5f) * targetSize;
            depth[corner] = ndc.z;
        }

        const float doubleArea = EdgeFunction(pixel[0], pixel[1], pixel[2]);
        if (doubleArea == 0.0f)
            continue;

        // Conservative box, then the sample test. Dividing by the signed area normalises the winding, so the
        // inside test is the same three signs for either orientation.
        const float2 lo = min(min(pixel[0], pixel[1]), pixel[2]);
        const float2 hi = max(max(pixel[0], pixel[1]), pixel[2]);
        const int2 boxMin = max(int2(floor(lo)), int2(0, 0));
        const int2 boxMax = min(int2(ceil(hi)), int2(targetSize) - 1);
        if (any(boxMin > boxMax))
            continue;

        const uint id = PackVisId(jobIndex, triangleIndex);

        for (int y = boxMin.y; y <= boxMax.y; ++y)
        {
            for (int x = boxMin.x; x <= boxMax.x; ++x)
            {
                const float2 sample = float2(float(x), float(y)) + 0.5f;
                const float w0 = EdgeFunction(pixel[1], pixel[2], sample) / doubleArea;
                const float w1 = EdgeFunction(pixel[2], pixel[0], sample) / doubleArea;
                const float w2 = EdgeFunction(pixel[0], pixel[1], sample) / doubleArea;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                    continue;

                // Depth is linear in screen-space barycentrics, so this is the value the rasteriser would
                // interpolate at the pixel centre.
                const float z = w0 * depth[0] + w1 * depth[1] + w2 * depth[2];

                uint slot;
                InterlockedAdd(g_StampArgs[1], 1u, slot);
                if (slot < kMaxStamps)
                    g_Stamps[slot] = uint4(uint(x), uint(y), asuint(z), id);
                else
                    InterlockedAdd(g_StampArgs[3], 1u);
            }
        }
    }
}
