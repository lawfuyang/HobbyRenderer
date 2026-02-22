#include "Renderer.h"
#include "Scene.h"
#include "Camera.h"
#include "Config.h"
#include "Utilities.h"
#include "CommonResources.h"
#include "shaders/ShaderShared.h"

RGTextureHandle g_RG_SkyVisibility;
RGTextureHandle g_RG_HistorySkyVisibility;
extern RGTextureHandle g_RG_DepthTexture;

class VolumetricSkyVisibilityRenderer : public IRenderer
{
    const uint32_t kPixelsPerFroxel = 100; // TODO: expose to imgui?
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        if (!renderer->m_EnableSky || renderer->m_Mode == RenderingMode::ReferencePathTracer) return false;

        uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        uint32_t resX = DivideAndRoundUp(width, kPixelsPerFroxel);
        uint32_t resY = DivideAndRoundUp(height, kPixelsPerFroxel);
        uint32_t resZ = (uint32_t)renderer->m_SkyVisibilityZCount;

        RGTextureDesc desc;
        desc.m_NvrhiDesc.width = resX;
        desc.m_NvrhiDesc.height = resY;
        desc.m_NvrhiDesc.depth = resZ;
        desc.m_NvrhiDesc.format = nvrhi::Format::R8_UNORM;
        desc.m_NvrhiDesc.dimension = nvrhi::TextureDimension::Texture3D;
        desc.m_NvrhiDesc.isUAV = true;
        desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;

        desc.m_NvrhiDesc.debugName = "SkyVisibility_Main";
        renderGraph.DeclareTexture(desc, g_RG_SkyVisibility);

        desc.m_NvrhiDesc.debugName = "SkyVisibility_History";
        renderGraph.DeclarePersistentTexture(desc, g_RG_HistorySkyVisibility);

        renderGraph.ReadTexture(g_RG_DepthTexture);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        Scene& scene = renderer->m_Scene;

        nvrhi::utils::ScopedMarker marker(commandList, "Volumetric Sky Visibility");

        uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;
        uint32_t resX = DivideAndRoundUp(width, kPixelsPerFroxel);  
        uint32_t resY = DivideAndRoundUp(height, kPixelsPerFroxel); 
        uint32_t resZ = (uint32_t)renderer->m_SkyVisibilityZCount;

        nvrhi::TextureHandle mainTex = renderGraph.GetTexture(g_RG_SkyVisibility, RGResourceAccessMode::Write);
        nvrhi::TextureHandle historyTex = renderGraph.GetTexture(g_RG_HistorySkyVisibility, RGResourceAccessMode::Read);
        nvrhi::TextureHandle depthBuffer = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);

        float farPlane = scene.GetSceneBoundingRadius() * 0.5f;

        VolumetricSkyVisibilityConstants consts{};
        consts.m_View = renderer->m_Scene.m_View;
        consts.m_PrevView = renderer->m_Scene.m_ViewPrev;
        consts.m_CameraPos = Vector4{ renderer->m_Scene.m_Camera.GetPosition().x, renderer->m_Scene.m_Camera.GetPosition().y, renderer->m_Scene.m_Camera.GetPosition().z, 1.0f };
        consts.m_SunDirection = scene.m_SunDirection;
        consts.m_ResolutionX = resX;
        consts.m_ResolutionY = resY;
        consts.m_ResolutionZ = resZ;
        consts.m_RaysPerFroxel = (uint32_t)renderer->m_SkyVisibilityRays;
        consts.m_FrameIndex = renderer->m_FrameNumber;
        consts.m_SkyVisibilityFar = farPlane;
        consts.m_SkyVisibilityGridZParams = CalculateGridZParams(0.1f, farPlane, 1.0f, renderer->m_SkyVisibilityZCount);
        consts.m_InvDeviceZToWorldZTransform = CreateInvDeviceZToWorldZTransform(renderer->m_Scene.m_Camera.GetProjMatrix());

        const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(consts), "SkyVisibility_Constants", 1);
        nvrhi::BufferHandle cb = renderer->m_RHI->m_NvrhiDevice->createBuffer(cbDesc);
        commandList->writeBuffer(cb, &consts, sizeof(consts));

        nvrhi::BindingSetDesc bsetDesc;
        bsetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, cb),
            nvrhi::BindingSetItem::Texture_UAV(0, mainTex),
            nvrhi::BindingSetItem::Texture_SRV(0, historyTex),
            nvrhi::BindingSetItem::Texture_SRV(1, depthBuffer),
            nvrhi::BindingSetItem::RayTracingAccelStruct(2, scene.m_TLAS),
            nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().LinearClamp),
            nvrhi::BindingSetItem::Sampler(1, CommonResources::GetInstance().PointClamp),
        };

        // 1. Visibility Pass
        {
            Renderer::RenderPassParams params;
            params.commandList = commandList;
            params.shaderName = "VolumetricSkyVisibility_VisibilityCS";
            params.bindingSetDesc = bsetDesc;
            params.dispatchParams = { DivideAndRoundUp(resX, 4), DivideAndRoundUp(resY, 4), DivideAndRoundUp(resZ, 4) };
            renderer->AddComputePass(params);
        }

        // 2. Accumulation Pass
        if (renderer->m_EnableSkyVisibilityTemporal)
        {
            Renderer::RenderPassParams params;
            params.commandList = commandList;
            params.shaderName = "VolumetricSkyVisibility_TemporalCS";
            params.bindingSetDesc = bsetDesc;
            params.dispatchParams = { DivideAndRoundUp(resX, 4), DivideAndRoundUp(resY, 4), DivideAndRoundUp(resZ, 4) };
            renderer->AddComputePass(params);
        }

        commandList->copyTexture(historyTex, nvrhi::TextureSlice{}, mainTex, nvrhi::TextureSlice{});
    }

    const char* GetName() const override { return "Volumetric Sky Visibility"; }
};

REGISTER_RENDERER(VolumetricSkyVisibilityRenderer);
