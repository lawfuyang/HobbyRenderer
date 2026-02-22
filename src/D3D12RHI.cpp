#include "pch.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <directx/d3dx12_check_feature_support.h>
#include <nvrhi/d3d12.h>

#include "GraphicRHI.h"
#include "Config.h"

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\"; }

using Microsoft::WRL::ComPtr;

IDXGIAdapter1* g_DXGIAdapter = nullptr;

class D3D12GraphicRHI : public GraphicRHI
{
public:
    SDL_Window* m_Window = nullptr;
    ComPtr<IDXGIAdapter1> m_Adapter;
    ComPtr<IDXGIFactory6> m_Factory;
    ComPtr<ID3D12Device> m_Device;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<IDXGISwapChain3> m_SwapChain;
    bool m_bTearingSupported = false;
    bool m_bTightAlignmentSupported = false;

    ~D3D12GraphicRHI() override { Shutdown(); }

    void Initialize(SDL_Window* window) override
    {
        m_Window = window;

        if (Config::Get().m_EnableValidation)
        {
            ComPtr<ID3D12Debug> debugController;
            const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
            if (SUCCEEDED(hr))
            {
                debugController->EnableDebugLayer();

                if (Config::Get().m_EnableGPUAssistedValidation)
                {
                    ComPtr<ID3D12Debug1> debugController1;
                    if (SUCCEEDED(debugController->QueryInterface(IID_PPV_ARGS(&debugController1))))
                    {
                        debugController1->SetEnableGPUBasedValidation(TRUE);
                    }
                    else
                    {
                        SDL_LOG_ASSERT_FAIL("debugController->QueryInterface(ID3D12Debug1) failed", "debugController->QueryInterface(ID3D12Debug1) failed: 0x%08X", hr);
                    }
                }
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("D3D12GetDebugInterface failed", "D3D12GetDebugInterface failed: 0x%08X", hr);
            }
        }

        HRESULT hr = CreateDXGIFactory2(Config::Get().m_EnableValidation ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_Factory));
        if (FAILED(hr))
        {
            SDL_LOG_ASSERT_FAIL("CreateDXGIFactory2 failed", "CreateDXGIFactory2 failed: 0x%08X", hr);
            return;
        }

        for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != m_Factory->EnumAdapters1(adapterIndex, &m_Adapter); ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            m_Adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            if (SUCCEEDED(D3D12CreateDevice(m_Adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }

        if (m_Adapter == nullptr)
        {
            SDL_LOG_ASSERT_FAIL("No suitable DXGI adapter found", "No suitable DXGI adapter found");
            return;
        }

        g_DXGIAdapter = m_Adapter.Get();

        // Try creating with a baseline level first to get a device to query features from
        hr = D3D12CreateDevice(m_Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
        if (FAILED(hr))
        {
            SDL_LOG_ASSERT_FAIL("D3D12CreateDevice failed", "D3D12CreateDevice (baseline) failed: 0x%08X", hr);
            return;
        }

        CD3DX12FeatureSupport featureSupport;
        hr = featureSupport.Init(m_Device.Get());
        if (SUCCEEDED(hr))
        {
            D3D_FEATURE_LEVEL maxLevel = featureSupport.MaxSupportedFeatureLevel();
            if (maxLevel > D3D_FEATURE_LEVEL_11_0)
            {
                // Re-create with highest supported feature level
                m_Device.Reset();
                hr = D3D12CreateDevice(m_Adapter.Get(), maxLevel, IID_PPV_ARGS(&m_Device));
                if (FAILED(hr))
                {
                    SDL_LOG_ASSERT_FAIL("D3D12CreateDevice failed", "D3D12CreateDevice (max level) failed: 0x%08X", hr);
                    return;
                }
            }

            BOOL tearingSupported{};
            m_Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearingSupported, sizeof(tearingSupported));
            m_bTearingSupported = tearingSupported;

            m_bTightAlignmentSupported = (featureSupport.TightAlignmentSupportTier() != D3D12_TIGHT_ALIGNMENT_TIER_NOT_SUPPORTED);
        }
        else
        {
            SDL_LOG_ASSERT_FAIL("featureSupport.Init failed", "featureSupport.Init failed: 0x%08X", hr);
        }

        if (Config::Get().m_EnableValidation)
        {
            ComPtr<ID3D12InfoQueue> infoQueue;
            hr = m_Device.As(&infoQueue);
            if (SUCCEEDED(hr))
            {
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

                // Ignore specific warnings
                D3D12_MESSAGE_ID deniedMessages[] = {
                    D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED,
                    D3D12_MESSAGE_ID_RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS // temp until fix to force depth attachment to shader_read state if both SRV & DSV
                };
                D3D12_INFO_QUEUE_FILTER filter = {};
                filter.DenyList.NumIDs = _countof(deniedMessages);
                filter.DenyList.pIDList = deniedMessages;
                infoQueue->AddStorageFilterEntries(&filter);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("m_Device.As(ID3D12InfoQueue) failed", "m_Device.As(ID3D12InfoQueue) failed: 0x%08X", hr);
            }
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue));
        if (FAILED(hr))
        {
            SDL_LOG_ASSERT_FAIL("CreateCommandQueue failed", "CreateCommandQueue failed: 0x%08X", hr);
            return;
        }

        nvrhi::d3d12::DeviceDesc nvrhiDesc;
        nvrhiDesc.pDevice = m_Device.Get();
        nvrhiDesc.pGraphicsCommandQueue = m_CommandQueue.Get();
        nvrhiDesc.errorCB = &ms_NvrhiCallback;
        nvrhiDesc.enableHeapDirectlyIndexed = true;

        m_NvrhiDevice = nvrhi::d3d12::createDevice(nvrhiDesc);

        if (Config::Get().m_EnableValidation)
        {
            m_NvrhiDevice = nvrhi::validation::createValidationLayer(m_NvrhiDevice);
        }
    }

    void Shutdown() override
    {
        if (m_Device && m_CommandQueue)
        {
            ComPtr<ID3D12Fence> fence;
            if (SUCCEEDED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            {
                HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                if (event)
                {
                    m_CommandQueue->Signal(fence.Get(), 1);
                    if (fence->GetCompletedValue() < 1)
                    {
                        fence->SetEventOnCompletion(1, event);
                        WaitForSingleObject(event, INFINITE);
                    }
                    CloseHandle(event);
                }
                else
                {
                    SDL_LOG_ASSERT_FAIL("CreateEventEx failed", "Failed to create event for GPU sync during shutdown");
                }
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("CreateFence failed", "Failed to create fence for GPU sync during shutdown");
            }
        }

        for (uint32_t i = 0; i < GraphicRHI::SwapchainImageCount; i++)
        {
            m_NvrhiSwapchainTextures[i] = nullptr;
        }
        m_NvrhiDevice = nullptr;

        m_SwapChain.Reset();
        m_CommandQueue.Reset();
        m_Device.Reset();
        m_Factory.Reset();
        m_Adapter.Reset();
    }

    bool CreateSwapchain(uint32_t width, uint32_t height) override
    {
        if (m_Window == nullptr)
        {
            SDL_LOG_ASSERT_FAIL("SDL Window is null", "SDL Window is null during swapchain creation");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = GraphicRHI::SwapchainImageCount;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapChainDesc.Scaling = DXGI_SCALING_NONE;
        swapChainDesc.Flags = m_bTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(m_Window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = m_Factory->CreateSwapChainForHwnd(m_CommandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(hr))
        {
            SDL_LOG_ASSERT_FAIL("CreateSwapChainForHwnd failed", "CreateSwapChainForHwnd failed: 0x%08X", hr);
            return false;
        }

        hr = swapChain1.As(&m_SwapChain);
        if (FAILED(hr))
        {
            SDL_LOG_ASSERT_FAIL("IDXGISwapChain1::As(IDXGISwapChain3) failed", "Failed to cast swapchain to IDXGISwapChain3: 0x%08X", hr);
            return false;
        }

        m_SwapchainFormat = nvrhi::Format::RGBA8_UNORM;
        m_SwapchainExtent = { width, height };

        for (uint32_t i = 0; i < GraphicRHI::SwapchainImageCount; i++)
        {
            ComPtr<ID3D12Resource> backBuffer;
            hr = m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
            if (FAILED(hr))
            {
                SDL_LOG_ASSERT_FAIL("IDXGISwapChain3::GetBuffer failed", "Failed to get swapchain buffer %u: 0x%08X", i, hr);
                return false;
            }

            nvrhi::TextureDesc textureDesc;
            textureDesc.width = width;
            textureDesc.height = height;
            textureDesc.format = m_SwapchainFormat;
            textureDesc.debugName = "Swapchain Buffer";
            textureDesc.initialState = nvrhi::ResourceStates::Present;
            textureDesc.keepInitialState = true;
            textureDesc.isRenderTarget = true;

            m_NvrhiSwapchainTextures[i] = m_NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(backBuffer.Get()), textureDesc);
            if (!m_NvrhiSwapchainTextures[i])
            {
                SDL_LOG_ASSERT_FAIL("NVRHI createHandleForNativeTexture failed", "Failed to wrap back buffer %u into NVRHI handle", i);
                return false;
            }
        }

        return true;
    }

    bool AcquireNextSwapchainImage(uint32_t* outImageIndex) override
    {
        if (!m_SwapChain)
        {
            SDL_LOG_ASSERT_FAIL("Swapchain is null", "AcquireNextSwapchainImage called with null swapchain");
            return false;
        }
        *outImageIndex = m_SwapChain->GetCurrentBackBufferIndex();
        return true;
    }

    bool PresentSwapchain(uint32_t imageIndex) override
    {
        if (!m_SwapChain)
        {
            SDL_LOG_ASSERT_FAIL("Swapchain is null", "PresentSwapchain called with null swapchain");
            return false;
        }

        const UINT kSyncInterval = 0; // 0: no vsync, 1: vsync

        // When using sync interval 0, it is recommended to always pass the tearing flag when it is supported.
        const UINT kFlags = (kSyncInterval == 0 && m_bTearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;

        HRESULT hr = m_SwapChain->Present(kSyncInterval, kFlags);
        if (FAILED(hr))
        {
            SDL_LOG_ASSERT_FAIL("IDXGISwapChain3::Present failed", "Swapchain Presentation failed: 0x%08X", hr);
            return false;
        }
        return true;
    }

    float GetVRAMUsageMB() const override
    {
        if (!m_Adapter) return 0.0f;

        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(m_Adapter.As(&adapter3)))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info;
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            {
                return static_cast<float>(info.CurrentUsage) / 1024.0f / 1024.0f;
            }
        }
        return 0.0f;
    }

    nvrhi::GraphicsAPI GetGraphicsAPI() const override { return nvrhi::GraphicsAPI::D3D12; }

    void SetCommandListDebugName(const nvrhi::CommandListHandle& commandList, std::string_view name) override
    {
        if (name.empty())
            return;

        nvrhi::Object obj = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
        if (obj.pointer)
        {
            ID3D12GraphicsCommandList* pCmd = static_cast<ID3D12GraphicsCommandList*>(obj.pointer);
            if (pCmd)
            {
                int len = ::MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), nullptr, 0);
                if (len > 0)
                {
                    std::wstring wname(len, L'\0');
                    ::MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), &wname[0], len);
                    pCmd->SetName(wname.c_str());
                }
            }
        }
    }
};

std::unique_ptr<GraphicRHI> CreateD3D12GraphicRHI()
{
    return std::make_unique<D3D12GraphicRHI>();
}
