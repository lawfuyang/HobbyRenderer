// Adapted from NVIDIA Donut framework's NrdIntegration (NrdIntegration.cpp).
// Original copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
//
// Key adaptations for this codebase:
//   - Removed Donut dependencies (PlanarView, RenderTargets, BindingCache, dm:: math).
//   - Pool textures are owned directly (not via RenderGraph).
//   - Pipelines and samplers created from NRD's embedded DXIL/SPIRV bytecode.
//   - PackNormalRoughness pre-pass dispatched via Renderer::AddComputePass.
//   - FillNRDCommonSettings uses this codebase's PlanarViewConstants / scene.

#include "NrdIntegration.h"
#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

#include "shaders/ShaderShared.h"

// ============================================================================
// Format helpers
// ============================================================================

static nvrhi::Format GetNvrhiFormat(nrd::Format format)
{
    switch (format)
    {
    case nrd::Format::R8_UNORM:             return nvrhi::Format::R8_UNORM;
    case nrd::Format::R8_SNORM:             return nvrhi::Format::R8_SNORM;
    case nrd::Format::R8_UINT:              return nvrhi::Format::R8_UINT;
    case nrd::Format::R8_SINT:              return nvrhi::Format::R8_SINT;
    case nrd::Format::RG8_UNORM:            return nvrhi::Format::RG8_UNORM;
    case nrd::Format::RG8_SNORM:            return nvrhi::Format::RG8_SNORM;
    case nrd::Format::RG8_UINT:             return nvrhi::Format::RG8_UINT;
    case nrd::Format::RG8_SINT:             return nvrhi::Format::RG8_SINT;
    case nrd::Format::RGBA8_UNORM:          return nvrhi::Format::RGBA8_UNORM;
    case nrd::Format::RGBA8_SNORM:          return nvrhi::Format::RGBA8_SNORM;
    case nrd::Format::RGBA8_UINT:           return nvrhi::Format::RGBA8_UINT;
    case nrd::Format::RGBA8_SINT:           return nvrhi::Format::RGBA8_SINT;
    case nrd::Format::RGBA8_SRGB:           return nvrhi::Format::SRGBA8_UNORM;
    case nrd::Format::R16_UNORM:            return nvrhi::Format::R16_UNORM;
    case nrd::Format::R16_SNORM:            return nvrhi::Format::R16_SNORM;
    case nrd::Format::R16_UINT:             return nvrhi::Format::R16_UINT;
    case nrd::Format::R16_SINT:             return nvrhi::Format::R16_SINT;
    case nrd::Format::R16_SFLOAT:           return nvrhi::Format::R16_FLOAT;
    case nrd::Format::RG16_UNORM:           return nvrhi::Format::RG16_UNORM;
    case nrd::Format::RG16_SNORM:           return nvrhi::Format::RG16_SNORM;
    case nrd::Format::RG16_UINT:            return nvrhi::Format::RG16_UINT;
    case nrd::Format::RG16_SINT:            return nvrhi::Format::RG16_SINT;
    case nrd::Format::RG16_SFLOAT:          return nvrhi::Format::RG16_FLOAT;
    case nrd::Format::RGBA16_UNORM:         return nvrhi::Format::RGBA16_UNORM;
    case nrd::Format::RGBA16_SNORM:         return nvrhi::Format::RGBA16_SNORM;
    case nrd::Format::RGBA16_UINT:          return nvrhi::Format::RGBA16_UINT;
    case nrd::Format::RGBA16_SINT:          return nvrhi::Format::RGBA16_SINT;
    case nrd::Format::RGBA16_SFLOAT:        return nvrhi::Format::RGBA16_FLOAT;
    case nrd::Format::R32_UINT:             return nvrhi::Format::R32_UINT;
    case nrd::Format::R32_SINT:             return nvrhi::Format::R32_SINT;
    case nrd::Format::R32_SFLOAT:           return nvrhi::Format::R32_FLOAT;
    case nrd::Format::RG32_UINT:            return nvrhi::Format::RG32_UINT;
    case nrd::Format::RG32_SINT:            return nvrhi::Format::RG32_SINT;
    case nrd::Format::RG32_SFLOAT:          return nvrhi::Format::RG32_FLOAT;
    case nrd::Format::RGB32_UINT:           return nvrhi::Format::RGB32_UINT;
    case nrd::Format::RGB32_SINT:           return nvrhi::Format::RGB32_SINT;
    case nrd::Format::RGB32_SFLOAT:         return nvrhi::Format::RGB32_FLOAT;
    case nrd::Format::RGBA32_UINT:          return nvrhi::Format::RGBA32_UINT;
    case nrd::Format::RGBA32_SINT:          return nvrhi::Format::RGBA32_SINT;
    case nrd::Format::RGBA32_SFLOAT:        return nvrhi::Format::RGBA32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return nvrhi::Format::R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT:  return nvrhi::Format::UNKNOWN; // not representable
    case nrd::Format::R11_G11_B10_UFLOAT:   return nvrhi::Format::R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:   return nvrhi::Format::UNKNOWN; // not representable
    default:                                 return nvrhi::Format::UNKNOWN;
    }
}

// ============================================================================
// NRD allocation callbacks (plain malloc/free wrappers)
// ============================================================================

static void* NrdAllocate(void* /*userArg*/, size_t size, size_t /*alignment*/)
{
    return malloc(size);
}

static void* NrdReallocate(void* /*userArg*/, void* memory, size_t size, size_t /*alignment*/)
{
    return realloc(memory, size);
}

static void NrdFree(void* /*userArg*/, void* memory)
{
    free(memory);
}

// ============================================================================
// FillNRDCommonSettings
// ============================================================================

void FillNRDCommonSettings(nrd::CommonSettings& settings)
{
    Renderer* renderer = Renderer::GetInstance();
    const PlanarViewConstants& view     = renderer->m_Scene.m_View;
    const PlanarViewConstants& prevView = renderer->m_Scene.m_ViewPrev;

    const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
    const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

    // NRD expects column-major matrices with column-vector convention.
    // Our matrices are row-major with row-vector convention (DirectXMath style).
    // Copying the bytes directly into NRD's slots is equivalent to transposing,
    // which is exactly what NRD needs — the row/column duality cancels out.
    // Use non-jittered variants (_NoOffset) as required by NRD.
    static_assert(sizeof(settings.viewToClipMatrix) == sizeof(view.m_MatViewToClipNoOffset));
    memcpy(settings.viewToClipMatrix,      &view.m_MatViewToClipNoOffset,      sizeof(settings.viewToClipMatrix));
    memcpy(settings.viewToClipMatrixPrev,  &prevView.m_MatViewToClipNoOffset,  sizeof(settings.viewToClipMatrixPrev));
    memcpy(settings.worldToViewMatrix,     &view.m_MatWorldToView,             sizeof(settings.worldToViewMatrix));
    memcpy(settings.worldToViewMatrixPrev, &prevView.m_MatWorldToView,         sizeof(settings.worldToViewMatrixPrev));

    settings.resourceSize[0]     = static_cast<uint16_t>(width);
    settings.resourceSize[1]     = static_cast<uint16_t>(height);
    settings.resourceSizePrev[0] = static_cast<uint16_t>(width);
    settings.resourceSizePrev[1] = static_cast<uint16_t>(height);
    settings.rectSize[0]         = static_cast<uint16_t>(width);
    settings.rectSize[1]         = static_cast<uint16_t>(height);
    settings.rectSizePrev[0]     = static_cast<uint16_t>(width);
    settings.rectSizePrev[1]     = static_cast<uint16_t>(height);

    // Motion vectors are in pixel space (Δx, Δy). NRD expects:
    //   pixelUvPrev = pixelUv + mv.xy → scale pixels → UV.
    settings.motionVectorScale[0] = 1.0f / static_cast<float>(width);
    settings.motionVectorScale[1] = 1.0f / static_cast<float>(height);
    settings.motionVectorScale[2] = 1.0f; // mv.z is already in view-space units

    settings.isMotionVectorInWorldSpace = false;

    settings.frameIndex = renderer->m_FrameNumber;
    // Flush history on the very first frame so permanent textures start clean.
    settings.accumulationMode = (renderer->m_FrameNumber == 0)
        ? nrd::AccumulationMode::CLEAR_AND_RESTART
        : nrd::AccumulationMode::CONTINUE;

    settings.isHistoryConfidenceAvailable        = false;
    settings.isDisocclusionThresholdMixAvailable = false;
    settings.isBaseColorMetalnessAvailable       = false;

    // Sky pixels are written as 1e6 in GenerateViewZ — safely outside this range.
    settings.denoisingRange = 1000.0f;
}

// ============================================================================
// NrdIntegration
// ============================================================================
NrdIntegration::~NrdIntegration()
{
    if (m_Instance)
    {
        nrd::DestroyInstance(*m_Instance);
        m_Instance    = nullptr;
        m_Initialized = false;
    }
}

bool NrdIntegration::Initialize(uint32_t width, uint32_t height)
{
    SDL_assert(!m_Initialized && "NrdIntegration::Initialize called twice");

    Renderer* renderer = Renderer::GetInstance();
    nvrhi::DeviceHandle device = renderer->m_RHI->m_NvrhiDevice;

    // -------------------------------------------------------------------------
    // Create NRD instance
    // -------------------------------------------------------------------------
    {
        const nrd::DenoiserDesc denoisers[] = { { /*id=*/0, m_Denoiser } };

        nrd::InstanceCreationDesc instanceCreationDesc{};
        instanceCreationDesc.allocationCallbacks.Allocate   = NrdAllocate;
        instanceCreationDesc.allocationCallbacks.Reallocate = NrdReallocate;
        instanceCreationDesc.allocationCallbacks.Free       = NrdFree;
        instanceCreationDesc.denoisers    = denoisers;
        instanceCreationDesc.denoisersNum = 1u;

        if (nrd::CreateInstance(instanceCreationDesc, m_Instance) != nrd::Result::SUCCESS)
        {
            SDL_Log("[NrdIntegration] nrd::CreateInstance failed for denoiser %s",
                    nrd::GetDenoiserString(m_Denoiser));
            return false;
        }
        SDL_Log("[NrdIntegration] NRD instance created for denoiser %s",
                nrd::GetDenoiserString(m_Denoiser));
    }

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_Instance);
    const nrd::LibraryDesc*  libraryDesc  = nrd::GetLibraryDesc();

    // -------------------------------------------------------------------------
    // Volatile constant buffer
    // -------------------------------------------------------------------------
    m_ConstantBuffer = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(
            instanceDesc->constantBufferMaxDataSize,
            "NrdConstantBuffer",
            instanceDesc->descriptorPoolDesc.setsMaxNum * 4));

    if (!m_ConstantBuffer)
    {
        SDL_assert(false && "NrdIntegration: failed to create constant buffer");
        return false;
    }

    // -------------------------------------------------------------------------
    // Samplers
    // -------------------------------------------------------------------------
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
            SDL_assert(false && "NrdIntegration: unknown NRD sampler mode");
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // SPIRV binding offsets (from NRD library descriptor)
    // -------------------------------------------------------------------------
    nvrhi::VulkanBindingOffsets bindingOffsets;
    bindingOffsets.shaderResource  = libraryDesc->spirvBindingOffsets.textureOffset;
    bindingOffsets.sampler         = libraryDesc->spirvBindingOffsets.samplerOffset;
    bindingOffsets.constantBuffer  = libraryDesc->spirvBindingOffsets.constantBufferOffset;
    bindingOffsets.unorderedAccess = libraryDesc->spirvBindingOffsets.storageTextureAndBufferOffset;

    // -------------------------------------------------------------------------
    // Shared binding layout 0: constant buffer + samplers
    // -------------------------------------------------------------------------
    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility     = nvrhi::ShaderType::Compute;
        layoutDesc.bindingOffsets = bindingOffsets;
        layoutDesc.registerSpace  = instanceDesc->constantBufferAndSamplersSpaceIndex;

        layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(instanceDesc->constantBufferRegisterIndex));

        for (uint32_t i = 0; i < instanceDesc->samplersNum; ++i)
        {
            layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(instanceDesc->samplersBaseRegisterIndex + i));
        }

        m_BindingLayout0 = device->createBindingLayout(layoutDesc);
        if (!m_BindingLayout0)
        {
            SDL_assert(false && "NrdIntegration: failed to create CB+samplers binding layout");
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // Per-pipeline resources: create shader, per-pipeline binding layout, pipeline
    // -------------------------------------------------------------------------
    for (uint32_t p = 0; p < instanceDesc->pipelinesNum; ++p)
    {
        NrdPipeline pipeline;
        const nrd::PipelineDesc& nrdPipeline = instanceDesc->pipelines[p];

        // Build shader cache key from NRD shader identifier.
        // NRD format:  "FileName.cs.hlsl|KEY1=VAL1|KEY2=VAL2"
        // Cache key:   "FileName.cs_main"                       (no permutations)
        //              "FileName.cs_main_KEY1=VAL1_KEY2=VAL2"   (with sorted permutations)
        const std::string shaderKey = [&]() {
            std::string identStr = nrdPipeline.shaderIdentifier;

            // Strip ".hlsl" suffix to get the stem (e.g. "REBLUR_ClassifyTiles.cs")
            const size_t hlslPos = identStr.find(".hlsl");
            std::string stem = (hlslPos != std::string::npos) ? identStr.substr(0, hlslPos) : identStr;

            // All NRD shaders use "main" as their entry point
            std::string key = stem + "_main";

            // Parse pipe-delimited permutations (e.g., "KEY1=VAL1", "KEY2=VAL2", ...)
            std::vector<std::string> permutations;
            if (hlslPos != std::string::npos)
            {
                std::string remainder = identStr.substr(hlslPos + 5); // skip ".hlsl"
                while (remainder.find('|') != std::string::npos)
                {
                    remainder = remainder.substr(remainder.find('|') + 1);
                    const size_t nextPipe = remainder.find('|');
                    permutations.push_back(remainder.substr(0, nextPipe));
                }
            }

            // Sort permutations alphabetically to match std::map iteration order (loaded shaders are stored in a map)
            std::sort(permutations.begin(), permutations.end());
            for (const std::string& perm : permutations)
                key += "_" + perm;

            return key;
        }();

        pipeline.Shader = renderer->GetShaderHandle(shaderKey);

        if (!pipeline.Shader)
        {
            SDL_assert(false && "NrdIntegration: failed to create NRD shader");
            return false;
        }

        // Per-pipeline binding layout 1: texture SRV/UAV resources
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility     = nvrhi::ShaderType::Compute;
        layoutDesc.bindingOffsets = bindingOffsets;
        layoutDesc.registerSpace  = instanceDesc->resourcesSpaceIndex;

        for (uint32_t rangeIdx = 0; rangeIdx < nrdPipeline.resourceRangesNum; ++rangeIdx)
        {
            const nrd::ResourceRangeDesc& range = nrdPipeline.resourceRanges[rangeIdx];

            for (uint32_t d = 0; d < range.descriptorsNum; ++d)
            {
                const uint32_t slot = instanceDesc->resourcesBaseRegisterIndex + d;

                if (range.descriptorType == nrd::DescriptorType::TEXTURE)
                {
                    layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
                }
                else
                {
                    layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(slot));
                }
            }
        }

        pipeline.BindingLayout1 = device->createBindingLayout(layoutDesc);
        if (!pipeline.BindingLayout1)
        {
            SDL_assert(false && "NrdIntegration: failed to create resource binding layout");
            return false;
        }

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { m_BindingLayout0, pipeline.BindingLayout1 };
        pipelineDesc.CS             = pipeline.Shader;
        pipeline.Pipeline = device->createComputePipeline(pipelineDesc);
        if (!pipeline.Pipeline)
        {
            SDL_assert(false && "NrdIntegration: failed to create compute pipeline");
            return false;
        }

        m_Pipelines.push_back(std::move(pipeline));
    }

    // -------------------------------------------------------------------------
    // Permanent and transient pool textures
    // -------------------------------------------------------------------------
    const uint32_t totalPoolSize = instanceDesc->permanentPoolSize + instanceDesc->transientPoolSize;
    for (uint32_t i = 0; i < totalPoolSize; ++i)
    {
        const bool isPermanent = (i < instanceDesc->permanentPoolSize);
        const uint32_t poolIdx = isPermanent ? i : (i - instanceDesc->permanentPoolSize);

        const nrd::TextureDesc& nrdTex = isPermanent
            ? instanceDesc->permanentPool[poolIdx]
            : instanceDesc->transientPool[poolIdx];

        const nvrhi::Format fmt = GetNvrhiFormat(nrdTex.format);
        if (fmt == nvrhi::Format::UNKNOWN)
        {
            SDL_assert(false && "NrdIntegration: unsupported NRD texture format");
            return false;
        }

        char name[64];
        snprintf(name, sizeof(name), "NRD %s [%u]",
                 isPermanent ? "Permanent" : "Transient", poolIdx);

        nvrhi::TextureDesc texDesc;
        texDesc.width         = (width  + nrdTex.downsampleFactor - 1u) / nrdTex.downsampleFactor;
        texDesc.height        = (height + nrdTex.downsampleFactor - 1u) / nrdTex.downsampleFactor;
        texDesc.format        = fmt;
        texDesc.mipLevels     = 1;
        texDesc.dimension     = nvrhi::TextureDimension::Texture2D;
        texDesc.initialState  = nvrhi::ResourceStates::ShaderResource;
        texDesc.keepInitialState = true;
        texDesc.isUAV         = true;
        texDesc.debugName     = name;

        nvrhi::TextureHandle tex = device->createTexture(texDesc);
        if (!tex)
        {
            SDL_assert(false && "NrdIntegration: failed to create pool texture");
            return false;
        }

        if (isPermanent)
            m_PermanentTextures.push_back(std::move(tex));
        else
            m_TransientTextures.push_back(std::move(tex));
    }

    SDL_Log("[NrdIntegration] Initialized: %u permanent, %u transient pool textures, %u pipelines",
            instanceDesc->permanentPoolSize, instanceDesc->transientPoolSize,
            instanceDesc->pipelinesNum);

    // -------------------------------------------------------------------------
    // Packed normal+roughness texture (R10G10B10A2_UNORM)
    // Written each frame by the PackNormalRoughness pre-pass.
    // -------------------------------------------------------------------------
    {
        nvrhi::TextureDesc packDesc;
        packDesc.width            = width;
        packDesc.height           = height;
        packDesc.format           = nvrhi::Format::R10G10B10A2_UNORM;
        packDesc.dimension        = nvrhi::TextureDimension::Texture2D;
        packDesc.initialState     = nvrhi::ResourceStates::ShaderResource;
        packDesc.keepInitialState = true;
        packDesc.isUAV            = true;
        packDesc.debugName        = "NRD PackedNormalRoughness";

        m_PackedNormalRoughnessTex = device->createTexture(packDesc);
        if (!m_PackedNormalRoughnessTex)
        {
            SDL_assert(false && "NrdIntegration: failed to create packed normal texture");
            return false;
        }
    }

    m_Initialized = true;
    return true;
}

// ============================================================================
// ResolveResource
// ============================================================================

nvrhi::ITexture* NrdIntegration::ResolveResource(
    nrd::ResourceType type, uint16_t indexInPool,
    nvrhi::ITexture* packedNormals,
    nvrhi::ITexture* diffuse,  nvrhi::ITexture* specular,
    nvrhi::ITexture* viewZ,    nvrhi::ITexture* motionVectors,
    nvrhi::ITexture* outDiffuse, nvrhi::ITexture* outSpecular) const
{
    switch (type)
    {
    case nrd::ResourceType::TRANSIENT_POOL:
        SDL_assert(indexInPool < static_cast<uint16_t>(m_TransientTextures.size()));
        return m_TransientTextures[indexInPool];

    case nrd::ResourceType::PERMANENT_POOL:
        SDL_assert(indexInPool < static_cast<uint16_t>(m_PermanentTextures.size()));
        return m_PermanentTextures[indexInPool];

    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:        return packedNormals;
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:   return diffuse;
    case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:   return specular;
    case nrd::ResourceType::IN_VIEWZ:                   return viewZ;
    case nrd::ResourceType::IN_MV:                      return motionVectors;
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:  return outDiffuse;
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:  return outSpecular;

    default:
        SDL_Log("[NrdIntegration] WARNING: unhandled NRD resource type %s",
                nrd::GetResourceTypeString(type));
        SDL_assert(false && "Unhandled NRD resource type");
        return nullptr;
    }
}

// ============================================================================
// RunDenoiserPasses
// ============================================================================

void NrdIntegration::RunDenoiserPasses(
    nvrhi::ICommandList*       commandList,
    nvrhi::ITexture*           gbufferNormals,
    nvrhi::ITexture*           gbufferORM,
    nvrhi::ITexture*           diffuseRadiance,
    nvrhi::ITexture*           specularRadiance,
    nvrhi::ITexture*           viewZ,
    nvrhi::ITexture*           motionVectors,
    nvrhi::ITexture*           outDiffuse,
    nvrhi::ITexture*           outSpecular,
    const nrd::CommonSettings& commonSettings,
    const void*                denoiserSettings)
{
    SDL_assert(m_Initialized && "NrdIntegration::RunDenoiserPasses called before Initialize");

    nvrhi::utils::ScopedMarker marker{ commandList, "NRD Denoise" };

    Renderer* renderer = Renderer::GetInstance();
    nvrhi::DeviceHandle device = renderer->m_RHI->m_NvrhiDevice;
    const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
    const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

    // -------------------------------------------------------------------------
    // PackNormalRoughness pre-pass
    // Converts GBuffer oct-encoded normals (RG16_FLOAT) + roughness (from ORM .r)
    // into R10G10B10A2_UNORM as expected by NRD's IN_NORMAL_ROUGHNESS slot.
    // -------------------------------------------------------------------------
    {
        nvrhi::utils::ScopedMarker packMarker{ commandList, "NRD: Pack Normal+Roughness" };

        const Vector2U resolution{ width, height };

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(resolution)),
            nvrhi::BindingSetItem::Texture_SRV(0, gbufferNormals),
            nvrhi::BindingSetItem::Texture_SRV(1, gbufferORM),
            nvrhi::BindingSetItem::Texture_UAV(0, m_PackedNormalRoughnessTex),
        };

        Renderer::RenderPassParams passParams{};
        passParams.commandList       = commandList;
        passParams.shaderName        = "PackNormalRoughness_CSMain";
        passParams.bindingSetDesc    = bindingSetDesc;
        passParams.pushConstants     = &resolution;
        passParams.pushConstantsSize = sizeof(resolution);
        passParams.dispatchParams.x  = DivideAndRoundUp(width,  8u);
        passParams.dispatchParams.y  = DivideAndRoundUp(height, 8u);
        passParams.dispatchParams.z  = 1u;

        renderer->AddComputePass(passParams);
    }

    // -------------------------------------------------------------------------
    // Configure NRD denoiser settings
    // -------------------------------------------------------------------------
    constexpr nrd::Identifier kID = 0;

    if (denoiserSettings)
        NRD_CHECK(nrd::SetDenoiserSettings(*m_Instance, kID, denoiserSettings));

    NRD_CHECK(nrd::SetCommonSettings(*m_Instance, commonSettings));

    // -------------------------------------------------------------------------
    // Retrieve NRD dispatch list
    // -------------------------------------------------------------------------
    const nrd::DispatchDesc* dispatchDescs  = nullptr;
    uint32_t                 dispatchDescNum = 0;
    NRD_CHECK(nrd::GetComputeDispatches(*m_Instance, &kID, 1, dispatchDescs, dispatchDescNum));

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_Instance);

    // -------------------------------------------------------------------------
    // Submit each NRD dispatch
    // -------------------------------------------------------------------------
    for (uint32_t dispatchIdx = 0; dispatchIdx < dispatchDescNum; ++dispatchIdx)
    {
        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[dispatchIdx];
        const nrd::PipelineDesc& pipelineDesc = instanceDesc->pipelines[dispatchDesc.pipelineIndex];

        nvrhi::utils::ScopedMarker dispatchMarker{ commandList, dispatchDesc.name ? dispatchDesc.name : "NRD Dispatch" };

        // Write constant buffer data for this dispatch
        SDL_assert(m_ConstantBuffer);
        if (dispatchDesc.constantBufferDataSize > 0)
        {
            commandList->writeBuffer(m_ConstantBuffer, dispatchDesc.constantBufferData, dispatchDesc.constantBufferDataSize);
        }
        else
        {
            // NRD may dispatch compute shaders that don't need constant buffer data — in that case, write zeros
            std::vector<uint8_t> zeroData(instanceDesc->constantBufferMaxDataSize, 0);
            commandList->writeBuffer(m_ConstantBuffer, zeroData.data(), zeroData.size());
        }

        // Build binding set 0: constant buffer + samplers (shared layout)
        nvrhi::BindingSetDesc setDesc0;
        setDesc0.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(instanceDesc->constantBufferRegisterIndex, m_ConstantBuffer));
        
        for (uint32_t i = 0; i < instanceDesc->samplersNum; ++i)
        {
            setDesc0.bindings.push_back(nvrhi::BindingSetItem::Sampler(instanceDesc->samplersBaseRegisterIndex + i, m_Samplers[i]));
        }

        // Build binding set 1: per-dispatch texture resources (per-pipeline layout)
        nvrhi::BindingSetDesc setDesc1;
        uint32_t resourceIndex = 0;
        for (uint32_t rangeIdx = 0; rangeIdx < pipelineDesc.resourceRangesNum; ++rangeIdx)
        {
            const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[rangeIdx];

            for (uint32_t d = 0; d < range.descriptorsNum; ++d)
            {
                SDL_assert(resourceIndex < dispatchDesc.resourcesNum);
                const nrd::ResourceDesc& res = dispatchDesc.resources[resourceIndex++];

                nvrhi::ITexture* texture = ResolveResource(
                    res.type, res.indexInPool,
                    m_PackedNormalRoughnessTex,
                    diffuseRadiance, specularRadiance,
                    viewZ, motionVectors,
                    outDiffuse, outSpecular);

                if (!texture)
                {
                    SDL_Log("[NrdIntegration] ERROR: unresolved resource (type=%s, pool=%u) in dispatch '%s'",
                            nrd::GetResourceTypeString(res.type), res.indexInPool,
                            dispatchDesc.name ? dispatchDesc.name : "?");
                    SDL_assert(false && "NrdIntegration: unresolved NRD resource");
                }
                
                if (range.descriptorType == nrd::DescriptorType::TEXTURE)
                {
                    setDesc1.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(instanceDesc->resourcesBaseRegisterIndex + d, texture));
                }
                else
                {
                    setDesc1.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(instanceDesc->resourcesBaseRegisterIndex + d, texture));
                }
            }
        }

        const NrdPipeline& pipeline = m_Pipelines[dispatchDesc.pipelineIndex];

        nvrhi::BindingSetHandle bindingSet0 = device->createBindingSet(setDesc0, m_BindingLayout0);
        nvrhi::BindingSetHandle bindingSet1 = device->createBindingSet(setDesc1, pipeline.BindingLayout1);

        SDL_assert(bindingSet0 && bindingSet1);

        nvrhi::ComputeState state;
        state.bindings = { bindingSet0, bindingSet1 };
        state.pipeline = pipeline.Pipeline;

        commandList->setComputeState(state);
        commandList->dispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight);
    }
}
