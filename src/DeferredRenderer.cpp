#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "shaders/ShaderShared.h"

class DeferredRenderer : public IRenderer
{
public:
    void Initialize() override {}
    void Render(nvrhi::CommandListHandle commandList) override
    {
        PROFILE_FUNCTION();
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::utils::ScopedMarker marker(commandList, "Deferred Lighting");

        nvrhi::BindingSetDesc bset;
        
        const Vector3 camPos = renderer->m_Camera.GetPosition();

        // Deferred CB
        const nvrhi::BufferDesc deferredCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(DeferredLightingConstants), "DeferredCB", 1);
        const nvrhi::BufferHandle deferredCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(deferredCBD);

        DeferredLightingConstants dcb{};
        dcb.m_InvViewProj = renderer->m_Camera.GetInvViewProjMatrix();
        dcb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 1.0f };
        dcb.m_LightDirection = renderer->m_Scene.GetDirectionalLightDirection();
        dcb.m_LightIntensity = renderer->m_Scene.m_DirectionalLight.intensity / 10000.0f;
        dcb.m_EnableRTShadows = renderer->m_EnableRTShadows ? 1 : 0;
        dcb.m_DebugMode = renderer->m_DebugMode;
        commandList->writeBuffer(deferredCB, &dcb, sizeof(dcb), 0);

        bset.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(1, deferredCB),
            nvrhi::BindingSetItem::Texture_SRV(0, renderer->m_GBufferAlbedo),
            nvrhi::BindingSetItem::Texture_SRV(1, renderer->m_GBufferNormals),
            nvrhi::BindingSetItem::Texture_SRV(2, renderer->m_GBufferORM),
            nvrhi::BindingSetItem::Texture_SRV(3, renderer->m_GBufferEmissive),
            nvrhi::BindingSetItem::Texture_SRV(4, renderer->m_DepthTexture),
            nvrhi::BindingSetItem::RayTracingAccelStruct(5, renderer->m_Scene.m_TLAS),
            nvrhi::BindingSetItem::Texture_UAV(0, renderer->m_HDRColorTexture)
        };

        const uint32_t dispatchX = DivideAndRoundUp(renderer->m_RHI->m_SwapchainExtent.x, 8);
        const uint32_t dispatchY = DivideAndRoundUp(renderer->m_RHI->m_SwapchainExtent.y, 8);

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderName = "DeferredLighting_DeferredLighting_CSMain",
            .bindingSetDesc = bset,
            .dispatchParams = { .x = dispatchX, .y = dispatchY, .z = 1 }
        };

        renderer->AddComputePass(params);
    }

    const char* GetName() const override { return "Deferred"; }
};

REGISTER_RENDERER(DeferredRenderer);
