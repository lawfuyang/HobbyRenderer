#ifndef BINDLESS_HLSLI
#define BINDLESS_HLSLI

#include "srrhi/hlsl/Common.hlsli"

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

// Streaming-aware variant: if minMipIndex > 0 (streaming enabled for this texture),
// performs an opportunistic sample with MinMip fallback and writes sampler feedback.
// When minMipIndex == 0, degrades to a regular Sample() with no feedback write.
//
// Parameters:
//   textureIndex   — bindless index of the reserved (tiled) Texture2D
//   samplerIndex   — bindless index of the anisotropic sampler
//   minMipIndex    — bindless index of the R32_FLOAT MinMip residency texture (0 = not streaming)
//   feedbackIndex  — bindless index of the FeedbackTexture2D UAV (0 = not streaming)
//   uv             — texture coordinates
//
// Requires [earlydepthstencil] on the pixel shader entry point.
float4 SampleBindlessStreamedTexture(uint textureIndex, uint samplerIndex, uint minMipIndex, uint feedbackIndex, float2 uv)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];

    if (minMipIndex == 0)
    {
        // Not a streaming texture — regular sample, no feedback
        return tex.Sample(sam, uv);
    }

    // Streaming path: opportunistic sample, fall back via MinMip on miss
    uint status;
    float4 color = tex.Sample(sam, uv, 0, 0, status);
    if (!CheckAccessFullyMapped(status))
    {
        Texture2D<float> minMipTex = ResourceDescriptorHeap[NonUniformResourceIndex(minMipIndex)];
        SamplerState pointSam = SamplerDescriptorHeap[NonUniformResourceIndex(srrhi::CommonConsts::SAMPLER_POINT_CLAMP_INDEX)];
        float minResidentMip = minMipTex.SampleLevel(pointSam, uv, 0);
        color = tex.Sample(sam, uv, 0, minResidentMip, status);
    }

    // Write sampler feedback so the streaming system knows which tiles are needed.
    // feedbackIndex == 0 means no feedback texture was registered for this slot.
    if (feedbackIndex != 0)
    {
        FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> feedbackTex =
            ResourceDescriptorHeap[NonUniformResourceIndex(feedbackIndex)];
        feedbackTex.WriteSamplerFeedback(tex, sam, uv);
    }

    return color;
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

// 3D Texture helpers
float4 SampleBindlessTexture3DLevel(uint textureIndex, uint samplerIndex, float3 uvw, float lod)
{
    Texture3D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];
    return tex.SampleLevel(sam, uvw, lod);
}

#endif // BINDLESS_HLSLI
