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

    // Allow the CPU to record frame N+1 while the GPU still executes frame N. The render graph keeps
    // separate transient heap sets per frame in flight, so this costs one extra set of transient
    // memory. --no-frame-pipelining restores the fully serialized submission.
    bool m_EnableFramePipelining = true;

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
    // Cooks the per-primitive cluster LOD into the cooked mesh file. This is the load-time half of the switch: the
    // panel's "Enable Cluster LOD" checkbox only chooses the draw path at runtime, so a scene cooked with
    // --cluster 0 has no cluster data to draw from.
    bool m_EnableClusterLod = true;
    // Draws through the cluster mesh shader. -1 = follow m_EnableClusterLod, 0 = keep drawing through the legacy
    // meshlet path while the cluster data is still cooked, which is what an A/B needs: the cooked cache is the
    // same file for both legs, so neither run has to re-cook the scene. The panel's "Enable Cluster LOD" checkbox
    // continues this value at runtime.
    int m_ClusterDrawEnabled = -1;
    // Frame on which --cluster-stats prints the GPU cluster counters. -1 = never.
    int m_ClusterStatsFrame = -1;

    // Cluster traversal (phase 1). The values mirror the ImGui controls so a capture run can be scripted.
    float m_ClusterPixelError = 1.0f;
    int m_ClusterForcedLOD = -1;      // -1 = from the metric, >= 0 = one level for every instance
    int m_ClusterMaxClusters = 0;     // 0 = no clamp; each bucket's buffers are sized by its own worst case
    bool m_ClusterDisableCulling = false;
    uint32_t m_ClusterDebugMode = 0;
    bool m_ClusterFreezeCulling = false;
    bool m_ClusterTwoPass = false;
    // Discrete LOD (phase 6): an instance whose cut fits a single LOD level is seeded at that level's node and
    // drawn whole, and ray tracing is patched with the per-mesh LOD that matches it. Off = the continuous cut, i.e.
    // the pre-phase-6 image, for an A/B.
    bool m_ClusterDiscreteLod = true;

    // The VRAM budget both streaming systems spend: the texture tile heaps and the cluster group arena. 0 means
    // "whatever the OS says this application may use", which is the panel slider's default too.
    uint64_t m_StreamingBudgetMiB = 0;
    // Frames a resident group may go untouched before the age filter asks for it back.
    uint32_t m_ClusterStreamAgeThreshold = 16;
    // Bytes of group blobs copied out of the cooked file per frame. Bounds disk/PCIe bandwidth and the load
    // ring's stall; 0 = only the per-frame group cap applies.
    uint32_t m_ClusterLoadBytesPerFrame = 16u * 1024u * 1024u;
    // VRAM a scene's ray-tracing footprint (vertices + indices + BLAS) may use before every RT feature is turned
    // off for it. 0 = the GPU's streaming budget. `--rt-max-scene-mb` exists to force the oversized path.
    uint32_t m_RayTracingBudgetMiB = 0;
    // Streamed import: cook one primitive at a time and flush each to a part file, so a scene whose decoded glTF
    // does not fit in RAM can still be imported (docs/cluster-lod-upstream-review-2026-11-23.md §22).
    // The load sets this itself from the scene's own buffer sizes (SceneLoader::ApplyStreamImportPolicy), so a
    // scene that cannot be cooked in memory never tries to be; --stream-import pins the choice either way.
    bool m_bStreamImport = false;
    // -1 = decide from the scene's size, 0 = never stream, 1 = always stream. Set by --stream-import.
    int m_StreamImportPolicy = -1;
    // Streamed import stops after this many primitives (0 = all), for a first look at a huge scene.
    uint32_t m_ImportPrimitiveLimit = 0;
    // A primitive with more triangles than this is split into independent parts that are cooked and streamed on
    // their own. The cluster build's dedup, level-0 clusterisation and partition are serial within a part, so
    // splitting a huge primitive is what lets those phases use every core (report §22.4). 0 = never split.
    uint32_t m_SplitPrimitiveTriangles = 2000000;
    // Prints the streaming residency stats on this frame. -1 = never.
    int m_ClusterStreamStatsFrame = -1;

    // TAA is on by default; a pixel-exact A/B needs it off, and the panel was the only way to turn it off.
    bool m_bNoTAA = false;

    // srrhi::CommonConsts::DEBUG_MODE_*: the base pass' debug visualisation. Lighting noise hides a G-buffer
    // difference, so an exactness A/B compares one of these views instead of the composite.
    int m_DebugMode = 0;

    // Ray-traced sun shadows are the noisiest part of the composite; an A/B that isolates the draw path turns
    // them off (the panel toggle does the same at runtime).
    bool m_bNoRTShadows = false;

    // ReSTIR DI off: the sun shadow then comes from the deferred pass' own single shadow ray (hard shadows)
    // instead of the RTXDI chain, which is the path an OMM A/B compares on both sides.
    bool m_bNoReSTIRDI = false;

    // Auto exposure off: the adaptation history makes two runs of the SAME configuration differ by ~0.6/255 on
    // mean, which is larger than the shadow difference an exactness A/B is looking for.
    bool m_bNoAutoExposure = false;

    // The legacy meshlet path's LOD override: -1 = automatic. A cluster A/B compares the cluster cut at
    // forced level 0 against the meshlet path at LOD 0, and the panel was the only way to set it.
    int m_ForcedLOD = -1;

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

    // ReSTIR quality preset: 0 = Balanced (the FullSample's "Medium"), 1 = HighPerformance
    // (its "Fast"), 2 = HighQuality (its "Ultra"). Selected with --restir-preset.
    int m_ReSTIRPreset = 0;

    // ReSTIR checkerboard override: -1 = the quality preset decides (Balanced and HighPerformance
    // enable it, HighQuality does not), 0 = force off, 1 = force on. Selected with --restir-checkerboard.
    int m_ReSTIRCheckerboard = -1;

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

    // Frame pacer, in frames per second. The loop sleeps to hit it, so a CPU-bound frame shows a sleep bar and a
    // GPU that sits idle half the frame: how fast the CPU can submit cannot be measured while it is paced.
    // 0 = uncapped.
    uint32_t m_TargetFPS = 200;

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
