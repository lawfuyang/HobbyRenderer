#pragma once

#include "pch.h"
#include "RenderGraph.h"

#include "NRD.h"

// ============================================================================
// NRD Error Handling
// ============================================================================

// Assert + log on any non-SUCCESS nrd::Result.  Usage: NRD_CHECK(nrd::CreateInstance(...));
#define NRD_CHECK(call)                                                                             \
    do {                                                                                            \
        if (nrd::Result _nrdResult = (call); _nrdResult != nrd::Result::SUCCESS)                   \
        {                                                                                           \
            SDL_Log("[NRD] Call failed (%s:%d): %s => result=%u",                                  \
                    __FILE__, __LINE__, #call, static_cast<uint32_t>(_nrdResult));                  \
            SDL_assert(false && "NRD call failed");                                                 \
        }                                                                                           \
    } while(0)

// ============================================================================
// DenoisePassDesc
// ============================================================================

// Number of user-facing NRD resource slots (excludes TRANSIENT_POOL, PERMANENT_POOL, MAX_NUM).
inline constexpr uint32_t kNRDAppResourceTypeCount = static_cast<uint32_t>(nrd::ResourceType::TRANSIENT_POOL);

struct DenoisePassDesc
{
    // NRD resource handles indexed by nrd::ResourceType (raw NVRHI texture handles).
    // Populate only the slots relevant to the active denoiser.
    // TRANSIENT_POOL and PERMANENT_POOL entries are managed internally by DenoiserHelper.
    //
    // Special handling for IN_NORMAL_ROUGHNESS:
    //   If rawNormalsHandle is valid, DenoiserHelper packs normals + roughness internally
    //   into R10G10B10A2_UNORM and overrides this slot.  Leave it empty unless you are
    //   supplying an already-packed texture (no rawNormalsHandle provided).
    nvrhi::TextureHandle resources[kNRDAppResourceTypeCount] = {};

    // Per-frame common settings (view matrices, resolution, jitter, etc.).
    // Must be fully populated before calling AddDenoisePass / Execute.
    nrd::CommonSettings commonSettings = {};

    // Optional per-denoiser settings (e.g. nrd::ReblurSettings, nrd::SigmaSettings).
    // Passed verbatim to nrd::SetDenoiserSettings each frame.  nullptr = skip.
    const void* denoiserSettings = nullptr;
};

void FillNRDCommonSettingsHelper(nrd::CommonSettings& settings);

// ============================================================================
// DenoiserHelper
// ============================================================================

// Wraps a single nrd::Instance and manages its lifetime, pools, and dispatch
// submission.  Owns permanent and transient pool textures and declares them
// through the RenderGraph each setup phase.  Caller-provided NRD resource
// handles are passed as nvrhi::TextureHandles
//
// Typical usage pattern (inside an IRenderer):
//
//   // Initialization phase:
//   void MyRenderer::Initialize()
//   {
//       m_DenoiserHelper = std::make_unique<DenoiserHelper>(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);
//       m_DenoiserHelper->Initialize();
//   }
//
//   // Setup phase:
//   void MyRenderer::Setup(RenderGraph& rg)
//   {
//       m_DenoiserHelper->Setup(rg);
//   }
//
//   // Render phase:
//   void MyRenderer::Render(nvrhi::CommandListHandle cmd, const RenderGraph& rg)
//   {
//       DenoisePassDesc desc;
//       desc[nrd::ResourceType::IN_MV]   = g_MotionVecTexture;
//       desc[nrd::ResourceType::IN_VIEWZ]= g_LinearZTexture;
//       desc[nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST] = g_DiffuseTexture;
//       desc[nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST]= g_DenoisedDiffuseTexture;
//       FillNRDCommonSettings(desc.commonSettings);
//       m_DenoiserHelper->Execute(cmd, rg, desc);
//   }

class DenoiserHelper
{
public:
    explicit DenoiserHelper(nrd::Denoiser denoiser);
    ~DenoiserHelper();

    // Create the NRD instance and allocate permanent pool descriptor specs.
    // Must be called after the NVRHI device is ready.
    void Initialize();

    // Destroy the NRD instance and all owned resources.
    void Shutdown();

    // Declare NRD permanent and transient pool textures in the RenderGraph.
    // Must be called from inside an IRenderer::Setup() while the RenderGraph
    // setup context is active.
    void Setup(RenderGraph& renderGraph);

    // Submit the NRD pre-pass (PackNormalRoughness, if requested) and all
    // NRD compute dispatch calls.  Must be called inside an IRenderer::Render()
    // after RenderGraph::Compile() has run.
    void Execute(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph, const DenoisePassDesc& desc);

    nrd::Denoiser GetDenoiserType() const { return m_Denoiser; }
    nrd::Identifier GetIdentifier() const { return m_Identifier; }
    bool IsInitialized() const { return m_NRDInstance != nullptr; }

private:
    // Run a compute pass that converts GBuffer normals (octahedron RG16_FLOAT) and
    // roughness (from ORM RG8_UNORM .x) into the R10G10B10A2_UNORM format expected
    // by NRD_FrontEnd_PackNormalAndRoughness.
    void PackNormalRoughness(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph, const DenoisePassDesc& desc);

    // Build the shader cache key that matches what Renderer::LoadShaders generates
    // for an NRD shader identifier string of the form
    // "FileName.cs.hlsl|KEY1=VAL1|KEY2=VAL2".
    static std::string BuildShaderCacheKey(const char* nrdShaderIdentifier);

    // Resolve an NRD ResourceDesc to the physical nvrhi::TextureHandle it maps to.
    nvrhi::TextureHandle ResolveResource(nrd::ResourceType type, uint16_t indexInPool, const DenoisePassDesc& desc, const RenderGraph& renderGraph) const;

    // -------------------------------------------------------------------------
    // Private data
    // -------------------------------------------------------------------------

    nrd::Denoiser    m_Denoiser;
    nrd::Identifier  m_Identifier;
    nrd::Instance*   m_NRDInstance = nullptr;

    // NRD sampler handles (indexed to match nrd::InstanceDesc::samplers).
    nvrhi::SamplerHandle m_Samplers[static_cast<uint32_t>(nrd::Sampler::MAX_NUM)] = {};

    // Permanent pool – these textures persist across frames (history buffers).
    // Handles to textures declared via DeclarePersistentTexture in Setup().
    std::vector<RGTextureHandle> m_PermanentTextureHandles;

    // Transient pool – declared fresh each frame through the RenderGraph.
    std::vector<RGTextureDesc>   m_TransientTextureDescs;
    std::vector<RGTextureHandle> m_TransientTextureHandles;

    // Internal packed normal+roughness texture (R10G10B10A2_UNORM).
    // Valid only when rawNormalsHandle is provided in the desc.
    RGTextureHandle m_PackedNormalRoughnessHandle;

    bool m_bNeedsPackedNormalRoughness = false;
};
