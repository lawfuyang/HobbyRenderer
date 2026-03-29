#ifndef RAYTRACING_COMMON_HLSLI
#define RAYTRACING_COMMON_HLSLI

#include "Bindless.hlsli"
#include "MeshCommon.hlsli"

#include "srrhi/hlsl/Common.hlsli"
#include "srrhi/hlsl/Instance.hlsli"

struct RayHitInfo
{
    uint m_InstanceIndex;
    uint m_PrimitiveIndex;
    float2 m_Barycentrics;
    float m_RayT;
};

struct FullHitAttributes
{
    float3 m_WorldPos;
    float3 m_WorldNormal;
    float3 m_WorldTangent;
    float m_TangentSign;
    float2 m_Uv;
};

struct TriangleVertices
{
    srrhi::Vertex v0, v1, v2;
};

TriangleVertices GetTriangleVertices(
    uint primitiveIndex,
    uint lodIndex,
    srrhi::MeshData mesh,
    StructuredBuffer<uint> indices,
    StructuredBuffer<srrhi::VertexQuantized> vertices)
{
    uint baseIndex = mesh.m_IndexOffsets[lodIndex];
    uint i0 = indices[baseIndex + 3 * primitiveIndex + 0];
    uint i1 = indices[baseIndex + 3 * primitiveIndex + 1];
    uint i2 = indices[baseIndex + 3 * primitiveIndex + 2];

    TriangleVertices tv;
    tv.v0 = UnpackVertex(vertices[i0]);
    tv.v1 = UnpackVertex(vertices[i1]);
    tv.v2 = UnpackVertex(vertices[i2]);
    return tv;
}

FullHitAttributes GetFullHitAttributes(
    RayHitInfo hit,
    RayDesc ray,
    srrhi::PerInstanceData inst,
    srrhi::MeshData mesh,
    StructuredBuffer<uint> indices,
    StructuredBuffer<srrhi::VertexQuantized> vertices)
{
    TriangleVertices tv = GetTriangleVertices(hit.m_PrimitiveIndex, inst.m_LODIndex, mesh, indices, vertices);

    float3 bary = float3(1.0f - hit.m_Barycentrics.x - hit.m_Barycentrics.y, hit.m_Barycentrics.x, hit.m_Barycentrics.y);

    FullHitAttributes attr;
    attr.m_WorldPos = ray.Origin + ray.Direction * hit.m_RayT;
    
    float3 localNormal = tv.v0.m_Normal * bary.x + tv.v1.m_Normal * bary.y + tv.v2.m_Normal * bary.z;
    attr.m_WorldNormal = TransformNormal(localNormal, inst.m_World);
    
    float3 localTangent = tv.v0.m_Tangent.xyz * bary.x + tv.v1.m_Tangent.xyz * bary.y + tv.v2.m_Tangent.xyz * bary.z;
    attr.m_WorldTangent = TransformNormal(localTangent, inst.m_World);
    attr.m_TangentSign = tv.v0.m_Tangent.w * bary.x + tv.v1.m_Tangent.w * bary.y + tv.v2.m_Tangent.w * bary.z;
    
    attr.m_Uv = tv.v0.m_Uv * bary.x + tv.v1.m_Uv * bary.y + tv.v2.m_Uv * bary.z;

    return attr;
}

float2 GetInterpolatedUV(
    uint primitiveIndex,
    uint lodIndex,
    float2 barycentrics,
    srrhi::MeshData mesh,
    StructuredBuffer<uint> indices,
    StructuredBuffer<srrhi::VertexQuantized> vertices)
{
    TriangleVertices tv = GetTriangleVertices(primitiveIndex, lodIndex, mesh, indices, vertices);
    return tv.v0.m_Uv * (1.0f - barycentrics.x - barycentrics.y) + tv.v1.m_Uv * barycentrics.x + tv.v2.m_Uv * barycentrics.y;
}

struct RayGradients
{
    float2 uv;
    float2 ddx;
    float2 ddy;
};

RayGradients GetShadowRayGradients(
    TriangleVertices tv,
    float2 bary,
    float3 rayOrigin,
    float4x4 world)
{
    float3 p0 = MatrixMultiply(float4(tv.v0.m_Pos, 1.0f), world).xyz;
    float3 p1 = MatrixMultiply(float4(tv.v1.m_Pos, 1.0f), world).xyz;
    float3 p2 = MatrixMultiply(float4(tv.v2.m_Pos, 1.0f), world).xyz;

    float3 hitPos = p0 * (1.0f - bary.x - bary.y) + p1 * bary.x + p2 * bary.y;
    float dist = length(hitPos - rayOrigin);

    float3 edge1 = p1 - p0;
    float3 edge2 = p2 - p0;
    float triangleArea = length(cross(edge1, edge2)) * 0.5f;

    float2 uv0 = tv.v0.m_Uv;
    float2 uv1 = tv.v1.m_Uv;
    float2 uv2 = tv.v2.m_Uv;
    
    RayGradients grad;
    grad.uv = uv0 * (1.0f - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;

    float2 uvMin = min(uv0, min(uv1, uv2));
    float2 uvMax = max(uv0, max(uv1, uv2));
    float2 uvRange = uvMax - uvMin;

    float gradientScale = triangleArea / max(dist, 0.1f);
    
    grad.ddx = uvRange * gradientScale;
    grad.ddy = uvRange * gradientScale;
    return grad;
}

bool AlphaTest(
    float2 uv,
    srrhi::MaterialConstants mat,
    float lod = 0.0f)
{
    if (mat.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE)
        return true;
    
    if (mat.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_MASK)
    {
        float alpha = mat.m_BaseColor.w;
        if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ALBEDO) != 0)
        {
            alpha *= SampleBindlessTextureLevel(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uv, lod).w;
        }
        return alpha >= mat.m_AlphaCutoff;
    }

    return true;
}

bool AlphaTestGrad(
    float2 uv,
    float2 ddx_uv,
    float2 ddy_uv,
    srrhi::MaterialConstants mat)
{
    if (mat.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE)
        return true;
    
    if (mat.m_AlphaMode == srrhi::CommonConsts::ALPHA_MODE_MASK)
    {
        float alpha = mat.m_BaseColor.w;
        if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ALBEDO) != 0)
        {
            alpha *= SampleBindlessTextureGrad(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uv, ddx_uv, ddy_uv).w;
        }
        return alpha >= mat.m_AlphaCutoff;
    }

    return true;
}

struct PBRAttributes
{
    float3 baseColor;
    float alpha;
    float roughness;
    float metallic;
    float3 emissive;
    float3 normal;
};

PBRAttributes GetPBRAttributes(FullHitAttributes attr, srrhi::MaterialConstants mat, float lod = 0.0f)
{
    PBRAttributes pbrAttr;
    pbrAttr.baseColor = mat.m_BaseColor.xyz;
    pbrAttr.alpha = mat.m_BaseColor.w;
    if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ALBEDO) != 0)
    {
        float4 albedoSample = SampleBindlessTextureLevel(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, attr.m_Uv, lod);
        pbrAttr.baseColor *= albedoSample.xyz;
        pbrAttr.alpha *= albedoSample.w;
    }

    pbrAttr.roughness = mat.m_RoughnessMetallic.x;
    if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ROUGHNESS_METALLIC) != 0)
    {
        pbrAttr.roughness = SampleBindlessTextureLevel(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex, attr.m_Uv, lod).g;
    }

    pbrAttr.roughness = max(pbrAttr.roughness, 0.04f);
    
    pbrAttr.metallic = mat.m_RoughnessMetallic.y;
    if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ROUGHNESS_METALLIC) != 0)
    {
        pbrAttr.metallic = SampleBindlessTextureLevel(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex, attr.m_Uv, lod).b;
    }

    pbrAttr.emissive = mat.m_EmissiveFactor.xyz;
    if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_EMISSIVE) != 0)
    {
        pbrAttr.emissive *= SampleBindlessTextureLevel(mat.m_EmissiveTextureIndex, mat.m_EmissiveSamplerIndex, attr.m_Uv, lod).xyz;
    }

    // Normal (from normal map when available)
    if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_NORMAL) != 0)
    {
        float2 normalSample = SampleBindlessTextureLevel(mat.m_NormalTextureIndex, mat.m_NormalSamplerIndex, attr.m_Uv, lod).xy;
        pbrAttr.normal = TransformNormalWithTBN(normalSample, attr.m_WorldNormal, attr.m_WorldTangent, attr.m_TangentSign);
    }
    else
    {
        pbrAttr.normal = normalize(attr.m_WorldNormal);
    }

    return pbrAttr;
}

#endif // RAYTRACING_COMMON_HLSLI
