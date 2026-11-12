#include "BasePassCommon.hlsli"

/*
	Cluster bounds debug view (docs/cluster-lod-implementation-plan.md §3.4, mode 3).

	Draws the culling bounds of every visible cluster as a wireframe box, in the same framebuffer and with the
	same depth buffer as the cluster base pass. That is what makes it readable: a box that the traversal kept
	but that is not visible on screen is drawn only where its edges pass in front of geometry, and the depth
	write is off so the overlay cannot disturb the base pass or the HZB built from it.

	The bounds are the group's, not the cluster's - the group is the unit the cut decision and the culling were
	made with, so a box that is too large is exactly the looseness that kept the cluster.
*/

static const StructuredBuffer<srrhi::ClusterJob>   g_ClusterJobs    = srrhi::BasePassInputs::GetClusterJobs();
static const StructuredBuffer<srrhi::ClusterRecord> g_ClusterRecords = srrhi::BasePassInputs::GetClusterRecords();
static const StructuredBuffer<srrhi::ClusterGroup> g_ClusterGroups  = srrhi::BasePassInputs::GetClusterGroups();
static const StructuredBuffer<uint>                g_ClusterTriangles = srrhi::BasePassInputs::GetClusterTriangles();
static const StructuredBuffer<uint>                g_ClusterVertices  = srrhi::BasePassInputs::GetClusterVertices();

// 12 edges over 8 corners, where corner bit 0 is max/min x, bit 1 y and bit 2 z.
static const uint kBoxEdges[12][2] =
{
    { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },   // along x
    { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },   // along y
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }    // along z
};

struct ClusterBoundsPayload
{
    uint m_InstanceIndex;
    uint m_ClusterIndex;
};

groupshared ClusterBoundsPayload s_BoundsPayload;

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ASMain(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID
)
{
    const srrhi::ClusterJob job = g_ClusterJobs[groupId.x];

    if (groupThreadID.x == 0)
    {
        s_BoundsPayload.m_InstanceIndex = job.m_InstanceIndex;
        s_BoundsPayload.m_ClusterIndex = job.m_ClusterIndex;
    }

    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(1, 1, 1, s_BoundsPayload);
}

struct BoundsVertexOut
{
    float4 Position : SV_POSITION;
};

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
[outputtopology("line")]
void MSMain(
    uint3 groupThreadID : SV_GroupThreadID,
    in payload ClusterBoundsPayload payload,
    out vertices BoundsVertexOut vout[24],
    out indices uint2 lines[12]
)
{
    SetMeshOutputCounts(24, 12);

    const uint outputIndex = groupThreadID.x;

    if (outputIndex < 24)
    {
        const uint edge = outputIndex >> 1;
        const uint corner = kBoxEdges[edge][outputIndex & 1u];

        const srrhi::ClusterRecord cluster = g_ClusterRecords[payload.m_ClusterIndex];
        const srrhi::ClusterGroup group = g_ClusterGroups[cluster.m_GroupIndex];
        const srrhi::PerInstanceData instance = g_Instances[payload.m_InstanceIndex];

        const float3 local = float3(
            (corner & 1u) ? group.m_Bounds.m_Max.x : group.m_Bounds.m_Min.x,
            (corner & 2u) ? group.m_Bounds.m_Max.y : group.m_Bounds.m_Min.y,
            (corner & 4u) ? group.m_Bounds.m_Max.z : group.m_Bounds.m_Min.z);

        const float4 world = MatrixMultiply(float4(local, 1.0f), instance.m_World);

        BoundsVertexOut output;
        output.Position = MatrixMultiply(world, g_PerFrame.m_View.m_MatWorldToClip);
        vout[outputIndex] = output;
    }

    if (outputIndex < 12)
        lines[outputIndex] = uint2(outputIndex * 2u, outputIndex * 2u + 1u);
}

// The base pass G-buffer layout. The overlay writes no albedo and puts its colour in emissive, which the
// deferred pass adds without lighting it, so the lines stay legible whatever the surface under them looks like.
struct ClusterDebugOut
{
    float4 Albedo        : SV_TARGET0;
    float2 Normal        : SV_TARGET1;
    float2 ORM           : SV_TARGET2;
    float4 Emissive      : SV_TARGET3;
    float4 MotionVectors : SV_TARGET4;
    float2 GeoNormal     : SV_TARGET5;
};

ClusterDebugOut PSMain()
{
    ClusterDebugOut output;
    output.Albedo = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.Normal = float2(0.5f, 0.5f);
    output.ORM = float2(0.5f, 0.0f);
    output.Emissive = float4(1.0f, 0.55f, 0.1f, 1.0f);
    output.MotionVectors = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.GeoNormal = float2(0.5f, 0.5f);
    return output;
}

/*
	Per-triangle debug view (docs/cluster-lod-implementation-plan.md §3.4, mode 15).

	A mesh shader writes its vertices once and indexes them from its primitives, and a pixel shader in a mesh
	pipeline is not given SV_PrimitiveID, so the base pass cannot colour a triangle by its index. This overlay
	re-emits every visible cluster's triangles with each corner written out on its own, carrying the triangle's
	index, and colours it the way the Nanite debug view does.

	It shares the base pass's framebuffer, job list and depth buffer: the depth test is read-equal (the corner
	positions come from the same vertex data and the same matrices, so the depth matches exactly), the write is
	off, and the albedo target is overwritten - which is why the deferred pass passes this mode through unlit.
*/

static const uint kTrianglesPerMesh = 64;

struct ClusterTrianglesPayload
{
    uint m_InstanceIndex;
    uint m_ClusterIndex;
};

groupshared ClusterTrianglesPayload s_TrianglesPayload;

[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void TrianglesASMain(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID
)
{
    const srrhi::ClusterJob job = g_ClusterJobs[groupId.x];

    if (groupThreadID.x == 0)
    {
        s_TrianglesPayload.m_InstanceIndex = job.m_InstanceIndex;
        s_TrianglesPayload.m_ClusterIndex = job.m_ClusterIndex;
    }

    GroupMemoryBarrierWithGroupSync();

    // Two meshes per cluster: three corners per triangle would pass the mesh shader's 256-vertex output limit
    // for a full 128-triangle cluster in a single mesh.
    DispatchMesh(2, 1, 1, s_TrianglesPayload);
}

struct TriangleVertexOut
{
    float4 Position : SV_POSITION;
    nointerpolation uint m_ClusterID : TEXCOORD0;
    nointerpolation uint m_TriangleID : TEXCOORD1;
};

TriangleVertexOut MakeTriangleVertex(uint microIndex, srrhi::ClusterRecord cluster, srrhi::PerInstanceData instance,
    uint clusterID, uint triangleID)
{
    const uint vertexIndex = g_ClusterVertices[cluster.m_VertexOffset + microIndex];
    const srrhi::Vertex vertex = UnpackVertex(g_Vertices[vertexIndex]);

    TriangleVertexOut output;
    output.Position = MatrixMultiply(
        MatrixMultiply(float4(vertex.m_Pos, 1.0f), instance.m_World), g_PerFrame.m_View.m_MatWorldToClip);
    output.m_ClusterID = clusterID;
    output.m_TriangleID = triangleID;
    return output;
}

[numthreads(kTrianglesPerMesh, 1, 1)]
[outputtopology("triangle")]
void TrianglesMSMain(
    uint3 groupID : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID,
    in payload ClusterTrianglesPayload payload,
    out vertices TriangleVertexOut vout[3u * kTrianglesPerMesh],
    out indices uint3 triangles[kTrianglesPerMesh]
)
{
    const srrhi::ClusterRecord cluster = g_ClusterRecords[payload.m_ClusterIndex];
    const uint first = groupID.x * kTrianglesPerMesh;
    const uint count = first < cluster.m_TriangleCount ?
        min(kTrianglesPerMesh, cluster.m_TriangleCount - first) : 0;

    // Every thread has to reach this call with the same values.
    SetMeshOutputCounts(3u * count, count);

    const uint triangleIndex = groupThreadID.x;

    if (triangleIndex < count)
    {
        const srrhi::PerInstanceData instance = g_Instances[payload.m_InstanceIndex];
        const uint3 corners = UnpackTriangle(g_ClusterTriangles[cluster.m_TriangleOffset + first + triangleIndex]);
        const uint triangleID = first + triangleIndex;

        vout[triangleIndex * 3u + 0u] = MakeTriangleVertex(corners.x, cluster, instance, payload.m_ClusterIndex, triangleID);
        vout[triangleIndex * 3u + 1u] = MakeTriangleVertex(corners.y, cluster, instance, payload.m_ClusterIndex, triangleID);
        vout[triangleIndex * 3u + 2u] = MakeTriangleVertex(corners.z, cluster, instance, payload.m_ClusterIndex, triangleID);

        triangles[triangleIndex] = uint3(triangleIndex * 3u, triangleIndex * 3u + 1u, triangleIndex * 3u + 2u);
    }
}

ClusterDebugOut TrianglesPSMain(TriangleVertexOut input)
{
    // One colour per triangle, with the cluster mixed in so neighbouring clusters do not repeat the sequence.
    const uint id = input.m_TriangleID * 0x9E3779B1u + input.m_ClusterID * 0x85EBCA77u;

    ClusterDebugOut output;
    output.Albedo = float4(HashColor(id), 1.0f);
    output.Normal = float2(0.5f, 0.5f);
    output.ORM = float2(0.5f, 0.0f);
    output.Emissive = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.MotionVectors = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.GeoNormal = float2(0.5f, 0.5f);
    return output;
}
