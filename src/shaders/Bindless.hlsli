#ifndef BINDLESS_HLSLI
#define BINDLESS_HLSLI

#include "ShaderShared.h"

// Scene buffers and samplers in space1
// These matches the layout in BasePass.hlsl and are now used in DeferredLighting.hlsl for RT shadows.

// Helper function to sample from bindless textures using a sampler index.
// Used in Pixel Shaders where implicit derivatives are available for mipmapping/anisotropy.
float4 SampleBindlessTexture(uint textureIndex, uint samplerIndex, float2 uv)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];
    return tex.Sample(sam, uv);
}

// Helper function to sample from bindless textures at a specific LOD level.
// Required in Compute Shaders or Ray Tracing where implicit derivatives are unavailable.
float4 SampleBindlessTextureLevel(uint textureIndex, uint samplerIndex, float2 uv, float lod)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];
    return tex.SampleLevel(sam, uv, lod);
}

// Helper function to sample from bindless textures with explicit gradients.
// Useful for ray tracing where implicit derivatives are unavailable.
float4 SampleBindlessTextureGrad(uint textureIndex, uint samplerIndex, float2 uv, float2 ddx_uv, float2 ddy_uv)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];
    return tex.SampleGrad(sam, uv, ddx_uv, ddy_uv);
}

#endif // BINDLESS_HLSLI
