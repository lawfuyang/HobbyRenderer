#include "RenderDocCapture.h"

#if defined(__INTELLISENSE__) || defined(__JETBRAINS_IDE__)
#define HOBBY_RENDERER_WITH_RENDERDOC
#endif

// renderdoc_app.h is not vendored: external/renderdoc/ holds a link (or copy) of the header belonging to the
// installed RenderDoc, made by CMakeLists.txt. When the machine has no RenderDoc, HOBBY_RENDERER_WITH_RENDERDOC
// stays undefined and everything below collapses into the no-op definitions at the end of this file.
#if defined(HOBBY_RENDERER_WITH_RENDERDOC)

#include <renderdoc_app.h>

namespace
{
    using GetAPIFn = int (__cdecl*)(RENDERDOC_Version version, void** outAPIPointers);

    RENDERDOC_API_1_0_0* GetAPI(HMODULE module)
    {
        SDL_assert(module != nullptr);

        GetAPIFn getAPI = reinterpret_cast<GetAPIFn>(reinterpret_cast<void*>(GetProcAddress(module, "RENDERDOC_GetAPI")));
        if (getAPI == nullptr)
            return nullptr;

        // Ask for the oldest table: every RenderDoc answers it, it is a prefix of the newer ones, and every
        // entry used below sits at the same offset in all of them. A newer library simply hands back its
        // larger struct, which is why the same pointer type serves both.
        RENDERDOC_API_1_0_0* api = nullptr;
        if (getAPI(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api)) == 0)
            return nullptr;

        return api;
    }

    RENDERDOC_API_1_0_0* AsAPI(void* api)
    {
        SDL_assert(api != nullptr);
        return static_cast<RENDERDOC_API_1_0_0*>(api);
    }
}

bool RenderDocCapture::Load(std::string_view dllPath)
{
    SDL_assert(m_Module == nullptr && m_API == nullptr);

    std::vector<std::filesystem::path> candidates;
    if (!dllPath.empty())
    {
        candidates.emplace_back(std::filesystem::path{ std::string(dllPath) });
    }
    else
    {
        candidates.emplace_back(std::filesystem::path{ SDL_GetBasePath() } / "renderdoc.dll");

        if (const char* programFiles = std::getenv("ProgramFiles"))
            candidates.emplace_back(std::filesystem::path{ programFiles } / "RenderDoc" / "renderdoc.dll");
    }

    for (const std::filesystem::path& candidate : candidates)
    {
        // LoadLibraryEx rather than LoadLibrary: renderdoc.dll sits next to the executable, and loading it as
        // a data-only module would skip its D3D12 hook installation, so the plain load is required.
        HMODULE module = LoadLibraryW(candidate.wstring().c_str());
        if (module == nullptr)
            continue;

        if (RENDERDOC_API_1_0_0* api = GetAPI(module))
        {
            m_Module = module;
            m_API = api;
            SDL_Log("[RenderDoc] Loaded %s", candidate.string().c_str());
            return true;
        }

        FreeLibrary(module);
    }

    if (!dllPath.empty())
        SDL_Log("[RenderDoc] Could not load renderdoc.dll from %s (error %lu)", std::string(dllPath).c_str(), GetLastError());
    else
        SDL_Log("[RenderDoc] No renderdoc.dll found; in-app captures are unavailable. Pass --renderdoc=<path> to point at one.");
    return false;
}

void RenderDocCapture::Unload()
{
    // The hooks stay installed for the life of the process; dropping the API table is all the renderer needs.
    m_API = nullptr;
    m_Module = nullptr;
}

void RenderDocCapture::SetCapturePathTemplate(std::string_view pathTemplate)
{
    if (m_API == nullptr)
        return;

    const std::string path{ pathTemplate };
    AsAPI(m_API)->SetCaptureFilePathTemplate(path.c_str());
    SDL_Log("[RenderDoc] Capture path template: %s", path.c_str());
}

void RenderDocCapture::BeginFrameCapture()
{
    if (m_API == nullptr)
        return;

    RENDERDOC_API_1_0_0* api = AsAPI(m_API);
    if (api->IsFrameCapturing() != 0)
    {
        // Two overlapping captures are undefined behaviour (crashes included), so never start a second one.
        SDL_Log("[RenderDoc] A frame capture is already running; ignoring the request");
        return;
    }

    api->StartFrameCapture(nullptr, nullptr);
}

bool RenderDocCapture::EndFrameCapture()
{
    if (m_API == nullptr)
        return false;

    RENDERDOC_API_1_0_0* api = AsAPI(m_API);
    const bool bCaptured = api->EndFrameCapture(nullptr, nullptr) != 0;

    if (bCaptured)
    {
        const std::string path = GetNewestCapturePath();
        SDL_Log("[RenderDoc] Capture written: %s", path.empty() ? "<unknown>" : path.c_str());
    }
    else
    {
        SDL_Log("[RenderDoc] EndFrameCapture failed: nothing was captured");
    }

    return bCaptured;
}

std::string RenderDocCapture::GetNewestCapturePath() const
{
    if (m_API == nullptr)
        return {};

    RENDERDOC_API_1_0_0* api = AsAPI(m_API);
    const uint32_t captureCount = api->GetNumCaptures();
    if (captureCount == 0)
        return {};

    char filename[1024] = {};
    uint32_t pathLength = static_cast<uint32_t>(sizeof(filename));
    uint64_t timestamp = 0;
    if (api->GetCapture(captureCount - 1, filename, &pathLength, &timestamp) == 0)
        return {};

    return std::string{ filename };
}

#else

// Built without RenderDoc: every entry point is still defined so no call site needs a preprocessor branch, and
// IsAvailable() stays false, which is what greys out the panel control and makes the frame loop skip capture.
bool RenderDocCapture::Load(std::string_view)
{
    SDL_Log("[RenderDoc] This build has no RenderDoc support: renderdoc_app.h was not found when CMake ran, so "
            "in-app captures are disabled.");
    return false;
}

void RenderDocCapture::Unload()
{
}

void RenderDocCapture::SetCapturePathTemplate(std::string_view)
{
}

void RenderDocCapture::BeginFrameCapture()
{
}

bool RenderDocCapture::EndFrameCapture()
{
    return false;
}

std::string RenderDocCapture::GetNewestCapturePath() const
{
    return {};
}

#endif

RenderDocCapture g_RenderDoc;
