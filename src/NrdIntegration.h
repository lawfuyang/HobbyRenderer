#pragma once

#include "pch.h"
#include "NRD.h"

// ============================================================================
// NRD Error Handling
// ============================================================================

#define NRD_CHECK(call)                                                                              \
    do {                                                                                             \
        if (nrd::Result _r = (call); _r != nrd::Result::SUCCESS)                                    \
        {                                                                                            \
            SDL_Log("[NRD] Call failed (%s:%d): %s => result=%u",                                   \
                    __FILE__, __LINE__, #call, static_cast<uint32_t>(_r));                           \
            SDL_assert(false && "NRD call failed");                                                  \
        }                                                                                            \
    } while(0)

// ============================================================================
// Helper
// ============================================================================

// Fill nrd::CommonSettings from the current frame's scene view matrices,
// motion vector scale, resolution, and frame index.
void FillNRDCommonSettings(nrd::CommonSettings& settings);

// ============================================================================
// NrdIntegration
// ============================================================================

// Adapted from NVIDIA's Donut framework NrdIntegration (NrdIntegration.h/.cpp).
//
// Manages a single NRD denoiser instance: directly creates and owns compute
// pipelines from NRD's embedded DXIL/SPIRV bytecode, samplers, and all
// permanent/transient pool textures.  Dispatches NRD passes directly through
// the NVRHI command list (no RenderGraph involvement for pool textures).
//
// Also owns an internal R10G10B10A2_UNORM packed normal+roughness texture and
// runs a PackNormalRoughness pre-pass each frame to convert the GBuffer's
// oct-encoded RG16_FLOAT normals into the format expected by NRD.
//
// Typical usage:
//
//   // Initialization:
//   m_NrdIntegration = std::make_unique<NrdIntegration>(
//       device, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
//   m_NrdIntegration->Initialize(width, height);
//
//   // Per-frame render:
//   nrd::CommonSettings commonSettings{};
//   FillNRDCommonSettings(commonSettings);
//   m_NrdIntegration->RunDenoiserPasses(
//       commandList,
//       rawGBufferNormals, rawGBufferORM,
//       rawDiffuse, rawSpecular, linearViewZ, motionVectors,
//       outDenoisedDiffuse, outDenoisedSpecular,
//       commonSettings, &relaxSettings);

class NrdIntegration
{
public:
    NrdIntegration(nrd::Denoiser denoiser)
        : m_Denoiser(denoiser)
    {
    }

    ~NrdIntegration();

    // Create NRD instance, allocate pool textures, build compute pipelines from
    // NRD's embedded bytecode.  Must be called once before RunDenoiserPasses.
    bool Initialize(uint32_t width, uint32_t height);

    bool IsAvailable() const { return m_Initialized; }

    // Run the full NRD denoiser dispatch chain for one frame.
    //
    // gbufferNormals  — oct-encoded normals (RG16_FLOAT) read from GBuffer.
    // gbufferORM      — ORM texture (RG8_UNORM); roughness is in the .r channel.
    // diffuseRadiance — packed RELAX diffuse input  (IN_DIFF_RADIANCE_HITDIST).
    // specularRadiance— packed RELAX specular input (IN_SPEC_RADIANCE_HITDIST).
    // viewZ           — linear view-space depth (IN_VIEWZ).
    // motionVectors   — screen-space motion (IN_MV).
    // outDiffuse      — denoised diffuse output  (OUT_DIFF_RADIANCE_HITDIST).
    // outSpecular     — denoised specular output (OUT_SPEC_RADIANCE_HITDIST).
    //
    // gbufferNormals + gbufferORM are packed internally into R10G10B10A2_UNORM
    // before being forwarded to NRD as IN_NORMAL_ROUGHNESS.
    void RunDenoiserPasses(
        nvrhi::ICommandList*    commandList,
        nvrhi::ITexture*        gbufferNormals,
        nvrhi::ITexture*        gbufferORM,
        nvrhi::ITexture*        diffuseRadiance,
        nvrhi::ITexture*        specularRadiance,
        nvrhi::ITexture*        viewZ,
        nvrhi::ITexture*        motionVectors,
        nvrhi::ITexture*        outDiffuse,
        nvrhi::ITexture*        outSpecular,
        const nrd::CommonSettings& commonSettings,
        const void*             denoiserSettings);

private:
    // -------------------------------------------------------------------------
    // Per-pipeline data (shader + binding layout + pipeline object).
    // Indexed by instanceDesc->pipelines[].
    // -------------------------------------------------------------------------
    struct NrdPipeline
    {
        nvrhi::ShaderHandle          Shader;
        nvrhi::BindingLayoutHandle   BindingLayout1; // per-pipeline resource layout
        nvrhi::ComputePipelineHandle Pipeline;
    };

    const nrd::Denoiser              m_Denoiser;
    bool                             m_Initialized = false;
    nrd::Instance*                   m_Instance    = nullptr;

    // Shared binding layout: constant buffer + samplers (space = cbAndSamplers space).
    nvrhi::BindingLayoutHandle       m_BindingLayout0;

    // Persistent volatile constant buffer shared across all NRD dispatches in a frame.
    nvrhi::BufferHandle              m_ConstantBuffer;

    std::vector<NrdPipeline>         m_Pipelines;
    nvrhi::SamplerHandle             m_Samplers[(int)nrd::Sampler::MAX_NUM];

    // NRD internal pool textures (owned directly, not via RenderGraph).
    std::vector<nvrhi::TextureHandle> m_PermanentTextures;
    std::vector<nvrhi::TextureHandle> m_TransientTextures;

    // Packed normal+roughness (R10G10B10A2_UNORM) — written each frame by
    // the PackNormalRoughness pre-pass and read by NRD as IN_NORMAL_ROUGHNESS.
    nvrhi::TextureHandle             m_PackedNormalRoughnessTex;

    // Resolve an NRD ResourceDesc to the physical NVRHI texture it should
    // be bound to for a given dispatch.
    nvrhi::ITexture* ResolveResource(
        nrd::ResourceType type, uint16_t indexInPool,
        nvrhi::ITexture* packedNormals,
        nvrhi::ITexture* diffuse,  nvrhi::ITexture* specular,
        nvrhi::ITexture* viewZ,    nvrhi::ITexture* motionVectors,
        nvrhi::ITexture* outDiffuse, nvrhi::ITexture* outSpecular) const;
};
