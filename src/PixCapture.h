#pragma once

// In-application PIX GPU captures, next to the RenderDoc equivalent in RenderDocCapture.h.
//
// Two modules are involved and they are not the same thing:
//   - WinPixEventRuntime.dll carries the marker and capture entry points (PIXBeginEvent, PIXBeginCapture2,
//     PIXEndCapture). It is linked in, so nothing has to be loaded at runtime for those symbols.
//   - WinPixGpuCapturer.dll is the capturer PIXBeginCapture2 hands the frame to. PIX looks it up by name
//     next to the executable, so CMake stages a copy there; Load() also accepts an explicit path and will
//     fall back to the newest PIX installation on the machine.
//
// A capture is a bracket around exactly one frame, the same contract RenderDocCapture has: BeginCapture
// before the frame's first command list, EndCapture after the present.

class PixCapture
{
public:
    // capturerDllPath empty means "look in the usual places" (see the .cpp). Returns false, with a log
    // line, when neither that path nor the PIX installation yields a capturer; every other member then
    // degrades to a no-op.
    [[nodiscard]] bool Load(std::string_view capturerDllPath);
    void Unload();

    [[nodiscard]] bool IsAvailable() const { return m_bCapturerLoaded; }

    // False when the binary was built without PIX support at all, i.e. no WinPixEventRuntime was available
    // when CMake ran.
    static constexpr bool IsCompiledIn()
    {
#if defined(HOBBY_RENDERER_WITH_PIX)
        return true;
#else
        return false;
#endif
    }

    // PIX captures a swapchain through a window handle; without it a windowed app can capture a frame PIX
    // does not recognise as the target.
    void SetTargetWindow(void* windowHandle);

    // "<dir>/<stem>"; a capture is written as "<stem>-frame<N>.wpix" in that directory, which is created
    // when missing.
    void SetCapturePathTemplate(std::string_view pathTemplate);

    // The frame number only appears in the file name; PIX names nothing itself.
    [[nodiscard]] bool BeginCapture(uint64_t frameNumber);
    // Same bracket named "<stem><tag>.wpix". A frame capture starts at a frame boundary, so the micromap and
    // acceleration structure builds - which happen while the scene loads - are only inside a capture that
    // brackets the load itself.
    [[nodiscard]] bool BeginCaptureTagged(std::string_view tag);
    // Sends the frame to the capturer and waits for it to be written, so the path returned by
    // GetNewestCapturePath() is on disk by the time this returns.
    [[nodiscard]] bool EndCapture();

    // Absolute path of the capture EndCapture wrote; empty when it wrote none.
    [[nodiscard]] std::string GetNewestCapturePath() const;

private:
    void* m_CapturerModule = nullptr;      // HMODULE of WinPixGpuCapturer.dll
    bool  m_bCapturerLoaded = false;
    bool  m_bCaptureActive = false;
    std::string m_Directory;               // capture directory, used for the path scan
    std::string m_FileStem;                // scene name, prefixed onto the capture file name
    std::wstring m_FileName;               // PIX wants the target file as PCWSTR
    std::string m_LastCapturePath;
};

extern PixCapture g_Pix;
