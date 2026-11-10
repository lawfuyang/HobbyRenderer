#pragma once



struct Config
{
    // Graphics settings
    bool m_EnableValidation = false;
    bool m_EnableGPUAssistedValidation = false;
    // Path to a scene to load (empty = none)
    std::string m_ScenePath = "";

    bool ExecutePerPass = false;
    bool ExecutePerPassAndWait = false;

    // Enable render graph aliasing
    bool m_EnableRenderGraphAliasing = true;

    // Disable sampler feedback entirely. Needed under tools that do not implement
    // it (e.g. RenderDoc 1.46). Texture streaming then falls back to requesting
    // the most detailed mip of every texture instead of the observed mips.
    bool m_DisableSamplerFeedback = false;

    // Opacity micromaps are opt-in: every measurement in this renderer had the masks costing more GPU time than
    // the alpha test they replace, and the shadow silhouette still drifted at deep subdivision. The panel toggle
    // does the same thing at runtime, but A/B runs have no way to press it.
    bool m_EnableOmm = false;

    // Opacity micromap bake overrides. 0 keeps OmmBakeParams' own default; the subdivision level and the
    // micro-triangle edge target decide how closely the masks follow a texel-level alpha silhouette, so runs
    // that compare shadow edges need to vary them without a rebuild.
    uint32_t m_OmmMaxLevel = 0;      // 0 = default, else 1..12
    float m_OmmTargetEdge = 0.0f;    // 0 = default, else texels per micro-triangle edge

    // Dumps the cooked cluster LOD of every primitive (clusters, groups, levels, triangles and worst error per
    // level, plus the phase 0 DAG and cut checks) once the scene is loaded.
    bool m_bClusterStats = false;
    // Cooks the per-primitive cluster LOD into the cooked mesh file. Nothing draws it yet.
    bool m_EnableClusterLod = true;
    // Frame on which --cluster-stats prints the GPU cluster counters. -1 = never.
    int m_ClusterStatsFrame = -1;

    // Cluster traversal (phase 1). The values mirror the ImGui controls so a capture run can be scripted.
    float m_ClusterPixelError = 1.0f;
    int m_ClusterForcedLOD = -1;      // -1 = from the metric, >= 0 = one level for every instance
    int m_ClusterMaxClusters = 0;     // 0 = scene-derived
    bool m_ClusterDisableCulling = false;
    uint32_t m_ClusterDebugMode = 0;
    bool m_ClusterFreezeCulling = false;

    // TAA is on by default; a pixel-exact A/B needs it off, and the panel was the only way to turn it off.
    bool m_bNoTAA = false;

    // Opens the Phase 5 texel-classification view on this primitive index (0 = the first primitive). The panel
    // exposes the same controls; this exists so a screenshot run can be scripted.
    int m_OmmDebugPrimitive = -1;    // -1 = view closed
    int m_OmmDebugMip = 0;

    // Writes the classification of every mip of this primitive's alpha texture to bin/omm_class_p<N>_mip<M>.png
    // and skips the view. Used to check a mip chain for a per-level drift.
    int m_OmmDumpMipsPrimitive = -1;

    // RenderDoc in-app capture (see RenderDocCapture.h). The DLL has to be loaded before the D3D12 device
    // is created, so this is a start-up decision rather than a runtime toggle.
    bool m_bRenderDoc = false;
    std::string m_RenderDocDllPath = ""; // empty = the usual locations (exe directory, then Program Files)
    int m_RenderDocCaptureFrame = -1;     // capture this frame number, then exit; -1 = never

    // Write bin/screenshot.jpg on this frame number and stop; -1 = never. Comparison runs use it so an A/B
    // pair does not depend on somebody pressing the screenshot key at the right moment.
    int m_ScreenshotFrame = -1;

    // PIX in-app capture (see PixCapture.h). The capturer module is loaded at start-up, and a capture of one
    // frame is taken with PIXBeginCapture2/PIXEndCapture rather than from the PIX UI.
    bool m_bPix = false;
    std::string m_PixCapturerDllPath = ""; // empty = exe directory, then the newest PIX installation
    std::string m_PixCaptureDir = "";      // empty = bin/captures/<scene stem>
    int m_PixCaptureFrame = -1;            // capture this frame number, then exit; -1 = never
    // Capture the scene load instead of a frame: the micromap and acceleration structure builds happen there,
    // so this is the only capture that contains the OMM builds and the geometries that link them.
    bool m_bPixCaptureLoad = false;

    // Rotate the view by this many degrees per second. Streaming and shadow problems that only appear while the
    // camera moves need a scripted camera to be reproducible.
    float m_CameraSpinDegreesPerSecond = 0.0f;

    // Run the RTXDI screen-space chain (reservoirs, shading, the SHARC query) and the NRD
    // denoiser at half the render resolution; CompositingPass upsamples the result with a
    // depth-aware filter. On by default because it takes ~10 ms off a Bistro frame at 1080p.
    // The panel exposes the same toggle, and `--full-res-di` turns it off for an A/B run.
    bool m_HalfResDI = true;

    // ReSTIR quality preset: 0 = Balanced (the FullSample's "Medium"), 1 = HighPerformance
    // (its "Fast"), 2 = HighQuality (its "Ultra"). Selected with --restir-preset.
    int m_ReSTIRPreset = 0;

    // Vertical field of view in degrees, overriding the scene camera's own value; 0 = use the scene's.
    // A comparison against another renderer needs the same projection on both sides, and nothing else in the
    // pipeline can move this: the value otherwise comes from the glTF camera.
    float m_FovDegrees = 0.0f;

    // Draw no ImGui windows. A screenshot shows the backbuffer as it was presented, so the panel of the frame
    // before sits in the pixels; an A/B image pair cannot carry that.
    bool m_bHideUI = false;

    // Bias on the auto-exposure, in stops, positive = brighter. Two renderers adapt the eye differently, so a
    // comparison has to put both at the same brightness before the differences underneath it can be read.
    float m_ExposureCompensationStops = 0.0f;

    // Pass ablations. Measuring which pass owns a difference means rendering without it, and the panel is not
    // reachable from a scripted run.
    bool m_bNoIndirect = false; // indirect lighting off (RTXDI/Deferred read no indirect)
    bool m_bNoDenoise = false;  // NRD REBLUR off
    bool m_bNoSky = false;      // sky drawn as black, and no sky ambient in the deferred shader

    // Sharpening applied by the TAA resolve, 0..1. The sample's resolve leaves more high-frequency detail than
    // an unsharpened resolve does, and this is the knob that closes that gap.
    float m_TaaSharpness = -1.0f; // < 0 = leave the renderer's default

    // Log one line per frame slower than this, with the streaming and VRAM state that frame saw, so a hitch can
    // be attributed to a subsystem after the fact. 0 = off.
    float m_HitchLogThresholdMs = 0.0f;

    // Write profiler_dump.html and stop on the first frame slower than this. The dump is what says which pass
    // absorbed the spike, and a hitch is far too rare to catch by hand. 0 = off.
    float m_HitchDumpThresholdMs = 0.0f;

    // Add more configuration options here as needed
    // int renderWidth = 1920;
    // int renderHeight = 1080;
    // float renderScale = 1.0f;

    static Config& Get() { return s_Instance; }
    static void ParseCommandLine(int argc, char* argv[]);

private:
    static Config s_Instance;
};

inline Config Config::s_Instance{};
