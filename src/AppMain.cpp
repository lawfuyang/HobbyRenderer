// The library builds with src/pch.h precompiled; this entry point does not (SDL defines its WinMain in the
// precompiled header), so it includes the same header directly.
#include "pch.h"

#include "Renderer.h"
#include "Config.h"
#include "PixCapture.h"
#include "RenderDocCapture.h"

// The application's entry point. It lives outside Renderer.cpp simply to keep the renderer's own translation unit
// down to the renderer.
//
// SDL_MAIN_HANDLED is defined project-wide, so SDL neither renames main() nor provides the Windows entry point:
// the windowed app is a WIN32-subsystem program and gets its own WinMain below.
static int RunApplication(int argc, char* argv[])
{
    // SDL_MAIN_HANDLED means SDL was not told by a WinMain that the app is ready; SDL_Init is what needs to know.
    SDL_SetMainReady();
    InstallOutputLog();

    // -----------------------------------------------------------------------
    // Normal renderer mode
    // -----------------------------------------------------------------------
    Renderer renderer{};
    Config::ParseCommandLine(argc, argv);

    // RenderDoc hooks D3D12CreateDevice through this module's import table, so the DLL has to be in the
    // process before Renderer::Initialize creates the device.
    if (Config::Get().m_bRenderDoc && g_RenderDoc.Load(Config::Get().m_RenderDocDllPath))
    {
        const std::filesystem::path captureStem = Config::Get().m_ScenePath.empty()
            ? std::filesystem::path{ "capture" }
            : std::filesystem::path{ Config::Get().m_ScenePath }.stem();
        g_RenderDoc.SetCapturePathTemplate((std::filesystem::path{ SDL_GetBasePath() } / "captures" / captureStem).string());
    }

    // PIX does not inject into the process the way RenderDoc does: PIXBeginCapture2 hands the frame to the
    // capturer module, which therefore has to be loadable before the capture is taken. The capture file name
    // comes from here, since PIX names nothing itself.
    if (Config::Get().m_bPix && g_Pix.Load(Config::Get().m_PixCapturerDllPath))
    {
        if (!Config::Get().m_PixCaptureDir.empty())
        {
            g_Pix.SetCapturePathTemplate(Config::Get().m_PixCaptureDir);
        }
        else
        {
            const std::filesystem::path captureStem = Config::Get().m_ScenePath.empty()
                ? std::filesystem::path{ "capture" }
                : std::filesystem::path{ Config::Get().m_ScenePath }.stem();
            g_Pix.SetCapturePathTemplate((std::filesystem::path{ SDL_GetBasePath() } / "captures" / captureStem).string());
        }
    }

    renderer.Initialize();

    renderer.Run();
    renderer.Shutdown();

    // Everything below happens in ~Renderer: its members release their GPU resources and m_RHI takes
    // the device (and the Context those resources reference) down last.
    SDL_Log("[Shutdown] Renderer going out of scope");
    return 0;
}

int main(int argc, char* argv[])
{
    return RunApplication(argc, argv);
}

// The WIN32-subsystem entry point. The command line is taken from the CRT's own argv copy, which is populated
// for both entry points and keeps the Unicode command line intact (the app's switches are ASCII paths).
int WINAPI WinMain(HINSTANCE /*instance*/, HINSTANCE /*previousInstance*/, LPSTR /*commandLine*/, int /*showCommand*/)
{
    return RunApplication(__argc, __argv);
}
