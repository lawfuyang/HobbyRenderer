#pragma once


#include "NRD.h"
#include "RenderGraph.h"

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
// Manages a single NRD denoiser instance: creates and owns compute pipelines
// from NRD's embedded DXIL/SPIRV bytecode and samplers. All permanent/transient
// pool textures are now managed via the RenderGraph.
//
// Also owns an internal R10G10B10A2_UNORM packed normal+roughness texture and
// runs a PackNormalRoughness pre-pass each frame to convert the GBuffer's
// oct-encoded RG16_FLOAT normals into the format expected by NRD.
//
// Typical usage:
//
//   // Initialization (called during RTXDIRenderer::Initialize):
//   m_NrdIntegration = std::make_unique<NrdIntegration>(
//       nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
//
//   // During RTXDIRenderer::Setup(RenderGraph&), declare pool textures:
//   m_NrdIntegration->Initialize(renderGraph, width, height);
//
//   // Per-frame render:
//   nrd::CommonSettings commonSettings{};
//   FillNRDCommonSettings(commonSettings);
//   m_NrdIntegration->RunDenoiserPasses(
//       commandList, renderGraph,
//       packedNormalRoughnessTex, rawGBufferORM,
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

    // Create NRD instance, allocate pool textures via RenderGraph, and build
    // compute pipelines from NRD's embedded bytecode.  Must be called during
    // the RenderGraph Setup phase.
    bool Initialize();

    void Setup(RenderGraph& renderGraph);

    // Run the full NRD denoiser dispatch chain for one frame.
    //
    // commandList     — NVRHI command list for GPU work submission.
    // renderGraph     — RenderGraph for retrieving pool textures.
    // packedNormalRoughnessTex  — packed normal+roughness texture (R10G10B10A2_UNORM) written by the PackNormalRoughness pre-pass.
    // gbufferORM      — ORM texture (RG8_UNORM); roughness is in the .r channel.
    // diffuseRadiance — packed RELAX diffuse input  (IN_DIFF_RADIANCE_HITDIST).
    // specularRadiance— packed RELAX specular input (IN_SPEC_RADIANCE_HITDIST).
    // viewZ           — linear view-space depth (IN_VIEWZ).
    // motionVectors   — screen-space motion (IN_MV).
    // outDiffuse      — denoised diffuse output  (OUT_DIFF_RADIANCE_HITDIST).
    // outSpecular     — denoised specular output (OUT_SPEC_RADIANCE_HITDIST).
    //
    // packedNormalRoughnessTex + gbufferORM are forwarded to NRD as IN_NORMAL_ROUGHNESS.
    void RunDenoiserPasses(
        nvrhi::ICommandList*       commandList,
        const RenderGraph&         renderGraph,
        nvrhi::ITexture*           packedNormalRoughnessTex,
        nvrhi::ITexture*           gbufferORM,
        nvrhi::ITexture*           diffuseRadiance,
        nvrhi::ITexture*           specularRadiance,
        nvrhi::ITexture*           viewZ,
        nvrhi::ITexture*           motionVectors,
        nvrhi::ITexture*           outDiffuse,
        nvrhi::ITexture*           outSpecular,
        const nrd::CommonSettings& commonSettings,
        const void*                denoiserSettings);

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
    nrd::Instance*                   m_Instance    = nullptr;

    // Shared binding layout: constant buffer + samplers (space = cbAndSamplers space).
    nvrhi::BindingLayoutHandle       m_BindingLayout0;

    // Persistent volatile constant buffer shared across all NRD dispatches in a frame.
    nvrhi::BufferHandle              m_ConstantBuffer;

    std::vector<NrdPipeline>         m_Pipelines;
    nvrhi::SamplerHandle             m_Samplers[(int)nrd::Sampler::MAX_NUM];

    // NRD managed pool textures via RenderGraph.
    std::vector<RGTextureHandle>     m_PermanentPoolTextures;
    std::vector<RGTextureHandle>     m_TransientPoolTextures;

    // Resolve an NRD ResourceDesc to the physical NVRHI texture it should
    // be bound to for a given dispatch.
    nvrhi::ITexture* ResolveResource(
        nrd::ResourceType type, uint16_t indexInPool,
        const RenderGraph& renderGraph,
        nvrhi::ITexture* packedNormalRoughnessTex,
        nvrhi::ITexture* diffuse,  nvrhi::ITexture* specular,
        nvrhi::ITexture* viewZ,    nvrhi::ITexture* motionVectors,
        nvrhi::ITexture* outDiffuse, nvrhi::ITexture* outSpecular) const;
};
