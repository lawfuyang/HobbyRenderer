#ifndef COMMON_HLSLI
#define COMMON_HLSLI

struct FullScreenVertexOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

uint32_t DivideAndRoundUp(uint32_t dividend, uint32_t divisor)
{
    return (dividend + divisor - 1) / divisor;
}

// Standard MatrixMultiply helper to enforce consistent multiplication order: mul(vector, matrix)
float4 MatrixMultiply(float4 v, float4x4 m)
{
    return mul(v, m);
}

float3 MatrixMultiply(float3 v, float3x3 m)
{
    return mul(v, m);
}

float GetMaxScale(float4x4 m)
{
    return max(length(m[0].xyz), max(length(m[1].xyz), length(m[2].xyz)));
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
    return normalize(MatrixMultiply(normal, adjugateWorldMatrix));
}

// Convert a UV to clip space coordinates (XY: [-1, 1])
float2 UVToClipXY(float2 uv)
{
    return uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
}

// Convert a clip position after projection and perspective divide to a UV
float2 ClipXYToUV(float2 xy)
{
    return xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
}

float3 DecodeOct(float2 e)
{
    float3 v = float3(e, 1.0f - abs(e.x) - abs(e.y));
    float t = max(-v.z, 0.0f);
    v.x += v.x >= 0.0f ? -t : t;
    v.y += v.y >= 0.0f ? -t : t;
    return normalize(v);
}

// Unpacks a 2 channel normal to xyz
float3 TwoChannelNormalX2(float2 normal)
{
    float2 xy = 2.0f * normal - 1.0f;
    float z = sqrt(saturate(1.0f - dot(xy, xy)));
    return float3(xy.x, xy.y, z);
}

float3 TransformNormalWithTBN(float2 nmSample, float3 normal, float3 tangent, float tangentSign)
{
    float3 normalMap = TwoChannelNormalX2(nmSample);
    float3 n_w = normalize(normal);
    float3 t_w = normalize(tangent);
    t_w = normalize(t_w - n_w * dot(t_w, n_w));
    float3 b_w = normalize(cross(n_w, t_w) * tangentSign);
    float3x3 TBN = float3x3(t_w, b_w, n_w);
    return normalize(MatrixMultiply(normalMap, TBN));
}

static const float3 kEarthCenter = float3(0.0f, -6360000.0f, 0.0f);

#endif // COMMON_HLSLI
