#ifndef GBUFFER_MATERIAL_HLSLI
#define GBUFFER_MATERIAL_HLSLI

#include "BasePassCommon.hlsli"
#include "Bindless.hlsli"
#include "CommonLighting.hlsli"
#include "StreamingMipLUT.hlsli"

// The G-buffer material evaluation, shared by the raster base pass and the visibility resolve.
//
// One definition, two geometry paths: the raster path calls it with the interpolant the mesh/vertex shader
// produced, and the resolve calls it with the interpolant it rebuilt from a triangle id. Everything the two could
// disagree about - the texture flags, the alpha test, the debug views, the motion vectors - lives here, so they
// cannot drift apart.
//
// It reads the base pass' binding set, which is why the resolve binds that set too (the visibility buffer itself is
// the resolve's own, in a separate set).

static const StructuredBuffer<srrhi::MaterialConstants> g_Materials = srrhi::BasePassInputs::GetMaterials();

struct GBufferOut
{
    float4 Albedo        : SV_TARGET0;
    float2 Normal        : SV_TARGET1;
    float2 ORM           : SV_TARGET2;
    float4 Emissive      : SV_TARGET3;
    float4 MotionVectors : SV_TARGET4;
    float2 GeoNormal     : SV_TARGET5;  // World-space geometric normal (no normal map), oct-encoded
};

float3 GetDebugColor(uint debugMode, uint instanceID, uint meshletID, uint lodIndex, uint clusterID)
{
    if (debugMode == srrhi::CommonConsts::DEBUG_MODE_INSTANCES)
    {
        return HashColor(instanceID);
    }
    else if (debugMode == srrhi::CommonConsts::DEBUG_MODE_MESHLETS)
    {
        // Color by meshlet ID
        return HashColor(meshletID);
    }
    else if (debugMode == srrhi::CommonConsts::DEBUG_MODE_CLUSTERS)
    {
        // Neighbouring clusters of one level are numbered consecutively, so the hash is what makes the
        // boundaries between them readable.
        return HashColor(clusterID);
    }
    else if (debugMode == srrhi::CommonConsts::DEBUG_MODE_CLUSTER_LOD)
    {
        // A 32-level ramp instead of the 8 fixed LOD colours: a cluster cut can reach level 31, and the
        // interesting thing about the view is how many levels are on screen at once.
        const float level = saturate(float(lodIndex) / 15.0f);
        return saturate(float3(level * 2.0f, 1.0f - abs(level * 2.0f - 1.0f), 2.0f - level * 2.0f));
    }
    else if (debugMode == srrhi::CommonConsts::DEBUG_MODE_LOD)
    {
        float3 lodColors[srrhi::CommonConsts::MAX_LOD_COUNT] = {
            float3(0.0, 1.0, 0.0), // 0: Green
            float3(1.0, 0.0, 0.0), // 1: Red
            float3(0.0, 1.0, 1.0), // 2: Cyan
            float3(1.0, 0.0, 1.0), // 3: Magenta
            float3(1.0, 1.0, 0.0), // 4: Yellow
            float3(0.0, 0.0, 1.0), // 5: Blue
            float3(0.5, 0.0, 0.0), // 6: Dark Red
            float3(0.0, 0.5, 0.0)  // 7: Dark Green
        };

        // A cluster's LOD level rides in lodIndex and can go past the eight meshlet LODs, so wrap instead of
        // reading past the table.
        return lodColors[lodIndex % srrhi::CommonConsts::MAX_LOD_COUNT];
    }
    return float3(0.0f, 0.0f, 0.0f); // Should not reach here
}

// Mip color LUT from SamplerFeedbackStreaming reference (GetLodVisualizationColor.h)
float3 GetStreamingMipDebugColor(uint minMipIndex, uint2 minMipDims, float2 uv)
{
    if (minMipIndex == 0)
    {
        return float3(0.3f, 0.3f, 0.3f); // Gray: not a streaming texture
    }

    Texture2D<uint> minMipTex = ResourceDescriptorHeap[NonUniformResourceIndex(minMipIndex)];
    // Apply frac() to handle tiling/repeating UVs (uv > 1.0), matching the WRAP
    // sampler behavior used when actually sampling the streaming texture.
    float2 wrappedUV = frac(uv);
    uint2 coord = uint2(wrappedUV * float2(minMipDims));
    coord = min(coord, minMipDims - 1);  // clamp for uv == 1.0
    uint minResidentMip = minMipTex.Load(uint3(coord, 0));
    int mipLevel = int(minResidentMip);

    return GetStreamingMipLUTColor(mipLevel);
}

bool MaterialHasAlbedo(srrhi::MaterialConstants mat)
{
    return (mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ALBEDO) != 0;
}

// The alpha the masked bucket tests. Shared so that the raster path and the id raster cannot disagree about which
// texels survive: a difference here is a fringe of missing or extra pixels along every silhouette.
float AlbedoAlpha(srrhi::MaterialConstants mat, float4 albedoSample, bool hasAlbedo)
{
    return hasAlbedo ? (albedoSample.w * mat.m_BaseColor.w) : mat.m_BaseColor.w;
}

GBufferOut EvaluateGBufferMaterial(VSOut input)
{
    // Instance + material
    srrhi::PerInstanceData inst = g_Instances[input.instanceID];
    srrhi::MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

    // Texture sampling (only when present)
    bool hasAlbedo = MaterialHasAlbedo(mat);
    float4 albedoSample = hasAlbedo
        ? SampleBindlessStreamedTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex,
                                        mat.m_AlbedoMinMipIndex, mat.m_AlbedoFeedbackIndex, input.uv,
                                        g_PerFrame.m_ForcedTextureMip, uint2(mat.m_MinMipDimsX, mat.m_MinMipDimsY))
        : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);

    // Alpha test (discard) as early as possible
    float alpha = AlbedoAlpha(mat, albedoSample, hasAlbedo);

#if defined(ALPHA_TEST)
    if (alpha < mat.m_AlphaCutoff)
    {
        discard;
    }
#endif

    bool hasORM = (mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ROUGHNESS_METALLIC) != 0;
    float4 ormSample = hasORM
        ? SampleBindlessStreamedTexture(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex,
                                        mat.m_RoughnessMinMipIndex, mat.m_RoughnessFeedbackIndex, input.uv,
                                        g_PerFrame.m_ForcedTextureMip, uint2(mat.m_MinMipDimsX, mat.m_MinMipDimsY))
        : float4(mat.m_RoughnessMetallic.x, mat.m_RoughnessMetallic.y, 1.0f, 0.0f); // R=occ, G=rough, B=metal

    bool hasNormal = (mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_NORMAL) != 0;
    float4 nmSample = hasNormal
        ? SampleBindlessStreamedTexture(mat.m_NormalTextureIndex, mat.m_NormalSamplerIndex,
                                        mat.m_NormalMinMipIndex, mat.m_NormalFeedbackIndex, input.uv,
                                        g_PerFrame.m_ForcedTextureMip, uint2(mat.m_MinMipDimsX, mat.m_MinMipDimsY))
        : float4(0.5f, 0.5f, 1.0f, 0.0f);

    bool hasEmissive = (mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_EMISSIVE) != 0;
    float4 emissiveSample = hasEmissive
        ? SampleBindlessStreamedTexture(mat.m_EmissiveTextureIndex, mat.m_EmissiveSamplerIndex,
                                        mat.m_EmissiveMinMipIndex, mat.m_EmissiveFeedbackIndex, input.uv,
                                        g_PerFrame.m_ForcedTextureMip, uint2(mat.m_MinMipDimsX, mat.m_MinMipDimsY))
        : float4(1.0f, 1.0f, 1.0f, 1.0f);

    // Normal (from normal map when available)
    float3 N;
    if (hasNormal)
    {
        N = TransformNormalWithTBN(nmSample.xy, input.normal, input.tangent.xyz, input.tangent.w);
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
    if (hasORM)
    {
        // ORM texture layout: G = roughness, B = metallic
        roughness = ormSample.g;
        metallic = ormSample.b;
    }

    // Prevent perfectly smooth surfaces to avoid artifacts
    roughness = max(roughness, 0.04f);

    // Emissive
    float3 emissive = mat.m_EmissiveFactor.xyz;
    if (hasEmissive)
    {
        emissive *= emissiveSample.xyz;
    }

    GBufferOut output;
    output.Albedo = float4(baseColor, alpha);
    output.Normal = EncodeNormal(N);
    output.ORM = float2(roughness, metallic);
    output.Emissive = float4(emissive, 1.0f);
    output.GeoNormal = EncodeNormal(normalize(input.normal));

    output.MotionVectors.xyz = ComputeMotionVectors(input.worldPos, input.prevWorldPos, g_PerFrame.m_View, g_PerFrame.m_PrevView);
    output.MotionVectors.w = 0; // Unused

    // Debug visualizations
    if (g_PerFrame.m_DebugMode != srrhi::CommonConsts::DEBUG_MODE_NONE)
    {
        if (g_PerFrame.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_INSTANCES ||
            g_PerFrame.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_MESHLETS ||
            g_PerFrame.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_CLUSTERS ||
            g_PerFrame.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_CLUSTER_LOD ||
            g_PerFrame.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_LOD)
        {
            float3 debugColor = GetDebugColor(g_PerFrame.m_DebugMode, input.instanceID, input.meshletID, input.lodIndex, input.clusterID);
            output.Albedo = float4(debugColor, alpha);
            output.Normal = float2(0.5f, 0.5f); // Default normal
            output.ORM = float2(0.5f, 0.0f); // Default ORM
            output.Emissive = float4(0.0f, 0.0f, 0.0f, 1.0f); // No emissive
        }
        else if (g_PerFrame.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_STREAMING_MIP)
        {
            output.Albedo = float4(0,0,0,0);
            output.Normal = float2(0.5f, 0.5f);
            output.ORM = float2(0.5f, 0.0f);
            output.Emissive = float4(GetStreamingMipDebugColor(mat.m_AlbedoMinMipIndex, uint2(mat.m_MinMipDimsX, mat.m_MinMipDimsY), input.uv), 1.0f);
        }
    }

    return output;
}

#endif // GBUFFER_MATERIAL_HLSLI
