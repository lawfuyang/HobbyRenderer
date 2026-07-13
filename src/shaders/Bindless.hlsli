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

// Helper: sample a texture at a forced mip level, clamped to the texture's mip count.
float4 SampleTextureClampedMip(Texture2D tex, SamplerState sam, float2 uv, int forcedMip)
{
    uint dummyW, dummyH, numMips;
    tex.GetDimensions(0, dummyW, dummyH, numMips);
    return tex.SampleLevel(sam, uv, (uint)clamp(forcedMip, 0, (int)(numMips - 1)));
}

// Helper: write sampler feedback for a streaming texture.
// UV is passed through unchanged — the sampler's address mode determines
// wrapping behaviour.  A WRAP sampler at uv=1.2 correctly maps to uv=0.2
// in the feedback map.  Clamping would silently request the wrong tile.
void WriteStreamingFeedback(Texture2D tex, SamplerState sam, float2 uv, uint feedbackIndex, int forcedMip)
{
    if (feedbackIndex != 0)
    {
        FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> feedbackTex = ResourceDescriptorHeap[NonUniformResourceIndex(feedbackIndex)];

        if (forcedMip >= 0)
        {
            feedbackTex.WriteSamplerFeedbackLevel(tex, sam, uv, forcedMip);
        }
        else
        {
            feedbackTex.WriteSamplerFeedback(tex, sam, uv);
        }
    }
}

// Streaming-aware variant: if minMipIndex > 0 (streaming enabled for this texture),
// performs an opportunistic sample with MinMip fallback and writes sampler feedback.
// When minMipIndex == 0, degrades to a regular Sample() with no feedback write.
//
// Parameters:
//   textureIndex   — bindless index of the reserved (tiled) Texture2D
//   samplerIndex   — bindless index of the anisotropic sampler
//   minMipIndex    — bindless index of the R8_UINT MinMip residency texture (0 = not streaming)
//   feedbackIndex  — bindless index of the FeedbackTexture2D UAV (0 = not streaming)
//   uv             — texture coordinates
//   forcedMip      — -1 = auto, >=0 = force this mip level (clamped to texture's mip count)
//
// Requires [earlydepthstencil] on the pixel shader entry point.
float4 SampleBindlessStreamedTexture(uint textureIndex, uint samplerIndex, uint minMipIndex, uint feedbackIndex, float2 uv, int forcedMip)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];

    // Forced mip path: use SampleLevel, clamped to the texture's mip chain, and still write feedback.
    if (forcedMip >= 0)
    {
        float4 color = SampleTextureClampedMip(tex, sam, uv, forcedMip);
        WriteStreamingFeedback(tex, sam, uv, feedbackIndex, forcedMip);
        return color;
    }

    if (minMipIndex == srrhi::CommonConsts::DEFAULT_TEXTURE_BLACK)
    {
        // Not a streaming texture — regular sample, no feedback
        return tex.Sample(sam, uv);
    }

    // Streaming path: opportunistic sample, fall back via MinMip on miss
    uint status;
    float4 color = tex.Sample(sam, uv, 0, 0, status);
    if (!CheckAccessFullyMapped(status))
    {
        Texture2D<uint> minMipTex = ResourceDescriptorHeap[NonUniformResourceIndex(minMipIndex)];
        SamplerState maxReductionSam = SamplerDescriptorHeap[NonUniformResourceIndex(srrhi::CommonConsts::SAMPLER_POINT_MAX_REDUCTION_CLAMP_INDEX)];
        // Sample() with a max-reduction sampler returns the maximum mip value
        // across the filter footprint — i.e. the coarsest resident mip in the region.
        // This works correctly on R8_UINT textures (unlike SampleLevel which ignores reduction mode).
        uint minResidentMip = minMipTex.Sample(maxReductionSam, uv);
        color = tex.Sample(sam, uv, 0, minResidentMip, status);

        // Last-resort fallback: if the MinMip-clamped sample is STILL unmapped
        // (MinMip texture is stale / not yet updated for this tile), walk coarser
        // mips one by one until we find a mapped one.  Packed mips are always
        // resident so this loop always terminates.
        if (false)
        {
            if (!CheckAccessFullyMapped(status))
            {
                for (uint mip = minResidentMip + 1; mip < srrhi::CommonConsts::MAX_MIP_COUNT; ++mip)
                {
                    color = tex.Sample(sam, uv, 0, mip, status);
                    if (CheckAccessFullyMapped(status))
                        break;
                }
            }
        }
    }

    WriteStreamingFeedback(tex, sam, uv, feedbackIndex, forcedMip);
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
