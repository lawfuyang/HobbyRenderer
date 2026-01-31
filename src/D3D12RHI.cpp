#include "pch.h"
#include "GraphicRHI.h"

class D3D12GraphicRHI : public GraphicRHI
{
public:
    bool Initialize(SDL_Window* window) override { nvrhi::utils::NotImplemented(); return false; }
    void Shutdown() override { nvrhi::utils::NotImplemented(); }
    bool CreateSwapchain(uint32_t width, uint32_t height) override { nvrhi::utils::NotImplemented(); return false; }
    bool AcquireNextSwapchainImage(uint32_t* outImageIndex) override { nvrhi::utils::NotImplemented(); return false; }
    bool PresentSwapchain(uint32_t imageIndex) override { nvrhi::utils::NotImplemented(); return false; }
    nvrhi::GraphicsAPI GetGraphicsAPI() const override { return nvrhi::GraphicsAPI::D3D12; }
};

std::unique_ptr<GraphicRHI> CreateD3D12GraphicRHI()
{
    return std::make_unique<D3D12GraphicRHI>();
}
