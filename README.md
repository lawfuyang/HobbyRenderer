# HobbyRenderer

A real-time renderer built in C++20 on Direct3D 12, with an NVIDIA RTX feature set: ReSTIR
direct and indirect illumination, a spatial-hash radiance cache, NRD denoising, ray-traced shadows,
opacity micromaps, GPU-driven meshlet geometry and sampler-feedback virtual texturing.

<img width="1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/241f11a4-709c-4598-a0d2-2364fb758e73" />
<img width="1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/d8f3c499-4f13-4549-8c5e-dd43b4b3b963" />

For how the pieces fit together — `IRenderer`, the render graph, resource binding, srrhi, ShaderIDs and
the build pipeline — see **[PIPELINE.md](PIPELINE.md)**. For capturing and reading PIX/RenderDoc frames,
see **[docs/pix-capture-debugging.md](docs/pix-capture-debugging.md)**.

## Features

### Lighting and denoising
- **ReSTIR DI**: stochastic direct-light sampling over local, emissive-triangle and environment lights,
  with ReGIR, temporal and spatial resampling, a boiling filter, and selectable bias correction
  (off / basic / ray-traced) per resampling stage
- **ReGIR (Onion mode)**: 5 detail and 10 coverage layers, camera-following, configurable cell size,
  build-sample count and jitter, matching the NVIDIA FullSample's defaults
- **ReSTIR GI**: indirect lighting through RTXDI's ReSTIR GI with temporal and spatial resampling,
  final-visibility rays, MIS and additive BRDF blending
- **SHARC**: spatial-hash radiance cache for screen-space indirect lighting — sparse update, temporal
  resolve and eviction, and a screen-space query pass with a heatmap debug view
- **Selectable indirect technique**: none, ReSTIR GI, SHARC, or GI + SHARC composited together
- **Quality presets**: `--restir-preset balanced|highperf|highquality` map to the FullSample's
  Medium / Fast / Ultra sample budgets (8/8/8, 4/4/4 and 16/16/16 ReGIR-RIS samples, with matching
  BRDF sample counts, spatial sample counts and bias-correction modes)
- **Half-resolution lighting**: the RTXDI chain and NRD can run at half the render resolution
  (`m_HalfResDI`, on by default) with a depth-aware joint-bilateral upsample in the compositing pass;
  `--full-res-di` overrides
- **NRD REBLUR denoising**: diffuse + specular REBLUR with anti-firefly filtering, 3×3 hit-distance
  reconstruction, and NRD-pack normal/roughness input
- **Ray-traced shadows**: inline ray queries for the sun, with an option to disable shadow tracing

### Geometry and acceleration structures
- **GPU-driven meshlet rendering**: mesh and amplification shaders over generated meshlets (64 vertices,
  96 triangles), with indirect dispatch built on the GPU
- **Two-phase occlusion culling**: frustum culling in phase 1, HZB occlusion culling with a phase-2 pass
  over the survivors, cone/silhouette culling, and per-instance LOD selection
- **Hierarchical Z-buffer**: built with AMD FidelityFX Single Pass Downsampler (min reduction)
- **Hierarchical LOD**: up to 8 levels generated with meshoptimizer progressive simplification, plus
  vertex quantization
- **Multi-LOD acceleration structures**: per-LOD BLAS with a `TLASPatch` compute pass keeping the TLAS
  instance addresses current as LODs change
- **Opacity micromaps (OMM)**: alpha-masked geometry is baked into 4-state micromaps (adaptive
  subdivision, `m_MaxLevel` 6, `m_TargetEdge` 3, selectable alpha mip), uploaded, and linked into the
  BLAS so masked rays resolve opacity in hardware instead of being treated as opaque. Baked from the
  masked base-colour DDS's alpha channel.
  CLI: `--enable-omm`, `--omm-max-level`, `--omm-target-edge`, `--omm-debug`, `--omm-debug-mip`,
  `--omm-dump-mips`. The panel has a debug view over texel classification, the micromap grid and the
  candidate heatmap.

### Materials, transparency and post-processing
- **PBR**: metallic/roughness workflow with normal, ORM and emissive maps;
  `KHR_materials_pbrSpecularGlossiness` is converted on import; IOR, transmission, thickness,
  attenuation distance/colour and thin-surface flags
- **Transparency**: forward pass for transparent geometry with transmission, spectral attenuation and
  volumetric absorption
- **HDR pipeline**: luminance histogram → EV100 auto-exposure (adaptation speed, EV clamps, exposure
  compensation) → PBR Neutral tone mapping → sRGB, plus an scRGB HDR path with a Reinhard rolloff above
  SDR white for wide-gamut displays
- **FSR3 TAA**: AMD FidelityFX temporal anti-aliasing with jitter cancellation, HDR aware, runtime
  sharpness (`--taa-sharpness`), a debug view, and exposure-aware pre-exposure
- **Bloom**: pyramid prefilter / downsample / upsample / composite with intensity, knee, threshold
  (only what is above SDR white blooms) and upsample radius
- **Atmosphere**: a Bruneton precomputed atmosphere drives the sky and the sun in every rendering mode

### Streaming
- **Sampler-feedback virtual texturing**: per-texture tile residency driven by GPU feedback, with an
  RTX Tiled Texture Manager backend, hysteresis eviction, asynchronous tile I/O on a worker pool,
  per-frame tile budgets, minmip tracking and packed-mip prefetch; `--disable-sampler-feedback` forces
  everything resident for A/B runs
- **Tooling**: tile residency debug view and per-texture residency statistics in the panel

### Engine and tooling
- **Render graph**: transient resource allocation with aliasing, automatic dependency ordering, pass
  toggling, per-pass submission (`--execute-per-pass`, `--execute-per-pass-and-wait`), optional
  aliasing (`--disable-rendergraph-aliasing`), and a visualizer for resource lifetimes, heap usage and
  pass access
- **srrhi**: a `.sr` shader-resource description compiled into C++ argument structs and HLSL binding
  code, so a pass's bindings are declared once (see PIPELINE.md)
- **ShaderIDs**: `.cfg`-driven shader table generation
- **Hot reload**: F5 recompiles and rebinds shaders without restarting
- **Multithreaded recording**: 12-worker `TaskScheduler` with `ParallelFor`, parallel command-list
  recording and deferred garbage collection
- **Profiling**: MicroProfile CPU scopes with HTML dump, per-scope GPU timer queries (`GpuPassTimers`,
  up to 256 scopes per frame), pipeline statistics, and a per-pass CPU/GPU table in the panel.
  `--hitch-log <ms>` logs frames slower than a threshold with streaming and VRAM state;
  `--hitch-dump <ms>` writes `profiler_dump.html` and exits on the first such frame
- **Capture support**: PIX3 markers, in-app PIX captures (`--pix`, `--pix-capture-frame`,
  `--pix-capture-load`, `--pix-capture-dir`) and in-app RenderDoc captures (`--renderdoc`,
  `--renderdoc-capture-frame`) — never both on one frame
- **Scene loading**: glTF 2.0 via cgltf, a versioned binary scene cache, mesh optimization, LOD
  generation, and an animation system (linear, step, cubic spline, SLERP, Catmull-Rom; transform and
  material-emissive channels, topologically sorted)
- **Camera state**: per-scene position, yaw and pitch persisted and restored on load
- **Screenshots**: `--screenshot-frame <n>` writes a lossless `bin/screenshot.png` and exits; `Ctrl+P`
  at runtime
- **Comparison switches** for A/B runs against another renderer: `--fov`, `--exposure`, `--hide-ui`,
  `--taa-sharpness`, and the pass ablations `--no-indirect`, `--no-denoise`, `--no-sky`

## Requirements

- Windows 10/11 x64
- A Direct3D 12 GPU with ray tracing and **shader model 6.9** — opacity micromaps and inline ray queries
  need it, which is why the Agility SDK 1.620.1 runtime is staged into `bin/`
- A recent NVIDIA driver for the RTX feature set (RTXDI, SHARC, NRD)

The renderer targets Direct3D 12 only. NVRHI is used as the GPU abstraction layer, but there is no
Vulkan or D3D11 backend in this build, and shaders compile to DXIL only.

## Building

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target HobbyRenderer
```

The executable lands in `bin/` for every configuration. Generators run as part of the build, ordered by
CMake dependencies:

| Target | Purpose |
|---|---|
| `build_srrhi` | `.sr` files → C++ binding headers and HLSL includes |
| `build_shaderids` | `.cfg` files → `src/shaders/ShaderIDs.h` |
| `build_shaders` | `.hlsl` files → DXIL in `bin/shaders/dxil` |
| `HobbyRenderer` | the executable |

Files under `src/shaders/srrhi/` and `src/shaders/ShaderIDs.h` are generated — edit the `.sr`/`.cfg`
sources instead.

## Command line

Every switch is parsed in `src/Config.cpp`; the useful ones:

| Switch | Effect |
|---|---|
| `--scene <path>` | scene to load (`.gltf`/`.glb`, or a `.scene.json` descriptor with models and lights) |
| `--restir-preset balanced\|highperf\|highquality` | ReSTIR sample budget (default `balanced`) |
| `--half-res-di` / `--full-res-di` | lighting and denoising resolution |
| `--fov <degrees>` | vertical FOV override |
| `--exposure <stops>` | auto-exposure bias |
| `--taa-sharpness <0..1>` | sharpening in the TAA resolve |
| `--hide-ui` | draw no ImGui windows |
| `--screenshot-frame <n>` | write `bin/screenshot.png` on frame n and exit |
| `--enable-omm`, `--omm-max-level`, `--omm-target-edge`, `--omm-debug` | opacity micromaps |
| `--disable-sampler-feedback`, `--disable-rendergraph-aliasing` | force full residency / no aliasing |
| `--execute-per-pass`, `--execute-per-pass-and-wait` | submit one render-graph pass at a time |
| `--pix[=<dll>]`, `--pix-capture-frame <n>`, `--pix-capture-load`, `--pix-capture-dir <dir>` | PIX |
| `--renderdoc[=<dll>]`, `--renderdoc-capture-frame <n>` | RenderDoc |
| `--camera-spin <deg/s>` | scripted rotation, for reproducing motion-only artifacts |
| `--hitch-log <ms>`, `--hitch-dump <ms>` | slow-frame logging / profiler dump |
| `--rhidebug`, `--rhidebug-gpu` | D3D12 debug layer / GPU-based validation |
| `--help` | print the list |

## Architecture

### Render graph
Passes declare what they read and write; the graph allocates transient targets, aliases them, orders the
passes, and exposes a visualizer. PIPELINE.md has the details.

### Renderers
Each pass is an `IRenderer` registered with the renderer registry, with its own `Setup`/`Render` and its
own GPU timer group. Passes can be toggled individually from the panel.

### Rendering modes
- **Normal**: deferred G-buffer (albedo, normals, ORM, emissive, motion vectors) plus the full RTX
  pipeline — ReSTIR DI/GI, SHARC, NRD, ray-traced shadows, TAA, bloom, tonemap
- **Reference Path Tracer**: unbiased Monte Carlo path tracer with next-event estimation, Russian
  roulette and BRDF importance sampling, accumulated progressively

### Scene
glTF 2.0 through cgltf, cached to a versioned binary, with meshlet and LOD generation, animation, a light
buffer for ray tracing (directional with angular size, point/spot, environment), and OMM baking.

## Dependencies

Fetched and built by CMake (versions from `CMakeLists.txt`):

- [SDL3](https://www.libsdl.org/) 3.4.20 — windowing and input
- [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) — D3D12 abstraction (forked copy under `external/nvrhi`)
- [Agility SDK](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12) 1.620.1 — D3D12 runtime and headers
- [ImGui](https://github.com/ocornut/imgui) — immediate-mode UI
- [cgltf](https://github.com/jkuhlmann/cgltf) 1.15 — glTF 2.0 parsing
- [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) v1.9.2609 — HLSL → DXIL
- [ShaderMake](https://github.com/NVIDIA-RTX/ShaderMake) — offline shader compilation
- [meshoptimizer](https://github.com/zeux/meshoptimizer) — optimization, quantization, simplification
- [MathLib](https://github.com/NVIDIA-RTX/MathLib) 11 — vector/matrix math
- [RTXDI](https://github.com/NVIDIA-RTX/RTXDI) — ReSTIR DI/GI, ReGIR, light presampling
- [NRD](https://github.com/NVIDIA-RTX/NRD) 4.18.0 — REBLUR denoising
- [SHARC](https://github.com/NVIDIAGameWorks/SHARC) — spatial hash radiance cache
- [rtxts-ttm](https://github.com/NVIDIA-RTX/rtxts-ttm) 0.7.1 — tiled texture manager for streaming
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.4.0 — FSR3 TAA and SPD
- [microprofile](https://github.com/jonasmr/microprofile) — CPU profiling
- [stb_image](https://github.com/nothings/stb) — image loading, PNG/JPG writing
- [WinPixEventRuntime](https://www.nuget.org/packages/WinPixEventRuntime) — PIX markers and capture

## Repository layout

```
src/                 engine sources (renderers, RHI, render graph, scene, streaming)
  Omm/               opacity micromap baker, runtime and debug view
  Streaming/         sampler-feedback tiles, async tile I/O, residency debug view
  FidelityFX/        vendored FidelityFX SDK (FSR3 upscaler and SPD)
  shaders/           HLSL, .sr interface files, shader tables
external/            dependencies (downloaded or vendored)
ShaderIDsGenerator/  ShaderIDs code generator
bin/                 output: executable, shaders, captures, screenshots
docs/                capture tooling and analysis notes
```
