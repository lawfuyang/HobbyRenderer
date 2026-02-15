#include "ShaderShared.h"
#include "Culling.h"
#include "Bindless.hlsli"
#include "CommonLighting.hlsli"

cbuffer PerFrameCB : register(b0)
{
    ForwardLightingPerFrameData g_PerFrame;
};

StructuredBuffer<PerInstanceData> g_Instances : register(t0);
StructuredBuffer<MaterialConstants> g_Materials : register(t1);
StructuredBuffer<VertexQuantized> g_Vertices : register(t2);
StructuredBuffer<Meshlet> g_Meshlets : register(t3);
StructuredBuffer<uint> g_MeshletVertices : register(t4);
StructuredBuffer<uint> g_MeshletTriangles : register(t5);
StructuredBuffer<MeshletJob> g_MeshletJobs : register(t6);
StructuredBuffer<MeshData> g_MeshData : register(t7);
StructuredBuffer<uint> g_Indices : register(t10);
Texture2D<float> g_HZB : register(t8);
RaytracingAccelerationStructure g_SceneAS : register(t9);
Texture2D g_OpaqueColor : register(t11);

void UnpackMeshletBV(Meshlet m, out float3 center, out float radius)
{
    center.x = f16tof32(m.m_CenterRadius[0] & 0xFFFF);
    center.y = f16tof32(m.m_CenterRadius[0] >> 16);
    center.z = f16tof32(m.m_CenterRadius[1] & 0xFFFF);
    radius   = f16tof32(m.m_CenterRadius[1] >> 16);
}

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

float2 ComputeMotionVectors(float3 worldPos, float3 prevWorldPos)
{
    float4 clipPos = mul(float4(worldPos, 1.0), g_PerFrame.m_View.m_MatWorldToClip);
    float4 prevClipPos = mul(float4(prevWorldPos, 1.0), g_PerFrame.m_PrevView.m_MatWorldToClip);

    clipPos.xyz /= clipPos.w;
    prevClipPos.xyz /= prevClipPos.w;

    float2 windowPos = clipPos.xy * g_PerFrame.m_View.m_ClipToWindowScale + g_PerFrame.m_View.m_ClipToWindowBias;
    float2 prevWindowPos = prevClipPos.xy * g_PerFrame.m_PrevView.m_ClipToWindowScale + g_PerFrame.m_PrevView.m_ClipToWindowBias;

    return prevWindowPos.xy - windowPos.xy + (g_PerFrame.m_View.m_PixelOffset - g_PerFrame.m_PrevView.m_PixelOffset);
}

struct VSOut
{
    float4 Position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 prevWorldPos : TEXCOORD5;
    nointerpolation uint instanceID : TEXCOORD2;
    nointerpolation uint meshletID : TEXCOORD3;
    nointerpolation uint lodIndex : TEXCOORD4;
};

VSOut PrepareVSOut(Vertex v, PerInstanceData inst, uint instanceID, uint meshletID, uint lodIndex)
{
    VSOut o;
    float4 worldPos = mul(float4(v.m_Pos, 1.0f), inst.m_World);
    o.Position = mul(worldPos, g_PerFrame.m_View.m_MatWorldToClip);

    o.normal = TransformNormal(v.m_Normal, inst.m_World);
    o.uv = v.m_Uv;
    o.worldPos = worldPos.xyz;
    o.prevWorldPos = mul(float4(v.m_Pos, 1.0f), inst.m_PrevWorld).xyz;
    o.instanceID = instanceID;
    o.meshletID = meshletID;
    o.lodIndex = lodIndex;
    return o;
}

VSOut VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_StartInstanceLocation)
{
    PerInstanceData inst = g_Instances[instanceID];
    Vertex v = UnpackVertex(g_Vertices[vertexID]);
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

        float3 meshletCenter;
        float meshletRadius;
        UnpackMeshletBV(m, meshletCenter, meshletRadius);

        // Transform meshlet sphere to world space, then to view space
        float4 worldCenter = mul(float4(meshletCenter, 1.0f), inst.m_World);
        float3 viewCenter = mul(worldCenter, g_PerFrame.m_View.m_MatWorldToView).xyz;

        // Approximate world-space radius using max scale from world matrix
        float worldRadius = meshletRadius * GetMaxScale(inst.m_World);

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
            SamplerState minSam = SamplerDescriptorHeap[NonUniformResourceIndex(SAMPLER_MIN_REDUCTION_INDEX)];
            bVisible &= OcclusionSphereTest(viewCenter, worldRadius, uint2(g_PerFrame.m_HZBWidth, g_PerFrame.m_HZBHeight), g_PerFrame.m_P00, g_PerFrame.m_P11, g_HZB, minSam);
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
        Vertex v = UnpackVertex(g_Vertices[vertexIndex]);
        
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

struct GBufferOut
{
    float4 Albedo        : SV_TARGET0;
    float2 Normal        : SV_TARGET1;
    float4 ORM           : SV_TARGET2;
    float4 Emissive      : SV_TARGET3;
    float2 MotionVectors : SV_TARGET4;
};

float3 GetDebugColor(uint debugMode, uint instanceID, uint meshletID, uint lodIndex)
{
    if (debugMode == DEBUG_MODE_INSTANCES)
    {
        return HashColor(instanceID);
    }
    else if (debugMode == DEBUG_MODE_MESHLETS)
    {
        // Color by meshlet ID
        return HashColor(meshletID);
    }
    else if (debugMode == DEBUG_MODE_LOD)
    {
        float3 lodColors[MAX_LOD_COUNT] = {
            float3(0.0, 1.0, 0.0), // 0: Green
            float3(1.0, 0.0, 0.0), // 1: Red
            float3(0.0, 1.0, 1.0), // 2: Cyan
            float3(1.0, 0.0, 1.0), // 3: Magenta
            float3(1.0, 1.0, 0.0), // 4: Yellow
            float3(0.0, 0.0, 1.0), // 5: Blue
            float3(0.5, 0.0, 0.0), // 6: Dark Red
            float3(0.0, 0.5, 0.0)  // 7: Dark Green
        };
        
        return lodColors[lodIndex];
    }
    return float3(0.0f, 0.0f, 0.0f); // Should not reach here
}

// Direction of refracted light.
float3 GetVolumeTransmissionRay(float3 n, float3 v, float thickness, float ior, float3 modelScale)
{
    float3 refractionVector = refract(-v, normalize(n), 1.0 / ior);
    return normalize(refractionVector) * thickness * modelScale;
}

// Compute attenuated light as it travels through a volume.
float3 ApplyVolumeAttenuation(float3 radiance, float transmissionDistance, float3 attenuationColor, float attenuationDistance)
{
    if (attenuationDistance <= 0.0)
    {
        // Attenuation distance is +∞ (which we indicate by zero), i.e. the transmitted color is not attenuated at all.
        return radiance;
    }
    else
    {
        // Compute light attenuation using Beer's law.
        float3 transmittance = pow(max(attenuationColor, 0.0001), float3(transmissionDistance / attenuationDistance, transmissionDistance / attenuationDistance, transmissionDistance / attenuationDistance));
        return transmittance * radiance;
    }
}

#if defined(FORWARD_TRANSPARENT)
float4 Forward_PSMain(VSOut input) : SV_TARGET
#elif defined(ALPHA_TEST)
GBufferOut GBuffer_PSMain_AlphaTest(VSOut input)
#else
GBufferOut GBuffer_PSMain(VSOut input)
#endif
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
    
#if defined(ALPHA_TEST)
    if (alpha < mat.m_AlphaCutoff)
    {
        discard;
    }
#endif

    bool hasORM = (mat.m_TextureFlags & TEXFLAG_ROUGHNESS_METALLIC) != 0;
    float4 ormSample = hasORM
        ? SampleBindlessTexture(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex, input.uv)
        : float4(mat.m_RoughnessMetallic.x, mat.m_RoughnessMetallic.y, 1.0f, 0.0f); // R=occ, G=rough, B=metal

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

    // Base color
    float3 baseColor = hasAlbedo ? (albedoSample.xyz * mat.m_BaseColor.xyz) : mat.m_BaseColor.xyz;

    // Material properties (roughness, metallic)
    float roughness = mat.m_RoughnessMetallic.x;
    float metallic = mat.m_RoughnessMetallic.y;
    float occlusion = 1.0f;
    if (hasORM)
    {
        // ORM texture layout: R = occlusion, G = roughness, B = metallic
        occlusion = ormSample.x;
        roughness = ormSample.y;
        metallic = ormSample.z;
    }

    // Prevent perfectly smooth surfaces to avoid artifacts
    roughness = max(roughness, 0.05f);

    // Emissive
    float3 emissive = mat.m_EmissiveFactor.xyz;
    if (hasEmissive)
    {
        emissive *= emissiveSample.xyz;
    }

#if defined(FORWARD_TRANSPARENT)
    float3 V = normalize(g_PerFrame.m_CameraPos.xyz - input.worldPos);
    
    if (dot(N, V) < 0.0f)
    {
        N = -N; // Shading normals must always face the viewer
    }

    float3 L = g_PerFrame.m_LightDirection;

    LightingInputs lightingInputs;
    lightingInputs.N = N;
    lightingInputs.V = V;
    lightingInputs.L = L;
    lightingInputs.baseColor = baseColor;
    lightingInputs.roughness = roughness;
    lightingInputs.metallic = metallic;
    lightingInputs.ior = mat.m_IOR;
    lightingInputs.lightIntensity = g_PerFrame.m_LightIntensity;
    lightingInputs.worldPos = input.worldPos;
    lightingInputs.radianceMipCount = g_PerFrame.m_RadianceMipCount;
    lightingInputs.enableRTShadows = g_PerFrame.m_EnableRTShadows != 0;
    lightingInputs.sceneAS = g_SceneAS;
    lightingInputs.instances = g_Instances;
    lightingInputs.meshData = g_MeshData;
    lightingInputs.materials = g_Materials;
    lightingInputs.indices = g_Indices;
    lightingInputs.vertices = g_Vertices;

    PrepareLightingByproducts(lightingInputs);

    LightingComponents directLighting = ComputeDirectionalLighting(lightingInputs);

    IBLComponents iblComp = ComputeIBL(lightingInputs);
    float3 ibl = iblComp.ibl;
    ibl *= float(g_PerFrame.m_EnableIBL) * g_PerFrame.m_IBLIntensity;

    float3 color = directLighting.diffuse + directLighting.specular + ibl;

    // Refraction logic
    if (mat.m_TransmissionFactor > 0.0)
    {
        // Get model scale for world-space thickness
        float3 modelScale = float3(
            length(inst.m_World[0].xyz),
            length(inst.m_World[1].xyz),
            length(inst.m_World[2].xyz)
        );

        // --- Refraction ray tracing ---
        float3 transmissionRay = GetVolumeTransmissionRay(N, V, mat.m_ThicknessFactor, mat.m_IOR, modelScale);
        float3 refractedRayExit = input.worldPos + transmissionRay;

        // Project to screen space
        float4 refractClipPos = mul(float4(refractedRayExit, 1.0), g_PerFrame.m_View.m_MatWorldToClip);
        float2 refractUV = (refractClipPos.xy / refractClipPos.w) * float2(0.5, -0.5) + 0.5;

        // Sample background with roughness-based LOD
        float lod = log2(g_PerFrame.m_OpaqueColorDimensions.x) * roughness * clamp(mat.m_IOR * 2.0 - 2.0, 0.0, 1.0);
        SamplerState clampSam = SamplerDescriptorHeap[NonUniformResourceIndex(SAMPLER_ANISOTROPIC_CLAMP_INDEX)];
        float3 refractedColor = g_OpaqueColor.SampleLevel(clampSam, refractUV, lod).rgb;

        // --- Volume Attenuation (Beer-Lambert) ---
        refractedColor = ApplyVolumeAttenuation(refractedColor, length(transmissionRay), mat.m_AttenuationColor, mat.m_AttenuationDistance);

        // --- Base Color Filter (glTF Spec) ---
        refractedColor *= baseColor;

        // --- Fresnel split ---
        // We use the same view-dependent Fresnel for the background blend
        float3 transmitted = refractedColor * (1.0 - lightingInputs.F);
        float3 reflected = directLighting.specular + iblComp.radiance * float(g_PerFrame.m_EnableIBL) * g_PerFrame.m_IBLIntensity;

        float3 transmissionLighting = reflected + transmitted;

        // Blend based on transmission factor (diffuse is replaced)
        color = lerp(color, transmissionLighting, mat.m_TransmissionFactor);
    }

    color += emissive;

    float2 motionVectors = ComputeMotionVectors(input.worldPos, input.prevWorldPos);

    // Debug visualizations
    if (g_PerFrame.m_DebugMode != DEBUG_MODE_NONE)
    {
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_INSTANCES ||
            g_PerFrame.m_DebugMode == DEBUG_MODE_MESHLETS ||
            g_PerFrame.m_DebugMode == DEBUG_MODE_LOD)
        {
            color = GetDebugColor(g_PerFrame.m_DebugMode, input.instanceID, input.meshletID, input.lodIndex);
        }
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_WORLD_NORMALS)
            color = N * 0.5f + 0.5f;
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_ALBEDO)
            color = baseColor;
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_ROUGHNESS)
            color = roughness.xxx;
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_METALLIC)
            color = metallic.xxx;
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_EMISSIVE)
            color = emissive;
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_IRRADIANCE)
            color = iblComp.irradiance;
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_RADIANCE)
            color = iblComp.radiance;
        else if (g_PerFrame.m_DebugMode == DEBUG_MODE_IBL)
            color = ibl;
    }

    return float4(color, alpha);
#else
    GBufferOut output;
    output.Albedo = float4(baseColor, alpha);
    output.Normal = EncodeNormal(N);
    output.ORM = float4(occlusion, roughness, metallic, 0.0f);
    output.Emissive = float4(emissive, 1.0f);

    output.MotionVectors = ComputeMotionVectors(input.worldPos, input.prevWorldPos);
    
    // Debug visualizations
    if (g_PerFrame.m_DebugMode != DEBUG_MODE_NONE)
    {
        if (g_PerFrame.m_DebugMode == DEBUG_MODE_INSTANCES ||
            g_PerFrame.m_DebugMode == DEBUG_MODE_MESHLETS ||
            g_PerFrame.m_DebugMode == DEBUG_MODE_LOD)
        {
            float3 debugColor = GetDebugColor(g_PerFrame.m_DebugMode, input.instanceID, input.meshletID, input.lodIndex);
            output.Albedo = float4(debugColor, alpha);
            output.Normal = float2(0.5f, 0.5f); // Default normal
            output.ORM = float4(1.0f, 0.5f, 0.0f, 0.0f); // Default ORM
            output.Emissive = float4(0.0f, 0.0f, 0.0f, 1.0f); // No emissive
        }
    }
    
    return output;
#endif
}
