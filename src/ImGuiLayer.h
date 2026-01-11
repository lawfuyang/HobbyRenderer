#pragma once

#include "pch.h"

class ImGuiLayer
{
public:
    bool Initialize(SDL_Window* window);
    void Shutdown();
    void ProcessEvent(const SDL_Event& event);
    void RenderFrame();

private:
    bool CreateDeviceObjects();
    void DestroyDeviceObjects();

    SDL_Window* m_Window = nullptr;

    // GPU Resources (similar to imgui_impl_vulkan)
    nvrhi::SamplerHandle m_FontSampler;
    nvrhi::TextureHandle m_FontTexture;
    nvrhi::BufferHandle m_VertexBuffer;
    nvrhi::BufferHandle m_IndexBuffer;
    
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::GraphicsPipelineHandle m_Pipeline;

    uint32_t m_VertexBufferSize = 0;
    uint32_t m_IndexBufferSize = 0;
};
