#pragma once

#include "pch.h"

struct GraphicRHI
{
    bool Initialize(SDL_Window* window);
    void Shutdown();
    bool CreateSurface(SDL_Window* window);
    void DestroySurface();
    bool CreateSwapchain(uint32_t width, uint32_t height);
    void DestroySwapchain();
    bool AcquireNextSwapchainImage(uint32_t* outImageIndex);
    bool PresentSwapchain(uint32_t imageIndex);

    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    uint32_t m_GraphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;

    // Swapchain members
    static constexpr uint32_t SwapchainImageCount = 2;
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    VkFormat m_SwapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_SwapchainExtent = {0, 0};
    VkFence m_ImageAcquireFence = VK_NULL_HANDLE;
    VkImage m_SwapchainImages[SwapchainImageCount] = {};
    VkImageView m_SwapchainImageViews[SwapchainImageCount] = {};

    // Debug messenger
    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

    // Format conversion utilities
    static nvrhi::Format VkFormatToNvrhiFormat(VkFormat vkFormat);
    
    // Extension accessors for code reuse
    const std::vector<const char*>& GetInstanceExtensions() const { return m_InstanceExtensions; }
    const std::vector<const char*>& GetDeviceExtensions() const { return m_DeviceExtensions; }

private:
    bool CreateInstance();
    void DestroyInstance();
    bool SetupDebugMessenger();
    void DestroyDebugMessenger();
    VkPhysicalDevice ChoosePhysicalDevice();
    bool CreateLogicalDevice();
    void DestroyLogicalDevice();
    
    std::vector<const char*> m_InstanceExtensions;
    std::vector<const char*> m_DeviceExtensions;
};
