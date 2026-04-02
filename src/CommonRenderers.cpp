#include "Renderer.h"
#include "Utilities.h"

#include "shaders/srrhi/cpp/TLASPatch.h"

// Global Render Graph Handles for Transient Resources
RGTextureHandle g_RG_DepthTexture;
RGTextureHandle g_RG_HZBTexture;
RGTextureHandle g_RG_GBufferAlbedo;
RGTextureHandle g_RG_GBufferNormals;
RGTextureHandle g_RG_GBufferGeoNormals;
RGTextureHandle g_RG_GBufferORM;
RGTextureHandle g_RG_GBufferEmissive;
RGTextureHandle g_RG_GBufferMotionVectors;
RGTextureHandle g_RG_HDRColor;
RGTextureHandle g_RG_ExposureTexture;

// ============================================================================
// Renderer Implementations
// ============================================================================

class ClearRenderer : public IRenderer
{
    bool m_NeedHZBClear = false;
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;
        
        // Declare transient depth texture
        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::DEPTH_FORMAT;
            desc.m_NvrhiDesc.isRenderTarget = true;
            desc.m_NvrhiDesc.debugName = "DepthBuffer_RG";
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::DepthWrite;
            desc.m_NvrhiDesc.keepInitialState = true;
            desc.m_NvrhiDesc.setClearValue(nvrhi::Color{ Renderer::DEPTH_FAR, 0.0f, 0.0f, 0.0f });
            
            renderGraph.DeclareTexture(desc, g_RG_DepthTexture);
        }

        // HDR Color Texture
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = renderer->m_Mode == RenderingMode::ReferencePathTracer ? Renderer::PATH_TRACER_HDR_COLOR_FORMAT : Renderer::HDR_COLOR_FORMAT;
            desc.m_NvrhiDesc.debugName = "HDRColorTexture_RG";
            desc.m_NvrhiDesc.isRenderTarget = true;
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            desc.m_NvrhiDesc.setClearValue(nvrhi::Color{});
            renderGraph.DeclareTexture(desc, g_RG_HDRColor);
        }
        
        // Declare transient GBuffer textures
        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            RGTextureDesc gbufferDesc;
            gbufferDesc.m_NvrhiDesc.width = width;
            gbufferDesc.m_NvrhiDesc.height = height;
            gbufferDesc.m_NvrhiDesc.isRenderTarget = true;
            gbufferDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            gbufferDesc.m_NvrhiDesc.keepInitialState = true;
            gbufferDesc.m_NvrhiDesc.setClearValue(nvrhi::Color{});
            
            // Albedo: RGBA8
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_ALBEDO_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferAlbedo_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferAlbedo);
            
            // Normals: RG16_FLOAT
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_NORMALS_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferNormals_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferNormals);

            // Geo Normals: RG16_FLOAT (geometric primitive normal, no normal map)
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_NORMALS_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferGeoNormals_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferGeoNormals);

            // ORM: RGBA8
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_ORM_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferORM_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferORM);
            
            // Emissive: RGBA8
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_EMISSIVE_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferEmissive_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferEmissive);
            
            // Motion Vectors: RG16_FLOAT
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_MOTION_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferMotion_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferMotionVectors);
        }

        // HZB Texture (Persistent)
        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            uint32_t sw = width;
            uint32_t sh = height;
            uint32_t hzbWidth = NextLowerPow2(sw);
            uint32_t hzbHeight = NextLowerPow2(sh);
            hzbWidth = hzbWidth > sw ? hzbWidth >> 1 : hzbWidth;
            hzbHeight = hzbHeight > sh ? hzbHeight >> 1 : hzbHeight;
            uint32_t maxDim = std::max(hzbWidth, hzbHeight);
            uint32_t mipLevels = 0;
            while (maxDim > 0) {
                mipLevels++;
                maxDim >>= 1;
            }

            RGTextureDesc hzbDesc;
            hzbDesc.m_NvrhiDesc.width = hzbWidth;
            hzbDesc.m_NvrhiDesc.height = hzbHeight;
            hzbDesc.m_NvrhiDesc.mipLevels = mipLevels;
            hzbDesc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
            hzbDesc.m_NvrhiDesc.debugName = "HZB";
            hzbDesc.m_NvrhiDesc.isUAV = true;
            hzbDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            
            m_NeedHZBClear = renderGraph.DeclarePersistentTexture(hzbDesc, g_RG_HZBTexture);
        }

        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = 1;
            desc.m_NvrhiDesc.height = 1;
            desc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
            desc.m_NvrhiDesc.debugName = "ExposureTexture_RG";
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            renderGraph.DeclarePersistentTexture(desc, g_RG_ExposureTexture);
        }

        return true;
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Get transient resources from render graph
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);
        commandList->clearTextureFloat(hdrColor, nvrhi::AllSubresources, nvrhi::Color{});

        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            nvrhi::TextureHandle depthTexture = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferAlbedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferNormals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferGeoNormals = renderGraph.GetTexture(g_RG_GBufferGeoNormals, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferORM = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferEmissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferMotion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

            // Clear depth for reversed-Z (clear to 0.0f) and stencil to 0
            commandList->clearDepthStencilTexture(depthTexture, nvrhi::AllSubresources, true, Renderer::DEPTH_FAR, true, 0);

            // clear gbuffers
            commandList->clearTextureFloat(gbufferAlbedo, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferNormals, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferGeoNormals, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferORM, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferEmissive, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferMotion, nvrhi::AllSubresources, nvrhi::Color{});

            if (m_NeedHZBClear)
            {
                nvrhi::TextureHandle hzbTexture = renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Write);
                commandList->clearTextureFloat(hzbTexture, nvrhi::AllSubresources, nvrhi::Color{ Renderer::DEPTH_FAR, 0.0f, 0.0f, 0.0f });
            }
        }
    }
    const char* GetName() const override { return "Clear"; }
};

class TLASRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        Scene& scene = renderer->m_Scene;

        if (!scene.m_TLAS || !scene.m_RTInstanceDescBuffer || scene.m_RTInstanceDescs.empty())
            return false;

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        Scene& scene = renderer->m_Scene;

        // ── TLAS Patch ────────────────────────────────────────────────────────
        // Dispatch TLASPatch_CS to write the correct per-LOD BLAS address and
        // m_LODIndex for each instance, using the LOD indices written by the
        // GPU culling passes into m_InstanceLODBuffer.
        if (scene.m_BLASAddressBuffer && scene.m_InstanceLODBuffer)
        {
            PROFILE_GPU_SCOPED("TLAS Patch", commandList);

            const uint32_t numInstances = (uint32_t)scene.m_InstanceData.size();
            const uint32_t dispatchX = DivideAndRoundUp(numInstances, 64u);

            srrhi::TLASPatchInputs inputs;
            inputs.m_PC.SetInstanceCount(numInstances);
            inputs.SetBLASAddresses(scene.m_BLASAddressBuffer);
            inputs.SetInstanceLOD(scene.m_InstanceLODBuffer);
            inputs.SetRTInstanceDescs(scene.m_RTInstanceDescBuffer);
            inputs.SetInstanceData(scene.m_InstanceDataBuffer);

            Renderer::RenderPassParams params;
            params.commandList           = commandList;
            params.shaderID              = ShaderID::TLASPATCH_TLASPATCH_CSMAIN;
            params.bindingSetDesc        = Renderer::CreateBindingSetDesc(inputs);
            params.bIncludeBindlessResources = false;
            params.pushConstants         = &inputs.m_PC;
            params.pushConstantsSize     = srrhi::TLASPatchInputs::PushConstantBytes;
            params.dispatchParams        = { .x = dispatchX, .y = 1, .z = 1 };
            renderer->AddComputePass(params);
        }

        // ── TLAS Build ────────────────────────────────────────────────────────
        // Always rebuild TLAS after TLASPatch_CS because LOD changes swap the
        // per-instance BLAS addresses every frame, even when transforms are
        // unchanged.  Without a rebuild the TLAS would keep referencing stale
        // BLAS pointers, causing primitive-index mismatches in RT shaders.
        {
            commandList->buildTopLevelAccelStructFromBuffer(
                scene.m_TLAS,
                scene.m_RTInstanceDescBuffer,
                0,
                (uint32_t)scene.m_RTInstanceDescs.size()
            );
        }
    }

    const char* GetName() const override { return "TLAS Update"; }
};

REGISTER_RENDERER(ClearRenderer);
REGISTER_RENDERER(TLASRenderer);
