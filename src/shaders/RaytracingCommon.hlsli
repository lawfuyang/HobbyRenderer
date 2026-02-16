#ifndef RAYTRACING_COMMON_HLSLI
#define RAYTRACING_COMMON_HLSLI

#include "ShaderShared.h"
#include "Bindless.hlsli"

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
    float2 m_Uv;
};

struct TriangleVertices
{
    Vertex v0, v1, v2;
};

TriangleVertices GetTriangleVertices(
    uint primitiveIndex,
    MeshData mesh,
    StructuredBuffer<uint> indices,
    StructuredBuffer<VertexQuantized> vertices)
{
    uint baseIndex = mesh.m_IndexOffsets[0];
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
    PerInstanceData inst,
    MeshData mesh,
    StructuredBuffer<uint> indices,
    StructuredBuffer<VertexQuantized> vertices)
{
    TriangleVertices tv = GetTriangleVertices(hit.m_PrimitiveIndex, mesh, indices, vertices);

    float3 bary = float3(1.0f - hit.m_Barycentrics.x - hit.m_Barycentrics.y, hit.m_Barycentrics.x, hit.m_Barycentrics.y);

    FullHitAttributes attr;
    attr.m_WorldPos = ray.Origin + ray.Direction * hit.m_RayT;
    
    float3 localNormal = tv.v0.m_Normal * bary.x + tv.v1.m_Normal * bary.y + tv.v2.m_Normal * bary.z;
    attr.m_WorldNormal = TransformNormal(localNormal, inst.m_World);
    
    attr.m_Uv = tv.v0.m_Uv * bary.x + tv.v1.m_Uv * bary.y + tv.v2.m_Uv * bary.z;

    return attr;
}

float2 GetInterpolatedUV(
    uint primitiveIndex,
    float2 barycentrics,
    MeshData mesh,
    StructuredBuffer<uint> indices,
    StructuredBuffer<VertexQuantized> vertices)
{
    TriangleVertices tv = GetTriangleVertices(primitiveIndex, mesh, indices, vertices);
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
    MaterialConstants mat,
    float lod = 0.0f)
{
    if (mat.m_AlphaMode == ALPHA_MODE_OPAQUE)
        return true;
    
    if (mat.m_AlphaMode == ALPHA_MODE_MASK)
    {
        float alpha = mat.m_BaseColor.w;
        if ((mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0)
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
    MaterialConstants mat)
{
    if (mat.m_AlphaMode == ALPHA_MODE_OPAQUE)
        return true;
    
    if (mat.m_AlphaMode == ALPHA_MODE_MASK)
    {
        float alpha = mat.m_BaseColor.w;
        if ((mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0)
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
    float roughness;
    float metallic;
    float3 emissive;
};

PBRAttributes GetPBRAttributes(
    float2 uv,
    MaterialConstants mat,
    float lod = 0.0f)
{
    PBRAttributes attr;
    attr.baseColor = mat.m_BaseColor.xyz;
    if ((mat.m_TextureFlags & TEXFLAG_ALBEDO) != 0)
    {
        attr.baseColor *= SampleBindlessTextureLevel(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, uv, lod).xyz;
    }

    attr.roughness = mat.m_RoughnessMetallic.x;
    if ((mat.m_TextureFlags & TEXFLAG_ROUGHNESS_METALLIC) != 0)
    {
        attr.roughness *= SampleBindlessTextureLevel(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex, uv, lod).g;
    }
    
    attr.metallic = mat.m_RoughnessMetallic.y;
    if ((mat.m_TextureFlags & TEXFLAG_ROUGHNESS_METALLIC) != 0)
    {
        attr.metallic *= SampleBindlessTextureLevel(mat.m_RoughnessMetallicTextureIndex, mat.m_RoughnessSamplerIndex, uv, lod).b;
    }

    attr.emissive = mat.m_EmissiveFactor.xyz;
    if ((mat.m_TextureFlags & TEXFLAG_EMISSIVE) != 0)
    {
        attr.emissive += SampleBindlessTextureLevel(mat.m_EmissiveTextureIndex, mat.m_EmissiveSamplerIndex, uv, lod).xyz;
    }

    return attr;
}

#endif // RAYTRACING_COMMON_HLSLI
