#include "GraphicRHI.h"
#include "Config.h"
#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.h>

// Define the Vulkan dynamic dispatcher - this needs to occur in exactly one cpp file in the program.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace
{
    const char* PhysicalDeviceTypeString(vk::PhysicalDeviceType type)
    {
        switch (type)
        {
        case vk::PhysicalDeviceType::eDiscreteGpu: return "Discrete GPU";
        case vk::PhysicalDeviceType::eIntegratedGpu: return "Integrated GPU";
        case vk::PhysicalDeviceType::eVirtualGpu: return "Virtual GPU";
        case vk::PhysicalDeviceType::eCpu: return "CPU";
        default: return "Other";
        }
    }

    // Get supported instance extensions from those requested
    std::vector<const char*> GetSupportedInstanceExtensions(const std::vector<const char*>& requested, const std::vector<const char*>& layers = {})
    {
        std::set<std::string> availableExts;
        
        // Get extensions from implementation
        {
            const std::vector<vk::ExtensionProperties> implExtensions = vk::enumerateInstanceExtensionProperties();
            for (const auto& ext : implExtensions)
            {
                const char* name = ext.extensionName;
                availableExts.insert(std::string(name));
            }
        }
        
        // Get extensions from layers
        for (const char* layerName : layers)
        {
            const std::vector<vk::ExtensionProperties> layerExtensions = vk::enumerateInstanceExtensionProperties(std::string(layerName));
            for (const auto& ext : layerExtensions)
            {
                const char* name = ext.extensionName;
                availableExts.insert(std::string(name));
            }
        }
        
        std::vector<const char*> supported;
        for (const char* extensionName : requested)
        {
            if (availableExts.count(extensionName))
            {
                supported.push_back(extensionName);
                SDL_Log("[Init] Instance extension supported: %s", extensionName);
            }
        }
        
        return supported;
    }

    // Get supported device extensions from those requested
    std::vector<const char*> GetSupportedDeviceExtensions(vk::PhysicalDevice physicalDevice, const std::vector<const char*>& requested)
    {
        const std::vector<vk::ExtensionProperties> availableExtensions = physicalDevice.enumerateDeviceExtensionProperties();
        std::vector<const char*> supported;
        
        for (const char* extensionName : requested)
        {
            for (const vk::ExtensionProperties& ext : availableExtensions)
            {
                if (std::strcmp(extensionName, ext.extensionName) == 0)
                {
                    supported.push_back(extensionName);
                    SDL_Log("[Init] Device extension supported: %s", extensionName);
                    break;
                }
            }
        }
        
        return supported;
    }

    bool SupportsGraphicsQueue(vk::PhysicalDevice device, uint32_t& outQueueFamily)
    {
        const std::vector<vk::QueueFamilyProperties> queueFamilies = device.getQueueFamilyProperties();
        for (uint32_t i = 0; i < queueFamilies.size(); ++i)
        {
            const vk::QueueFamilyProperties& qf = queueFamilies[i];
            if ((qf.queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlagBits{} && qf.queueCount > 0)
            {
                outQueueFamily = i;
                return true;
            }
        }
        return false;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        (void)pUserData;

        const char* severityStr = "";
        if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            severityStr = "ERROR";
        else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            severityStr = "WARNING";
        else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
            severityStr = "INFO";
        else
            severityStr = "VERBOSE";

        const char* typeStr = "";
        if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)
            typeStr = "GENERAL";
        else if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
            typeStr = "VALIDATION";
        else if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
            typeStr = "PERFORMANCE";

        SDL_Log("[Vulkan %s][%s] %s (ID: %u)", severityStr, typeStr, pCallbackData->pMessage, pCallbackData->messageIdNumber);

        // Break in debugger on validation errors and warnings
        if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ||
            (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
        {
#ifdef _MSC_VER
            __debugbreak();
#else
            __builtin_trap();
#endif
        }

        return VK_FALSE;
    }
}

// Helper: set Vulkan debug name using VK_EXT_debug_utils via Vulkan-Hpp dispatch
static void SetVkObjectName(VkDevice device, VkObjectType objType, uint64_t handle, const char* name)
{
    if (!device || !name)
        return;

    vk::Device vkDevice = static_cast<vk::Device>(device);
    vk::DebugUtilsObjectNameInfoEXT info;
    info.pNext = nullptr;
    info.objectType = static_cast<vk::ObjectType>(objType);
    info.objectHandle = handle;
    info.pObjectName = name;

    vkDevice.setDebugUtilsObjectNameEXT(info);
}

void GraphicRHI::SetDebugName(const nvrhi::TextureHandle& texture, std::string_view name)
{
    if (!m_Device || !texture)
        return;

    // Try image
    nvrhi::Object imgObj = texture->getNativeObject(nvrhi::ObjectTypes::VK_Image);
    if (imgObj.integer)
    {
        SetVkObjectName(m_Device, VK_OBJECT_TYPE_IMAGE, imgObj.integer, name.data());
    }

    // Try image view
    nvrhi::Object viewObj = texture->getNativeObject(nvrhi::ObjectTypes::VK_ImageView);
    if (viewObj.integer)
    {
        std::string viewName;
        viewName.reserve(name.size() + 6);
        viewName.append(name.data(), name.size());
        viewName.append("_view");
        SetVkObjectName(m_Device, VK_OBJECT_TYPE_IMAGE_VIEW, viewObj.integer, viewName.c_str());
    }
}

void GraphicRHI::SetDebugName(const nvrhi::BufferHandle& buffer, std::string_view name)
{
    if (!m_Device || !buffer)
        return;

    nvrhi::Object bufObj = buffer->getNativeObject(nvrhi::ObjectTypes::VK_Buffer);
    if (bufObj.integer)
    {
        SetVkObjectName(m_Device, VK_OBJECT_TYPE_BUFFER, bufObj.integer, name.data());
    }
}


bool GraphicRHI::Initialize(SDL_Window* window)
{
    if (!CreateInstance())
    {
        return false;
    }

    if (!CreateSurface(window))
    {
        return false;
    }

    m_PhysicalDevice = ChoosePhysicalDevice();
    if (m_PhysicalDevice == VK_NULL_HANDLE)
    {
        return false;
    }

    return CreateLogicalDevice();
}

void GraphicRHI::Shutdown()
{
    DestroySwapchain();
    DestroyLogicalDevice();
    DestroySurface();
    DestroyInstance();
}

bool GraphicRHI::CreateInstance()
{
    SDL_Log("[Init] Creating Vulkan instance");

    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        SDL_LOG_ASSERT_FAIL("SDL_Vulkan_LoadLibrary failed", "SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
        return false;
    }

    vk::detail::DynamicLoader dl;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    uint32_t sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (!sdlExtensions)
    {
        SDL_LOG_ASSERT_FAIL("SDL_Vulkan_GetInstanceExtensions failed", "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    SDL_Log("[Init] SDL3 requires %u Vulkan instance extensions:", sdlExtensionCount);
    m_InstanceExtensions.clear();
    m_InstanceExtensions.reserve(sdlExtensionCount);
    for (uint32_t i = 0; i < sdlExtensionCount; ++i)
    {
        SDL_Log("[Init]   - %s", sdlExtensions[i]);
        m_InstanceExtensions.push_back(sdlExtensions[i]);
    }

    // Check if validation is enabled
    const bool enableValidation = Config::Get().m_EnableValidation;
    const bool enableGPUAssisted = Config::Get().m_EnableValidation && Config::Get().m_EnableGPUAssistedValidation;
    bool hasValidationFeaturesExtension = false;

    // Enable validation layers (modern unified layer)
    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };

    // Optional NVRHI-compatible instance extensions for future-proofing
    const std::vector<const char*> optionalInstanceExtensions = {
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
    };

    // Filter to only supported extensions
    const std::vector<const char*> supportedOptionalInstanceExtensions = GetSupportedInstanceExtensions(optionalInstanceExtensions);
    m_InstanceExtensions.insert(m_InstanceExtensions.end(), supportedOptionalInstanceExtensions.begin(), supportedOptionalInstanceExtensions.end());

    // Add validation layer extensions if validation is enabled
    if (enableValidation)
    {
        std::vector<const char*> validationExtensions = {
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME
        };
        if (enableGPUAssisted)
        {
            validationExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }
        std::vector<const char*> supportedValidationExtensions = GetSupportedInstanceExtensions(validationExtensions, validationLayers);
        m_InstanceExtensions.insert(m_InstanceExtensions.end(), supportedValidationExtensions.begin(), supportedValidationExtensions.end());
        
        // Check if validation features extension is supported
        for (const char* ext : supportedValidationExtensions)
        {
            if (std::strcmp(ext, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0)
            {
                hasValidationFeaturesExtension = true;
                SDL_Log("[Init] VK_EXT_validation_features extension is supported");
                break;
            }
        }
        if (!hasValidationFeaturesExtension && enableGPUAssisted)
        {
            SDL_Log("[Init] Warning: VK_EXT_validation_features extension not supported, GPU-assisted validation disabled");
        }
    }

    if (enableValidation)
    {
        // Check if validation layers are available
        const std::vector<vk::LayerProperties> availableLayers = vk::enumerateInstanceLayerProperties();
        for (const char* layerName : validationLayers)
        {
            bool found = false;
            for (const vk::LayerProperties& layerProps : availableLayers)
            {
                if (std::strcmp(layerName, layerProps.layerName) == 0)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                SDL_LOG_ASSERT_FAIL("Required validation layer not available", "[Init] Validation layer not available: %s", layerName);
                return false;
            }
        }

        SDL_Log("[Init] Enabling Vulkan validation layers");
    }

    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName = "Agentic Renderer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_4;

    VkValidationFeaturesEXT validationFeatures{};
    if (enableGPUAssisted && hasValidationFeaturesExtension)
    {
        validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validationFeatures.enabledValidationFeatureCount = 1;
        VkValidationFeatureEnableEXT enabledFeatures[] = { VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT };
        validationFeatures.pEnabledValidationFeatures = enabledFeatures;
        SDL_Log("[Init] Enabling GPU-assisted Vulkan validation");
    }

    vk::InstanceCreateInfo createInfo{};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(m_InstanceExtensions.size());
    createInfo.ppEnabledExtensionNames = m_InstanceExtensions.data();
    if (enableValidation)
    {
        if (enableGPUAssisted && hasValidationFeaturesExtension)
        {
            createInfo.pNext = &validationFeatures;
        }
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    }
    else
    {
        createInfo.enabledLayerCount = 0;
    }

    vk::Instance instanceHandle = vk::createInstance(createInfo);
    if (!instanceHandle)
    {
        SDL_LOG_ASSERT_FAIL("vkCreateInstance failed", "vkCreateInstance failed");
        return false;
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(instanceHandle);

    m_Instance = static_cast<VkInstance>(instanceHandle);
    SDL_Log("[Init] Vulkan instance created successfully");

    if (!SetupDebugMessenger())
    {
        return false;
    }

    return true;
}

bool GraphicRHI::CreateLogicalDevice()
{
    if (m_GraphicsQueueFamily == VK_QUEUE_FAMILY_IGNORED)
    {
        SDL_LOG_ASSERT_FAIL("Graphics queue family not set", "Graphics queue family not set before logical device creation");
        return false;
    }

    vk::PhysicalDevice vkPhysical = static_cast<vk::PhysicalDevice>(m_PhysicalDevice);

    const float queuePriority = 1.0f;
    vk::DeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.queueFamilyIndex = m_GraphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    // Enable device extensions
    m_DeviceExtensions.clear();
    m_DeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // Optional NVRHI-compatible device extensions for future-proofing
    const std::vector<const char*> optionalDeviceExtensions = {
        // Core graphics extensions
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        
        // Ray tracing extensions and their dependencies
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME,
        
        // Advanced rendering
        VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,
        VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,
        VK_NV_MESH_SHADER_EXTENSION_NAME,
        
        // Debug extensions
        VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
        
        // Descriptor extensions
        VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
    };

    // Filter to only supported extensions
    const std::vector<const char*> supportedOptionalDeviceExtensions = GetSupportedDeviceExtensions(vkPhysical, optionalDeviceExtensions);
    m_DeviceExtensions.insert(m_DeviceExtensions.end(), supportedOptionalDeviceExtensions.begin(), supportedOptionalDeviceExtensions.end());

    SDL_Log("[Init] Using %zu device extensions", m_DeviceExtensions.size());

    // Enable device features
    vk::PhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
    deviceFeatures.multiDrawIndirect = VK_TRUE;
    deviceFeatures.drawIndirectFirstInstance = VK_TRUE;

    // Enable Vulkan 1.3 features
    vk::PhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.dynamicRendering = VK_TRUE;
    vulkan13Features.synchronization2 = VK_TRUE;

    // Enable mutable descriptor type features (VK_EXT_mutable_descriptor_type)
    vk::PhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableDescriptorFeatures{};
    mutableDescriptorFeatures.pNext = &vulkan13Features;
    mutableDescriptorFeatures.mutableDescriptorType = VK_TRUE;

    // Enable Vulkan 1.2 features
    vk::PhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.pNext = &mutableDescriptorFeatures;
    vulkan12Features.bufferDeviceAddress = VK_TRUE;
    vulkan12Features.descriptorBindingVariableDescriptorCount = VK_TRUE;
    vulkan12Features.runtimeDescriptorArray = VK_TRUE;
    vulkan12Features.timelineSemaphore = VK_TRUE;
    vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;

    vk::PhysicalDeviceVulkan11Features vulkan11Features{};
    vulkan11Features.pNext = &vulkan12Features;
    vulkan11Features.shaderDrawParameters = VK_TRUE;

    vk::DeviceCreateInfo createInfo{};
    createInfo.pNext = &vulkan11Features;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(m_DeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = m_DeviceExtensions.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    vk::Device vkDevice = vkPhysical.createDevice(createInfo);
    if (!vkDevice)
    {
        SDL_LOG_ASSERT_FAIL("vkCreateDevice failed", "vkCreateDevice failed");
        return false;
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkDevice);

    m_Device = static_cast<VkDevice>(vkDevice);
    m_GraphicsQueue = static_cast<VkQueue>(vkDevice.getQueue(m_GraphicsQueueFamily, 0));

    SDL_Log("[Init] Vulkan logical device created (graphics family %u)", m_GraphicsQueueFamily);
    return true;
}

bool GraphicRHI::CreateSurface(SDL_Window* window)
{
    if (m_Surface != VK_NULL_HANDLE)
    {
        return true; // already created
    }

    if (!window || m_Instance == VK_NULL_HANDLE)
    {
        SDL_LOG_ASSERT_FAIL("Invalid state for surface creation", "SDL window or Vulkan instance not ready for surface creation");
        return false;
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, m_Instance, nullptr, &surface))
    {
        SDL_LOG_ASSERT_FAIL("SDL_Vulkan_CreateSurface failed", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    m_Surface = surface;
    SDL_Log("[Init] Vulkan surface created successfully");
    return true;
}

void GraphicRHI::DestroySurface()
{
    if (m_Surface != VK_NULL_HANDLE)
    {
        SDL_Log("[Shutdown] Destroying Vulkan surface");
        SDL_Vulkan_DestroySurface(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }
}

void GraphicRHI::DestroyInstance()
{
    DestroyDebugMessenger();

    if (m_Instance != VK_NULL_HANDLE)
    {
        SDL_Log("[Shutdown] Destroying Vulkan instance");
        vk::Instance vkInstance = static_cast<vk::Instance>(m_Instance);
        vkInstance.destroy();
        m_Instance = VK_NULL_HANDLE;
        m_PhysicalDevice = VK_NULL_HANDLE;
        m_GraphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
        SDL_Vulkan_UnloadLibrary();
    }
}

bool GraphicRHI::SetupDebugMessenger()
{
    if (!Config::Get().m_EnableValidation)
    {
        return true;
    }

    vk::Instance vkInstance = static_cast<vk::Instance>(m_Instance);

    vk::DebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning;
    createInfo.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                             vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                             vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    createInfo.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(&DebugCallback);

    try
    {
        m_DebugMessenger = static_cast<VkDebugUtilsMessengerEXT>(vkInstance.createDebugUtilsMessengerEXT(createInfo));
        SDL_Log("[Init] Debug messenger created successfully");
    }
    catch (const vk::SystemError& e)
    {
        SDL_Log("[Init] Failed to create debug messenger: %s", e.what());
        return false;
    }

    return true;
}

void GraphicRHI::DestroyDebugMessenger()
{
    if (m_DebugMessenger != VK_NULL_HANDLE && m_Instance != VK_NULL_HANDLE)
    {
        SDL_Log("[Shutdown] Destroying debug messenger");
        vk::Instance vkInstance = static_cast<vk::Instance>(m_Instance);
        vkInstance.destroyDebugUtilsMessengerEXT(static_cast<vk::DebugUtilsMessengerEXT>(m_DebugMessenger));
        m_DebugMessenger = VK_NULL_HANDLE;
    }
}

void GraphicRHI::DestroyLogicalDevice()
{
    if (m_Device != VK_NULL_HANDLE)
    {
        SDL_Log("[Shutdown] Destroying Vulkan logical device");
        vk::Device vkDevice = static_cast<vk::Device>(m_Device);
        vkDevice.destroy();
        m_Device = VK_NULL_HANDLE;
        m_GraphicsQueue = VK_NULL_HANDLE;
        m_GraphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    }
}

VkPhysicalDevice GraphicRHI::ChoosePhysicalDevice()
{
    SDL_Log("[Init] Selecting Vulkan physical device");

    vk::Instance vkInstance = static_cast<vk::Instance>(m_Instance);
    const std::vector<vk::PhysicalDevice> devices = vkInstance.enumeratePhysicalDevices();

    if (devices.empty())
    {
        SDL_LOG_ASSERT_FAIL("No Vulkan physical devices found", "No Vulkan physical devices found");
        return VK_NULL_HANDLE;
    }

    vk::PhysicalDevice selected{};
    uint32_t selectedQueueFamily = VK_QUEUE_FAMILY_IGNORED;

    for (vk::PhysicalDevice device : devices)
    {
        uint32_t graphicsFamily = VK_QUEUE_FAMILY_IGNORED;
        if (!SupportsGraphicsQueue(device, graphicsFamily))
        {
            const vk::PhysicalDeviceProperties props = device.getProperties();
            SDL_Log("[Init] Skipping device without graphics queue: %s (%s)", props.deviceName, PhysicalDeviceTypeString(props.deviceType));
            continue;
        }

        const vk::PhysicalDeviceProperties props = device.getProperties();
        if (!selected)
        {
            selected = device;
            selectedQueueFamily = graphicsFamily;
        }

        if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
        {
            selected = device;
            selectedQueueFamily = graphicsFamily;
            break; // prefer the first discrete GPU we find
        }
    }

    if (!selected)
    {
        SDL_LOG_ASSERT_FAIL("No suitable Vulkan physical device found", "No suitable Vulkan physical device with a graphics queue was found");
        return VK_NULL_HANDLE;
    }

    const vk::PhysicalDeviceProperties chosenProps = selected.getProperties();
    SDL_Log("[Init] Selected device: %s (%s)", chosenProps.deviceName, PhysicalDeviceTypeString(chosenProps.deviceType));

    m_GraphicsQueueFamily = selectedQueueFamily;
    return static_cast<VkPhysicalDevice>(selected);
}

bool GraphicRHI::CreateSwapchain(uint32_t width, uint32_t height)
{
    if (m_Swapchain != VK_NULL_HANDLE)
    {
        SDL_Log("[Swapchain] Swapchain already created, destroying old one");
        DestroySwapchain();
    }

    if (m_Device == VK_NULL_HANDLE || m_Surface == VK_NULL_HANDLE || m_PhysicalDevice == VK_NULL_HANDLE)
    {
        SDL_LOG_ASSERT_FAIL("Invalid state for swapchain creation", "[Swapchain] Cannot create swapchain: device, surface, or physical device not initialized");
        return false;
    }

    SDL_Log("[Init] Creating Vulkan swapchain (size: %ux%u)", width, height);

    vk::PhysicalDevice vkPhysical = static_cast<vk::PhysicalDevice>(m_PhysicalDevice);
    vk::Device vkDevice = static_cast<vk::Device>(m_Device);
    vk::SurfaceKHR vkSurface = static_cast<vk::SurfaceKHR>(m_Surface);

    // Check if the graphics queue family supports presentation
    if (!SDL_Vulkan_GetPresentationSupport(m_Instance, m_PhysicalDevice, m_GraphicsQueueFamily))
    {
        SDL_LOG_ASSERT_FAIL("Graphics queue family does not support presentation", "[Swapchain] Graphics queue family does not support presentation");
        return false;
    }

    // Query surface capabilities
    const vk::SurfaceCapabilitiesKHR surfaceCapabilities = vkPhysical.getSurfaceCapabilitiesKHR(vkSurface);

    // Choose swap extent
    VkExtent2D swapExtent;
    if (surfaceCapabilities.currentExtent.width != UINT32_MAX)
    {
        swapExtent = static_cast<VkExtent2D>(surfaceCapabilities.currentExtent);
    }
    else
    {
        swapExtent = {
            std::clamp(width, surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width),
            std::clamp(height, surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height)
        };
    }

    // Query surface formats
    const std::vector<vk::SurfaceFormatKHR> surfaceFormats = vkPhysical.getSurfaceFormatsKHR(vkSurface);
    if (surfaceFormats.empty())
    {
        SDL_LOG_ASSERT_FAIL("No surface formats available", "[Swapchain] No surface formats available");
        return false;
    }

    // Prefer SRGB format if available
    vk::SurfaceFormatKHR selectedFormat = surfaceFormats[0];
    for (const vk::SurfaceFormatKHR& format : surfaceFormats)
    {
        if (format.format == vk::Format::eB8G8R8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
        {
            selectedFormat = format;
            break;
        }
    }

    SDL_Log("[Init] Swapchain format: %s, color space: %s", vk::to_string(selectedFormat.format).c_str(), vk::to_string(selectedFormat.colorSpace).c_str());

    // Determine present mode
    const std::vector<vk::PresentModeKHR> presentModes = vkPhysical.getSurfacePresentModesKHR(vkSurface);
    vk::PresentModeKHR selectedPresentMode = vk::PresentModeKHR::eMailbox;
    if (std::find(presentModes.begin(), presentModes.end(), vk::PresentModeKHR::eMailbox) == presentModes.end())
    {
        selectedPresentMode = vk::PresentModeKHR::eFifo; // always available
    }

    // Determine image count - we always want exactly 2 images
    constexpr uint32_t desiredImageCount = GraphicRHI::SwapchainImageCount;
    SDL_assert(surfaceCapabilities.minImageCount <= desiredImageCount && "Swapchain requires more than 2 images");
    SDL_assert(surfaceCapabilities.maxImageCount == 0 || surfaceCapabilities.maxImageCount >= desiredImageCount && "Swapchain cannot support 2 images");

    // Create swapchain
    vk::SwapchainCreateInfoKHR swapchainCreateInfo{};
    swapchainCreateInfo.surface = vkSurface;
    swapchainCreateInfo.minImageCount = desiredImageCount;
    swapchainCreateInfo.imageFormat = selectedFormat.format;
    swapchainCreateInfo.imageColorSpace = selectedFormat.colorSpace;
    swapchainCreateInfo.imageExtent = static_cast<vk::Extent2D>(swapExtent);
    swapchainCreateInfo.imageArrayLayers = 1;
    swapchainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment | 
                                     vk::ImageUsageFlagBits::eTransferSrc | 
                                     vk::ImageUsageFlagBits::eTransferDst;
    swapchainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
    swapchainCreateInfo.queueFamilyIndexCount = 0;
    swapchainCreateInfo.pQueueFamilyIndices = nullptr;
    swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
    swapchainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    swapchainCreateInfo.presentMode = selectedPresentMode;
    swapchainCreateInfo.clipped = VK_TRUE;
    swapchainCreateInfo.oldSwapchain = nullptr;

    vk::SwapchainKHR vkSwapchain = vkDevice.createSwapchainKHR(swapchainCreateInfo);
    if (!vkSwapchain)
    {
        SDL_LOG_ASSERT_FAIL("vkCreateSwapchainKHR failed", "[Swapchain] vkCreateSwapchainKHR failed");
        return false;
    }

    m_Swapchain = static_cast<VkSwapchainKHR>(vkSwapchain);
    m_SwapchainFormat = static_cast<VkFormat>(selectedFormat.format);
    m_SwapchainExtent = swapExtent;

    // Get swapchain images
    const std::vector<vk::Image> vkImages = vkDevice.getSwapchainImagesKHR(vkSwapchain);
    SDL_assert(vkImages.size() == SwapchainImageCount && "Swapchain must have exactly 2 images");
    for (size_t i = 0; i < SwapchainImageCount; ++i)
    {
        m_SwapchainImages[i] = static_cast<VkImage>(vkImages[i]);
    }

    SDL_Log("[Init] Swapchain created with %u images", SwapchainImageCount);

    // Create image views
    for (size_t i = 0; i < SwapchainImageCount; ++i)
    {
        vk::ImageViewCreateInfo imageViewCreateInfo{};
        imageViewCreateInfo.image = static_cast<vk::Image>(m_SwapchainImages[i]);
        imageViewCreateInfo.viewType = vk::ImageViewType::e2D;
        imageViewCreateInfo.format = static_cast<vk::Format>(m_SwapchainFormat);
        imageViewCreateInfo.components = {
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity
        };
        imageViewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
        imageViewCreateInfo.subresourceRange.levelCount = 1;
        imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
        imageViewCreateInfo.subresourceRange.layerCount = 1;

        vk::ImageView vkImageView = vkDevice.createImageView(imageViewCreateInfo);
        if (!vkImageView)
        {
            SDL_LOG_ASSERT_FAIL("createImageView failed", "[Swapchain] Failed to create image view %zu", i);
            return false;
        }

        m_SwapchainImageViews[i] = static_cast<VkImageView>(vkImageView);
    }

    SDL_Log("[Init] Swapchain image views created successfully");

    if (m_ImageAcquireFence == VK_NULL_HANDLE)
    {
        vk::FenceCreateInfo fenceInfo{};
        fenceInfo.flags = {};
        vk::Fence fence = vkDevice.createFence(fenceInfo);
        if (!fence)
        {
            SDL_Log("[Swapchain] Failed to create image acquire fence");
            return false;
        }
        m_ImageAcquireFence = static_cast<VkFence>(fence);
    }

    return true;
}

void GraphicRHI::DestroySwapchain()
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return; // device already destroyed
    }

    vk::Device vkDevice = static_cast<vk::Device>(m_Device);

    SDL_Log("[Shutdown] Destroying swapchain image views");
    for (size_t i = 0; i < SwapchainImageCount; ++i)
    {
        if (m_SwapchainImageViews[i] != VK_NULL_HANDLE)
        {
            vkDevice.destroyImageView(static_cast<vk::ImageView>(m_SwapchainImageViews[i]));
            m_SwapchainImageViews[i] = VK_NULL_HANDLE;
        }
    }

    if (m_Swapchain != VK_NULL_HANDLE)
    {
        SDL_Log("[Shutdown] Destroying Vulkan swapchain");
        vkDevice.destroySwapchainKHR(static_cast<vk::SwapchainKHR>(m_Swapchain));
        m_Swapchain = VK_NULL_HANDLE;
    }

    if (m_ImageAcquireFence != VK_NULL_HANDLE)
    {
        vkDevice.destroyFence(static_cast<vk::Fence>(m_ImageAcquireFence));
        m_ImageAcquireFence = VK_NULL_HANDLE;
    }

    for (size_t i = 0; i < SwapchainImageCount; ++i)
    {
        m_SwapchainImages[i] = VK_NULL_HANDLE;
    }
    m_SwapchainFormat = VK_FORMAT_UNDEFINED;
    m_SwapchainExtent = {0, 0};
}

bool GraphicRHI::AcquireNextSwapchainImage(uint32_t* outImageIndex)
{
    if (!outImageIndex)
    {
        return false;
    }

    vk::Device device = static_cast<vk::Device>(m_Device);
    vk::SwapchainKHR swapchain = static_cast<vk::SwapchainKHR>(m_Swapchain);
    if (!device || !swapchain)
    {
        SDL_Log("[Swapchain] Device or swapchain invalid during acquire");
        return false;
    }

    vk::ResultValue<uint32_t> acquired = device.acquireNextImageKHR(swapchain, UINT64_MAX, vk::Semaphore(), static_cast<vk::Fence>(m_ImageAcquireFence));
    if (acquired.result != vk::Result::eSuccess && acquired.result != vk::Result::eSuboptimalKHR)
    {
        SDL_LOG_ASSERT_FAIL("acquireNextImageKHR failed", "[Swapchain] acquireNextImageKHR failed: %s", vk::to_string(acquired.result).c_str());
        return false;
    }

    if (m_ImageAcquireFence != VK_NULL_HANDLE)
    {
        vk::Result waitResult = device.waitForFences(static_cast<vk::Fence>(m_ImageAcquireFence), VK_TRUE, UINT64_MAX);
        if (waitResult != vk::Result::eSuccess)
        {
            SDL_LOG_ASSERT_FAIL("waitForFences after acquire failed", "[Swapchain] waitForFences after acquire failed: %s", vk::to_string(waitResult).c_str());
            return false;
        }

        device.resetFences(static_cast<vk::Fence>(m_ImageAcquireFence));
    }

    *outImageIndex = acquired.value;
    return true;
}

bool GraphicRHI::PresentSwapchain(uint32_t imageIndex)
{
    vk::Queue queue = static_cast<vk::Queue>(m_GraphicsQueue);
    vk::SwapchainKHR swapchain = static_cast<vk::SwapchainKHR>(m_Swapchain);
    if (!queue || !swapchain)
    {
        SDL_Log("[Swapchain] Queue or swapchain invalid during present");
        return false;
    }

    vk::PresentInfoKHR presentInfo{};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    vk::Result result = queue.presentKHR(presentInfo);
    if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR)
    {
        return true;
    }

    SDL_LOG_ASSERT_FAIL("presentKHR failed", "[Swapchain] presentKHR failed: %s", vk::to_string(result).c_str());
    return false;
}

nvrhi::Format GraphicRHI::VkFormatToNvrhiFormat(VkFormat vkFormat)
{
    switch (vkFormat)
    {
    case VK_FORMAT_UNDEFINED:                      return nvrhi::Format::UNKNOWN;
    case VK_FORMAT_R8_UINT:                        return nvrhi::Format::R8_UINT;
    case VK_FORMAT_R8_SINT:                        return nvrhi::Format::R8_SINT;
    case VK_FORMAT_R8_UNORM:                       return nvrhi::Format::R8_UNORM;
    case VK_FORMAT_R8_SNORM:                       return nvrhi::Format::R8_SNORM;
    case VK_FORMAT_R8G8_UINT:                      return nvrhi::Format::RG8_UINT;
    case VK_FORMAT_R8G8_SINT:                      return nvrhi::Format::RG8_SINT;
    case VK_FORMAT_R8G8_UNORM:                     return nvrhi::Format::RG8_UNORM;
    case VK_FORMAT_R8G8_SNORM:                     return nvrhi::Format::RG8_SNORM;
    case VK_FORMAT_R16_UINT:                       return nvrhi::Format::R16_UINT;
    case VK_FORMAT_R16_SINT:                       return nvrhi::Format::R16_SINT;
    case VK_FORMAT_R16_UNORM:                      return nvrhi::Format::R16_UNORM;
    case VK_FORMAT_R16_SNORM:                      return nvrhi::Format::R16_SNORM;
    case VK_FORMAT_R16_SFLOAT:                     return nvrhi::Format::R16_FLOAT;
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:          return nvrhi::Format::BGRA4_UNORM;
    case VK_FORMAT_B5G6R5_UNORM_PACK16:            return nvrhi::Format::B5G6R5_UNORM;
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:          return nvrhi::Format::B5G5R5A1_UNORM;
    case VK_FORMAT_R8G8B8A8_UINT:                  return nvrhi::Format::RGBA8_UINT;
    case VK_FORMAT_R8G8B8A8_SINT:                  return nvrhi::Format::RGBA8_SINT;
    case VK_FORMAT_R8G8B8A8_UNORM:                 return nvrhi::Format::RGBA8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM:                 return nvrhi::Format::RGBA8_SNORM;
    case VK_FORMAT_B8G8R8A8_UNORM:                 return nvrhi::Format::BGRA8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:                  return nvrhi::Format::SRGBA8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:                  return nvrhi::Format::SBGRA8_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:       return nvrhi::Format::R10G10B10A2_UNORM;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:        return nvrhi::Format::R11G11B10_FLOAT;
    case VK_FORMAT_R16G16_UINT:                    return nvrhi::Format::RG16_UINT;
    case VK_FORMAT_R16G16_SINT:                    return nvrhi::Format::RG16_SINT;
    case VK_FORMAT_R16G16_UNORM:                   return nvrhi::Format::RG16_UNORM;
    case VK_FORMAT_R16G16_SNORM:                   return nvrhi::Format::RG16_SNORM;
    case VK_FORMAT_R16G16_SFLOAT:                  return nvrhi::Format::RG16_FLOAT;
    case VK_FORMAT_R32_UINT:                       return nvrhi::Format::R32_UINT;
    case VK_FORMAT_R32_SINT:                       return nvrhi::Format::R32_SINT;
    case VK_FORMAT_R32_SFLOAT:                     return nvrhi::Format::R32_FLOAT;
    case VK_FORMAT_R16G16B16A16_UINT:              return nvrhi::Format::RGBA16_UINT;
    case VK_FORMAT_R16G16B16A16_SINT:              return nvrhi::Format::RGBA16_SINT;
    case VK_FORMAT_R16G16B16A16_SFLOAT:            return nvrhi::Format::RGBA16_FLOAT;
    case VK_FORMAT_R16G16B16A16_UNORM:             return nvrhi::Format::RGBA16_UNORM;
    case VK_FORMAT_R16G16B16A16_SNORM:             return nvrhi::Format::RGBA16_SNORM;
    case VK_FORMAT_R32G32_UINT:                    return nvrhi::Format::RG32_UINT;
    case VK_FORMAT_R32G32_SINT:                    return nvrhi::Format::RG32_SINT;
    case VK_FORMAT_R32G32_SFLOAT:                  return nvrhi::Format::RG32_FLOAT;
    case VK_FORMAT_R32G32B32_UINT:                 return nvrhi::Format::RGB32_UINT;
    case VK_FORMAT_R32G32B32_SINT:                 return nvrhi::Format::RGB32_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT:               return nvrhi::Format::RGB32_FLOAT;
    case VK_FORMAT_R32G32B32A32_UINT:              return nvrhi::Format::RGBA32_UINT;
    case VK_FORMAT_R32G32B32A32_SINT:              return nvrhi::Format::RGBA32_SINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT:            return nvrhi::Format::RGBA32_FLOAT;
    case VK_FORMAT_D16_UNORM:                      return nvrhi::Format::D16;
    case VK_FORMAT_D24_UNORM_S8_UINT:             return nvrhi::Format::D24S8;
    case VK_FORMAT_D32_SFLOAT:                     return nvrhi::Format::D32;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:            return nvrhi::Format::D32S8;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:          return nvrhi::Format::BC1_UNORM;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:           return nvrhi::Format::BC1_UNORM_SRGB;
    case VK_FORMAT_BC2_UNORM_BLOCK:               return nvrhi::Format::BC2_UNORM;
    case VK_FORMAT_BC2_SRGB_BLOCK:                return nvrhi::Format::BC2_UNORM_SRGB;
    case VK_FORMAT_BC3_UNORM_BLOCK:               return nvrhi::Format::BC3_UNORM;
    case VK_FORMAT_BC3_SRGB_BLOCK:                return nvrhi::Format::BC3_UNORM_SRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK:               return nvrhi::Format::BC4_UNORM;
    case VK_FORMAT_BC4_SNORM_BLOCK:               return nvrhi::Format::BC4_SNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK:               return nvrhi::Format::BC5_UNORM;
    case VK_FORMAT_BC5_SNORM_BLOCK:               return nvrhi::Format::BC5_SNORM;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:             return nvrhi::Format::BC6H_UFLOAT;
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:             return nvrhi::Format::BC6H_SFLOAT;
    case VK_FORMAT_BC7_UNORM_BLOCK:               return nvrhi::Format::BC7_UNORM;
    case VK_FORMAT_BC7_SRGB_BLOCK:                return nvrhi::Format::BC7_UNORM_SRGB;
    default:
        SDL_Log("[Warning] Unsupported VkFormat: %d", vkFormat);
        return nvrhi::Format::UNKNOWN;
    }
}
