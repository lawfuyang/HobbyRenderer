#include "pch.h"
#include "ImGuiLayer.h"
#include "Renderer.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

bool ImGuiLayer::Initialize(SDL_Window* window)
{
    m_Window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(m_Window))
    {
        SDL_Log("[Init] Failed to initialize ImGui SDL3 backend");
        SDL_assert(false && "ImGui_ImplSDL3_InitForVulkan failed");
        return false;
    }

    if (!CreateDeviceObjects())
    {
        SDL_Log("[Init] Failed to create ImGui device objects");
        return false;
    }

    SDL_Log("[Init] ImGui initialized successfully");
    return true;
}

void ImGuiLayer::Shutdown()
{
    SDL_Log("[Shutdown] Shutting down ImGui");
    DestroyDeviceObjects();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool ImGuiLayer::CreateDeviceObjects()
{
    SDL_Log("[Init] Creating ImGui device objects");
    Renderer* renderer = Renderer::GetInstance();

    // Create sampler for font texture (bilinear sampling)
    {
        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllFilters(true); // Linear filtering
        samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
        m_FontSampler = renderer->m_NvrhiDevice->createSampler(samplerDesc);
        
        if (!m_FontSampler)
        {
            SDL_Log("[Error] Failed to create ImGui font sampler");
            return false;
        }
    }

    // Create input layout (vertex attributes)
    {
        nvrhi::VertexAttributeDesc attributes[] = {
            nvrhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(ImDrawVert, pos))
                .setBufferIndex(0),
            nvrhi::VertexAttributeDesc()
                .setName("TEXCOORD")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(ImDrawVert, uv))
                .setBufferIndex(0),
            nvrhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(nvrhi::Format::RGBA8_UNORM)
                .setOffset(offsetof(ImDrawVert, col))
                .setBufferIndex(0)
        };

        // Note: vertexShader parameter is only used by DX11 backend, unused in Vulkan
        m_InputLayout = renderer->m_NvrhiDevice->createInputLayout(attributes, 3, nullptr);
        
        if (!m_InputLayout)
        {
            SDL_Log("[Error] Failed to create ImGui input layout");
            return false;
        }
    }

    // Create a single binding layout that includes push constants (VS) and texture/sampler (PS)
    {
        nvrhi::BindingLayoutDesc descriptorLayoutDesc;
        descriptorLayoutDesc.visibility = nvrhi::ShaderType::All; // covers VS (push constants) and PS (texture/sampler)
        descriptorLayoutDesc.bindingOffsets.shaderResource = Renderer::SPIRV_TEXTURE_SHIFT; // matches ShaderMake tRegShift default
        descriptorLayoutDesc.bindingOffsets.sampler = Renderer::SPIRV_SAMPLER_SHIFT;        // matches ShaderMake sRegShift default
        descriptorLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float) * 4), // push constants for VS
            nvrhi::BindingLayoutItem::Texture_SRV(0),   // logical slot -> SPIR-V binding shift
            nvrhi::BindingLayoutItem::Sampler(0)        // logical slot -> SPIR-V binding shift
        };

        m_BindingLayout = renderer->m_NvrhiDevice->createBindingLayout(descriptorLayoutDesc);
        
        if (!m_BindingLayout)
        {
            SDL_Log("[Error] Failed to create ImGui binding layout");
            return false;
        }
    }

    // Create font texture
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = width;
        textureDesc.height = height;
        textureDesc.format = nvrhi::Format::RGBA8_UNORM;
        textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
        textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        textureDesc.keepInitialState = true;
        textureDesc.isRenderTarget = false;
        textureDesc.isUAV = false;
        textureDesc.debugName = "ImGui Font Texture";

        m_FontTexture = renderer->m_NvrhiDevice->createTexture(textureDesc);
        
        if (!m_FontTexture)
        {
            SDL_Log("[Error] Failed to create ImGui font texture");
            return false;
        }

        // Upload font texture data
        nvrhi::CommandListHandle commandList = renderer->AcquireCommandList();
        commandList->writeTexture(m_FontTexture, 0, 0, pixels, width * 4);
        renderer->SubmitCommandList(commandList);
        renderer->ExecutePendingCommandLists();
        renderer->ReleaseCommandList(commandList);
    }

    // Create graphics pipeline
    {
        Renderer* renderer = Renderer::GetInstance();
        
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.VS = renderer->GetShaderHandle("imgui_VSMain");
        pipelineDesc.PS = renderer->GetShaderHandle("imgui_PSMain");
        pipelineDesc.inputLayout = m_InputLayout;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;

        // Render state
        nvrhi::RasterState& rasterState = pipelineDesc.renderState.rasterState;
        rasterState.cullMode = nvrhi::RasterCullMode::None;
        rasterState.frontCounterClockwise = false;

        // Blend state - alpha blending for ImGui
        nvrhi::BlendState::RenderTarget& blendState = pipelineDesc.renderState.blendState.targets[0];
        blendState.blendEnable = true;
        blendState.srcBlend = nvrhi::BlendFactor::SrcAlpha;
        blendState.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
        blendState.blendOp = nvrhi::BlendOp::Add;
        blendState.srcBlendAlpha = nvrhi::BlendFactor::One;
        blendState.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
        blendState.blendOpAlpha = nvrhi::BlendOp::Add;

        // Depth stencil state - no depth testing for ImGui
        nvrhi::DepthStencilState& depthState = pipelineDesc.renderState.depthStencilState;
        depthState.depthTestEnable = false;
        depthState.depthWriteEnable = false;
        depthState.stencilEnable = false;

        // Framebuffer info - rendering to swapchain
        nvrhi::FramebufferInfoEx fbInfo;
        fbInfo.colorFormats = { renderer->m_RHI.VkFormatToNvrhiFormat(renderer->m_RHI.m_SwapchainFormat) };

        m_Pipeline = renderer->m_NvrhiDevice->createGraphicsPipeline(pipelineDesc, fbInfo);
        
        if (!m_Pipeline)
        {
            SDL_Log("[Error] Failed to create ImGui graphics pipeline");
            return false;
        }
    }

    SDL_Log("[Init] ImGui device objects created successfully");
    return true;
}

void ImGuiLayer::DestroyDeviceObjects()
{
    // Release all GPU resources
    m_Pipeline = nullptr;
    m_BindingLayout = nullptr;
    m_InputLayout = nullptr;
    m_IndexBuffer = nullptr;
    m_VertexBuffer = nullptr;
    m_FontTexture = nullptr;
    m_FontSampler = nullptr;

    m_VertexBufferSize = 0;
    m_IndexBufferSize = 0;
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::RenderFrame()
{
    return; // TODO
    
    Renderer* renderer = Renderer::GetInstance();

    const double fps = renderer->m_FPS;
    const double frameTime = renderer->m_FrameTime;

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Frame Time: %.3f ms", frameTime);
        ImGui::EndMainMenuBar();
    }

    if (ImGui::Begin("Property Grid"))
    {
        static bool s_ShowDemoWindow = false;
        if (ImGui::Checkbox("Show Demo Window", &s_ShowDemoWindow))
        {
            ImGui::ShowDemoWindow();
        }

        ImGui::End();
    }

    ImGui::Render();
}
