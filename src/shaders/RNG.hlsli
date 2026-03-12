#ifndef RNG_HLSLI
#define RNG_HLSLI

// ─── PCG Random Number Generator ─────────────────────────────────────────────
// From: https://www.pcg-random.org/
// Provides a fast stateful RNG suitable for use inside path-tracer wavefronts.
// Only compiled when PATH_TRACER_MODE is active (compute shaders, not rasterized passes).

struct RNG
{
    uint state;
};

uint PCGHash(uint seed)
{
    uint state = seed * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

RNG InitRNG(uint2 pixel, uint accumIndex)
{
    RNG rng;
    uint seed    = pixel.x + pixel.y * 65536u + accumIndex * 6700417u;
    rng.state    = PCGHash(seed);
    return rng;
}

float NextFloat(inout RNG rng)
{
    rng.state = PCGHash(rng.state);
    return float(rng.state) * (1.0f / 4294967296.0f);
}

float2 NextFloat2(inout RNG rng)
{
    return float2(NextFloat(rng), NextFloat(rng));
}

// Murmur3 finalizer — maps a uint to a pseudo-random uint.
uint murmur3(uint r)
{
#define ROT32(x, y) ((x << y) | (x >> (32 - y)))

    // https://en.wikipedia.org/wiki/MurmurHash
    uint c1 = 0xcc9e2d51;
    uint c2 = 0x1b873593;
    uint r1 = 15;
    uint r2 = 13;
    uint m  = 5;
    uint n  = 0xe6546b64;

    uint hash = r;
    uint k    = 39041; // random large prime number
    k *= c1;
    k  = ROT32(k, r1);
    k *= c2;

    hash ^= k;
    hash  = ROT32(hash, r2) * m + n;

    hash ^= 4;
    hash ^= (hash >> 16);
    hash *= 0x85ebca6b;
    hash ^= (hash >> 13);
    hash *= 0xc2b2ae35;
    hash ^= (hash >> 16);

#undef ROT32

    return hash;
}

// Convert a uint to a float in [0, 1) using the mantissa bits.
float randU2F(uint u)
{
    const uint one  = asuint(1.f);
    const uint mask = (1 << 23) - 1;
    return asfloat((mask & u) | one) - 1.f;
}

// Map a light index to a stable pseudo-random RGBA color.
// Returns float4 with alpha = 1.
float4 IndexToColor(uint index)
{
    float r = randU2F(murmur3(index));
    float g = randU2F(murmur3(index + 239));
    float b = randU2F(murmur3(index + 701));
    return float4(r, g, b, 1.0);
}

// Red-to-green gradient: t=0 → red, t=1 → green.
// Returns float4 with alpha = 1.
float4 rgLerp(float t)
{
    float3 red   = float3(1.0, 0.0, 0.0);
    float3 green = float3(0.0, 1.0, 0.0);
    return float4(lerp(red, green, t), 1.0);
}

#endif // RNG_HLSLI
