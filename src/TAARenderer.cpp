#include "Renderer.h"
#include "CommonResources.h"

#include "FidelityFX/api/include/ffx_api.hpp"
#include "FidelityFX/api/include/dx12/ffx_api_dx12.hpp"
#include "FidelityFX/upscalers/include/ffx_upscale.hpp"

#include <imgui.h>

// ============================================================================
// Global RG handles
// ============================================================================

RGTextureHandle g_RG_TAAOutput;
extern RGTextureHandle g_RG_HDRColor;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_ExposureTexture;

// ============================================================================
// FFX error macro
// ============================================================================

static const char* FFXReturnCodeToString(ffx::ReturnCode rc)
{
    switch (rc)
    {
    case ffx::ReturnCode::Ok: return "Ok";
    case ffx::ReturnCode::Error: return "Error";
    case ffx::ReturnCode::ErrorUnknownDesctype: return "ErrorUnknownDesctype";
    case ffx::ReturnCode::ErrorRuntimeError: return "ErrorRuntimeError";
    case ffx::ReturnCode::ErrorNoProvider: return "ErrorNoProvider";
    case ffx::ReturnCode::ErrorMemory: return "ErrorMemory";
    case ffx::ReturnCode::ErrorParameter: return "ErrorParameter";
    case ffx::ReturnCode::ErrorProviderNoSupportNewDesctype: return "ErrorProviderNoSupportNewDesctype";
    default: return "Unknown ReturnCode";
    }
}

#define FFX_CALL(x) \
    { \
        const ffx::ReturnCode _rc = (x); \
        if (_rc != ffx::ReturnCode::Ok) \
        { \
            SDL_Log("[FSR3] FFX call failed: %s (%s)", #x, FFXReturnCodeToString(_rc)); \
            SDL_assert(false && "FFX call failed"); \
        } \
    }

// ============================================================================
// TAARenderer
// ============================================================================

class TAARenderer : public IRenderer
{
    ffx::Context m_FSRContext = nullptr;

public:
    TAARenderer() = default;

    ~TAARenderer() override
    {
        Shutdown();
    }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    void Initialize() override
    {
        InitFSR();
    }

    void Shutdown()
    {
        if (m_FSRContext)
        {
            FFX_CALL(ffx::DestroyContext(m_FSRContext));
            m_FSRContext = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    // FSR3 context creation
    // -------------------------------------------------------------------------

    static void FfxMsgCallback(uint32_t type, const wchar_t* message)
    {
        // Convert wide string to UTF-8 for SDL_Log
        int len = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0)
        {
            std::string utf8(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8.data(), len, nullptr, nullptr);
            SDL_Log("[FSR3] %s: %s",
                type == FFX_API_MESSAGE_TYPE_ERROR ? "Error" : "Warning",
                utf8.c_str());
        }
        SDL_assert(type != FFX_API_MESSAGE_TYPE_ERROR && "FSR3 error");
    }

    void InitFSR()
    {
        Renderer* renderer = Renderer::GetInstance();
        ID3D12Device* nativeDevice = renderer->m_RHI->m_NvrhiDevice->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);

        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Native resolution TAA: render size == display size
        const FfxApiDimensions2D resolution{ width, height };

        ffx::CreateContextDescUpscale createDesc{};
        createDesc.maxRenderSize  = resolution;
        createDesc.maxUpscaleSize = resolution;
        createDesc.flags =
            FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
            FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
            FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
            FFX_UPSCALE_ENABLE_DEPTH_INFINITE;

    #if 0
        createDesc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    #endif

        createDesc.fpMessage = FfxMsgCallback;

        FFX_API_RETURN_OK;
        ffx::CreateBackendDX12Desc backendDesc{};
        backendDesc.device = nativeDevice;

        FFX_CALL(ffx::CreateContext(m_FSRContext, nullptr, createDesc, backendDesc));

        // Log selected provider version
        ffxQueryGetProviderVersion getVersion{};
        getVersion.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        FFX_CALL(ffx::Query(m_FSRContext, getVersion));
        SDL_Log("[FSR3] TAARenderer: provider version = [%s]", getVersion.versionName);

        // Set up global debug callback
        ffx::ConfigureDescGlobalDebug1 globalDebug{};
        globalDebug.debugLevel = 0;
        globalDebug.fpMessage  = FfxMsgCallback;
        FFX_CALL(ffx::Configure(m_FSRContext, globalDebug));
    }

    // -------------------------------------------------------------------------
    // RenderGraph Setup
    // -------------------------------------------------------------------------

    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        if (!renderer->m_bTAAEnabled)
            return false;

        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Declare the TAA output texture
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width        = width;
            desc.m_NvrhiDesc.height       = height;
            desc.m_NvrhiDesc.format       = Renderer::HDR_COLOR_FORMAT;
            desc.m_NvrhiDesc.debugName    = "TAAOutput_RG";
            desc.m_NvrhiDesc.isUAV        = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            renderGraph.DeclareTexture(desc, g_RG_TAAOutput);
        }

        // Read inputs
        renderGraph.ReadTexture(g_RG_HDRColor);
        renderGraph.ReadTexture(g_RG_GBufferMotionVectors);
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_ExposureTexture);

        return true;
    }

    // -------------------------------------------------------------------------
    // Render
    // -------------------------------------------------------------------------

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Retrieve textures
        nvrhi::TextureHandle colorTex    = renderGraph.GetTexture(g_RG_HDRColor,             RGResourceAccessMode::Read);
        nvrhi::TextureHandle motionTex   = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read);
        nvrhi::TextureHandle depthTex    = renderGraph.GetTexture(g_RG_DepthTexture,         RGResourceAccessMode::Read);
        nvrhi::TextureHandle exposureTex = renderGraph.GetTexture(g_RG_ExposureTexture,      RGResourceAccessMode::Read);
        nvrhi::TextureHandle outputTex   = renderGraph.GetTexture(g_RG_TAAOutput,            RGResourceAccessMode::Write);

        // Transition output to UAV state for FSR3
        commandList->setTextureState(outputTex, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        // Get native D3D12 resources
        ID3D12GraphicsCommandList* nativeCmdList = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
        ID3D12Resource* colorRes    = colorTex->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
        ID3D12Resource* motionRes   = motionTex->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
        ID3D12Resource* depthRes    = depthTex->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
        ID3D12Resource* exposureRes = exposureTex->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
        ID3D12Resource* outputRes   = outputTex->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);

        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Jitter offset from current frame's view constants
        const srrhi::PlanarViewConstants& view     = renderer->m_Scene.m_View;
        const FfxApiFloatCoords2D jitterOffset{ view.m_PixelOffset.x, view.m_PixelOffset.y };
        const FfxApiDimensions2D  renderSize{ width, height };

        ffx::DispatchDescUpscale dispatch{};
        dispatch.commandList    = nativeCmdList;
        dispatch.color          = ffxApiGetResourceDX12(colorRes,  FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.depth          = ffxApiGetResourceDX12(depthRes,  FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.motionVectors  = ffxApiGetResourceDX12(motionRes, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.exposure       = ffxApiGetResourceDX12(exposureRes, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.reactive       = ffxApiGetResourceDX12(nullptr);
        dispatch.transparencyAndComposition = ffxApiGetResourceDX12(nullptr);
        dispatch.output         = ffxApiGetResourceDX12(outputRes, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

        dispatch.jitterOffset       = jitterOffset;
        dispatch.motionVectorScale  = FfxApiFloatCoords2D{ 1.0f, 1.0f }; // motion vectors are in pixel space
        dispatch.renderSize         = renderSize;
        dispatch.upscaleSize        = renderSize; // native resolution — no upscaling

        dispatch.enableSharpening   = renderer->m_TAASharpness > 0.0f;
        dispatch.sharpness          = renderer->m_TAASharpness;
        dispatch.frameTimeDelta     = static_cast<float>(renderer->GetFrameTimeMs());
        dispatch.preExposure        = std::max(renderer->m_PrevFrameExposure, 1e-6f);
        dispatch.reset              = false;
        dispatch.cameraNear         = FLT_MAX;                          // reversed-Z infinite projection
        dispatch.cameraFar          = renderer->m_Scene.m_Camera.GetProjection().nearZ;
        dispatch.cameraFovAngleVertical = renderer->m_Scene.m_Camera.GetProjection().fovY;
        dispatch.viewSpaceToMetersFactor = 0.0f;
        dispatch.flags              = renderer->m_bTAADebugView ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;

        FFX_CALL(ffx::Dispatch(m_FSRContext, dispatch));
    }

    const char* GetName() const override { return "FSR3 TAA"; }
};

REGISTER_RENDERER(TAARenderer);
