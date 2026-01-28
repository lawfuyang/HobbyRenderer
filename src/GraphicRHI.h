#pragma once

#include "pch.h"

struct GraphicRHI
{
    nvrhi::DeviceHandle m_NvrhiDevice;

    static constexpr uint32_t SwapchainImageCount = 2;
    nvrhi::TextureHandle m_NvrhiSwapchainTextures[GraphicRHI::SwapchainImageCount];
    nvrhi::Format m_SwapchainFormat = nvrhi::Format::UNKNOWN;
    Vector2U m_SwapchainExtent = {0, 0};

    virtual ~GraphicRHI() = default;

    virtual bool Initialize(SDL_Window* window) = 0;
    virtual void Shutdown() = 0;
    virtual bool CreateSwapchain(uint32_t width, uint32_t height) = 0;
    virtual bool AcquireNextSwapchainImage(uint32_t* outImageIndex) = 0;
    virtual bool PresentSwapchain(uint32_t imageIndex) = 0;

    virtual nvrhi::GraphicsAPI GetGraphicsAPI() const = 0;
};

std::unique_ptr<GraphicRHI> CreateGraphicRHI(nvrhi::GraphicsAPI api);
