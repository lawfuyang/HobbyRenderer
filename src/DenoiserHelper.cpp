#include "DenoiserHelper.h"
#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

#include "shaders/ShaderShared.h"

extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;

// ============================================================================
// Format conversion helpers
// ============================================================================

static nvrhi::Format GetNVRHIFormat(nrd::Format format)
{
    switch (format)
    {
    case nrd::Format::R8_UNORM:              return nvrhi::Format::R8_UNORM;
    case nrd::Format::R8_SNORM:              return nvrhi::Format::R8_SNORM;
    case nrd::Format::R8_UINT:               return nvrhi::Format::R8_UINT;
    case nrd::Format::R8_SINT:               return nvrhi::Format::R8_SINT;
    case nrd::Format::RG8_UNORM:             return nvrhi::Format::RG8_UNORM;
    case nrd::Format::RG8_SNORM:             return nvrhi::Format::RG8_SNORM;
    case nrd::Format::RG8_UINT:              return nvrhi::Format::RG8_UINT;
    case nrd::Format::RG8_SINT:              return nvrhi::Format::RG8_SINT;
    case nrd::Format::RGBA8_UNORM:           return nvrhi::Format::RGBA8_UNORM;
    case nrd::Format::RGBA8_SNORM:           return nvrhi::Format::RGBA8_SNORM;
    case nrd::Format::RGBA8_UINT:            return nvrhi::Format::RGBA8_UINT;
    case nrd::Format::RGBA8_SINT:            return nvrhi::Format::RGBA8_SINT;
    case nrd::Format::RGBA8_SRGB:            return nvrhi::Format::SRGBA8_UNORM;
    case nrd::Format::R16_UNORM:             return nvrhi::Format::R16_UNORM;
    case nrd::Format::R16_SNORM:             return nvrhi::Format::R16_SNORM;
    case nrd::Format::R16_UINT:              return nvrhi::Format::R16_UINT;
    case nrd::Format::R16_SINT:              return nvrhi::Format::R16_SINT;
    case nrd::Format::R16_SFLOAT:            return nvrhi::Format::R16_FLOAT;
    case nrd::Format::RG16_UNORM:            return nvrhi::Format::RG16_UNORM;
    case nrd::Format::RG16_SNORM:            return nvrhi::Format::RG16_SNORM;
    case nrd::Format::RG16_UINT:             return nvrhi::Format::RG16_UINT;
    case nrd::Format::RG16_SINT:             return nvrhi::Format::RG16_SINT;
    case nrd::Format::RG16_SFLOAT:           return nvrhi::Format::RG16_FLOAT;
    case nrd::Format::RGBA16_UNORM:          return nvrhi::Format::RGBA16_UNORM;
    case nrd::Format::RGBA16_SNORM:          return nvrhi::Format::RGBA16_SNORM;
    case nrd::Format::RGBA16_UINT:           return nvrhi::Format::RGBA16_UINT;
    case nrd::Format::RGBA16_SINT:           return nvrhi::Format::RGBA16_SINT;
    case nrd::Format::RGBA16_SFLOAT:         return nvrhi::Format::RGBA16_FLOAT;
    case nrd::Format::R32_UINT:              return nvrhi::Format::R32_UINT;
    case nrd::Format::R32_SINT:              return nvrhi::Format::R32_SINT;
    case nrd::Format::R32_SFLOAT:            return nvrhi::Format::R32_FLOAT;
    case nrd::Format::RG32_UINT:             return nvrhi::Format::RG32_UINT;
    case nrd::Format::RG32_SINT:             return nvrhi::Format::RG32_SINT;
    case nrd::Format::RG32_SFLOAT:           return nvrhi::Format::RG32_FLOAT;
    case nrd::Format::RGB32_UINT:            return nvrhi::Format::RGB32_UINT;
    case nrd::Format::RGB32_SINT:            return nvrhi::Format::RGB32_SINT;
    case nrd::Format::RGB32_SFLOAT:          return nvrhi::Format::RGB32_FLOAT;
    case nrd::Format::RGBA32_UINT:           return nvrhi::Format::RGBA32_UINT;
    case nrd::Format::RGBA32_SINT:           return nvrhi::Format::RGBA32_SINT;
    case nrd::Format::RGBA32_SFLOAT:         return nvrhi::Format::RGBA32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM:  return nvrhi::Format::R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT:   return nvrhi::Format::UNKNOWN; // not representable
    case nrd::Format::R11_G11_B10_UFLOAT:    return nvrhi::Format::R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:    return nvrhi::Format::UNKNOWN; // not representable
    default:                                  return nvrhi::Format::UNKNOWN;
    }
}

// Returns true for OUT_* resource types; these must be bound as UAVs and
// registered as RenderGraph writes in Setup().
static bool IsOutputResourceType(nrd::ResourceType type)
{
    switch (type)
    {
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
    case nrd::ResourceType::OUT_DIFF_SH0:
    case nrd::ResourceType::OUT_DIFF_SH1:
    case nrd::ResourceType::OUT_SPEC_SH0:
    case nrd::ResourceType::OUT_SPEC_SH1:
    case nrd::ResourceType::OUT_DIFF_HITDIST:
    case nrd::ResourceType::OUT_SPEC_HITDIST:
    case nrd::ResourceType::OUT_DIFF_DIRECTION_HITDIST:
    case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:
    case nrd::ResourceType::OUT_SIGNAL:
    case nrd::ResourceType::OUT_VALIDATION:
        return true;
    default:
        return false;
    }
}

// ============================================================================
// DenoiserHelper – construction / destruction
// ============================================================================

DenoiserHelper::DenoiserHelper(nrd::Denoiser denoiser)
    : m_Denoiser(denoiser)
    , m_Identifier(static_cast<nrd::Identifier>(denoiser))
{
    SDL_assert(m_Denoiser != nrd::Denoiser::REFERENCE && "NRD Denoiser::REFERENCE is not supported in this sample");
    m_bNeedsPackedNormalRoughness = m_Denoiser != nrd::Denoiser::SIGMA_SHADOW && m_Denoiser != nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY;
}

DenoiserHelper::~DenoiserHelper()
{
    Shutdown();
}

// ============================================================================
// DenoiserHelper::Initialize
// ============================================================================

void DenoiserHelper::Initialize()
{
    SDL_assert(!m_NRDInstance && "DenoiserHelper::Initialize called twice without Shutdown");

    // -------------------------------------------------------------------------
    // Create NRD instance
    // -------------------------------------------------------------------------
    {
        const nrd::DenoiserDesc denoiserDescs[] =
        {
            { m_Identifier, m_Denoiser }
        };

        nrd::InstanceCreationDesc instanceCreationDesc{};
        instanceCreationDesc.denoisers    = denoiserDescs;
        instanceCreationDesc.denoisersNum = static_cast<uint32_t>(std::size(denoiserDescs));

        NRD_CHECK(nrd::CreateInstance(instanceCreationDesc, m_NRDInstance));
        SDL_Log("[DenoiserHelper] NRD instance created for denoiser %s", nrd::GetDenoiserString(m_Denoiser));
    }

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_NRDInstance);

    // -------------------------------------------------------------------------
    // Samplers
    // -------------------------------------------------------------------------
    SDL_assert(instanceDesc->samplersNum <= static_cast<uint32_t>(nrd::Sampler::MAX_NUM));
    for (uint32_t i = 0; i < instanceDesc->samplersNum; ++i)
    {
        switch (instanceDesc->samplers[i])
        {
        case nrd::Sampler::NEAREST_CLAMP:
            m_Samplers[i] = CommonResources::GetInstance().PointClamp;
            break;
        case nrd::Sampler::LINEAR_CLAMP:
            m_Samplers[i] = CommonResources::GetInstance().LinearClamp;
            break;
        default:
            SDL_assert(false && "Unknown NRD sampler mode");
            break;
        }
    }

    // -------------------------------------------------------------------------
    // Record permanent texture descriptors (actual textures will be declared in Setup)
    // -------------------------------------------------------------------------
    m_PermanentTextureHandles.resize(instanceDesc->permanentPoolSize);

    // -------------------------------------------------------------------------
    // Record transient texture descriptors (will be declared in Setup)
    // -------------------------------------------------------------------------
    m_TransientTextureHandles.resize(instanceDesc->transientPoolSize);
    m_TransientTextureDescs.reserve(instanceDesc->transientPoolSize);

    SDL_Log("[DenoiserHelper] Initialized: %u permanent textures, %u transient textures",
            instanceDesc->permanentPoolSize, instanceDesc->transientPoolSize);
}

// ============================================================================
// DenoiserHelper::Shutdown
// ============================================================================

void DenoiserHelper::Shutdown()
{
    if (!m_NRDInstance)
        return;

    m_PermanentTextureHandles.clear();
    m_TransientTextureDescs.clear();
    m_TransientTextureHandles.clear();

    for (auto& s : m_Samplers)
        s = nullptr;

    nrd::DestroyInstance(*m_NRDInstance);
    m_NRDInstance = nullptr;

    SDL_Log("[DenoiserHelper] Shutdown complete");
}

// ============================================================================
// DenoiserHelper::Setup
// ============================================================================

void DenoiserHelper::Setup(RenderGraph& renderGraph)
{
    SDL_assert(m_NRDInstance && "DenoiserHelper::Setup called before Initialize");

    Renderer* renderer = Renderer::GetInstance();
    const uint32_t renderWidth = renderer->m_RHI->m_SwapchainExtent.x;
    const uint32_t renderHeight = renderer->m_RHI->m_SwapchainExtent.y;
    SDL_assert(renderWidth > 0 && renderHeight > 0);

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_NRDInstance);

    auto GetRGTextureDesc = [&](uint32_t i, bool bIsPermanent)
        {
            const nrd::TextureDesc& nrdTexDesc = bIsPermanent ? instanceDesc->permanentPool[i] : instanceDesc->transientPool[i];

            const nvrhi::Format fmt = GetNVRHIFormat(nrdTexDesc.format);
            SDL_assert(fmt != nvrhi::Format::UNKNOWN && "Unsupported NRD texture format");

            RGTextureDesc rgDesc;
            rgDesc.m_NvrhiDesc.width = DivideAndRoundUp(renderWidth, nrdTexDesc.downsampleFactor);
            rgDesc.m_NvrhiDesc.height = DivideAndRoundUp(renderHeight, nrdTexDesc.downsampleFactor);
            rgDesc.m_NvrhiDesc.format = fmt;
            rgDesc.m_NvrhiDesc.dimension = nvrhi::TextureDimension::Texture2D;
            rgDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            rgDesc.m_NvrhiDesc.isUAV = true;
            char debugNameBuf[64];
            snprintf(debugNameBuf, sizeof(debugNameBuf), "NRD %s [%u]", bIsPermanent ? "Permanent" : "Transient", i);
            rgDesc.m_NvrhiDesc.debugName = debugNameBuf;

            return rgDesc;
        };

    // -------------------------------------------------------------------------
    // Declare permanent pool textures (persistent across frames)
    // -------------------------------------------------------------------------
    for (uint32_t i = 0; i < instanceDesc->permanentPoolSize; ++i)
    {
        renderGraph.DeclarePersistentTexture(GetRGTextureDesc(i, true), m_PermanentTextureHandles[i]);
    }

    // -------------------------------------------------------------------------
    // Declare transient pool textures (fresh each frame)
    // -------------------------------------------------------------------------
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_TransientTextureHandles.size()); ++i)
    {
        renderGraph.DeclareTexture(GetRGTextureDesc(i, false), m_TransientTextureHandles[i]);
    }

    // -------------------------------------------------------------------------
    // Declare the internal packed normal+roughness texture
    // -------------------------------------------------------------------------
    if (m_bNeedsPackedNormalRoughness)
    {
        RGTextureDesc packDesc;
        packDesc.m_NvrhiDesc.width        = renderWidth;
        packDesc.m_NvrhiDesc.height       = renderHeight;
        packDesc.m_NvrhiDesc.format       = nvrhi::Format::R10G10B10A2_UNORM;
        packDesc.m_NvrhiDesc.dimension    = nvrhi::TextureDimension::Texture2D;
        packDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        packDesc.m_NvrhiDesc.isUAV        = true;
        packDesc.m_NvrhiDesc.debugName    = "NRD Packed Normal+Roughness";

        renderGraph.DeclareTexture(packDesc, m_PackedNormalRoughnessHandle);

        renderGraph.ReadTexture(g_RG_GBufferNormals);
        renderGraph.ReadTexture(g_RG_GBufferORM);
    }
}

// ============================================================================
// DenoiserHelper::Execute
// ============================================================================

void DenoiserHelper::Execute(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph, const DenoisePassDesc& desc)
{
    PROFILE_FUNCTION();

    SDL_assert(m_NRDInstance && "DenoiserHelper::Execute called before Initialize");

    nvrhi::utils::ScopedMarker scopedMarker{ commandList, "NRD Denoise" };

    Renderer* renderer = Renderer::GetInstance();
    nvrhi::DeviceHandle device = renderer->m_RHI->m_NvrhiDevice;
    const uint32_t renderWidth = renderer->m_RHI->m_SwapchainExtent.x;
    const uint32_t renderHeight = renderer->m_RHI->m_SwapchainExtent.y;

    // -------------------------------------------------------------------------
    // Optional: pack GBuffer normals + roughness into NRD's expected format
    // -------------------------------------------------------------------------
    if (m_bNeedsPackedNormalRoughness)
    {
        PackNormalRoughness(commandList, renderGraph, desc);
    }

    // -------------------------------------------------------------------------
    // Configure NRD denoiser and common settings
    // -------------------------------------------------------------------------
    if (desc.denoiserSettings)
    {
        NRD_CHECK(nrd::SetDenoiserSettings(*m_NRDInstance, m_Identifier, desc.denoiserSettings));
    }

    NRD_CHECK(nrd::SetCommonSettings(*m_NRDInstance, desc.commonSettings));

    // -------------------------------------------------------------------------
    // Retrieve compute dispatches
    // -------------------------------------------------------------------------
    const nrd::Identifier identifiers[] = { m_Identifier };
    const nrd::DispatchDesc* dispatchDescs = nullptr;
    uint32_t dispatchDescNum = 0;

    NRD_CHECK(nrd::GetComputeDispatches(*m_NRDInstance,
                                        identifiers,
                                        static_cast<uint32_t>(std::size(identifiers)),
                                        dispatchDescs,
                                        dispatchDescNum));

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_NRDInstance);

    // -------------------------------------------------------------------------
    // Create volatile constant buffer on demand
    // -------------------------------------------------------------------------
    const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(
        instanceDesc->constantBufferMaxDataSize,
        "NRD Constant Buffer (temp)",
        /*maxVersions=*/1);
    nvrhi::BufferHandle constantBuffer = device->createBuffer(cbDesc);
    SDL_assert(constantBuffer && "Failed to create NRD constant buffer");

    // Prime the volatile constant buffer so NVRHI doesn't complain about an
    // unwritten volatile buffer being bound.
    {
        std::vector<uint8_t> dummyData(instanceDesc->constantBufferMaxDataSize, 0u);
        commandList->writeBuffer(constantBuffer, dummyData.data(), dummyData.size());
    }

    // -------------------------------------------------------------------------
    // Submit each NRD dispatch via AddComputePass
    // -------------------------------------------------------------------------
    for (uint32_t dispatchIdx = 0; dispatchIdx < dispatchDescNum; ++dispatchIdx)
    {
        PROFILE_SCOPED("NRD Dispatch");
        
        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[dispatchIdx];
        const nrd::PipelineDesc& pipelineDesc = instanceDesc->pipelines[dispatchDesc.pipelineIndex];

        nvrhi::BindingSetDesc resourceBindingSetDesc;
        nvrhi::BindingSetDesc constantBindingSetDesc;

        // -----------------------------------------------------------------
        // Build resource binding set (space = resourcesSpaceIndex)
        // -----------------------------------------------------------------
        uint32_t resourceIndex = 0;
        for (uint32_t rangeIdx = 0; rangeIdx < pipelineDesc.resourceRangesNum; ++rangeIdx)
        {
            const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[rangeIdx];

            for (uint32_t descOffset = 0; descOffset < range.descriptorsNum; ++descOffset)
            {
                SDL_assert(resourceIndex < dispatchDesc.resourcesNum && "NRD resource index out of bounds");
                const nrd::ResourceDesc& nrdResource = dispatchDesc.resources[resourceIndex];

                nvrhi::TextureHandle texture = ResolveResource(nrdResource.type, nrdResource.indexInPool, desc, renderGraph);
                if (!texture)
                {
                    SDL_Log("[DenoiserHelper] WARNING: Unresolved NRD resource (type=%s, indexInPool=%u) for dispatch '%s'. Skipping dispatch.",
                            nrd::GetResourceTypeString(nrdResource.type), nrdResource.indexInPool,
                            dispatchDesc.name ? dispatchDesc.name : "unknown");
                    SDL_assert(false && "Unresolved NRD resource");
                }

                nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources;
                subresources.baseMipLevel = 0;
                subresources.numMipLevels = 1;

                const nvrhi::ResourceType nvrhiType = (range.descriptorType == nrd::DescriptorType::TEXTURE)
                    ? nvrhi::ResourceType::Texture_SRV
                    : nvrhi::ResourceType::Texture_UAV;

                nvrhi::BindingSetItem setItem = nvrhi::BindingSetItem::None();
                setItem.resourceHandle = texture;
                setItem.slot = instanceDesc->resourcesBaseRegisterIndex + descOffset;
                setItem.subresources = subresources;
                setItem.type = nvrhiType;

                resourceBindingSetDesc.bindings.push_back(setItem);
                ++resourceIndex;
            }
        }

        // -----------------------------------------------------------------
        // Build constant buffer + sampler binding set
        // (space = constantBufferAndSamplersSpaceIndex)
        // -----------------------------------------------------------------
        if (dispatchDesc.constantBufferDataSize > 0 && !dispatchDesc.constantBufferDataMatchesPreviousDispatch)
        {
            commandList->writeBuffer(constantBuffer, dispatchDesc.constantBufferData, dispatchDesc.constantBufferDataSize);
        }

        if (pipelineDesc.hasConstantData)
        {
            constantBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(instanceDesc->constantBufferRegisterIndex, constantBuffer));
        }

        for (uint32_t samplerIdx = 0; samplerIdx < instanceDesc->samplersNum; ++samplerIdx)
        {
            constantBindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(instanceDesc->samplersBaseRegisterIndex + samplerIdx, m_Samplers[samplerIdx]));
        }

        // -----------------------------------------------------------------
        // Resolve shader cache key from NRD shaderIdentifier
        // -----------------------------------------------------------------
        const std::string shaderKey = BuildShaderCacheKey(pipelineDesc.shaderIdentifier);

        // -----------------------------------------------------------------
        // Call AddComputePass to handle pipeline creation and dispatch
        // -----------------------------------------------------------------
        Renderer::RenderPassParams passParams{};
        passParams.commandList = commandList;
        passParams.shaderName = shaderKey;
        passParams.bindingSetDesc = resourceBindingSetDesc;
        passParams.registerSpace = instanceDesc->resourcesSpaceIndex;
        passParams.additionalBindingSets.push_back({ constantBindingSetDesc, instanceDesc->constantBufferAndSamplersSpaceIndex });
        passParams.bIncludeBindlessResources = false; // NRD shaders use explicit bindings for all resources, so no need to bind the global tables
        passParams.dispatchParams.x = dispatchDesc.gridWidth;
        passParams.dispatchParams.y = dispatchDesc.gridHeight;
        passParams.dispatchParams.z = 1;

        renderer->AddComputePass(passParams);
    }
}

// ============================================================================
// DenoiserHelper::PackNormalRoughness
// ============================================================================

void DenoiserHelper::PackNormalRoughness(nvrhi::CommandListHandle commandList,
                                          const RenderGraph& renderGraph,
                                          const DenoisePassDesc& desc)
{
    nvrhi::utils::ScopedMarker marker{ commandList, "DenoiserHelper: Pack Normal+Roughness" };

    nvrhi::TextureHandle gBufferNormals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Read);
    nvrhi::TextureHandle gBufferORM = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Read);
    nvrhi::TextureHandle packedDst = renderGraph.GetTexture(m_PackedNormalRoughnessHandle, RGResourceAccessMode::Write);

    const Vector2U outputResolution{ packedDst->getDesc().width, packedDst->getDesc().height };

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings =
    {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(outputResolution)),
        nvrhi::BindingSetItem::Texture_SRV(0, gBufferNormals),
        nvrhi::BindingSetItem::Texture_SRV(1, gBufferORM),
        nvrhi::BindingSetItem::Texture_UAV(0, packedDst),
    };

    Renderer::RenderPassParams passParams{};
    passParams.commandList          = commandList;
    passParams.shaderName           = "PackNormalRoughness_CSMain";
    passParams.bindingSetDesc       = bindingSetDesc;
    passParams.pushConstants        = &outputResolution;
    passParams.pushConstantsSize    = sizeof(outputResolution);
    passParams.dispatchParams.x     = DivideAndRoundUp(outputResolution.x, 8u);
    passParams.dispatchParams.y     = DivideAndRoundUp(outputResolution.y, 8u);
    passParams.dispatchParams.z     = 1;

    Renderer::GetInstance()->AddComputePass(passParams);
}

// ============================================================================
// DenoiserHelper::BuildShaderCacheKey
// ============================================================================

std::string DenoiserHelper::BuildShaderCacheKey(const char* nrdShaderIdentifier)
{
    // NRD format:  "FileName.cs.hlsl|KEY1=VAL1|KEY2=VAL2"
    // Cache key:   "FileName.cs_main"                          (no permutations)
    //              "FileName.cs_main_KEY1=VAL1_KEY2=VAL2"      (2+ sorted permutations)
    //
    // (Mirrors Renderer::LoadShaders key generation exactly.)

    std::string identStr = nrdShaderIdentifier;

    // Strip ".hlsl" suffix to get the stem (e.g. "REBLUR_ClassifyTiles.cs")
    const size_t hlslPos = identStr.find(".hlsl");
    std::string stem = (hlslPos != std::string::npos) ? identStr.substr(0, hlslPos) : identStr;

    // All NRD shaders use "main" as their entry point
    std::string key = stem + "_main";

    // Parse pipe-delimited permutations
    std::vector<std::string> permutations;
    {
        std::string remainder = (hlslPos != std::string::npos) ? identStr.substr(hlslPos) : "";
        while (remainder.find('|') != std::string::npos)
        {
            remainder = remainder.substr(remainder.find('|') + 1);
            permutations.push_back(remainder.substr(0, remainder.find('|')));
        }
    }

    // Sort alphabetically to match the std::map iteration order used at load time.
    std::ranges::sort(permutations);
    for (const std::string& perm : permutations)
        key += "_" + perm;

    return key;
}

// ============================================================================
// DenoiserHelper::ResolveResource
// ============================================================================

nvrhi::TextureHandle DenoiserHelper::ResolveResource(nrd::ResourceType type,
                                                       uint16_t indexInPool,
                                                       const DenoisePassDesc& desc,
                                                       const RenderGraph& renderGraph) const
{
    switch (type)
    {
    // TRANSIENT_POOL / PERMANENT_POOL are handled here; caller-provided resources
    // are passed in as raw nvrhi::TextureHandles.
    case nrd::ResourceType::TRANSIENT_POOL:
        SDL_assert(indexInPool < static_cast<uint16_t>(m_TransientTextureHandles.size()));
        return renderGraph.GetTexture(m_TransientTextureHandles[indexInPool], RGResourceAccessMode::Write);

    case nrd::ResourceType::PERMANENT_POOL:
        SDL_assert(indexInPool < static_cast<uint16_t>(m_PermanentTextureHandles.size()));
        return renderGraph.GetTexture(m_PermanentTextureHandles[indexInPool], RGResourceAccessMode::Write);

    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
        // If we ran the pack pass, return the packed texture; otherwise fall through
        // to the generic path and use whatever the caller placed in the resources array.
        if (m_PackedNormalRoughnessHandle.IsValid())
        {
            return renderGraph.GetTexture(m_PackedNormalRoughnessHandle, RGResourceAccessMode::Read);
        }
        [[fallthrough]];

    default:
        // Generic: resolve from the caller-provided resources array (raw texture handles).
        if (static_cast<uint32_t>(type) < kNRDAppResourceTypeCount)
        {
            const nvrhi::TextureHandle& handle = desc.resources[static_cast<uint32_t>(type)];
            if (handle)
            {
                return handle;
            }
        }
        return nullptr;
    }
}

void FillNRDCommonSettingsHelper(nrd::CommonSettings& settings)
{
    Renderer* renderer = Renderer::GetInstance();
    const PlanarViewConstants& view     = renderer->m_Scene.m_View;
    const PlanarViewConstants& prevView = renderer->m_Scene.m_ViewPrev;

    const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
    const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

    // -------------------------------------------------------------------------
    // Matrices
    // -------------------------------------------------------------------------
    // NRD expects column-major matrices with column vectors (v_out = M * v_in).
    // Our matrices are row-major with row vectors (v_out = v_in * M_DX).
    // Copying row-major bytes into NRD's column-major slot is equivalent to
    // passing M_DX^T, which is exactly what NRD needs to reproduce the same
    // transformation — the row/column duality cancels out.
    //
    // Use non-jittered variants (_NoOffset) as required by NRD.
    static_assert(sizeof(settings.viewToClipMatrix) == sizeof(view.m_MatViewToClipNoOffset), "Matrix size mismatch");
    memcpy(settings.viewToClipMatrix,     &view.m_MatViewToClipNoOffset,     sizeof(settings.viewToClipMatrix));
    memcpy(settings.viewToClipMatrixPrev, &prevView.m_MatViewToClipNoOffset, sizeof(settings.viewToClipMatrixPrev));
    memcpy(settings.worldToViewMatrix,    &view.m_MatWorldToView,            sizeof(settings.worldToViewMatrix));
    memcpy(settings.worldToViewMatrixPrev,&prevView.m_MatWorldToView,        sizeof(settings.worldToViewMatrixPrev));

    // -------------------------------------------------------------------------
    // Viewport / resource size
    // -------------------------------------------------------------------------
    settings.resourceSize[0]     = static_cast<uint16_t>(width);
    settings.resourceSize[1]     = static_cast<uint16_t>(height);
    settings.resourceSizePrev[0] = static_cast<uint16_t>(width);
    settings.resourceSizePrev[1] = static_cast<uint16_t>(height);
    settings.rectSize[0]         = static_cast<uint16_t>(width);
    settings.rectSize[1]         = static_cast<uint16_t>(height);
    settings.rectSizePrev[0]     = static_cast<uint16_t>(width);
    settings.rectSizePrev[1]     = static_cast<uint16_t>(height);

    // -------------------------------------------------------------------------
    // Motion vector scale
    // -------------------------------------------------------------------------
    // Our GBuffer motion vectors are in pixel space (Δx, Δy).
    // NRD expects: pixelUvPrev = pixelUv + mv.xy, so scale pixels → UV.
    settings.motionVectorScale[0] = 1.0f / static_cast<float>(width);
    settings.motionVectorScale[1] = 1.0f / static_cast<float>(height);
    // Our motionVector.z stores (prevViewZ - curViewZ) in view-space units,
    // so pass it through unscaled.
    settings.motionVectorScale[2] = 1.0f;

    settings.isMotionVectorInWorldSpace = false;

    // -------------------------------------------------------------------------
    // Camera jitter (TAA sub-pixel offset in [-0.5, 0.5] range)
    // -------------------------------------------------------------------------
    // TODO: uncomment when TAA is implemented
    // settings.cameraJitter[0] = view.m_PixelOffset.x;
    // settings.cameraJitter[1] = view.m_PixelOffset.y;
    // settings.cameraJitterPrev[0] = prevView.m_PixelOffset.x;
    // settings.cameraJitterPrev[1] = prevView.m_PixelOffset.y;

    // -------------------------------------------------------------------------
    // Frame index and accumulation mode
    // -------------------------------------------------------------------------
    settings.frameIndex = renderer->m_FrameNumber;
    // On the very first frame NRD's permanent pool textures are uninitialized.
    // Send CLEAR_AND_RESTART once so RELAX flushes stale history, then CONTINUE.
    settings.accumulationMode = (renderer->m_FrameNumber == 0)
        ? nrd::AccumulationMode::CLEAR_AND_RESTART
        : nrd::AccumulationMode::CONTINUE;

    // -------------------------------------------------------------------------
    // Optional inputs — disabled for now
    // -------------------------------------------------------------------------
    settings.isHistoryConfidenceAvailable        = false; // TODO: add confidence input and enable
    settings.isDisocclusionThresholdMixAvailable = false;
    settings.isBaseColorMetalnessAvailable       = false;

    // -------------------------------------------------------------------------
    // Denoising range — match FullSample default (1000 m).
    // Sky pixels are written as 1e6 in GenerateViewZ so they fall outside this range.
    // Using too large a value (e.g. 500000) causes NRD to attempt depth tracking at
    // extreme distances where precision is insufficient, contributing to ghosting.
    // -------------------------------------------------------------------------
    settings.denoisingRange = 1000.0f;
}
