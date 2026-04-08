#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "shaders/srrhi/cpp/PathTracer.h"

extern RGTextureHandle g_RG_HDRColor;

class PathTracerRenderer : public IRenderer
{
    RGTextureHandle m_AccumulationBuffer;
    uint32_t m_AccumulationIndex = 0;

public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        
        RGTextureDesc desc;
        desc.m_NvrhiDesc.width = renderer->m_RHI->m_SwapchainExtent.x;
        desc.m_NvrhiDesc.height = renderer->m_RHI->m_SwapchainExtent.y;
        desc.m_NvrhiDesc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.m_NvrhiDesc.isUAV = true;
        desc.m_NvrhiDesc.debugName = "AccumulationBuffer";
        desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        renderGraph.DeclarePersistentTexture(desc, m_AccumulationBuffer);

        renderGraph.WriteTexture(g_RG_HDRColor);
        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        Renderer* renderer = Renderer::GetInstance();

        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);
        nvrhi::TextureHandle accumBuffer = renderGraph.GetTexture(m_AccumulationBuffer, RGResourceAccessMode::Write);
        const nvrhi::TextureDesc& hdrDesc = hdrColor->getDesc();

        // Camera change detection
        bool reset = false;
        if (memcmp(&renderer->m_Scene.m_View.m_MatWorldToClipNoOffset, &renderer->m_Scene.m_ViewPrev.m_MatWorldToClipNoOffset, sizeof(Matrix)) != 0)
        {
            reset = true;
        }

        if (reset)
        {
            m_AccumulationIndex = 0;
        }

        // Pause animations
        renderer->m_EnableAnimations = false;

        const nvrhi::BufferDesc pathTracerCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::PathTracerConstants), "PathTracerCB", 1);
        const nvrhi::BufferHandle pathTracerCB = renderer->m_RHI->m_NvrhiDevice->createBuffer(pathTracerCBD);

        srrhi::PathTracerConstants cb;
        cb.SetView(renderer->m_Scene.m_View);
        cb.SetCameraPos(DirectX::XMFLOAT4{ renderer->m_Scene.m_Camera.GetPosition().x, renderer->m_Scene.m_Camera.GetPosition().y, renderer->m_Scene.m_Camera.GetPosition().z, 1.0f });
        cb.SetLightCount(renderer->m_Scene.m_LightCount);
        cb.SetAccumulationIndex(m_AccumulationIndex);
        cb.SetFrameIndex(renderer->m_FrameNumber);
        cb.SetMaxBounces(renderer->m_PathTracerMaxBounces);
        cb.SetJitter(DirectX::XMFLOAT2{ Halton(m_AccumulationIndex + 1, 2) - 0.5f, Halton(m_AccumulationIndex + 1, 3) - 0.5f });
        cb.SetSunDirection(renderer->m_Scene.GetSunDirection());
        {
            // Sun angular radius for soft shadows — use the directional light's m_AngularSize field.
            // the last light is guaranteed to be a directional light (ensured by SortLightsAddDefaultDirectionalLight).
            const float angularSizeDeg = !renderer->m_Scene.m_Lights.empty()
                ? renderer->m_Scene.m_Lights.back().m_AngularSize
                : 0.533f; // fallback to real sun size in degrees
            const float halfAngleRad = angularSizeDeg * 0.5f * (DirectX::XM_PI / 180.0f);
            cb.SetCosSunAngularRadius(cosf(halfAngleRad));
        }

        commandList->writeBuffer(pathTracerCB, &cb, sizeof(cb), 0);

        srrhi::PathTracerInputs ptInputs;
        ptInputs.SetPathTracerCB(pathTracerCB);
        ptInputs.SetSceneAS(renderer->m_Scene.m_TLAS);
        ptInputs.SetLights(renderer->m_Scene.m_LightBuffer);
        ptInputs.SetInstances(renderer->m_Scene.m_InstanceDataBuffer);
        ptInputs.SetMeshData(renderer->m_Scene.m_MeshDataBuffer);
        ptInputs.SetMaterials(renderer->m_Scene.m_MaterialConstantsBuffer);
        ptInputs.SetIndices(renderer->m_Scene.m_IndexBuffer);
        ptInputs.SetVertices(renderer->m_Scene.m_VertexBufferQuantized);
        ptInputs.SetOutput(hdrColor, 0);
        ptInputs.SetAccumulation(accumBuffer, 0);
        nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(ptInputs);

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderID = ShaderID::PATHTRACER_PATHTRACER_CSMAIN_PATH_TRACER_MODE_1,
            .bindingSetDesc = bset,
            .dispatchParams = {
                .x = DivideAndRoundUp(hdrDesc.width, 8),
                .y = DivideAndRoundUp(hdrDesc.height, 8),
                .z = 1
            }
        };

        renderer->AddComputePass(params);

        m_AccumulationIndex++;
    }

    const char* GetName() const override { return "ReferencePathTracer"; }
};

REGISTER_RENDERER(PathTracerRenderer);
