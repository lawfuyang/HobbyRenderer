#pragma once

#include "pch.h"

class NvrhiErrorCallback : public nvrhi::IMessageCallback
{
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override
    {
        const char* severityStr = "Unknown";
        switch (severity)
        {
        case nvrhi::MessageSeverity::Info:
            severityStr = "Info";
            break;
        case nvrhi::MessageSeverity::Warning:
            severityStr = "Warning";
            break;
        case nvrhi::MessageSeverity::Error:
            severityStr = "Error";
            break;
        case nvrhi::MessageSeverity::Fatal:
            severityStr = "Fatal";
            break;
        }
        SDL_Log("[NVRHI %s] %s", severityStr, messageText);

        // Assert on warning and above
        if (severity >= nvrhi::MessageSeverity::Warning)
        {
            SDL_assert(false && messageText);
        }
    }
};

class GraphicRHI
{
public:

    nvrhi::DeviceHandle m_NvrhiDevice;

    static constexpr uint32_t SwapchainImageCount = 2;
    nvrhi::TextureHandle m_NvrhiSwapchainTextures[GraphicRHI::SwapchainImageCount];
    nvrhi::Format m_SwapchainFormat = nvrhi::Format::UNKNOWN;
    Vector2U m_SwapchainExtent = {0, 0};

    inline static NvrhiErrorCallback ms_NvrhiCallback;

    virtual ~GraphicRHI() = default;

    virtual bool Initialize(SDL_Window* window) = 0;
    virtual void Shutdown() = 0;
    virtual bool CreateSwapchain(uint32_t width, uint32_t height) = 0;
    virtual bool AcquireNextSwapchainImage(uint32_t* outImageIndex) = 0;
    virtual bool PresentSwapchain(uint32_t imageIndex) = 0;

    virtual nvrhi::GraphicsAPI GetGraphicsAPI() const = 0;
};

inline std::unique_ptr<GraphicRHI> CreateGraphicRHI(nvrhi::GraphicsAPI api)
{
    // Forward declarations of factory functions
    extern std::unique_ptr<GraphicRHI> CreateVulkanGraphicRHI();
    extern std::unique_ptr<GraphicRHI> CreateD3D12GraphicRHI();

    if (api == nvrhi::GraphicsAPI::D3D12)
    {
        return CreateD3D12GraphicRHI();
    }
    return CreateVulkanGraphicRHI();
}
