#pragma once
class NvrhiErrorCallback : public nvrhi::IMessageCallback
{
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override;
};

class GraphicRHI
{
public:

    nvrhi::DeviceHandle m_NvrhiDevice;

    static constexpr uint32_t SwapchainImageCount = 2;
    nvrhi::TextureHandle m_NvrhiSwapchainTextures[GraphicRHI::SwapchainImageCount];
    nvrhi::Format m_SwapchainFormat = nvrhi::Format::UNKNOWN;
    Vector2U m_SwapchainExtent = {0, 0};

    // HDR display support: detected in Initialize(), consumed by HDRRenderer
    bool m_bIsHDR = false;
    float m_MaxDisplayNits = 80.0f;  // SDR default (80 nits). Updated by D3D12RHI CreateSwapchain.

    inline static NvrhiErrorCallback ms_NvrhiCallback;

    virtual ~GraphicRHI() = default;

    virtual void Initialize(SDL_Window* window) = 0;
    // Releases the device and every object the RHI owns. Called from ~D3D12GraphicRHI, i.e. when
    // Renderer's m_RHI is destroyed, so that the device outlives every subsystem holding nvrhi
    // resources. Do not call it while such resources are still alive; use ReleaseSwapchain().
    virtual void Shutdown() = 0;
    // Releases only the presentation resources (back buffers + swapchain) so the window can be
    // destroyed while the device itself stays alive for the remaining owners.
    virtual void ReleaseSwapchain() = 0;
    virtual bool CreateSwapchain(uint32_t width, uint32_t height) = 0;
    virtual bool AcquireNextSwapchainImage(uint32_t* outImageIndex) = 0;
    virtual bool PresentSwapchain(uint32_t imageIndex) = 0;

    virtual float GetVRAMUsageMB() const = 0;

    virtual nvrhi::GraphicsAPI GetGraphicsAPI() const = 0;

    // Optional: set a debug name on an internal command list's native object
    virtual void SetCommandListDebugName(const nvrhi::CommandListHandle& /*commandList*/, std::string_view /*name*/) { }
};

inline std::unique_ptr<GraphicRHI> CreateGraphicRHI()
{
    // Forward declarations of factory functions
    extern std::unique_ptr<GraphicRHI> CreateD3D12GraphicRHI();
    return CreateD3D12GraphicRHI();
}
