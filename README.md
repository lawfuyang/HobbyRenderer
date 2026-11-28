# HobbyRenderer

A real-time renderer built in C++20 on Direct3D 12, with an NVIDIA RTX feature set: ReSTIR
direct and indirect illumination, a spatial-hash radiance cache, NRD denoising, ray-traced shadows,
opacity micromaps, GPU-driven cluster-LOD geometry with a visibility buffer, and sampler-feedback virtual
texturing.

<img width="1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/241f11a4-709c-4598-a0d2-2364fb758e73" />
<img width="1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/d8f3c499-4f13-4549-8c5e-dd43b4b3b963" />

### Cluster LOD
| None | Meshlets | LODs | Triangles |
|---|---|---|---|
| <img width="1920" height="1080" alt="none" src="https://github.com/user-attachments/assets/216b175a-20f4-488c-9314-6d845bb02190" /> | <img width="1920" height="1080" alt="clusters" src="https://github.com/user-attachments/assets/2b778981-9168-4452-8c9a-71cd996e1474" /> | <img width="1920" height="1080" alt="LODs" src="https://github.com/user-attachments/assets/55919719-94d5-44c5-ab18-cf393bf25680" /> | <img width="1920" height="1080" alt="triangles" src="https://github.com/user-attachments/assets/10078f4c-12b2-4e0c-888c-b85d8fe2c25c" /> |

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
- **Checkerboard rendering**: ReSTIR DI/GI and the SHARC query trace half the pixels per frame
  (`rtxdi::CheckerboardMode::Black`, on for the Balanced and High Performance presets) and NRD
  reconstructs the other field; `--restir-checkerboard 0|1` overrides what the preset chose
- **NRD REBLUR denoising**: diffuse + specular REBLUR with anti-firefly filtering, 3×3 hit-distance
  reconstruction, and NRD-pack normal/roughness input
- **Ray-traced shadows**: inline ray queries for the sun, with an option to disable shadow tracing

### Geometry and acceleration structures
- **GPU-driven geometry**: one mesh-shader workgroup per cluster, dispatched indirectly over the visible
  cluster list the traversal wrote. Cluster LOD plus the visibility buffer is the only geometry pipeline —
  there is no vertex-shader G-buffer path and no meshlet fallback for opaque and masked geometry
- **Visibility buffer**: the opaque and masked buckets rasterise a packed 32-bit visibility id per pixel
  instead of six G-buffer values, and a compute resolve turns the ids back into albedo, normals, ORM,
  emissive and motion vectors. The pixel shader that writes an id is one `uint` store, so the draw is
  bandwidth- and instruction-cheap; material evaluation happens once per pixel in the resolve, which is
  where a future material graph belongs (`docs/visibility_buffer_implementation_plan.md`)
- **Compute software raster**: clusters whose projected half-extent is under a pixel threshold are routed
  to a compute rasteriser that writes coverage stamps replayed through the hardware depth test, instead of
  paying mesh-shader launch and helper-lane cost for a few pixels. It is implemented and correct but ships
  off (`--compute-raster`), because the id pass has so little shading to waste that the dispatch and barrier
  cost more than they save — measured at +5.7 % GPU time on the animals scene, a wash on Bistro
- **Two-phase occlusion culling**: frustum culling in phase 1, HZB occlusion culling with a phase-2 pass
  over the survivors, cone/silhouette culling, and per-instance LOD selection
- **Hierarchical Z-buffer**: built with AMD FidelityFX Single Pass Downsampler (min reduction)
- **Hierarchical LOD**: up to 8 levels generated with meshoptimizer progressive simplification, plus
  vertex quantization. The cluster LOD DAG below replaces it for opaque and masked geometry; this per-mesh
  chain now serves the transparent bucket and ray tracing
- **Multi-LOD acceleration structures**: per-LOD BLAS with a `TLASPatch` compute pass keeping the TLAS
  instance addresses current as LODs change. When the cluster path draws, the LOD it patches is the one
  the cluster traversal classified, so rays and rasterisation consume the same geometry
- **Opacity micromaps (OMM)**: alpha-masked geometry is baked into 4-state micromaps (adaptive
  subdivision, `m_MaxLevel` 6, `m_TargetEdge` 3, selectable alpha mip), uploaded, and linked into the
  BLAS so masked rays resolve opacity in hardware instead of being treated as opaque. Baked from the
  masked base-colour DDS's alpha channel.
  CLI: `--enable-omm`, `--omm-max-level`, `--omm-target-edge`, `--omm-debug`, `--omm-debug-mip`,
  `--omm-dump-mips`. The panel has a debug view over texel classification, the micromap grid and the
  candidate heatmap.

### Cluster LOD (continuous LOD)
- **Offline DAG**: every primitive is clusterised (96 triangles, 64 vertices), partitioned into groups of
  ~16 clusters with `meshopt_partitionClusters`, and each group is decimated to roughly half its triangles
  with `meshopt_simplifyWithAttributes`, its border locked (welded positions plus
  `meshopt_SimplifyLockBorder`). A group that keeps more than 85 % of its triangles stays terminal and its
  clusters keep no replacement, so they are drawn at any distance. The result is a DAG — a coarser group
  can take clusters from several finer groups — not a per-mesh chain, and it is cooked once into the
  versioned mesh cache with the clusterisation config hashed into the cache key, so a parameter change
  re-cooks instead of mixing two layouts
- **GPU cut**: a per-bucket traversal (init, one node pass per tree depth, group pass, indirect-args pass)
  rejects whole subtrees on the frustum and the HZB, then draws a cluster when its group is detailed enough
  for the camera and every group that replaced it is not. The threshold is in pixels (`--cluster-error`,
  1.0 by default), so the cut follows the projection instead of a per-mesh heuristic. `ClusterLodTests`
  checks the DAG, the watertightness of a cut in welded position space and the cooked payload without a
  device
- **One mesh-shader workgroup per cluster**: `DispatchMeshIndirect` over the visible-cluster list, the mesh
  shader decoding a cluster's packed micro-indices into a 32-bit visibility id per pixel. Opaque and masked
  draw through it; the transparent bucket keeps the vertex-shader forward path until OIT lands
- **Per-cluster culling**: each cluster's own bounding sphere is tested against the HZB (a group spans
  several clusters, so its sphere is rarely occluded), sub-pixel clusters are dropped outright, and
  `--cluster-two-pass <0|1>` records what occlusion alone rejected so a second pass can revive it against
  the depth the first pass just wrote
- **Budgeted traversal queues**: the per-bucket node and group queues are sized
  as fixed budgets (`kMaxClusterNodeRefs`, `kMaxClusterGroupRefs`) instead of
  instances × tree, which an instanced scene turns into billions of references. An append past the
  capacity is dropped and counted (`ClusterCounters::m_RefCapped`, shown in the counters panel), so a
  cap costs image coverage and never a buffer overrun
- **Discrete LOD**: an instance whose cut resolves to a single level is seeded at that level's node and
  drawn whole, which costs the traversal nothing where it applies and gives ray tracing the level to match
- **Debug views**: cluster ids, a 32-level LOD ramp and a per-triangle
  overlay (Debug Modes 11–13, `--cluster-debug`), plus live traversal counters in the panel and
  `--cluster-stats-frame <n>`, which compares the GPU's cut against the CPU reference of the same rule
- **A/B switches**: `--cluster-force-lod <n>`, `--cluster-max <n>`,
  `--cluster-no-culling`, `--cluster-discrete-lod <0|1>` and `--cluster-freeze-culling` isolate the cut
  from the draw

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
- **Cluster geometry streaming**: a cluster group is the streaming unit — its triangle and vertex words
  are packed into one arena with a host free list and the cooked cache stores that blob with its vertex
  run meshopt-encoded (the Bistro cache is 1.47× smaller than the uncompressed form), the traversal
  requests the groups it cannot draw and a readback two frames later serves them, and an age filter
  sweeps only when the budget forced a trim. A missing group is requested while the finest resident
  level is drawn, so a level streams in without a hole
- **One VRAM budget for both**: `--stream-budget <MiB>` caps the texture tile heaps and the geometry arena
  together — the arena is sized from what the heaps leave and heaps are refused past what the arena leaves,
  with a floor at the coarsest-level cover so a tight budget draws coarse geometry instead of none
- **Tooling**: tile residency debug view and per-texture residency statistics in the panel, plus a cluster
  streaming panel with resident MB, activity and a per-LOD-level residency histogram
  (`--cluster-stream-stats-frame <n>`, `--cluster-stream-age <frames>`)

### Engine and tooling
- **Render graph**: two-level recording (a setup pass declares, the recorded body replays), transient
  allocation with aliasing, pass toggling, per-pass submission (`--execute-per-pass`,
  `--execute-per-pass-and-wait`), optional aliasing (`--disable-rendergraph-aliasing`), and a visualizer
  for resource lifetimes, heap usage and pass access. Passes record concurrently on the task pool, one
  command list each, and submit in schedule order; the transitions inside a list come from nvrhi's
  automatic resource-state tracking, and the graph requests a global sync barrier before the first pass
  that touches an aliased resource
- **srrhi**: a `.sr` shader-resource description compiled into C++ argument structs and HLSL binding
  code, so a pass's bindings are declared once (see PIPELINE.md)
- **ShaderIDs**: `.cfg`-driven shader table generation
- **Hot reload**: F5 recompiles and rebinds shaders without restarting
- **Multithreaded recording**: one taskflow pool (`TaskExecutor`, a worker per logical core) shared by
  the scene loader and the frame loop — `ParallelFor` for the load's parallel loops, the render graph's
  deferred pass list recorded concurrently on the same pool, and deferred garbage collection. The
  workers register with microprofile, so a CPU capture has one row per worker
- **Profiling**: MicroProfile CPU scopes with HTML dump, per-scope GPU timer queries (`GpuPassTimers`,
  up to 256 scopes per frame), pipeline statistics, and a per-pass CPU/GPU table in the panel.
  `--hitch-log <ms>` logs frames slower than a threshold with streaming and VRAM state;
  `--hitch-dump <ms>` writes `profiler_dump.html` and exits on the first such frame
- **Capture support**: PIX3 markers, in-app PIX captures (`--pix`, `--pix-capture-frame`,
  `--pix-capture-load`, `--pix-capture-dir`) and in-app RenderDoc captures (`--renderdoc`,
  `--renderdoc-capture-frame`) — never both on one frame
- **Scene loading**: glTF 2.0 via cgltf, a versioned binary scene cache, mesh optimization, LOD and
  cluster DAG generation, the cluster LOD cook, and an animation system (linear, step, cubic spline, SLERP,
  Catmull-Rom; transform and material-emissive channels, topologically sorted)
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
| `--restir-checkerboard <0\|1>` | force checkerboard rendering on/off over the preset |
| `--fov <degrees>` | vertical FOV override |
| `--exposure <stops>` | auto-exposure bias |
| `--taa-sharpness <0..1>` | sharpening in the TAA resolve |
| `--hide-ui` | draw no ImGui windows |
| `--screenshot-frame <n>` | write `bin/screenshot.png` on frame n and exit |
| `--enable-omm`, `--omm-max-level`, `--omm-target-edge`, `--omm-debug` | opacity micromaps |
| `--cluster-debug <0\|1\|2\|4>` | cluster debug view: off, cluster ids, LOD ramp, traversal counters |
| `--cluster-error <px>`, `--cluster-force-lod <n>`, `--cluster-max <n>` | cluster cut: pixel error, forced level, cluster cap |
| `--cluster-no-culling`, `--cluster-two-pass <0\|1>`, `--cluster-discrete-lod <0\|1>` | cluster cut: isolation, second pass, discrete LOD |
| `--cluster-stats`, `--cluster-stats-frame <n>` | dump the cooked DAG / print the traversal's counters |
| `--stream-budget <MiB>` | VRAM cap shared by the tile heaps and the geometry arena |
| `--cluster-stream-age <frames>`, `--cluster-stream-stats-frame <n>` | cluster streaming diagnostics |
| `--stream-import <0\|1>` | pin how the cook holds geometry: streamed parts, or one piece in memory (default: automatic, a scene over an eighth of RAM streams) |
| `--import-limit-primitives <n>` | cook only the first `n` primitives, for measuring a huge scene |
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
Passes declare the resources they read and write; the graph sizes and aliases the transients from those
declarations, gives every pass its own command list so the passes record concurrently on the task pool,
and exposes a visualizer. Transitions inside a command list are nvrhi's automatic state tracking, so the
graph itself places only the alias barriers. PIPELINE.md has the details, and
`docs/render-graph-article-comparison.md` compares the design against a published render-graph write-up,
including what it does not do yet (access kinds, batching, pass reordering).

### Renderers
Each pass is an `IRenderer` registered with the renderer registry, with its own `Setup`/`Render` and its
own GPU timer group. Passes can be toggled individually from the panel.

### Rendering modes
- **Normal**: deferred G-buffer (albedo, normals, ORM, emissive, motion vectors) plus the full RTX
  pipeline — ReSTIR DI/GI, SHARC, NRD, ray-traced shadows, TAA, bloom, tonemap
- **Reference Path Tracer**: unbiased Monte Carlo path tracer with next-event estimation, Russian
  roulette and BRDF importance sampling, accumulated progressively

### Scene
glTF 2.0 through cgltf, cached to a versioned binary, with LOD and cluster-DAG cooking, animation,
a light buffer for ray tracing (directional with angular size, point/spot, environment), and OMM baking.

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
- [taskflow](https://github.com/taskflow/taskflow) 4.1.0 — the task pool every parallel loop runs on
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
