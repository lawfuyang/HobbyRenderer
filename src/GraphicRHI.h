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

    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    uint32_t m_GraphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;

    // Swapchain members
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    VkFormat m_SwapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_SwapchainExtent = {0, 0};
    std::vector<VkImage> m_SwapchainImages;
    std::vector<VkImageView> m_SwapchainImageViews;

private:
    bool CreateInstance();
    void DestroyInstance();
    VkPhysicalDevice ChoosePhysicalDevice();
    bool CreateLogicalDevice();
    void DestroyLogicalDevice();
};
