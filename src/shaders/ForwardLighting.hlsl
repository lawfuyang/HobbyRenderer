#include "ShaderShared.h"
#include "Culling.h"

cbuffer PerFrameCB : register(b0, space1)
{
    ForwardLightingPerFrameData g_PerFrame;
};

StructuredBuffer<PerInstanceData> g_Instances : register(t0, space1);
StructuredBuffer<MaterialConstants> g_Materials : register(t1, space1);
StructuredBuffer<Vertex> g_Vertices : register(t2, space1);
StructuredBuffer<Meshlet> g_Meshlets : register(t3, space1);
StructuredBuffer<uint> g_MeshletVertices : register(t4, space1);
StructuredBuffer<uint> g_MeshletTriangles : register(t5, space1);
StructuredBuffer<MeshletJob> g_MeshletJobs : register(t6, space1);
StructuredBuffer<MeshData> g_MeshData : register(t7, space1);
Texture2D<float> g_HZB : register(t8, space1);
RaytracingAccelerationStructure g_SceneAS : register(t9, space1);

SamplerState g_SamplerAnisoClamp : register(s0, space1);
SamplerState g_SamplerAnisoWrap  : register(s1, space1);
SamplerState g_MinReductionSampler : register(s2, space1);

float3x3 MakeAdjugateMatrix(float4x4 m)
{
    return float3x3
    (
		cross(m[1].xyz, m[2].xyz),
		cross(m[2].xyz, m[0].xyz),
		cross(m[0].xyz, m[1].xyz)
	);
}

float3 TransformNormal(float3 normal, float4x4 worldMatrix)
{
    float3x3 adjugateWorldMatrix = MakeAdjugateMatrix(worldMatrix);
    return normalize(mul(normal, adjugateWorldMatrix));
}

struct VSOut
{
    float4 Position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    nointerpolation uint instanceID : TEXCOORD2;
    nointerpolation uint meshletID : TEXCOORD3;
    nointerpolation uint lodIndex : TEXCOORD4;
};

VSOut PrepareVSOut(Vertex v, PerInstanceData inst, uint instanceID, uint meshletID, uint lodIndex)
{
    VSOut o;
    float4 worldPos = mul(float4(v.m_Pos, 1.0f), inst.m_World);
    o.Position = mul(worldPos, g_PerFrame.m_ViewProj);

    o.normal = TransformNormal(v.m_Normal, inst.m_World);
    o.uv = v.m_Uv;
    o.worldPos = worldPos.xyz;
    o.instanceID = instanceID;
    o.meshletID = meshletID;
    o.lodIndex = lodIndex;
    return o;
}

VSOut VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_StartInstanceLocation)
{
    PerInstanceData inst = g_Instances[instanceID];
    Vertex v = g_Vertices[vertexID];
    return PrepareVSOut(v, inst, instanceID, 0xFFFFFFFF, 0);
}

struct MeshPayload
{
    uint m_InstanceIndex;
    uint m_LODIndex;
    uint m_MeshletIndices[kThreadsPerGroup];
};

groupshared MeshPayload s_Payload;

[numthreads(kThreadsPerGroup, 1, 1)]
void ASMain(
    uint3 dispatchThreadID : SV_DispatchThreadID,
    uint3 groupThreadID : SV_GroupThreadID,
    uint3 groupId : SV_GroupID,
    uint groupIndex : SV_GroupIndex
    DRAW_INDEX_ARG_COMMA
)
{
    MeshletJob job = g_MeshletJobs[GET_DRAW_INDEX()];
    uint instanceIndex = job.m_InstanceIndex;
    uint lodIndex = job.m_LODIndex;
    uint meshletOffset = groupId.x * kThreadsPerGroup;

    if (groupThreadID.x == 0)
    {
        s_Payload.m_InstanceIndex = instanceIndex;
        s_Payload.m_LODIndex = lodIndex;
    }

    bool bVisible = false;

    uint meshletIndex = meshletOffset + groupThreadID.x;
    PerInstanceData inst = g_Instances[instanceIndex];
    MeshData mesh = g_MeshData[inst.m_MeshDataIndex];

    if (meshletIndex < mesh.m_MeshletCounts[lodIndex])
    {
        uint absoluteMeshletIndex = mesh.m_MeshletOffsets[lodIndex] + meshletIndex;
        Meshlet m = g_Meshlets[absoluteMeshletIndex];

        // Transform meshlet sphere to world space, then to view space
        float4 worldCenter = mul(float4(m.m_Center, 1.0f), inst.m_World);
        float3 viewCenter = mul(worldCenter, g_PerFrame.m_View).xyz;

        // Approximate world-space radius using max scale from world matrix
        float worldRadius = m.m_Radius * GetMaxScale(inst.m_World);

        if (g_PerFrame.m_EnableFrustumCulling)
        {
            bVisible = FrustumSphereTest(viewCenter, worldRadius, g_PerFrame.m_FrustumPlanes);
        }
        else
        {
            bVisible = true;
        }

        if (bVisible && g_PerFrame.m_EnableOcclusionCulling)
        {
            bVisible &= OcclusionSphereTest(viewCenter, worldRadius, uint2(g_PerFrame.m_HZBWidth, g_PerFrame.m_HZBHeight), g_PerFrame.m_P00, g_PerFrame.m_P11, g_HZB, g_MinReductionSampler);
        }

        if (bVisible && g_PerFrame.m_EnableConeCulling)
        {
            uint packedCone = m.m_ConeAxisAndCutoff;
            float3 coneAxis;
            coneAxis.x = (float(packedCone & 0xFF) / 255.0f) * 2.0f - 1.0f;
            coneAxis.y = (float((packedCone >> 8) & 0xFF) / 255.0f) * 2.0f - 1.0f;
            coneAxis.z = (float((packedCone >> 16) & 0xFF) / 255.0f) * 2.0f - 1.0f;
            float coneCutoff = float((packedCone >> 24) & 0xFF) / 254.0f;

            float3 worldConeAxis = TransformNormal(coneAxis, inst.m_World);
            float3 dir = worldCenter.xyz - g_PerFrame.m_CullingCameraPos.xyz;
            float d = length(dir);

            if (dot(worldConeAxis, dir) >= coneCutoff * d + worldRadius)
            {
                bVisible = false;
            }
        }

        if (bVisible)
        {
            uint payloadIdx = WavePrefixCountBits(bVisible);
            s_Payload.m_MeshletIndices[payloadIdx] = absoluteMeshletIndex;
        }
    }

    uint numVisible = WaveActiveCountBits(bVisible);
    DispatchMesh(numVisible, 1, 1, s_Payload);
}

[numthreads(kMaxMeshletTriangles, 1, 1)]
[outputtopology("triangle")]
void MSMain(
    uint3 dispatchThreadID : SV_DispatchThreadID,
    uint3 groupThreadID : SV_GroupThreadID,
    uint3 groupId : SV_GroupID,
    uint groupIndex : SV_GroupIndex,
    in payload MeshPayload payload,
    out vertices VSOut vout[kMaxMeshletVertices],
    out indices uint3 triangles[kMaxMeshletTriangles]
)
{
    uint meshletIndex = payload.m_MeshletIndices[groupId.x];
    uint instanceIndex = payload.m_InstanceIndex;
    uint outputIdx = groupThreadID.x;

    Meshlet m = g_Meshlets[meshletIndex];
    
    SetMeshOutputCounts(m.m_VertexCount, m.m_TriangleCount);
    
    if (outputIdx < m.m_VertexCount)
    {
        uint vertexIndex = g_MeshletVertices[m.m_VertexOffset + outputIdx];
        Vertex v = g_Vertices[vertexIndex];
        
        PerInstanceData inst = g_Instances[instanceIndex];
        vout[outputIdx] = PrepareVSOut(v, inst, instanceIndex, meshletIndex, payload.m_LODIndex);
    }
    
    if (outputIdx < m.m_TriangleCount)
    {
        uint packedTri = g_MeshletTriangles[m.m_TriangleOffset + outputIdx];
        triangles[outputIdx] = uint3(packedTri & 0xFF, (packedTri >> 8) & 0xFF, (packedTri >> 16) & 0xFF);
    }
}

// Unpacks a 2 channel normal to xyz
float3 TwoChannelNormalX2(float2 normal)
{
    float2 xy = 2.0f * normal - 1.0f;
    float z = sqrt(saturate(1.0f - dot(xy, xy)));
    return float3(xy.x, xy.y, z);
}

// Christian Schuler, "Normal Mapping without Precomputed Tangents", ShaderX 5, Chapter 2.6, pp. 131-140
// See also follow-up blog post: http://www.thetenthplanet.de/archives/1180
float3x3 CalculateTBNWithoutTangent(float3 p, float3 n, float2 uv)
{
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float r = 1.0 / (duv1.x * duv2.y - duv2.x * duv1.y);
    float3 T = (dp1 * duv2.y - dp2 * duv1.y) * r;
    float3 B = (dp2 * duv1.x - dp1 * duv2.x) * r;
    return float3x3(normalize(T), normalize(B), n);
}

// 0.08 is a max F0 we define for dielectrics which matches with Crystalware and gems (0.05 - 0.08)
// This means we cannot represent Diamond-like surfaces as they have an F0 of 0.1 - 0.2
float DielectricSpecularToF0(float specular)
{
    return 0.08f * specular;
}

//Note from Filament: vec3 f0 = 0.16 * reflectance * reflectance * (1.0 - metallic) + baseColor * metallic;
// F0 is the base specular reflectance of a surface
// For dielectrics, this is monochromatic commonly between 0.02 (water) and 0.08 (gems) and derived from a separate specular value
// For conductors, this is based on the base color we provided
float3 ComputeF0(float specular, float3 baseColor, float metalness)
{
    return lerp(DielectricSpecularToF0(specular).xxx, baseColor, metalness);
}

float3 ComputeF0(float3 baseColor, float metalness)
{
    const float kMaterialSpecular = 0.5f;
    return lerp(DielectricSpecularToF0(kMaterialSpecular).xxx, baseColor, metalness);
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float3 F_Schlick(float3 f0, float VdotH)
{
    float Fc = pow(1.0f - VdotH, 5.0f);
    return Fc + (1.0f - Fc) * f0;
}

// GGX / Trowbridge-Reitz
// Note the division by PI here
// [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"]
float D_GGX(float a2, float NdotH)
{
    float d = (NdotH * a2 - NdotH) * NdotH + 1;
    return a2 / (PI * d * d);
}

// Appoximation of joint Smith term for GGX
// Returned value is G2 / (4 * NdotL * NdotV). So predivided by specular BRDF denominator
// [Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"]
float Vis_SmithJointApprox(float a2, float NdotV, float NdotL)
{
    float Vis_SmithV = NdotL * (NdotV * (1 - a2) + a2);
    float Vis_SmithL = NdotV * (NdotL * (1 - a2) + a2);
    return 0.5 * rcp(Vis_SmithV + Vis_SmithL);
}

// Oren-Nayar diffuse model returning the scalar BRDF multiplier
// https://mimosa-pudica.net/improved-oren-nayar.html
float OrenNayar(float NdotL, float NdotV, float LdotV, float a2, float albedo)
{
  float s = LdotV - NdotL * NdotV;
  float t = lerp(1.0, max(NdotL, NdotV), step(0.0, s));

  float A = 1.0 + a2 * (albedo / (a2 + 0.13) + 0.5 / (a2 + 0.33));
  float B = 0.45 * a2 / (a2 + 0.09);

  return albedo * max(0.0, NdotL) * (A + B * s / t) / PI;
}

// Helper function to sample from bindless textures using a sampler index.
float4 SampleBindlessTexture(uint textureIndex, uint samplerIndex, float2 uv)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    
    // Sample once using branch-based selection to avoid sampler-typed phi nodes.
    if (samplerIndex == SAMPLER_CLAMP_INDEX)
        return tex.Sample(g_SamplerAnisoClamp, uv);
    else
        return tex.Sample(g_SamplerAnisoWrap, uv);
}

float3 HashColor(uint id)
{
    uint h = id * 0x27D4EB2Du;
    h = h ^ (h >> 15);
    h = h * 0x85EBCA6Bu;
    h = h ^ (h >> 13);
    h = h * 0xC2B2AE35u;
    h = h ^ (h >> 16);
    return float3((h & 0xFF) / 255.0f, ((h >> 8) & 0xFF) / 255.0f, ((h >> 16) & 0xFF) / 255.0f);
}

float4 PSMain(VSOut input) : SV_TARGET
{
    // Instance + material
    PerInstanceData inst = g_Instances[input.instanceID];
    MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

    // Texture sampling (only when present)
    bool hasAlbedo = (mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0;
    float4 albedoSample = hasAlbedo
        ? SampleBindlessTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, input.uv)
        : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);

    // Alpha test (discard) as early as possible
    float alpha = hasAlbedo ? (albedoSample.w * mat.m_BaseColor.w) : mat.m_BaseColor.w;
    
#if ALPHA_TEST
    if (mat.m_AlphaMode == ALPHA_MODE_MASK && alpha < mat.m_AlphaCutoff)
    {
        discard;
    }
#endif

    bool hasORM = (mat.m_TextureFlags & TEXFLAG_ROUGHNESS_METALLIC) != 0;
    float4 ormSample = hasORM
        ? SampleBindlessTexture(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex, input.uv)
        : float4(mat.m_RoughnessMetallic.x, mat.m_RoughnessMetallic.y, 0.0f, 0.0f);

    bool hasNormal = (mat.m_TextureFlags & TEXFLAG_NORMAL) != 0;
    float4 nmSample = hasNormal
        ? SampleBindlessTexture(mat.m_NormalTextureIndex, mat.m_NormalSamplerIndex, input.uv)
        : float4(0.5f, 0.5f, 1.0f, 0.0f);

    bool hasEmissive = (mat.m_TextureFlags & TEXFLAG_EMISSIVE) != 0;
    float4 emissiveSample = hasEmissive
        ? SampleBindlessTexture(mat.m_EmissiveTextureIndex, mat.m_EmissiveSamplerIndex, input.uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);

    // Normal (from normal map when available)
    float3 N;
    if (hasNormal)
    {
        float3 normalMap = TwoChannelNormalX2(nmSample.xy);
        float3x3 TBN = CalculateTBNWithoutTangent(input.worldPos, input.normal, input.uv);
        N = normalize(mul(normalMap, TBN));
    }
    else
    {
        N = normalize(input.normal);
    }

    // View / light directions
    float3 V = normalize(g_PerFrame.m_CameraPos.xyz - input.worldPos);
    float3 L = g_PerFrame.m_LightDirection;
    float3 H = normalize(V + L);

    // Dot products
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float LdotV = saturate(dot(L, V));

    // Base color
    float3 baseColor = hasAlbedo ? (albedoSample.xyz * mat.m_BaseColor.xyz) : mat.m_BaseColor.xyz;

    // Material properties (roughness, metallic)
    float roughness = mat.m_RoughnessMetallic.x;
    float metallic = mat.m_RoughnessMetallic.y;
    if (hasORM)
    {
        // ORM texture layout: R = occlusion, G = roughness, B = metallic
        roughness = ormSample.y;
        metallic = ormSample.z;
    }

    // Derived values
    float a = roughness * roughness;
    float a2 = clamp(a * a, 0.0001f, 1.0f);

    // Diffuse (Oren-Nayar)
    float oren = OrenNayar(NdotL, NdotV, LdotV, a2, 1.0f);
    float3 diffuse = oren * (1.0f - metallic) * baseColor;

    // Specular
    const float materialSpecular = 0.5f;
    float3 specularColor = ComputeF0(materialSpecular, baseColor, metallic);

    float D = D_GGX(a2, NdotH);
    float Vis = Vis_SmithJointApprox(a2, NdotV, NdotL);
    float3 F = F_Schlick(specularColor, VdotH);
    float3 spec = (D * Vis) * F;

    // Lighting and ambient
    float3 radiance = float3(g_PerFrame.m_LightIntensity, g_PerFrame.m_LightIntensity, g_PerFrame.m_LightIntensity);
    
    // Raytraced shadows
    float shadow = 1.0f;
    if (g_PerFrame.m_EnableRTShadows != 0)
    {
        RayDesc ray;
        ray.Origin = input.worldPos + N * 0.001f;
        ray.Direction = L;
        ray.TMin = 0.0f;
        ray.TMax = 10000.0f;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            shadow = 0.0f;
        }
    }

    float3 ambient = (1.0f - shadow) * baseColor * 0.03f; // hack
    float3 color = ambient + (diffuse + spec) * radiance * NdotL * shadow;

    // Emissive
    float3 emissive = mat.m_EmissiveFactor.xyz;
    if (hasEmissive)
    {
        emissive *= emissiveSample.xyz;
    }
    color += emissive;

    // Debug visualizations
    if (g_PerFrame.m_DebugMode != DEBUG_MODE_NONE)
    {
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_INSTANCES)
            return float4(HashColor(input.instanceID), 1.0f);
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_MESHLETS)
            return float4(HashColor(input.meshletID), 1.0f);
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_WORLD_NORMALS)
            return float4(N, 1.0f);
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_ALBEDO)
            return float4(baseColor, 1.0f);
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_ROUGHNESS)
            return float4(roughness.xxx, 1.0f);
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_METALLIC)
            return float4(metallic.xxx, 1.0f);
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_EMISSIVE)
            return float4(emissive, 1.0f);
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_LOD)
        {
            float3 lodColors[] = {
                float3(0.0, 1.0, 0.0), // 0: Green
                float3(1.0, 0.0, 0.0), // 1: Red
                float3(0.0, 1.0, 1.0), // 2: Cyan
                float3(1.0, 0.0, 1.0), // 3: Magenta
                float3(1.0, 1.0, 0.0), // 4: Yellow
                float3(0.0, 0.0, 1.0), // 5: Blue
                float3(0.5, 0.0, 0.0), // 6: Dark Red
                float3(0.0, 0.5, 0.0)  // 7: Dark Green
            };
            return float4(lodColors[min(input.lodIndex, 7)], 1.0f);
        }
    }

    return float4(color, alpha);
}
