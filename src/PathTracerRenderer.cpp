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
        
        
        RGTextureDesc desc;
        desc.m_NvrhiDesc.width = g_Renderer.m_RHI->m_SwapchainExtent.x;
        desc.m_NvrhiDesc.height = g_Renderer.m_RHI->m_SwapchainExtent.y;
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
        

        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);
        nvrhi::TextureHandle accumBuffer = renderGraph.GetTexture(m_AccumulationBuffer, RGResourceAccessMode::Write);
        const nvrhi::TextureDesc& hdrDesc = hdrColor->getDesc();

        // Camera change detection
        bool reset = false;
        if (memcmp(&g_Renderer.m_Scene.m_View.m_MatWorldToClipNoOffset, &g_Renderer.m_Scene.m_ViewPrev.m_MatWorldToClipNoOffset, sizeof(Matrix)) != 0)
        {
            reset = true;
        }

        if (reset)
        {
            m_AccumulationIndex = 0;
        }

        // Pause animations
        g_Renderer.m_EnableAnimations = false;

        const nvrhi::BufferDesc pathTracerCBD = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::PathTracerConstants), "PathTracerCB", 1);
        const nvrhi::BufferHandle pathTracerCB = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(pathTracerCBD);

        srrhi::PathTracerConstants cb;
        cb.SetView(g_Renderer.m_Scene.m_View);
        cb.SetCameraPos(DirectX::XMFLOAT4{ g_Renderer.m_Scene.m_Camera.GetPosition().x, g_Renderer.m_Scene.m_Camera.GetPosition().y, g_Renderer.m_Scene.m_Camera.GetPosition().z, 1.0f });
        cb.SetLightCount(g_Renderer.m_Scene.m_LightCount);
        cb.SetAccumulationIndex(m_AccumulationIndex);
        cb.SetFrameIndex(g_Renderer.m_FrameNumber);
        cb.SetMaxBounces(g_Renderer.m_PathTracerMaxBounces);
        cb.SetJitter(DirectX::XMFLOAT2{ Halton(m_AccumulationIndex + 1, 2) - 0.5f, Halton(m_AccumulationIndex + 1, 3) - 0.5f });
        cb.SetSunDirection(g_Renderer.m_Scene.GetSunDirection());
        {
            // Sun angular radius for soft shadows — use the directional light's m_AngularSize field.
            // the last light is guaranteed to be a directional light (ensured by SortLightsAddDefaultDirectionalLight).
            const float angularSizeDeg = !g_Renderer.m_Scene.m_Lights.empty()
                ? g_Renderer.m_Scene.m_Lights.back().m_AngularSize
                : 0.533f; // fallback to real sun size in degrees
            const float halfAngleRad = angularSizeDeg * 0.5f * (DirectX::XM_PI / 180.0f);
            cb.SetCosSunAngularRadius(cosf(halfAngleRad));
        }

        commandList->writeBuffer(pathTracerCB, &cb, sizeof(cb), 0);

        srrhi::PathTracerInputs ptInputs;
        ptInputs.SetPathTracerCB(pathTracerCB);
        ptInputs.SetSceneAS(g_Renderer.m_Scene.m_TLAS);
        ptInputs.SetLights(g_Renderer.m_Scene.m_LightBuffer);
        ptInputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
        ptInputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
        ptInputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
        ptInputs.SetIndices(g_Renderer.m_Scene.m_IndexBuffer);
        ptInputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
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

        g_Renderer.AddComputePass(params);

        m_AccumulationIndex++;
    }

    const char* GetName() const override { return "ReferencePathTracer"; }
};

REGISTER_RENDERER(PathTracerRenderer);
