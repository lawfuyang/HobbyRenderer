#include "Config.h"
#include "shaders/srrhi/cpp/Common.h"

namespace
{

// ---------------------------------------------------------------------------
// Command line reading
//
// The switch chain in ParseCommandLine is what gives a flag its meaning; this cursor only owns the mechanics that
// every flag repeats — consuming the value that follows it, and the one failure they all share (a flag whose value
// is missing). The readers turn that into the assignment and the log line, so a flag costs one line:
//
//     else if (args.Is("--debug")) ReadInt(args, m_DebugMode, "Debug view mode");
// ---------------------------------------------------------------------------
struct CmdArgs
{
    int m_Argc;
    char** m_Argv;
    int m_Index = 0;

    CmdArgs(int argc, char** argv) : m_Argc(argc), m_Argv(argv) {}

    bool Next()
    {
        ++m_Index;
        return m_Index < m_Argc;
    }

    const char* Current() const { return m_Argv[m_Index]; }

    bool Is(const char* name) const { return std::strcmp(Current(), name) == 0; }

    // A flag that carries its value inside the same argument ("--renderdoc=<path>"): returns the text after the
    // prefix, or null when this argument is a different flag.
    const char* AttachedValue(const char* prefix) const
    {
        const size_t length = std::strlen(prefix);
        return std::strncmp(Current(), prefix, length) == 0 ? Current() + length : nullptr;
    }

    // The argument after the current one. A flag that needs a value and has none is a bug in the invocation, and
    // every such flag reports it the same way.
    const char* Value()
    {
        if (m_Index + 1 >= m_Argc)
        {
            SDL_LOG_ASSERT_FAIL("Command line flag without a value", "[Config] Missing value for %s", Current());
            return nullptr;
        }

        return m_Argv[++m_Index];
    }
};

// The readers return false only when the flag had no value (already logged), leaving the target untouched, so a
// clamped or validated flag can skip its own assignment with the same `if`.
bool ReadBool(CmdArgs& args, bool& target, const char* meaning)
{
    const char* value = args.Value();
    if (!value)
        return false;

    target = std::atoi(value) != 0;
    SDL_Log("[Config] %s: %s", meaning, target ? "enabled" : "disabled");
    return true;
}

bool ReadInt(CmdArgs& args, int& target, const char* meaning)
{
    const char* value = args.Value();
    if (!value)
        return false;

    target = std::atoi(value);
    SDL_Log("[Config] %s: %d", meaning, target);
    return true;
}

bool ReadUInt(CmdArgs& args, uint32_t& target, const char* meaning)
{
    const char* value = args.Value();
    if (!value)
        return false;

    target = static_cast<uint32_t>(std::atoi(value));
    SDL_Log("[Config] %s: %u", meaning, target);
    return true;
}

bool ReadFloat(CmdArgs& args, float& target, const char* meaning)
{
    const char* value = args.Value();
    if (!value)
        return false;

    target = static_cast<float>(std::atof(value));
    SDL_Log("[Config] %s: %.2f", meaning, target);
    return true;
}

bool ReadString(CmdArgs& args, std::string& target, const char* meaning)
{
    const char* value = args.Value();
    if (!value)
        return false;

    target = value;
    SDL_Log("[Config] %s: %s", meaning, target.c_str());
    return true;
}

void SetFlag(bool& target, bool value, const char* message)
{
    target = value;
    SDL_Log("[Config] %s", message);
}

void PrintHelp()
{
    SDL_Log("Hobby Renderer - Command Line Options:");
    SDL_Log("  --rhidebug                       Enable graphics API validation layers");
    SDL_Log("  --rhidebug-gpu                   Enable GPU-assisted validation (requires --rhidebug)");
    SDL_Log("  --execute-per-pass               Execute command lists per pass");
    SDL_Log("  --execute-per-pass-and-wait      Wait for idle after each pass execution");
    SDL_Log("  --disable-rendergraph-aliasing   Disable render graph aliasing");
    SDL_Log("  --frame-pipelining                Record frame N+1 while the GPU executes frame N (default)");
    SDL_Log("  --no-frame-pipelining             Wait for the GPU before recording each frame (serialized)");
    SDL_Log("  --disable-sampler-feedback       Disable sampler feedback (streaming requests the finest mip)");
    SDL_Log("  --enable-omm                     Build opacity micromaps (off by default; needs DXR 1.2)");
    SDL_Log("  --omm-max-level <n>              OMM bake subdivision level cap (default 6, valid 1..12)");
    SDL_Log("  --omm-target-edge <f>            OMM bake micro-triangle edge target in texels (default 3.0)");
    SDL_Log("  --omm-debug <primitive>          Open the OMM texel-classification view on this primitive");
    SDL_Log("  --omm-debug-mip <n>              Alpha mip the OMM debug view classifies (default 0)");
    SDL_Log("  --omm-dump-mips <primitive>      Dump every alpha mip classification of that primitive to bin/");
    SDL_Log("  --renderdoc[=<path>]             Load renderdoc.dll for in-app captures (before the device exists)");
    SDL_Log("  --renderdoc-capture-frame <n>    Capture frame n and exit (implies --renderdoc)");
    SDL_Log("  --pix[=<path>]                   Load WinPixGpuCapturer.dll for in-app PIX captures");
    SDL_Log("  --pix-capture-frame <n>          Take a PIX GPU capture of frame n and exit (implies --pix)");
    SDL_Log("  --pix-capture-load               Take a PIX GPU capture of the scene load: micromap and BLAS builds (implies --pix)");
    SDL_Log("  --pix-capture-dir <dir>          Where PIX captures are written (default bin/captures/<scene>)");
    SDL_Log("  --screenshot-frame <n>           Write screenshot.png on frame n and exit");
    SDL_Log("  --hide-ui                        Draw no ImGui windows, so a screenshot is only the render");
    SDL_Log("  --exposure <stops>               Bias the auto-exposure; positive is brighter");
    SDL_Log("  --no-indirect                    Ablation: no indirect lighting");
    SDL_Log("  --no-denoise                     Ablation: no NRD denoising");
    SDL_Log("  --no-sky                         Ablation: no sky and no sky ambient");
    SDL_Log("  --taa-sharpness <0..1>           Sharpening applied by the TAA resolve");
    SDL_Log("  --cluster <0|1>                  Cook the per-primitive cluster LOD into the cooked mesh (default 1)");
    SDL_Log("  --cluster-stats                  Dump the cluster LOD of every primitive and run the phase 0 checks");
    SDL_Log("  --cluster-stats-frame <n>        Print the GPU cluster counters on frame n");
    SDL_Log("  --cluster-error <px>             Cluster LOD threshold in pixels (default 1.0)");
    SDL_Log("  --cluster-force-lod <n>          Force one cluster LOD level; -1 = pick from the metric");
    SDL_Log("  --cluster-max <n>                Cap the number of clusters drawn (0 = scene-derived)");
    SDL_Log("  --cluster-no-culling             Request every cluster of every visible instance");
    SDL_Log("  --cluster-debug <n>              Cluster debug view: 0 off, 1 clusters, 2 lod level, 3 bounds (these are Debug Modes 12-14), 4 counters");
    SDL_Log("  --force-lod <n>                  Force the meshlet path's LOD index; -1 = automatic (A/B against the cluster cut)");
    SDL_Log("  --cluster-draw <0|1>             Draw through the cluster mesh shader; 0 keeps the legacy meshlet draw");
    SDL_Log("  --cluster-two-pass <0|1>         Re-test what the first cluster pass rejected against this frame's depth");
    SDL_Log("  --cluster-discrete-lod <0|1>     Draw instances whose cut fits one LOD level from that level alone (default 1)");
    SDL_Log("  --debug <n>                      Base pass debug view (0 none, 4 albedo, 3 world normals, 12-15 cluster views)");
    SDL_Log("  --no-rt-shadows                  Disable ray-traced sun shadows (isolates the draw path in an A/B)");
    SDL_Log("  --no-restir-di                   Disable ReSTIR DI, so the sun shadow is one hard ray per pixel");
    SDL_Log("  --no-auto-exposure               Fixed exposure (no adaptation history in an exactness A/B)");
    SDL_Log("  --cluster-freeze-culling         Freeze the camera used for cluster culling (A/B determinism)");
    SDL_Log("  --stream-budget <MiB>            VRAM budget for texture + cluster streaming (0 = the OS budget)");
    SDL_Log("  --cluster-stream-age <frames>    Untouched frames before a resident group is unloaded (default 16)");
    SDL_Log("  --cluster-io-mb-per-frame <MiB>  Group blob bytes streamed per frame from the cooked file (default 16, 0 = uncapped)");
    SDL_Log("  --rt-max-scene-mb <MiB>          VRAM a scene's vertices+indices+BLAS may need before RT is turned off (0 = GPU budget)");
    SDL_Log("  --stream-import <0|1>            Override the cook's shape: 1 cooks one primitive at a time into resumable part files,");
    SDL_Log("                                   0 cooks the whole scene in memory (default: automatic, from the scene's decoded size)");
    SDL_Log("  --import-limit-primitives <n>    Stop the streamed import after n primitives (0 = all)");
    SDL_Log("  --split-primitive-tris <n>       Split primitives into n-triangle parts (0 = never)");
    SDL_Log("  --cluster-stream-stats-frame <n> Print the streaming residency stats on frame n");
    SDL_Log("  --no-taa                         Disable TAA (pixel-exact A/B and captures)");
    SDL_Log("  --restir-preset <name>           balanced (default, matches FullSample Medium), highperf, highquality");
    SDL_Log("  --restir-checkerboard <0|1>      Override the preset's checkerboard rendering (Balanced/highperf on, highquality off)");
    SDL_Log("  --fov <degrees>                  Override the vertical field of view; default is the scene camera's");
    SDL_Log("  --camera-spin <deg/s>            Rotate the view automatically, for reproduction runs");
    SDL_Log("  --hitch-log <ms>                 Log every frame slower than this with streaming/VRAM state");
    SDL_Log("  --hitch-dump <ms>                Dump the profiler and exit on the first frame slower than this");
    SDL_Log("  --target-fps <n>                 Frame pacer in FPS; 0 runs the loop unpaced (CPU/GPU overlap A/B)");
    SDL_Log("  --dump-rendergraph <n>           Write the render graph Markdown export after frame n");
    SDL_Log("  --rendergraph-strict-access      Assert when a pass binds a graph resource it did not declare");
    SDL_Log("  --rendergraph-own-barriers       Let the render graph place the barriers of its transients");
    SDL_Log("  --relayout                       Rebuild the render graph's transient layout once");
    SDL_Log("  --camera <n>                     Apply the scene's glTF camera n after load (-1 = the saved view)");
    SDL_Log("  --scene <path>                   Load the specified scene file");
    SDL_Log("  --help, -h                       Show this help message");
}

} // namespace

void Config::ParseCommandLine(int argc, char* argv[])
{
    Config& cfg = s_Instance;

    for (CmdArgs args{ argc, argv }; args.Next(); )
    {
        if (args.Is("--rhidebug"))
        {
            SetFlag(cfg.m_EnableValidation, true, "Validation enabled via command line");
        }
        else if (args.Is("--rhidebug-gpu"))
        {
            SetFlag(cfg.m_EnableGPUAssistedValidation, true, "GPU-assisted validation enabled via command line");
        }
        else if (args.Is("--scene"))
        {
            ReadString(args, cfg.m_ScenePath, "Scene");
        }
        else if (args.Is("--execute-per-pass"))
        {
            SetFlag(cfg.ExecutePerPass, true, "Execute per pass enabled via command line");
        }
        else if (args.Is("--execute-per-pass-and-wait"))
        {
            SetFlag(cfg.ExecutePerPassAndWait, true, "Execute per pass and wait enabled via command line");
        }
        else if (args.Is("--disable-rendergraph-aliasing"))
        {
            SetFlag(cfg.m_EnableRenderGraphAliasing, false, "Render graph aliasing disabled via command line");
        }
        else if (args.Is("--frame-pipelining"))
        {
            SetFlag(cfg.m_EnableFramePipelining, true, "Frame pipelining enabled via command line");
        }
        else if (args.Is("--no-frame-pipelining"))
        {
            SetFlag(cfg.m_EnableFramePipelining, false, "Frame pipelining disabled via command line");
        }
        else if (args.Is("--disable-sampler-feedback"))
        {
            SetFlag(cfg.m_DisableSamplerFeedback, true, "Sampler feedback disabled via command line");
        }
        else if (args.Is("--enable-omm"))
        {
            SetFlag(cfg.m_EnableOmm, true, "Opacity micromaps enabled via command line");
        }
        else if (args.Is("--cluster"))
        {
            ReadBool(args, cfg.m_EnableClusterLod, "Cluster LOD");
        }
        else if (args.Is("--cluster-stats"))
        {
            SetFlag(cfg.m_bClusterStats, true, "Cluster LOD statistics and checks enabled via command line");
        }
        else if (args.Is("--cluster-stats-frame"))
        {
            ReadInt(args, cfg.m_ClusterStatsFrame, "Cluster counters printed on frame");
        }
        else if (args.Is("--cluster-error"))
        {
            ReadFloat(args, cfg.m_ClusterPixelError, "Cluster pixel error threshold (px)");
        }
        else if (args.Is("--cluster-force-lod"))
        {
            ReadInt(args, cfg.m_ClusterForcedLOD, "Cluster LOD level forced to");
        }
        else if (args.Is("--cluster-max"))
        {
            ReadInt(args, cfg.m_ClusterMaxClusters, "Cluster count capped at");
        }
        else if (args.Is("--cluster-no-culling"))
        {
            SetFlag(cfg.m_ClusterDisableCulling, true,
                "Cluster culling disabled: every cluster of a visible instance is requested");
        }
        else if (args.Is("--cluster-draw"))
        {
            ReadInt(args, cfg.m_ClusterDrawEnabled, "Cluster draw (-1 follow cluster LOD, 0 legacy meshlet path)");
        }
        else if (args.Is("--force-lod"))
        {
            ReadInt(args, cfg.m_ForcedLOD, "Meshlet LOD forced to");
        }
        else if (args.Is("--no-rt-shadows"))
        {
            SetFlag(cfg.m_bNoRTShadows, true, "Ray-traced shadows disabled");
        }
        else if (args.Is("--no-restir-di"))
        {
            SetFlag(cfg.m_bNoReSTIRDI, true, "ReSTIR DI disabled: sun shadow from the deferred pass' own ray");
        }
        else if (args.Is("--target-fps"))
        {
            if (ReadUInt(args, cfg.m_TargetFPS, "Frame pacer (FPS)") && cfg.m_TargetFPS == 0)
            {
                SDL_Log("[Config] Frame pacer off: the loop runs as fast as it can record");
            }
        }
        else if (args.Is("--dump-rendergraph"))
        {
            ReadInt(args, cfg.m_RenderGraphDumpFrame, "Render graph export on frame");
        }
        else if (args.Is("--rendergraph-strict-access"))
        {
            SetFlag(cfg.m_bStrictRenderGraphAccess, true,
                "Render graph access checking on: undeclared bindings assert");
        }
        else if (args.Is("--rendergraph-own-barriers"))
        {
            SetFlag(cfg.m_bOwnRenderGraphBarriers, true, "Render graph owns the transients' barriers");
        }
        else if (args.Is("--relayout"))
        {
            SetFlag(cfg.m_bRelayout, true, "Render graph layout will be rebuilt once");
        }
        else if (args.Is("--camera"))
        {
            ReadInt(args, cfg.m_CameraIndex, "glTF camera index");
        }
        else if (args.Is("--debug"))
        {
            ReadInt(args, cfg.m_DebugMode, "Debug view mode");
        }
        else if (args.Is("--cluster-debug"))
        {
            const char* value = args.Value();
            if (!value)
                continue;

            // The cluster views are Debug Modes of their own now, so 1-3 select them there and get the debug
            // mode's settings (no bloom, no auto exposure); 4 is the traversal's own counters.
            const int mode = std::atoi(value);
            if (mode == 4)
            {
                cfg.m_ClusterDebugMode = 4;
                SDL_Log("[Config] Cluster debug view mode 4 (traversal counters)");
            }
            else if (mode >= 1 && mode <= 3)
            {
                cfg.m_DebugMode = srrhi::CommonConsts::DEBUG_MODE_CLUSTERS + (mode - 1);
                SDL_Log("[Config] Cluster debug view mode %d maps to Debug Mode %d", mode, cfg.m_DebugMode);
            }
            else if (mode != 0)
            {
                SDL_Log("[Config] Cluster debug view %d does not exist (0 off, 1 clusters, 2 lod level, 3 bounds, 4 counters)",
                    mode);
            }
        }
        else if (args.Is("--cluster-freeze-culling"))
        {
            SetFlag(cfg.m_ClusterFreezeCulling, true, "Cluster culling camera frozen");
        }
        else if (args.Is("--cluster-two-pass"))
        {
            ReadBool(args, cfg.m_ClusterTwoPass, "Cluster two-pass culling");
        }
        else if (args.Is("--cluster-discrete-lod"))
        {
            ReadBool(args, cfg.m_ClusterDiscreteLod, "Cluster discrete LOD");
        }
        else if (args.Is("--stream-budget"))
        {
            int mib = 0;
            if (ReadInt(args, mib, "Streaming VRAM budget (MiB)"))
                cfg.m_StreamingBudgetMiB = mib > 0 ? static_cast<uint64_t>(mib) : 0;
        }
        else if (args.Is("--cluster-stream-age"))
        {
            int frames = 0;
            if (ReadInt(args, frames, "Cluster stream age threshold (frames)"))
                cfg.m_ClusterStreamAgeThreshold = static_cast<uint32_t>(std::max(1, frames));
        }
        else if (args.Is("--cluster-io-mb-per-frame"))
        {
            int mib = 0;
            if (ReadInt(args, mib, "Cluster load budget (MB/frame)"))
                cfg.m_ClusterLoadBytesPerFrame = static_cast<uint32_t>(std::max(0, mib)) * 1024u * 1024u;
        }
        else if (args.Is("--stream-import"))
        {
            bool stream = false;
            if (ReadBool(args, stream, "Streamed glTF import"))
            {
                cfg.m_bStreamImport = stream;
                cfg.m_StreamImportPolicy = stream ? 1 : 0;
            }
        }
        else if (args.Is("--import-limit-primitives"))
        {
            int primitives = 0;
            if (ReadInt(args, primitives, "Streamed import primitive limit"))
                cfg.m_ImportPrimitiveLimit = static_cast<uint32_t>(std::max(0, primitives));
        }
        else if (args.Is("--split-primitive-tris"))
        {
            int triangles = 0;
            if (ReadInt(args, triangles, "Primitives split every N triangles (0 = never)"))
                cfg.m_SplitPrimitiveTriangles = static_cast<uint32_t>(std::max(0, triangles));
        }
        else if (args.Is("--rt-max-scene-mb"))
        {
            int mib = 0;
            if (ReadInt(args, mib, "Ray tracing budget (MB, 0 = the GPU's streaming budget)"))
                cfg.m_RayTracingBudgetMiB = static_cast<uint32_t>(std::max(0, mib));
        }
        else if (args.Is("--cluster-stream-stats-frame"))
        {
            ReadInt(args, cfg.m_ClusterStreamStatsFrame, "Cluster streaming stats printed on frame");
        }
        else if (args.Is("--no-taa"))
        {
            SetFlag(cfg.m_bNoTAA, true, "TAA disabled via command line");
        }
        else if (args.Is("--restir-preset"))
        {
            const char* name = args.Value();
            if (!name)
                continue;

            struct Preset
            {
                const char* m_Name;
                int m_Value;
            };
            static constexpr Preset kPresets[] = {
                { "balanced", 0 },
                { "highperf", 1 },
                { "highquality", 2 },
            };

            int preset = -1;
            for (const Preset& candidate : kPresets)
            {
                if (std::strcmp(name, candidate.m_Name) == 0)
                    preset = candidate.m_Value;
            }

            if (preset < 0)
            {
                SDL_LOG_ASSERT_FAIL("Unknown ReSTIR preset", "[Config] Unknown ReSTIR preset: %s", name);
            }
            else
            {
                cfg.m_ReSTIRPreset = preset;
                SDL_Log("[Config] ReSTIR preset: %s", name);
            }
        }
        else if (args.Is("--restir-checkerboard"))
        {
            ReadInt(args, cfg.m_ReSTIRCheckerboard, "ReSTIR checkerboard (-1 preset decides, 0 off, 1 on)");
        }
        else if (args.Is("--fov"))
        {
            float degrees = 0.0f;
            if (!ReadFloat(args, degrees, "Vertical FOV (degrees)"))
                continue;

            if (degrees <= 0.0f || degrees >= 180.0f)
            {
                SDL_LOG_ASSERT_FAIL("Vertical FOV must be inside (0, 180) degrees", "[Config] Invalid --fov value");
            }
            else
            {
                cfg.m_FovDegrees = degrees;
            }
        }
        else if (args.Is("--hide-ui"))
        {
            SetFlag(cfg.m_bHideUI, true, "ImGui windows disabled");
        }
        else if (args.Is("--exposure"))
        {
            ReadFloat(args, cfg.m_ExposureCompensationStops, "Exposure compensation (stops)");
        }
        else if (args.Is("--no-indirect"))
        {
            SetFlag(cfg.m_bNoIndirect, true, "Indirect lighting disabled");
        }
        else if (args.Is("--no-denoise"))
        {
            SetFlag(cfg.m_bNoDenoise, true, "NRD denoising disabled");
        }
        else if (args.Is("--no-sky"))
        {
            SetFlag(cfg.m_bNoSky, true, "Sky disabled");
        }
        else if (args.Is("--no-auto-exposure"))
        {
            SetFlag(cfg.m_bNoAutoExposure, true,
                "Auto exposure disabled: fixed exposure, so an A/B is not history dependent");
        }
        else if (args.Is("--taa-sharpness"))
        {
            ReadFloat(args, cfg.m_TaaSharpness, "TAA sharpness");
        }
        else if (args.Is("--renderdoc"))
        {
            SetFlag(cfg.m_bRenderDoc, true, "RenderDoc enabled via command line");
        }
        else if (const char* dllPath = args.AttachedValue("--renderdoc="))
        {
            cfg.m_bRenderDoc = true;
            cfg.m_RenderDocDllPath = dllPath;
            SDL_Log("[Config] RenderDoc enabled via command line, dll: %s", dllPath);
        }
        else if (args.Is("--renderdoc-capture-frame"))
        {
            if (ReadInt(args, cfg.m_RenderDocCaptureFrame, "RenderDoc will capture frame"))
                cfg.m_bRenderDoc = true;
        }
        else if (args.Is("--pix"))
        {
            SetFlag(cfg.m_bPix, true, "PIX enabled via command line");
        }
        else if (const char* dllPath = args.AttachedValue("--pix="))
        {
            cfg.m_bPix = true;
            cfg.m_PixCapturerDllPath = dllPath;
            SDL_Log("[Config] PIX enabled via command line, capturer dll: %s", dllPath);
        }
        else if (args.Is("--pix-capture-frame"))
        {
            if (ReadInt(args, cfg.m_PixCaptureFrame, "PIX will capture frame"))
                cfg.m_bPix = true;
        }
        else if (args.Is("--pix-capture-dir"))
        {
            ReadString(args, cfg.m_PixCaptureDir, "PIX captures go to");
        }
        else if (args.Is("--pix-capture-load"))
        {
            SetFlag(cfg.m_bPix, true, "PIX will capture the scene load");
            cfg.m_bPixCaptureLoad = true;
        }
        else if (args.Is("--omm-max-level"))
        {
            ReadUInt(args, cfg.m_OmmMaxLevel, "OMM bake max subdivision level");
        }
        else if (args.Is("--omm-debug"))
        {
            ReadInt(args, cfg.m_OmmDebugPrimitive, "OMM debug view primitive");
        }
        else if (args.Is("--omm-dump-mips"))
        {
            ReadInt(args, cfg.m_OmmDumpMipsPrimitive, "Dumping every alpha mip classification of primitive");
        }
        else if (args.Is("--omm-debug-mip"))
        {
            ReadInt(args, cfg.m_OmmDebugMip, "OMM debug view mip");
        }
        else if (args.Is("--omm-target-edge"))
        {
            ReadFloat(args, cfg.m_OmmTargetEdge, "OMM bake target edge (texels per micro-triangle)");
        }
        else if (args.Is("--screenshot-frame"))
        {
            ReadInt(args, cfg.m_ScreenshotFrame, "Screenshot frame (writes screenshot.png and exits)");
        }
        else if (args.Is("--camera-spin"))
        {
            ReadFloat(args, cfg.m_CameraSpinDegreesPerSecond, "Camera spin (deg/s)");
        }
        else if (args.Is("--hitch-log"))
        {
            ReadFloat(args, cfg.m_HitchLogThresholdMs, "Logging frames slower than (ms)");
        }
        else if (args.Is("--hitch-dump"))
        {
            ReadFloat(args, cfg.m_HitchDumpThresholdMs, "Dumping the profiler on the first frame slower than (ms)");
        }
        else if (args.Is("--help") || args.Is("-h"))
        {
            PrintHelp();
        }
        else
        {
            SDL_Log("[Config] Unknown command line argument: %s", args.Current());
        }
    }
}
