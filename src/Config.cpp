#include "Config.h"

void Config::ParseCommandLine(int argc, char* argv[])
{
    

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (std::strcmp(arg, "--rhidebug") == 0)
        {
            s_Instance.m_EnableValidation = true;
            SDL_Log("[Config] Validation enabled via command line");
        }
        else if (std::strcmp(arg, "--rhidebug-gpu") == 0)
        {
            s_Instance.m_EnableGPUAssistedValidation = true;
            SDL_Log("[Config] GPU-assisted validation enabled via command line");
        }
        else if (std::strcmp(arg, "--scene") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ScenePath = argv[++i];
                SDL_Log("[Config] Scene set via command line: %s", s_Instance.m_ScenePath.c_str());
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --scene", "[Config] Missing value for --scene");
            }
        }
        else if (std::strcmp(arg, "--execute-per-pass") == 0)
        {
            s_Instance.ExecutePerPass = true;
            SDL_Log("[Config] Execute per pass enabled via command line");
        }
        else if (std::strcmp(arg, "--execute-per-pass-and-wait") == 0)
        {
            s_Instance.ExecutePerPassAndWait = true;
            SDL_Log("[Config] Execute per pass and wait enabled via command line");
        }
        else if (std::strcmp(arg, "--disable-rendergraph-aliasing") == 0)
        {
            s_Instance.m_EnableRenderGraphAliasing = false;
            SDL_Log("[Config] Render graph aliasing disabled via command line");
        }
        else if (std::strcmp(arg, "--disable-sampler-feedback") == 0)
        {
            s_Instance.m_DisableSamplerFeedback = true;
            SDL_Log("[Config] Sampler feedback disabled via command line");
        }
        else if (std::strcmp(arg, "--enable-omm") == 0)
        {
            s_Instance.m_EnableOmm = true;
            SDL_Log("[Config] Opacity micromaps enabled via command line");
        }
        else if (std::strcmp(arg, "--half-res-di") == 0)
        {
            s_Instance.m_HalfResDI = true;
            SDL_Log("[Config] RTXDI + denoising at half resolution");
        }
        else if (std::strcmp(arg, "--full-res-di") == 0)
        {
            s_Instance.m_HalfResDI = false;
            SDL_Log("[Config] RTXDI + denoising at full resolution");
        }
        else if (std::strcmp(arg, "--cluster") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_EnableClusterLod = std::atoi(argv[++i]) != 0;
                SDL_Log("[Config] Cluster LOD %s via command line",
                    s_Instance.m_EnableClusterLod ? "enabled" : "disabled");
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --cluster", "[Config] Missing value for --cluster");
            }
        }
        else if (std::strcmp(arg, "--cluster-stats") == 0)
        {
            s_Instance.m_bClusterStats = true;
            SDL_Log("[Config] Cluster LOD statistics and checks enabled via command line");
        }
        else if (std::strcmp(arg, "--cluster-stats-frame") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ClusterStatsFrame = std::atoi(argv[++i]);
                SDL_Log("[Config] Cluster counters printed on frame %d", s_Instance.m_ClusterStatsFrame);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --cluster-stats-frame", "[Config] Missing value for --cluster-stats-frame");
            }
        }
        else if (std::strcmp(arg, "--cluster-error") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ClusterPixelError = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Cluster pixel error threshold: %.3f px", s_Instance.m_ClusterPixelError);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --cluster-error", "[Config] Missing value for --cluster-error");
            }
        }
        else if (std::strcmp(arg, "--cluster-force-lod") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ClusterForcedLOD = std::atoi(argv[++i]);
                SDL_Log("[Config] Cluster LOD level forced to %d", s_Instance.m_ClusterForcedLOD);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --cluster-force-lod", "[Config] Missing value for --cluster-force-lod");
            }
        }
        else if (std::strcmp(arg, "--cluster-max") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ClusterMaxClusters = std::atoi(argv[++i]);
                SDL_Log("[Config] Cluster count capped at %d", s_Instance.m_ClusterMaxClusters);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --cluster-max", "[Config] Missing value for --cluster-max");
            }
        }
        else if (std::strcmp(arg, "--cluster-no-culling") == 0)
        {
            s_Instance.m_ClusterDisableCulling = true;
            SDL_Log("[Config] Cluster culling disabled: every cluster of a visible instance is requested");
        }
        else if (std::strcmp(arg, "--cluster-debug") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ClusterDebugMode = static_cast<uint32_t>(std::atoi(argv[++i]));
                SDL_Log("[Config] Cluster debug view mode %u", s_Instance.m_ClusterDebugMode);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --cluster-debug", "[Config] Missing value for --cluster-debug");
            }
        }
        else if (std::strcmp(arg, "--cluster-freeze-culling") == 0)
        {
            s_Instance.m_ClusterFreezeCulling = true;
            SDL_Log("[Config] Cluster culling camera frozen");
        }
        else if (std::strcmp(arg, "--no-taa") == 0)
        {
            s_Instance.m_bNoTAA = true;
            SDL_Log("[Config] TAA disabled via command line");
        }
        else if (std::strcmp(arg, "--restir-preset") == 0)
        {
            if (i + 1 < argc)
            {
                const char* preset = argv[++i];
                if (std::strcmp(preset, "balanced") == 0)         s_Instance.m_ReSTIRPreset = 0;
                else if (std::strcmp(preset, "highperf") == 0)    s_Instance.m_ReSTIRPreset = 1;
                else if (std::strcmp(preset, "highquality") == 0) s_Instance.m_ReSTIRPreset = 2;
                else
                {
                    SDL_LOG_ASSERT_FAIL("Unknown ReSTIR preset", "[Config] Unknown ReSTIR preset");
                }
                SDL_Log("[Config] ReSTIR preset: %s", preset);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --restir-preset", "[Config] Missing value for --restir-preset");
            }
        }
        else if (std::strcmp(arg, "--fov") == 0)
        {
            if (i + 1 < argc)
            {
                const float degrees = static_cast<float>(std::atof(argv[++i]));
                if (degrees <= 0.0f || degrees >= 180.0f)
                {
                    SDL_LOG_ASSERT_FAIL("Vertical FOV must be inside (0, 180) degrees", "[Config] Invalid --fov value");
                }
                else
                {
                    s_Instance.m_FovDegrees = degrees;
                    SDL_Log("[Config] Vertical FOV overridden to %.2f degrees", degrees);
                }
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --fov", "[Config] Missing value for --fov");
            }
        }
        else if (std::strcmp(arg, "--hide-ui") == 0)
        {
            s_Instance.m_bHideUI = true;
            SDL_Log("[Config] ImGui windows disabled");
        }
        else if (std::strcmp(arg, "--exposure") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ExposureCompensationStops = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Exposure compensation %.2f stops", s_Instance.m_ExposureCompensationStops);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --exposure", "[Config] Missing value for --exposure");
            }
        }
        else if (std::strcmp(arg, "--no-indirect") == 0)
        {
            s_Instance.m_bNoIndirect = true;
            SDL_Log("[Config] Indirect lighting disabled");
        }
        else if (std::strcmp(arg, "--no-denoise") == 0)
        {
            s_Instance.m_bNoDenoise = true;
            SDL_Log("[Config] NRD denoising disabled");
        }
        else if (std::strcmp(arg, "--no-sky") == 0)
        {
            s_Instance.m_bNoSky = true;
            SDL_Log("[Config] Sky disabled");
        }
        else if (std::strcmp(arg, "--taa-sharpness") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_TaaSharpness = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] TAA sharpness %.2f", s_Instance.m_TaaSharpness);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --taa-sharpness", "[Config] Missing value for --taa-sharpness");
            }
        }
        else if (std::strcmp(arg, "--renderdoc") == 0)
        {
            s_Instance.m_bRenderDoc = true;
            SDL_Log("[Config] RenderDoc enabled via command line");
        }
        else if (std::strncmp(arg, "--renderdoc=", 12) == 0)
        {
            s_Instance.m_bRenderDoc = true;
            s_Instance.m_RenderDocDllPath = arg + 12;
            SDL_Log("[Config] RenderDoc enabled via command line, dll: %s", s_Instance.m_RenderDocDllPath.c_str());
        }
        else if (std::strcmp(arg, "--renderdoc-capture-frame") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_bRenderDoc = true;
                s_Instance.m_RenderDocCaptureFrame = std::atoi(argv[++i]);
                SDL_Log("[Config] RenderDoc will capture frame %d and exit", s_Instance.m_RenderDocCaptureFrame);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --renderdoc-capture-frame", "[Config] Missing value for --renderdoc-capture-frame");
            }
        }
        else if (std::strcmp(arg, "--pix") == 0)
        {
            s_Instance.m_bPix = true;
            SDL_Log("[Config] PIX enabled via command line");
        }
        else if (std::strncmp(arg, "--pix=", 6) == 0)
        {
            s_Instance.m_bPix = true;
            s_Instance.m_PixCapturerDllPath = arg + 6;
            SDL_Log("[Config] PIX enabled via command line, capturer dll: %s", s_Instance.m_PixCapturerDllPath.c_str());
        }
        else if (std::strcmp(arg, "--pix-capture-frame") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_bPix = true;
                s_Instance.m_PixCaptureFrame = std::atoi(argv[++i]);
                SDL_Log("[Config] PIX will capture frame %d and exit", s_Instance.m_PixCaptureFrame);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --pix-capture-frame", "[Config] Missing value for --pix-capture-frame");
            }
        }
        else if (std::strcmp(arg, "--pix-capture-dir") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_PixCaptureDir = argv[++i];
                SDL_Log("[Config] PIX captures go to %s", s_Instance.m_PixCaptureDir.c_str());
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --pix-capture-dir", "[Config] Missing value for --pix-capture-dir");
            }
        }
        else if (std::strcmp(arg, "--pix-capture-load") == 0)
        {
            s_Instance.m_bPix = true;
            s_Instance.m_bPixCaptureLoad = true;
            SDL_Log("[Config] PIX will capture the scene load");
        }
        else if (std::strcmp(arg, "--omm-max-level") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_OmmMaxLevel = static_cast<uint32_t>(std::atoi(argv[++i]));
                SDL_Log("[Config] OMM bake max subdivision level: %u", s_Instance.m_OmmMaxLevel);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --omm-max-level", "[Config] Missing value for --omm-max-level");
            }
        }
        else if (std::strcmp(arg, "--omm-debug") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_OmmDebugPrimitive = std::atoi(argv[++i]);
                SDL_Log("[Config] OMM debug view on primitive %d", s_Instance.m_OmmDebugPrimitive);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --omm-debug", "[Config] Missing value for --omm-debug");
            }
        }
        else if (std::strcmp(arg, "--omm-dump-mips") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_OmmDumpMipsPrimitive = std::atoi(argv[++i]);
                SDL_Log("[Config] Dumping every alpha mip classification of primitive %d", s_Instance.m_OmmDumpMipsPrimitive);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --omm-dump-mips", "[Config] Missing value for --omm-dump-mips");
            }
        }
        else if (std::strcmp(arg, "--omm-debug-mip") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_OmmDebugMip = std::atoi(argv[++i]);
                SDL_Log("[Config] OMM debug view mip: %d", s_Instance.m_OmmDebugMip);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --omm-debug-mip", "[Config] Missing value for --omm-debug-mip");
            }
        }
        else if (std::strcmp(arg, "--omm-target-edge") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_OmmTargetEdge = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] OMM bake target edge: %.2f texels per micro-triangle", s_Instance.m_OmmTargetEdge);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --omm-target-edge", "[Config] Missing value for --omm-target-edge");
            }
        }
        else if (std::strcmp(arg, "--screenshot-frame") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ScreenshotFrame = std::atoi(argv[++i]);
                SDL_Log("[Config] Screenshot will be written on frame %d and exit", s_Instance.m_ScreenshotFrame);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --screenshot-frame", "[Config] Missing value for --screenshot-frame");
            }
        }
        else if (std::strcmp(arg, "--camera-spin") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_CameraSpinDegreesPerSecond = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Camera will spin at %.1f deg/s", s_Instance.m_CameraSpinDegreesPerSecond);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --camera-spin", "[Config] Missing value for --camera-spin");
            }
        }
        else if (std::strcmp(arg, "--hitch-log") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_HitchLogThresholdMs = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Frames slower than %.1f ms will be logged", s_Instance.m_HitchLogThresholdMs);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --hitch-log", "[Config] Missing value for --hitch-log");
            }
        }
        else if (std::strcmp(arg, "--hitch-dump") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_HitchDumpThresholdMs = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Profiler will be dumped on the first frame slower than %.1f ms", s_Instance.m_HitchDumpThresholdMs);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --hitch-dump", "[Config] Missing value for --hitch-dump");
            }
        }
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            SDL_Log("Hobby Renderer - Command Line Options:");
            SDL_Log("  --rhidebug                       Enable graphics API validation layers");
            SDL_Log("  --rhidebug-gpu                   Enable GPU-assisted validation (requires --rhidebug)");
            SDL_Log("  --execute-per-pass               Execute command lists per pass");
            SDL_Log("  --execute-per-pass-and-wait      Wait for idle after each pass execution");
            SDL_Log("  --disable-rendergraph-aliasing   Disable render graph aliasing");
            SDL_Log("  --disable-sampler-feedback       Disable sampler feedback (streaming requests the finest mip)");
            SDL_Log("  --enable-omm                     Build opacity micromaps (off by default; needs DXR 1.2)");
            SDL_Log("  --omm-max-level <n>              OMM bake subdivision level cap (default 6, valid 1..12)");
            SDL_Log("  --omm-target-edge <f>            OMM bake micro-triangle edge target in texels (default 3.0)");
            SDL_Log("  --omm-debug <primitive>          Open the OMM texel-classification view on this primitive");
            SDL_Log("  --omm-debug-mip <n>              Alpha mip the OMM debug view classifies (default 0)");
            SDL_Log("  --omm-dump-mips <primitive>      Dump every alpha mip classification of that primitive to bin/");
            SDL_Log("  --renderdoc[=<path>]             Load renderdoc.dll for in-app captures (before the device exists)");
            SDL_Log("  --renderdoc-capture-frame <n>    Capture frame n and exit (implies --renderdoc)");
            SDL_Log("  --pix[=<path>]                  Load WinPixGpuCapturer.dll for in-app PIX captures");
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
            SDL_Log("  --half-res-di                    Run RTXDI and NRD at half the render resolution");
            SDL_Log("  --full-res-di                    Override: run RTXDI and NRD at full render resolution (default)");
            SDL_Log("  --cluster <0|1>                  Cook the per-primitive cluster LOD into the cooked mesh (default 1)");
            SDL_Log("  --cluster-stats                  Dump the cluster LOD of every primitive and run the phase 0 checks");
            SDL_Log("  --cluster-stats-frame <n>        Print the GPU cluster counters on frame n");
            SDL_Log("  --cluster-error <px>             Cluster LOD threshold in pixels (default 1.0)");
            SDL_Log("  --cluster-force-lod <n>          Force one cluster LOD level; -1 = pick from the metric");
            SDL_Log("  --cluster-max <n>                Cap the number of clusters drawn (0 = scene-derived)");
            SDL_Log("  --cluster-no-culling             Request every cluster of every visible instance");
            SDL_Log("  --cluster-debug <n>              Cluster debug view: 1 ids, 2 lod level, 3 bounds, 4 counters");
            SDL_Log("  --cluster-freeze-culling         Freeze the camera used for cluster culling (A/B determinism)");
            SDL_Log("  --no-taa                         Disable TAA (pixel-exact A/B and captures)");
            SDL_Log("  --restir-preset <name>           balanced (default, matches FullSample Medium), highperf, highquality");
            SDL_Log("  --fov <degrees>                  Override the vertical field of view; default is the scene camera's");
            SDL_Log("  --camera-spin <deg/s>            Rotate the view automatically, for reproduction runs");
            SDL_Log("  --hitch-log <ms>                 Log every frame slower than this with streaming/VRAM state");
            SDL_Log("  --hitch-dump <ms>                Dump the profiler and exit on the first frame slower than this");
            SDL_Log("  --scene <path>                   Load the specified scene file");
            SDL_Log("  --help, -h                       Show this help message");
        }
        else
        {
            SDL_Log("[Config] Unknown command line argument: %s", arg);
        }
    }
}
