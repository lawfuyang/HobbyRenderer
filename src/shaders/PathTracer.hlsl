#include "ShaderShared.h"
#include "RaytracingCommon.hlsli"
#include "CommonLighting.hlsli"

cbuffer PathTracerCB : register(b0)
{
    PathTracerConstants g_PathTracer;
};

RaytracingAccelerationStructure g_SceneAS : register(t0);
StructuredBuffer<GPULight> g_Lights : register(t1);
StructuredBuffer<PerInstanceData> g_Instances : register(t2);
StructuredBuffer<MeshData> g_MeshData : register(t3);
StructuredBuffer<MaterialConstants> g_Materials : register(t4);
StructuredBuffer<uint> g_Indices : register(t5);
StructuredBuffer<VertexQuantized> g_Vertices : register(t6);

RWTexture2D<float4> g_Output : register(u0);
RWTexture2D<float4> g_Accumulation : register(u1);

[numthreads(8, 8, 1)]
void PathTracer_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= (uint)g_PathTracer.m_View.m_ViewportSize.x || dispatchThreadID.y >= (uint)g_PathTracer.m_View.m_ViewportSize.y)
        return;

    float2 jitter = g_PathTracer.m_Jitter;
    float2 uv = (float2(dispatchThreadID.xy) + 0.5f + jitter) * g_PathTracer.m_View.m_ViewportSizeInv;
    float2 clipPos;
    clipPos.x = uv.x * 2.0f - 1.0f;
    clipPos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    
    // just use 0.9. we're only interested in the direction
    float4 rayEndFar = MatrixMultiply(float4(clipPos, 0.9f, 1.0f), g_PathTracer.m_View.m_MatClipToWorldNoOffset);
    rayEndFar.xyz /= rayEndFar.w;

    float3 rayDir = normalize(rayEndFar.xyz - g_PathTracer.m_CameraPos.xyz);
    float3 rayOrigin = g_PathTracer.m_CameraPos.xyz;

    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = rayDir;
    ray.TMin = 0.0f;
    ray.TMax = 1e10f;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
    
    float3 finalColor = float3(0, 0, 0);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instanceIndex = q.CandidateInstanceIndex();
            uint primitiveIndex = q.CandidatePrimitiveIndex();
            float2 bary = q.CandidateTriangleBarycentrics();

            PerInstanceData inst = g_Instances[instanceIndex];
            MeshData mesh = g_MeshData[inst.m_MeshDataIndex];
            MaterialConstants mat = g_Materials[inst.m_MaterialIndex];
            
            float2 uvSample = GetInterpolatedUV(primitiveIndex, bary, mesh, g_Indices, g_Vertices);

            if (AlphaTest(uvSample, mat))
            {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        RayHitInfo hit;
        hit.m_InstanceIndex = q.CommittedInstanceIndex();
        hit.m_PrimitiveIndex = q.CommittedPrimitiveIndex();
        hit.m_Barycentrics = q.CommittedTriangleBarycentrics();
        hit.m_RayT = q.CommittedRayT();

        PerInstanceData inst = g_Instances[hit.m_InstanceIndex];
        MeshData mesh = g_MeshData[inst.m_MeshDataIndex];
        MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

        FullHitAttributes attr = GetFullHitAttributes(hit, ray, inst, mesh, g_Indices, g_Vertices);
        PBRAttributes pbr = GetPBRAttributes(attr.m_Uv, mat, 0.0f);

        LightingInputs inputs;
        inputs.N = attr.m_WorldNormal;
        inputs.V = -rayDir;
        inputs.L = float3(0, 0, 0); // Placeholder
        inputs.worldPos = attr.m_WorldPos;
        inputs.baseColor = pbr.baseColor;
        inputs.roughness = pbr.roughness;
        inputs.metallic = pbr.metallic;
        inputs.ior = mat.m_IOR;
        inputs.radianceMipCount = 0; // not used
        inputs.enableRTShadows = true;
        inputs.sceneAS = g_SceneAS;
        inputs.instances = g_Instances;
        inputs.meshData = g_MeshData;
        inputs.materials = g_Materials;
        inputs.indices = g_Indices;
        inputs.vertices = g_Vertices;
        inputs.lights = g_Lights;
        inputs.sunRadiance = 0;
        inputs.sunDirection = 0;
        inputs.useSunRadiance = false;
        inputs.sunShadow = 1.0f;

        PrepareLightingByproducts(inputs);

        LightingComponents direct = AccumulateDirectLighting(inputs, g_PathTracer.m_LightCount);
        finalColor = direct.diffuse + direct.specular;

        finalColor += pbr.emissive;
    }
    else
    {
        // Sky, hardcode for now
        finalColor = float3(1, 1, 1);
    }

    float4 accum = float4(finalColor, 1.0f);
    if (g_PathTracer.m_AccumulationIndex > 0)
    {
        accum += g_Accumulation[dispatchThreadID.xy];
    }
    g_Accumulation[dispatchThreadID.xy] = accum;

    g_Output[dispatchThreadID.xy] = float4(accum.rgb / accum.a, 1.0f);
}
