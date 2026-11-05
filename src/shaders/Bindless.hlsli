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

// Coarsest mip level whose tiles are guaranteed mapped at this uv, read from the R8_UINT MinMip residency
// texture. frac() handles tiling/repeating UVs (uv > 1.0) the way the WRAP sampler does.
uint LoadMinResidentMip(uint minMipIndex, float2 uv, uint2 minMipDims)
{
    Texture2D<uint> minMipTex = ResourceDescriptorHeap[NonUniformResourceIndex(minMipIndex)];
    float2 wrappedUV = frac(uv);
    uint2 mcoord = uint2(wrappedUV * float2(minMipDims));
    mcoord = min(mcoord, minMipDims - 1);  // clamp for uv == 1.0
    return minMipTex.Load(uint3(mcoord, 0));
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
//   minMipDims     — (width, height) of the MinMip texture in texels, from MaterialConstants
//
// Requires [earlydepthstencil] on the pixel shader entry point.
float4 SampleBindlessStreamedTexture(uint textureIndex, uint samplerIndex, uint minMipIndex, uint feedbackIndex, float2 uv, int forcedMip, uint2 minMipDims)
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

    // Streaming path: read minMip FIRST and use it as MinLODClamp on the initial
    // sample.  This prevents the hardware from ever fetching an unmapped tile.
    uint minResidentMip = LoadMinResidentMip(minMipIndex, uv, minMipDims);

    // Sample with minResidentMip as the MinLODClamp — the hardware will never
    // pick a finer mip than what is resident, so the result is always mapped.
    uint status;
    float4 color = tex.Sample(sam, uv, 0, minResidentMip, status);

    // Last-resort fallback: if the sample is STILL unmapped (minMip texture is
    // transiently stale — e.g. the very first frame before any minMip upload),
    // walk coarser mips until we find a mapped one.  Packed mips are always
    // resident so this loop always terminates.
    if (!CheckAccessFullyMapped(status))
    {
        for (uint mip = minResidentMip + 1; mip < srrhi::CommonConsts::MAX_MIP_COUNT; ++mip)
        {
            color = tex.Sample(sam, uv, 0, mip, status);
            if (CheckAccessFullyMapped(status))
                break;
        }
    }

    WriteStreamingFeedback(tex, sam, uv, feedbackIndex, forcedMip);
    return color;
}

// Ray-tracing/compute variants of SampleBindlessStreamedTexture: the same residency clamp, at an explicit LOD
// and without writing sampler feedback (a ray has no pixel to charge it to).
//
// Alpha tests must go through these, not through a plain SampleLevel/SampleGrad. A tile that is not mapped yet
// reads back as zero, so an alpha-tested surface would report "transparent" to every shadow and GI ray until
// streaming catches up with the camera — and only its shadows would be wrong, because the base pass clamps and
// keeps drawing the surface itself.
//
// These deliberately sample WITHOUT a status operand. A status sample makes the hardware resolve the whole
// footprint and report partial mapping, which is a slow path, and inside a ray-traversal loop (one alpha test per
// alpha-tested triangle the ray crosses) that cost is multiplied by the number of candidates — a ray grazing a
// dense canopy stalled the GPU for hundreds of milliseconds. Clamping the level to the MinMip texture is enough
// on its own: that level's tiles are the ones streaming guarantees to be mapped.
float4 SampleBindlessStreamedTextureLevel(
    uint textureIndex, uint samplerIndex, uint minMipIndex, float2 uv, float lod, uint2 minMipDims)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];

    if (minMipIndex == srrhi::CommonConsts::DEFAULT_TEXTURE_BLACK)
        return tex.SampleLevel(sam, uv, lod);

    // Asking for a finer mip than the resident one is what produces the zeros, so clamp the level up first.
    const uint minResidentMip = LoadMinResidentMip(minMipIndex, uv, minMipDims);
    const float level = max(max(lod, 0.0f), (float)minResidentMip);
    return tex.SampleLevel(sam, uv, level);
}

float4 SampleBindlessStreamedTextureGrad(
    uint textureIndex, uint samplerIndex, uint minMipIndex, float2 uv, float2 ddx_uv, float2 ddy_uv, uint2 minMipDims)
{
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState sam = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];

    if (minMipIndex == srrhi::CommonConsts::DEFAULT_TEXTURE_BLACK)
        return tex.SampleGrad(sam, uv, ddx_uv, ddy_uv);

    // The clamp argument is the sampler's MinLOD, so the gradients still pick the anisotropic footprint but can
    // never resolve to a mip whose tiles are not mapped.
    const uint minResidentMip = LoadMinResidentMip(minMipIndex, uv, minMipDims);
    return tex.SampleGrad(sam, uv, ddx_uv, ddy_uv, 0, (float)minResidentMip);
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
