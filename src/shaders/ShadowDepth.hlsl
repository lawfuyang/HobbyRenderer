// ShadowDepth.hlsl — CSM depth-only meshlet pass
// Opaque path:  AS + MS (position-only) + null PS
// Masked path:  AS + MS (position + UV + instanceID) + alpha-test PS
//
// Compile variants (see shaders.cfg):
//   Base:                  ShadowDepth_ASMain / ShadowDepth_MSMain
//   -D SHADOW_ALPHA_TEST=1: ShadowDepth_ASMain / ShadowDepth_MSMain + ShadowDepth_AlphaTest_PSMain

#include "MeshCommon.hlsli"
#include "Bindless.hlsli"
#include "srrhi/hlsl/ShadowDepth.hlsli"

static const srrhi::ShadowDepthConstants                g_CB               = srrhi::ShadowDepthInputs::GetShadowDepthCB();
static const StructuredBuffer<srrhi::PerInstanceData>   g_Instances        = srrhi::ShadowDepthInputs::GetInstances();
static const StructuredBuffer<srrhi::MaterialConstants> g_Materials        = srrhi::ShadowDepthInputs::GetMaterials();
static const StructuredBuffer<srrhi::VertexQuantized>   g_Vertices         = srrhi::ShadowDepthInputs::GetVertices();
static const StructuredBuffer<srrhi::Meshlet>           g_Meshlets         = srrhi::ShadowDepthInputs::GetMeshlets();
static const StructuredBuffer<uint>                     g_MeshletVertices  = srrhi::ShadowDepthInputs::GetMeshletVertices();
static const StructuredBuffer<uint>                     g_MeshletTriangles = srrhi::ShadowDepthInputs::GetMeshletTriangles();
static const StructuredBuffer<srrhi::MeshletJob>        g_MeshletJobs      = srrhi::ShadowDepthInputs::GetMeshletJobs();
static const StructuredBuffer<srrhi::MeshData>          g_MeshData         = srrhi::ShadowDepthInputs::GetMeshData();

// ---------------------------------------------------------------------------
// Shared payload (AS → MS)
// ---------------------------------------------------------------------------
struct ShadowPayload
{
    uint m_InstanceIndex;
    uint m_LODIndex;
    uint m_MeshletIndices[srrhi::CommonConsts::kThreadsPerGroup];
};

groupshared ShadowPayload s_Payload;

cbuffer DrawIDCB : register(b255)
{
    uint g_DrawID;
};

// ---------------------------------------------------------------------------
// Amplification Shader — shared by both opaque and masked paths
// ---------------------------------------------------------------------------
[numthreads(srrhi::CommonConsts::kThreadsPerGroup, 1, 1)]
void ShadowDepth_ASMain(
    uint3 groupThreadID : SV_GroupThreadID,
    uint3 groupId       : SV_GroupID)
{
    srrhi::MeshletJob      job = g_MeshletJobs[g_DrawID];
    uint instanceIndex, lodIndex, meshletIndex, absoluteMeshletIndex;
    srrhi::PerInstanceData inst;
    srrhi::Meshlet         meshlet;

    AS_WritePayloadHeader(s_Payload, job.m_InstanceIndex, job.m_LODIndex, groupThreadID);

    bool bVisible = AS_DecodeMeshletIndex(
        job, g_Instances, g_MeshData, groupThreadID, groupId,
        instanceIndex, lodIndex, meshletIndex, absoluteMeshletIndex,
        inst, meshlet, g_Meshlets);

    if (bVisible)
    {
        // Frustum cull against the light-space VP — conservative clip-space AABB test.
        // No HZB or cone culling for shadow passes.
        float3 center;
        float  radius;
        UnpackMeshletBV(meshlet, center, radius);

        float4 worldCenter = MatrixMultiply(float4(center, 1.0f), inst.m_World);
        float4 lightClip   = MatrixMultiply(worldCenter, g_CB.m_ShadowViewProj);

        float w = abs(lightClip.w) + radius;
        bVisible = (abs(lightClip.x) <= w) && (abs(lightClip.y) <= w)
                && (lightClip.z >= -w)      && (lightClip.z <= w);
    }

    AS_CompactAndDispatch(s_Payload, bVisible, absoluteMeshletIndex);
}

// ---------------------------------------------------------------------------
// Mesh Shader output — position always present; UV + instanceID only for masked
// ---------------------------------------------------------------------------
struct ShadowMSOut
{
    float4 m_Position : SV_Position;
#if SHADOW_ALPHA_TEST
    float2 m_Uv         : TEXCOORD0;
    nointerpolation uint m_InstanceID : TEXCOORD1;
#endif
};

// ---------------------------------------------------------------------------
// Mesh Shader — single entry for both opaque and masked paths
// ---------------------------------------------------------------------------
[numthreads(srrhi::CommonConsts::kMaxMeshletTriangles, 1, 1)]
[outputtopology("triangle")]
void ShadowDepth_MSMain(
    uint3 groupThreadID : SV_GroupThreadID,
    uint3 groupId       : SV_GroupID,
    in payload ShadowPayload payload,
    out vertices ShadowMSOut verts[srrhi::CommonConsts::kMaxMeshletVertices],
    out indices  uint3       tris[srrhi::CommonConsts::kMaxMeshletTriangles])
{
    uint meshletIndex  = payload.m_MeshletIndices[groupId.x];
    uint instanceIndex = payload.m_InstanceIndex;
    uint outputIdx     = groupThreadID.x;

    srrhi::Meshlet         m    = g_Meshlets[meshletIndex];
    srrhi::PerInstanceData inst = g_Instances[instanceIndex];

    SetMeshOutputCounts(m.m_VertexCount, m.m_TriangleCount);

    if (outputIdx < m.m_VertexCount)
    {
        uint vertexIndex = g_MeshletVertices[m.m_VertexOffset + outputIdx];
        srrhi::Vertex v  = UnpackVertex(g_Vertices[vertexIndex]);

        float4 worldPos = MatrixMultiply(float4(v.m_Pos, 1.0f), inst.m_World);
        verts[outputIdx].m_Position = MatrixMultiply(worldPos, g_CB.m_ShadowViewProj);
#if SHADOW_ALPHA_TEST
        verts[outputIdx].m_Uv         = v.m_Uv;
        verts[outputIdx].m_InstanceID = instanceIndex;
#endif
    }

    if (outputIdx < m.m_TriangleCount)
        tris[outputIdx] = UnpackTriangle(g_MeshletTriangles[m.m_TriangleOffset + outputIdx]);
}

// ---------------------------------------------------------------------------
// Alpha-Test Pixel Shader — discards transparent fragments, no color output
// ---------------------------------------------------------------------------
#if SHADOW_ALPHA_TEST

void ShadowDepth_AlphaTest_PSMain(ShadowMSOut input)
{
    srrhi::PerInstanceData   inst = g_Instances[input.m_InstanceID];
    srrhi::MaterialConstants mat  = g_Materials[inst.m_MaterialIndex];

    float alpha = mat.m_BaseColor.w;
    if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ALBEDO) != 0)
    {
        SamplerState      sam       = SamplerDescriptorHeap[NonUniformResourceIndex(mat.m_AlbedoSamplerIndex)];
        Texture2D<float4> albedoTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_AlbedoTextureIndex)];
        alpha *= albedoTex.Sample(sam, input.m_Uv).a;
    }

    if (alpha < mat.m_AlphaCutoff)
        discard;
    // No SV_Target — depth written by hardware rasterizer
}

#endif // SHADOW_ALPHA_TEST
