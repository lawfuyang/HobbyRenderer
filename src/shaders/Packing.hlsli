#ifndef PACKING_HLSLI
#define PACKING_HLSLI

// ---- R11G11B10 UFLOAT ----
float3 Unpack_R11G11B10_UFLOAT(uint packed)
{
    float r = f16tof32((packed & 0x7FFu) << 4);
    float g = f16tof32(((packed >> 11) & 0x7FFu) << 4);
    float b = f16tof32(((packed >> 22) & 0x3FFu) << 5);
    return float3(r, g, b);
}

uint Pack_R11G11B10_UFLOAT(float3 rgb)
{
    uint r = (f32tof16(rgb.r) >> 4) & 0x7FFu;
    uint g = (f32tof16(rgb.g) >> 4) & 0x7FFu;
    uint b = (f32tof16(rgb.b) >> 5) & 0x3FFu;
    return r | (g << 11) | (b << 22);
}

// ---- R8G8B8 UFLOAT (packed into low 24 bits of a uint) ----
float3 Unpack_R8G8B8_UFLOAT(uint packed)
{
    float3 v;
    v.r = float((packed)       & 0xFF) / 255.0f;
    v.g = float((packed >> 8)  & 0xFF) / 255.0f;
    v.b = float((packed >> 16) & 0xFF) / 255.0f;
    return v;
}

uint Pack_R8G8B8_UFLOAT(float3 v)
{
    uint r = uint(saturate(v.r) * 255.0f + 0.5f);
    uint g = uint(saturate(v.g) * 255.0f + 0.5f);
    uint b = uint(saturate(v.b) * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16);
}

// ---- R8G8B8A8 Gamma UFLOAT ----
float4 Unpack_R8G8B8A8_Gamma_UFLOAT(uint packed)
{
    float4 v;
    v.r = float((packed)       & 0xFF) / 255.0f;
    v.g = float((packed >> 8)  & 0xFF) / 255.0f;
    v.b = float((packed >> 16) & 0xFF) / 255.0f;
    v.a = float((packed >> 24) & 0xFF) / 255.0f;
    v.rgb = pow(max(v.rgb, 0.0f), 2.2f);
    return v;
}

uint Pack_R8G8B8A8_Gamma_UFLOAT(float4 v)
{
    float3 encoded = pow(max(v.rgb, 0.0f), 1.0f / 2.2f);
    uint r = uint(saturate(encoded.r) * 255.0f + 0.5f);
    uint g = uint(saturate(encoded.g) * 255.0f + 0.5f);
    uint b = uint(saturate(encoded.b) * 255.0f + 0.5f);
    uint a = uint(saturate(v.a)       * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16) | (a << 24);
}

// ---- R8G8B8A8 UNORM ----
float4 Unpack_R8G8B8A8_UNORM(uint packed)
{
    float4 v;
    v.r = float((packed)       & 0xFF) / 255.0f;
    v.g = float((packed >> 8)  & 0xFF) / 255.0f;
    v.b = float((packed >> 16) & 0xFF) / 255.0f;
    v.a = float((packed >> 24) & 0xFF) / 255.0f;
    return v;
}

uint Pack_R8G8B8A8_UNORM(float4 v)
{
    uint r = uint(saturate(v.r) * 255.0f + 0.5f);
    uint g = uint(saturate(v.g) * 255.0f + 0.5f);
    uint b = uint(saturate(v.b) * 255.0f + 0.5f);
    uint a = uint(saturate(v.a) * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16) | (a << 24);
}

// ---- Oct-encoded normals (32-bit unorm) ----
float3 octToNdirUnorm32(uint packedNormal)
{
    float2 e;
    e.x = float(packedNormal & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
    e.y = float(packedNormal >> 16)    / 65535.0f * 2.0f - 1.0f;
    float3 v = float3(e, 1.0f - abs(e.x) - abs(e.y));
    float t = max(-v.z, 0.0f);
    v.x += (v.x >= 0.0f) ? -t : t;
    v.y += (v.y >= 0.0f) ? -t : t;
    return normalize(v);
}

uint ndirToOctUnorm32(float3 n)
{
    float3 v = n / (abs(n.x) + abs(n.y) + abs(n.z));
    if (v.z < 0.0f)
    {
        float x = v.x;
        v.x = (1.0f - abs(v.y)) * (v.x >= 0.0f ? 1.0f : -1.0f);
        v.y = (1.0f - abs(x))   * (v.y >= 0.0f ? 1.0f : -1.0f);
    }
    uint2 packed = uint2(
        uint(saturate(v.x * 0.5f + 0.5f) * 65535.0f + 0.5f),
        uint(saturate(v.y * 0.5f + 0.5f) * 65535.0f + 0.5f));
    return packed.x | (packed.y << 16);
}

// ---- R16G16B16A16 FLOAT (packed as two uint32s: xy in .x, zw in .y) ----
float4 Unpack_R16G16B16A16_FLOAT(uint2 packed)
{
    return float4(
        f16tof32(packed.x & 0xFFFF),
        f16tof32(packed.x >> 16),
        f16tof32(packed.y & 0xFFFF),
        f16tof32(packed.y >> 16));
}

uint2 Pack_R16G16B16A16_FLOAT(float4 v)
{
    return uint2(
        f32tof16(v.x) | (f32tof16(v.y) << 16),
        f32tof16(v.z) | (f32tof16(v.w) << 16));
}

// ---- square helper ----
float  square(float  x) { return x * x; }
float2 square(float2 x) { return x * x; }
float3 square(float3 x) { return x * x; }

#endif // PACKING_HLSLI
