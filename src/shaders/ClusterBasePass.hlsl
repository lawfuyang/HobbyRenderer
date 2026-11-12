#include "BasePassCommon.hlsli"

/*
	Cluster base pass (docs/cluster-lod-implementation-plan.md §4 phase 2).

	AS  one thread group per visible cluster: reads the job the traversal appended and hands it to the mesh
	    shader. The traversal has already done the frustum, occlusion and cut tests, so there is nothing to
	    reject here - a second test would only re-derive the same answer from the same bounds.

	MS  one cluster per workgroup: decodes the cluster's record, expands its vertices from the quantised vertex
	    buffer and its triangles from the packed micro indices, and writes the same VSOut the meshlet path
	    writes, so the G-buffer / alpha-test pixel shaders are shared unchanged.

	The vertex count and the triangle count come from the cluster record, which the CPU built from the cooked
	cluster data; the shader's output sizes are the builder's own limits (CommonConsts::kMaxCluster*).
*/

static const StructuredBuffer<srrhi::ClusterJob>    g_ClusterJobs      = srrhi::BasePassInputs::GetClusterJobs();
static const StructuredBuffer<srrhi::ClusterRecord> g_ClusterRecords   = srrhi::BasePassInputs::GetClusterRecords();
static const StructuredBuffer<uint>                 g_ClusterTriangles = srrhi::BasePassInputs::GetClusterTriangles();
static const StructuredBuffer<uint>                 g_ClusterVertices  = srrhi::BasePassInputs::GetClusterVertices();

struct ClusterMeshPayload
{
    uint m_InstanceIndex;
    uint m_ClusterIndex;
};

groupshared ClusterMeshPayload s_ClusterPayload;

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ASMain(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID
)
{
    // The dispatch launches one thread group per job in the visible-cluster list, so the dispatch group id is
    // the job index - the same contract the meshlet path uses for its own job list.
    const srrhi::ClusterJob job = g_ClusterJobs[groupId.x];

    if (groupThreadID.x == 0)
    {
        s_ClusterPayload.m_InstanceIndex = job.m_InstanceIndex;
        s_ClusterPayload.m_ClusterIndex = job.m_ClusterIndex;
    }

    // The payload is written by one thread and read by every mesh-shader thread, so the groupshared write has to
    // be visible before DispatchMesh is called.
    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(1, 1, 1, s_ClusterPayload);
}

[numthreads(srrhi::CommonConsts::kMaxClusterVertices, 1, 1)]
[outputtopology("triangle")]
void MSMain(
    uint3 groupThreadID : SV_GroupThreadID,
    in payload ClusterMeshPayload payload,
    out vertices VSOut vout[srrhi::CommonConsts::kMaxClusterVertices],
    out indices uint3 triangles[srrhi::CommonConsts::kMaxClusterTriangles]
)
{
    const uint outputIndex = groupThreadID.x;

    const srrhi::ClusterRecord cluster = g_ClusterRecords[payload.m_ClusterIndex];
    SetMeshOutputCounts(cluster.m_VertexCount, cluster.m_TriangleCount);

    const srrhi::PerInstanceData instance = g_Instances[payload.m_InstanceIndex];

    if (outputIndex < cluster.m_VertexCount)
    {
        const uint vertexIndex = g_ClusterVertices[cluster.m_VertexOffset + outputIndex];
        const srrhi::Vertex vertex = UnpackVertex(g_Vertices[vertexIndex]);

        // A cluster has no meshlet and no per-cluster material yet, so meshletID stays invalid; the LOD level
        // rides in lodIndex, which is what the LOD debug view and the motion-vector path read.
        VSOut output = PrepareVSOut(vertex, instance, payload.m_InstanceIndex, 0xFFFFFFFFu, cluster.m_LODLevel);
        output.clusterID = payload.m_ClusterIndex;
        vout[outputIndex] = output;
    }

    if (outputIndex < cluster.m_TriangleCount)
        triangles[outputIndex] = UnpackTriangle(g_ClusterTriangles[cluster.m_TriangleOffset + outputIndex]);
}
