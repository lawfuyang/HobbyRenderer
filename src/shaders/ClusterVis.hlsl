#include "BasePassCommon.hlsli"
#include "GBufferMaterial.hlsli"

/*
	Visibility raster (docs/visibility_buffer_implementation_plan.md §6).

	The geometry work is the cluster base pass's, verbatim: the same job list, the same cluster record decode, the
	same quantised vertex expansion and the same two transform multiplications. Nothing here re-derives a position,
	so the depth this path writes is bit-identical to the raster path's - which is what the phase's gate checks.

	What changes is what leaves the rasteriser. The mesh shader emits the clip position (and the uv, for the
	alpha-tested bucket that reuses this vertex output) plus a per-primitive pair of ids, and the pixel shader
	writes one packed id per pixel instead of six G-buffer values.

	A mesh pipeline gives its pixel shader no SV_PrimitiveID, so the triangle index can only reach it through the
	mesh shader's per-primitive output; the job index rides in the same struct.

	The alpha-tested variant resolves the test here, in the raster, exactly as the G-buffer path does - the uv is the
	rasteriser's own interpolant and the albedo sample goes through the same streamed sampler, so the two make the
	same decision texel for texel. Deferring the test to the resolve would leave nothing behind the discarded texels
	to shade, which is the hole this variant exists to prevent.

	Packing, 32 bits, and zero means "no surface":
	    31..30  list      0 = the opaque pass' own list, 1 = the two-pass culled pass, 2 = the masked bucket
	    29..7   job + 1   1-based, so no visible job encodes as zero
	    6..0    triangle  the triangle's index inside its cluster
*/

static const StructuredBuffer<srrhi::ClusterJob>    g_ClusterJobs    = srrhi::BasePassInputs::GetClusterJobs();
static const StructuredBuffer<srrhi::ClusterRecord> g_ClusterRecords = srrhi::BasePassInputs::GetClusterRecords();

struct ClusterVisPayload
{
    uint m_InstanceIndex;
    uint m_ClusterIndex;
    uint m_JobIndex;
};

groupshared ClusterVisPayload s_ClusterVisPayload;

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ASMain(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID
)
{
    const srrhi::ClusterJob job = g_ClusterJobs[groupId.x];

    if (groupThreadID.x == 0)
    {
        s_ClusterVisPayload.m_InstanceIndex = job.m_InstanceIndex;
        s_ClusterVisPayload.m_ClusterIndex = job.m_ClusterIndex;
        // The dispatch's group id is the job index, and that is what the pixel shader needs to name the cluster
        // back: the resolve reads the job's instance and cluster out of the same list.
        s_ClusterVisPayload.m_JobIndex = groupId.x;
    }

    // Written by one thread of the group, read by every mesh shader thread.
    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(1, 1, 1, s_ClusterVisPayload);
}

struct VisVertexOut
{
    float4 Position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// The per-primitive half of the pixel shader's input. The semantics must not collide with the vertex output's:
// a mesh pipeline packs both into one input signature.
struct VisPrimitiveOut
{
    uint m_JobIndex : SV_PrimitiveID;
    uint m_TriangleIndex : TEXCOORD1;
};

[numthreads(srrhi::CommonConsts::kMaxClusterVertices, 1, 1)]
[outputtopology("triangle")]
void MSMain(
    uint3 groupThreadID : SV_GroupThreadID,
    in payload ClusterVisPayload payload,
    out vertices VisVertexOut vout[srrhi::CommonConsts::kMaxClusterVertices],
    out indices uint3 triangles[srrhi::CommonConsts::kMaxClusterTriangles],
    out primitives VisPrimitiveOut pout[srrhi::CommonConsts::kMaxClusterTriangles]
)
{
    const uint outputIndex = groupThreadID.x;

    const srrhi::ClusterRecord cluster = g_ClusterRecords[payload.m_ClusterIndex];
    SetMeshOutputCounts(cluster.m_VertexCount, cluster.m_TriangleCount);

    const srrhi::PerInstanceData instance = g_Instances[payload.m_InstanceIndex];
    const uint streamBase = ClusterStreamBase(cluster);

    if (outputIndex < cluster.m_VertexCount)
    {
        const srrhi::Vertex vertex = UnpackVertex(ReadClusterVertex(streamBase, cluster, outputIndex));

        // The same two multiplications PrepareVSOut performs, in the same order. A different association of the
        // same matrices rounds differently, and the depth would stop matching the raster path's.
        const float4 worldPos = MatrixMultiply(float4(vertex.m_Pos, 1.0f), instance.m_World);

        VisVertexOut output;
        output.Position = MatrixMultiply(worldPos, g_PerFrame.m_View.m_MatWorldToClip);
        output.uv = vertex.m_Uv;
        vout[outputIndex] = output;
    }

    if (outputIndex < cluster.m_TriangleCount)
    {
        triangles[outputIndex] = UnpackTriangle(g_ClusterStreamData[streamBase + cluster.m_TriangleOffset + outputIndex]);

        VisPrimitiveOut primitive;
        primitive.m_JobIndex = payload.m_JobIndex;
        primitive.m_TriangleIndex = outputIndex;
        pout[outputIndex] = primitive;
    }
}

uint PSMain(VisPrimitiveOut primitive) : SV_TARGET
{
    return PackVisId(primitive.m_JobIndex, primitive.m_TriangleIndex);
}

// The masked bucket: the same alpha test the G-buffer path runs, then the id. The uv is the rasteriser's interpolant
// and the sample goes through the streamed sampler (which also writes the feedback the streaming needs), so the
// survives/stays-empty decision matches the raster path's texel for texel.
uint PSMainAlphaTest(VisPrimitiveOut primitive, VisVertexOut vertex) : SV_TARGET
{
    const srrhi::ClusterJob job = g_ClusterJobs[primitive.m_JobIndex];
    const srrhi::PerInstanceData instance = g_Instances[job.m_InstanceIndex];
    const srrhi::MaterialConstants mat = g_Materials[instance.m_MaterialIndex];

    const bool hasAlbedo = MaterialHasAlbedo(mat);
    const float4 albedoSample = hasAlbedo
        ? SampleBindlessStreamedTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex,
                                        mat.m_AlbedoMinMipIndex, mat.m_AlbedoFeedbackIndex, vertex.uv,
                                        g_PerFrame.m_ForcedTextureMip, uint2(mat.m_MinMipDimsX, mat.m_MinMipDimsY))
        : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);

    if (AlbedoAlpha(mat, albedoSample, hasAlbedo) < mat.m_AlphaCutoff)
        discard;

    return PackVisId(primitive.m_JobIndex, primitive.m_TriangleIndex);
}
