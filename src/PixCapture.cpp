#include "PixCapture.h"

#if defined(__INTELLISENSE__) || defined(__JETBRAINS_IDE__)
#define HOBBY_RENDERER_WITH_PIX
#endif

#if defined(HOBBY_RENDERER_WITH_PIX)

#include <pix3.h>

PixCapture g_Pix;

namespace
{
// Newest .wpix in the directory, since the capturer writes the file itself and may rename what it was given.
std::string FindNewestCapture(const std::string& directory)
{
    std::error_code error;
    std::filesystem::path newestPath;
    std::filesystem::file_time_type newestTime{};

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error))
    {
        if (error || !entry.is_regular_file(error) || entry.path().extension() != ".wpix")
            continue;

        const std::filesystem::file_time_type writeTime = entry.last_write_time(error);
        if (error)
            continue;

        if (newestPath.empty() || writeTime > newestTime)
        {
            newestPath = entry.path();
            newestTime = writeTime;
        }
    }

    return newestPath.empty() ? std::string() : newestPath.string();
}
} // namespace

bool PixCapture::Load(std::string_view capturerDllPath)
{
    SDL_assert(!m_bCapturerLoaded && "PixCapture::Load called twice");

    // An explicit path first, then next to the executable (where CMake stages the copy), then whatever
    // installation PIX itself reports.
    std::wstring explicitPath;
    if (!capturerDllPath.empty())
    {
        explicitPath.assign(capturerDllPath.begin(), capturerDllPath.end());
    }

    HMODULE module = nullptr;
    if (!explicitPath.empty())
        module = LoadLibraryW(explicitPath.c_str());

    if (!module)
    {
        const std::filesystem::path besideExe{
            std::filesystem::path{ SDL_GetBasePath() } / "WinPixGpuCapturer.dll" };
        if (std::filesystem::exists(besideExe))
            module = LoadLibraryW(besideExe.wstring().c_str());
    }

    if (!module)
    {
        module = PIXLoadLatestWinPixGpuCapturerLibrary();
    }

    if (!module)
    {
        SDL_Log("[PIX] No WinPixGpuCapturer.dll found (looked for '%s', next to the executable, and in every "
                "PIX installation); programmatic captures are unavailable",
                capturerDllPath.empty() ? "<none given>" : std::string(capturerDllPath).c_str());
        return false;
    }

    m_CapturerModule = module;
    m_bCapturerLoaded = true;
    SDL_Log("[PIX] WinPixGpuCapturer loaded, programmatic GPU captures are available");
    return true;
}

void PixCapture::Unload()
{
    // The capturer is intentionally left loaded: PIX keeps state in it and the process is exiting anyway.
    m_bCapturerLoaded = false;
    m_CapturerModule = nullptr;
    m_bCaptureActive = false;
}

void PixCapture::SetTargetWindow(void* windowHandle)
{
    if (!m_bCapturerLoaded || !windowHandle)
        return;

    const HRESULT hr = PIXSetTargetWindow(static_cast<HWND>(windowHandle));
    if (FAILED(hr))
        SDL_Log("[PIX] PIXSetTargetWindow failed: 0x%08X", static_cast<unsigned>(hr));
}

void PixCapture::SetCapturePathTemplate(std::string_view pathTemplate)
{
    const std::filesystem::path templatePath{ pathTemplate };
    m_Directory = templatePath.parent_path().string();
    m_FileStem = templatePath.filename().string();
    std::error_code error;
    std::filesystem::create_directories(m_Directory, error);
}

bool PixCapture::BeginCapture(uint64_t frameNumber)
{
    return BeginCaptureTagged("-frame" + std::to_string(frameNumber));
}

bool PixCapture::BeginCaptureTagged(std::string_view tag)
{
    if (!m_bCapturerLoaded)
        return false;

    SDL_assert(!m_bCaptureActive && "PIX capture started while another one is in flight");
    if (m_bCaptureActive)
        return false;

    if (m_Directory.empty())
    {
        SDL_Log("[PIX] No capture path template set; refusing to capture");
        return false;
    }

    std::filesystem::path capturePath = std::filesystem::path{ m_Directory } /
        (m_FileStem + std::string{ tag } + ".wpix");
    m_FileName = capturePath.wstring();

    PIXCaptureParameters parameters{};
    parameters.GpuCaptureParameters.FileName = m_FileName.c_str();

    const HRESULT hr = PIXBeginCapture2(PIX_CAPTURE_GPU, &parameters);
    if (FAILED(hr))
    {
        SDL_Log("[PIX] PIXBeginCapture2 failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    m_bCaptureActive = true;
    SDL_Log("[PIX] Capture '%s' started", capturePath.filename().string().c_str());
    return true;
}

bool PixCapture::EndCapture()
{
    if (!m_bCaptureActive)
        return false;

    m_bCaptureActive = false;

    // discard = FALSE: save the capture. The call returns once the capturer has written the file.
    const HRESULT hr = PIXEndCapture(FALSE);
    if (FAILED(hr))
    {
        SDL_Log("[PIX] PIXEndCapture failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    m_LastCapturePath = FindNewestCapture(m_Directory);
    if (m_LastCapturePath.empty())
    {
        SDL_Log("[PIX] Capture ended but no .wpix appeared in '%s'", m_Directory.c_str());
        return false;
    }

    SDL_Log("[PIX] Capture written: %s", m_LastCapturePath.c_str());
    return true;
}

std::string PixCapture::GetNewestCapturePath() const
{
    return m_LastCapturePath;
}

#else // !HOBBY_RENDERER_WITH_PIX

PixCapture g_Pix;

bool PixCapture::Load(std::string_view capturerDllPath)
{
    SDL_Log("[PIX] This build has no PIX support (no WinPixEventRuntime was found when CMake ran); ignoring '%s'",
            capturerDllPath.empty() ? "<none given>" : std::string(capturerDllPath).c_str());
    return false;
}

void PixCapture::Unload() {}
void PixCapture::SetTargetWindow(void*) {}
void PixCapture::SetCapturePathTemplate(std::string_view) {}
bool PixCapture::BeginCapture(uint64_t) { return false; }
bool PixCapture::EndCapture() { return false; }
std::string PixCapture::GetNewestCapturePath() const { return std::string(); }

#endif // HOBBY_RENDERER_WITH_PIX
