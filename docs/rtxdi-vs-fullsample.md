# Why my RTXDI looks worse than the NVIDIA FullSample

Reference: `D:\Workspace\RTXDI` (RTXDI SDK 3.1.0, `Samples/FullSample`). Mine: this repo.

## 1. What could and could not be compared

The FullSample **builds and runs**: `build\bin\FullSample.exe` exists, and it is the same donut
CMake project the earlier note assumed was unbuildable (the recipe is in §6). It ships
`nvngx_dlss.dll` and `nvngx_dlssd.dll` with `DONUT_WITH_DLSS:BOOL=ON`, so **DLSS-RR and DLSS-SR are
both exercised** - the denoiser difference in §5.1 is measurable now, not theoretical. Frames are
cheap: `--profiling.saveFrame <n> --profiling.saveFile <png>` renders to frame `n` and writes
`SceneRenderer.cpp:2057`'s `LdrColor`, which has no ImGui overlay (this repo's screenshot does).

What is still missing is the pass-by-pass half: PIX event lists and per-pass counters from both
sides. So what follows is a configuration comparison, one measurement taken on the built sample, and
one negative result about cross-app image differencing.

Known capture limits in this repo, from `docs/rtxdi-gpu-performance.md`:
- `rdc-tools` cannot open the 1.39 GB Bistro RenderDoc capture (its LZ4 section decoder takes `int`
  sizes); captures have to be taken at a reduced window size for it.
- PIX works, but `gpu__time_duration.sum` under hardware counters is inflated and only good for
  ranking.

The good news: `Libraries/Rtxdi/Include/Rtxdi/DI/*` in the sample is **byte-identical** to
`external/rtxdi/Include/Rtxdi/DI/*` here, so the resampling core is not the difference. Everything
that differs is application-side: parameters, the denoiser, and the render-application bridge.

## 2. What the sample actually runs by default

| thing | FullSample default | where |
|---|---|---|
| quality preset | `QualityPreset::Medium` | `Source/UserInterface.cpp:210` |
| denoiser | `DenoiserMode::DLSS_RR` | `Source/UserInterface.cpp:253` |
| anti-aliasing | `AntiAliasingMode::DLSS_SR` | `Source/UserInterface.cpp:214` |
| ReSTIR DI samples | ReGIR_RIS, **8** local-light samples, **1 BRDF sample** | `UserInterface.cpp` `ApplyPreset()`, Medium |
| bias correction | temporal **Raytraced**, spatial **Basic** | same |
| permutation sampling | **on** (`stParams.enablePermutationSampling = tParams.…`) | `Source/App/SceneRenderer.cpp:934` |
| secondary resampling (GI) | **on** | `UserInterface.cpp` Medium |
| REBLUR | `enableAntiFirefly`, `hitDistanceReconstructionMode = AREA_3X3` | `UserInterface.cpp` `SetDefaultDenoiserSettings()` |
| indirect lighting | ReSTIR PT available and used by the PT presets | `Source/RenderPasses/LightingPasses/LightingPasses.cpp` |

## 3. What this repo ran before the fix

| thing | mine (before) | consequence |
|---|---|---|
| quality preset | `Balanced` = the FullSample's **Fast** preset: ReGIR_RIS with **4** samples, `numBrdfSamples = 0` | half the local-light candidates and **no BRDF (environment/specular) candidate at all**; that alone accounts for visibly flatter specular and slower environment-light convergence |
| permutation sampling | forced **off** right after the preset (`enablePermutationSampling = 0u; // disabling this somehow increases image quality?`) | the sample enables it; it is an unbiased variance reduction, not a quality knob |
| REBLUR | `enableAntiFirefly` only | no hit-distance reconstruction, so the blur radius is limited by missing hit distances: REBLUR blurs more *and* still leaves noise |
| DI resolution | **half** (my previous change) | the sample never reduces DI resolution; this trades surface samples for time and is visible against it |
| denoiser/upscaler | NRD REBLUR + FSR3 TAA at native res | the sample defaults to DLSS-RR + DLSS-SR |

## 4. Changes applied

1. `RTXDIRenderer.cpp` `ApplyBalancedPreset()` now mirrors the FullSample's **Medium** preset: 8
   ReGIR RIS samples, `numBrdfSamples = 1`, permutation sampling on, raytraced temporal bias
   correction, Basic spatial, 8 disocclusion-boost samples, reuse-final-visibility on, secondary
   resampling on, and permutation sampling on for ReSTIR GI.
2. The `enablePermutationSampling = 0` override in `Initialize()` is gone.
3. `m_NRDReblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3`,
   matching `SetDefaultDenoiserSettings()`.
4. Half-resolution DI is **off by default** again (`Config::m_HalfResDI = false`, `--half-res-di`
   re-enables it). `HighQuality` was also aligned with the sample's Ultra preset.
5. New `--restir-preset <balanced|highperf|highquality>` so these can be A/B-ed from the command
   line instead of the panel.

### Cost of the alignment (Bistro, 1080p, GPU timer dump, one session)

| preset | RTXDIRenderer | of which spatial resampling | NRD | SHARC query |
|---|---|---|---|---|
| `highperf` (the old default) | 8.26 ms | 0.39 | 2.42 | 2.34 |
| `balanced` (new default = sample Medium) | 11.14 ms | 0.58 | 2.43 | 2.34 |
| `highquality` (sample Ultra) | 16.72 ms | 4.58 | 2.60 | 2.41 |

Matching the sample's default quality costs **+2.9 ms** of RTXDI against the old default.

## 5. What is still different, in the order it is worth attacking

### First, a measurement that reorders this list

Same pose, same resolution, only `--denoiser.mode` changed between two sample captures:

| pair | mean abs luminance diff | p95 |
|---|---|---|
| sample NRD_REBLUR vs sample DLSS-RR | **0.0106** | 0.032 |

The swap that item 1 below treats as the headline gap moves the image by about 1 % of range. It is
a real difference and DLSS-RR is the better image, but it is *not* what accounts for the visible
gap between the two applications.

The cross-app number is now available, and getting it required fixing three separate mistakes in
how the two frames were being produced. With those fixed the pair registers exactly (best integer
shift is 0,0) and the residual is dominated by one measurable knob rather than by geometry.

#### Registering the two frames

1. **Same asset file.** The sample always loads `Assets/Media/bistro/bistro.gltf`; `bistrox.gltf`
   is a different export. Both applications are now pointed at
   `D:\Workspace\RTXDI\Assets\Media\bistro\bistro.gltf`. Its `bistro.bin` is byte-identical (MD5
   `5ED8…`) to the copy under `D:\Workspace\GLTF Scenes\Bistro\`, and the two `.gltf` JSONs differ
   only by a `cameras` block and two extra nodes, so the geometry is the same file either way.
2. **Same lights.** `bistro.gltf` itself carries no punctual lights: the sample's Sun and its
   spotlights come from `bistro-rtxdi.scene.json`. This loader reads that format too, so both sides
   load the descriptor instead of the bare glTF, and the light rig stops being a variable.
3. **Same projection and mirroring.** `--fov 60` on this side (see below), and the pose converted
   into this repo's handedness. The glTF path converts right-handed to left-handed by negating Z of
   every translation and X/Y of every rotation (`SceneLoader.cpp`, `ProcessNodesAndHierarchy`), so a
   glTF-space point `(x, y, z)` is `(x, y, -z)` here. The sample's default Bistro camera is
   `position (-7.688, 2.0, 5.594)`, `direction (0.3539, 0, 0.9426)`, which becomes position
   `(-7.688, 2.0, -5.594)`, yaw `atan2(0.3539, -0.9426)` = 159.42°, pitch 0 — the values seeded into
   `bin/camera_state.json`.

Skipping step 3 is what produced the earlier "0.23 at the edge of the shift search": the same
numbers put this renderer in the street outside the restaurant while the sample was inside it.

#### The residual, with registration working

| measurement | mean abs luminance difference |
|---|---|
| unregistered (previous attempts) | 0.2308 |
| registered, exposure left as-is | 0.2308 |
| registered, exposure matched (+2.2 stops here) | **0.1115** |
| sample NRD_REBLUR vs sample DLSS-RR (reference for "small") | 0.0106 |

At the un-matched exposure a least-squares global gain fits 1.86 / 1.88 / 1.60 per channel, i.e.
this renderer was **about 2.2 stops darker** than the sample with a slight blue cast. That is not a
bug in the DI: it is two different exposure and tone-curve models.

| | this repo | sample |
|---|---|---|
| exposure input | luminance histogram → EV100 (`S = 100, K = 12.5`), clamped to EV [−7, 23], `exposure = 1/(2^EV · 1.2)` | luminance histogram → adapted *luminance*, clamped to [0.002, 0.2] |
| eye adaptation | single `m_AdaptationSpeed` = 5, exponential | `eyeAdaptationSpeedUp` 2.0 / `Down` 1.0 (donut `ToneMappingPass`) |
| bias | `m_ExposureCompensation`, 0 by default | `exposureBias` = −1 (`UserInterface.h:207`) |
| tone curve | Khronos PBR Neutral | extended Reinhard, `L(1 + L·w)/(1 + L)`, then a colour LUT |
| output | sRGB OETF in shader | LUT applies the sRGB transfer |

`--exposure <stops>` biases the auto-exposure so the two can be put at the same brightness before
anything underneath is read. What is left after that match, at 0.1115, is the pass-level list below:
the lamps' bloom is far wider here, edges are softer (TAA), and the indirect level is lower in the
bar area.

1. **DLSS-RR instead of NRD REBLUR.** The sample's default denoiser is the ray-reconstruction
   network, which is much better at RTXDI's noise than REBLUR and is fed extra inputs
   (`DLSSRRInputFormattingPass`, `ConfidencePass`, `FilterGradientsPass`). This repo cannot match it
   with REBLUR; it needs the DLSS-RR integration (NGX) plus those three pre-passes.
2. **ReSTIR PT.** SDK 3.1's flagship path resamples the *whole* light path, not just the direct
   segment; the sample's PT presets are a whole quality tier above DI+GI. This repo only has DI +
   ReSTIR GI/SHARC.
3. **Material model.** The sample's G-buffer carries `diffuseAlbedo` (R11G11B10) and
   `specularRough` (R8G8B8A8, gamma) separately, so `specularF0` comes from the material. This repo
   derives it from `metallic → lerp(0.04, albedo, metallic)`, which cannot express specular maps or
   non-metal F0 tints. Fine for Bistro, wrong for anything with a specular texture.
4. **Environment lighting.** *Correction to an earlier reading of this:* the sample does **not** use
   an HDRI for Bistro. Its descriptor (`Assets/Media/bistro-rtxdi.scene.json`) lists no `.hdr` or
   `.exr`, and `SceneRenderer::UpdateEnvironmentLightAndSunLight` routes a non-negative
   `environmentMapIndex` with no file map to `m_renderEnvironmentMapPass`, i.e. donut's procedural
   sky with default `SkyParameters` and `sunLight->irradiance = 1`. Both applications therefore light
   Bistro procedurally. With the light rig shared (`bistro-rtxdi.scene.json` loaded by both) and the
   exposure matched, the sun's contribution is the conspicuous remaining difference in the interior:
   the sample carries bright warm patches through the window and a brighter ceiling, this repo's
   windows are bright but the room receives noticeably less of it.

## 6. Reproducing a registered A/B

Both sides, same asset, same pose, same projection, same light rig, 1080p, TAA + NRD REBLUR.

```powershell
# --- this repo -------------------------------------------------------------
# the pose lives in bin/camera_state.json, keyed by scene path:
#   "D:/Workspace/RTXDI/Assets/Media/bistro-rtxdi.scene.json"
#     position [-7.688, 2.0, -5.594], yaw 2.78287 (159.42 deg), pitch 0
.\bin\HobbyRenderer.exe --scene "D:\Workspace\RTXDI\Assets\Media\bistro-rtxdi.scene.json" `
  --screenshot-frame 180 --fov 60 --exposure 2.2 --hide-ui

# --- the sample ------------------------------------------------------------
cmake -S D:\Workspace\RTXDI -B D:\Workspace\RTXDI\build -G "Visual Studio 18 2026" -A x64
cmake --build D:\Workspace\RTXDI\build --config Release --target FullSample

.\build\bin\FullSample.exe --scene.asset Bistro `
  --device.backBufferWidth 1920 --device.backBufferHeight 1080 `
  --rendering.width 1920 --rendering.height 1080 `
  --denoiser.mode NRD_REBLUR --postProcessing.aaMode TAA `
  --profiling.saveFrame 180 --profiling.saveFile D:\Workspace\RTXDI\shots\A_bistro_taa_nrd.png
```

The sample defaults to its own Bistro camera, which is the pose above before the handedness
conversion, so it needs no `--camera.position` / `--camera.direction` at all - passing converted
numbers here is how the earlier attempts ended up looking at the sky. `--camera.position` and
`--camera.direction` take glTF-space values (see `CommandLineArgs.cpp:424-426`).

Flags this repo gained for the comparison loop: `--fov <degrees>`, `--exposure <stops>`,
`--hide-ui`, and `--screenshot-frame` now writes `screenshot.png` instead of a JPEG (a JPEG's
ringing is the same size as the differences being measured).

Caveats that cost time already:

- `--denoiser.mode` accepts `NRD_REBLUR`, `NRD_RELAX`, `DLSS_RR`, `OFF`; the sample's own default is
  `DLSS_RR` with `AA = DLSS_SR`, so both have to be named explicitly.
- Its PNGs are uncompressed RGBA, so identical byte sizes do not mean identical images.
- `--help` does not print usage, it launches the app.
- Each app must be measured with its UI hidden. The sample saves before its UI is drawn; this repo
  captures the presented backbuffer, so without `--hide-ui` the previous frame's panel is in the
  pixels.

Then, per area of Bistro, one PIX capture from each app (`--pix-capture-frame` here; the sample has
no in-app capture switch, so it is attached externally) and `pixtool save-event-list` on both,
comparing per pass with `smsp__inst_executed.sum` / `l1tex__t_sectors.sum` (the two counters that
reproduce to 0.1 %).

## 7. Pass inventory of the registered frame

Left column: the markers a PIX capture of frame 180 produced here (`--pix-capture-frame 180`, then
`pixtool save-event-list`), in execution order. Right column: the sample's own
`ProfilerSection::Enum` (`Source/ProfilerSections.h`), i.e. what its GPU profiler reports.

| this repo (from the capture) | the sample |
|---|---|
| `Upload Dirty Instances`, `Upload Dirty Material Constants`, `Streaming TileFlush+UpdateMappings+BeginFrame` | — |
| `Opaque Renderer`: `Occlusion Culling Phase 1`, `GPUCulling_Culling`, `Build Indirect Arguments`, `GPUCulling_BuildIndirect`, `Base Pass Render (Phase 1)`, Phase 2 (occluded) | `GBufferFill`, `MeshProcessing` |
| `DownsampleTextureToPow2` → `SPD_CSMain`, `Generate HZB Mips`, `MaskedPass`, `HZBGeneratorPhase2` | — |
| `TLAS Update`, `TLAS Patch`, `BuildRaytracingAccelerationStructure` | `TlasUpdate` |
| `SHARCUpdate`, `SharcResolve` | — |
| `RTXDIRenderer`: `BuildEnvLightPDF`, `GenerateViewZ`, `PostprocessGBuffer`, `PrepareLights`, `Generate Local Light PDF Mips`, `PresampleLights`, `Generate Env Light PDF Mips`, `PresampleEnvironmentMap`, `PresampleReGIR`, `GenerateInitialSamples`, `TemporalResampling`, `SpatialResampling`, `ShadeSamples`, `SharcIndirectQuery` | `LocalLightPdfMap`, `PresampleLights`, `PresampleEnvMap`, `PresampleReGIR`, `InitialSamples`, `TemporalResampling`, `SpatialResampling`, `Shading`, `BrdfRays`, `ShadeSecondary`, `GITemporalResampling`, `GISpatialResampling`, `GIFusedResampling`, `GIFinalShading`, and the seven `PT*` passes when indirect is ReSTIR PT |
| `NRD Denoise`: `REBLUR_DiffuseSpecular` — classify tiles, hit-distance reconstruction, pre-pass, temporal accumulation, history fix, blur, post-blur, temporal stabilization | `Denoising` |
| `CompositingPass` | — |
| `DeferredLighting_PSMain` | — |
| `Sky_Sky_PSMain` | `EnvironmentMap` |
| `TransparentPass` | `Glass` |
| `FSR3 TAA` | `Resolve` |
| `Bloom`: prefilter, downsample, upsample, composite | inside `Frame` (donut `BloomPass`, sigma 32 px, blend 0.005) |
| `HDRPostProcess`: luminance histogram, exposure adaptation, tonemapping | inside `Frame` |
| `Streaming ResolveFeedback` | — |

What the inventory says about the measured gap:

- The direct-lighting chain is the same shape on both sides — presample, ReGIR, initial samples,
  temporal, spatial, shade, then NRD. The 0.1115 that is left after exposure matching is therefore
  not a missing pass in that chain.
- The indirect side is a different algorithm: the sample runs ReSTIR GI plus the ReSTIR PT set and
  feeds `Gradients` to its denoiser, while this repo runs one SHARC query and hands the result to
  REBLUR. This is where the two images' indirect level and blotchiness part company.
- Bloom is not a separate section in the sample. Both use a 0.005 blend, but the shapes differ:
  donut blurs Gaussians of 32 px sigma over a mip chain, this repo uses a 13-tap pyramid, and its
  prefilter had **no threshold** — every pixel entered the blur, so one lamp (emissive factor 40 in
  the asset) smeared over the whole frame. `m_BloomThreshold` (1.0 = SDR white, with the knee as a
  percentage of it) now limits the blur to the part of a pixel above white. Effect measured at
  matched mean brightness: lamp halos shrink from a frame-wide wash to a local glow, the frame's p95
  moves 0.848 → 0.938, and the pixel metric gets *worse* (0.1115 → 0.1662) because the old haze was
  accidentally lifting this renderer's crushed shadows. The haze is still wrong; the number is
  telling us the tone curve is the next thing to fix, not that the bloom should go back.

### Tonal shape at matched mean brightness (Bistro interior, 1080p)

| | p1 | p5 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| sample (extended Reinhard) | 0.052 | 0.103 | 0.427 | 0.708 | 0.993 |
| this repo (PBR Neutral) | 0.000 | 0.000 | 0.379 | 0.938 | 0.994 |

Same mean, but this renderer reaches pure black in its darkest 5 % while the sample never goes below
~0.05, and its highlights run 0.23 higher. PBR Neutral subtracts an offset of `x - 6.25x²` below
0.08 (`shaders/Tonemap.hlsl`), which crushes anything the exposure leaves in the shadows;
extended Reinhard is smooth and has no such term. Most of the shadow deficit below, however, turned
out to be missing light rather than the curve — see the direct-lighting section.

## 8. Direct lighting: the conversions match, the light set did not

Checked line by line against `PrepareLightsPass.cpp` (`ConvertLight`) and
`RTXDIRenderer::ConvertAnalyticalLight`:

| light type | sample | this repo | verdict |
|---|---|---|---|
| directional | `color * irradiance / solidAngle` | same | identical |
| spot, radius > 0 | `color * intensity / (π r²)`, sphere + shaping bit, `cos(outer)`, `1 - inner/outer` | same | identical |
| point, radius 0 | `color * intensity` as flux | same | identical |
| point, radius > 0 | `color * intensity / (π r²)` | same | identical |

ReSTIR DI itself is the same preset (Medium: 8 ReGIR RIS samples, 1 BRDF sample, raytraced temporal
BC, basic spatial BC, 8 disocclusion boost samples), and `prepareLights`/ReGIR/sampling pass names
line up. What differed was **which surfaces count as emitters**:

- The sample makes *every* triangle of an emissive geometry a light
  (`CountLightsInScene`: `any(emissiveColor != 0)` → `numIndices / 3`, no per-triangle filtering).
- This repo made the same list, then dropped triangles whose emitted flux was below `1e-4` of the
  brightest — **62 352 of 96 291 triangles (65 %) gone** on Bistro.

Measured with everything else held fixed (`--exposure` re-matched at each setting):

| light set | exposure to match mean | mean abs diff | p5 | p95 |
|---|---|---|---|---|
| 33 939 of 96 291 triangles | +4.4 stops | 0.1662 | 0.000 | 0.938 |
| all 96 291 (sample's rule) | +2.6 stops | **0.1357** | 0.022 | 0.892 |

So the filter was costing about **1.5 stops of interior brightness** and left the shadows at pure
black; `k_EmissiveTrianglePowerThreshold` is now 0 to match the sample. Its performance cost is
**unmeasured** — that is the one thing to measure before deciding whether the filter comes back.

With the light set matched, the visible difference in the A/B is no longer tone or brightness: it is
noise. The red wall and the dark ceiling carry blotches and speckle here that the sample does not
have, which puts the denoiser (REBLUR vs the sample's DLSS-RR default) back at the top of the list —
not on the sample's own signal, where the swap is worth 0.0106, but on the fact that REBLUR is being
asked to carry a noisier DI signal.

## 9. Pass ablations: no single pass owns the residual

Each row renders the same frame with one pass switched off (`--no-indirect`, `--no-denoise`,
`--no-sky`), or with a pass knob changed, at the exposure that matches the sample's mean (2.6 stops).
Reference: sample mean 0.409, this repo's matched frame is `mean|d|` 0.1357.

| variant | mean | mean abs diff | what it says |
|---|---|---|---|
| baseline | 0.409 | 0.1357 | — |
| `--no-indirect` | 0.392 | 0.1395 | indirect moves the difference by 0.004: the residual is not indirect |
| `--no-sky` | 0.408 | 0.1341 | sky contributes nothing to the difference |
| `--no-denoise` | 0.368 | 0.1420 | REBLUR changes the level (denoised is brighter) but not the structure |
| specular-glossiness read as specgloss | 0.409 | 0.1384 | see below; reverted |
| `--taa-sharpness 0.5` | 0.409 | 0.1361 | no metric change, but see the detail measure |

Two experiments that answered "no" and were not kept:

- **Specular-glossiness.** 234 of Bistro's 254 materials use `KHR_materials_pbrSpecularGlossiness`
  and this loader binds that map as the ORM texture, so `roughness` comes from the specular G
  channel and `metallic` from B (`BasePass.hlsl`), which is semantically wrong. Interpreting it
  properly instead (`roughness = 1 - A`, dielectrics only) moved the match *away* (0.1357 → 0.1384)
  and darkened the shadows, so it was reverted. The wrong reading stays on the known-issues list, but
  it is not what this image difference is made of.
- **Resolve sharpness.** Measured detail (mean |Laplacian|) is 0.0341 here against the sample's
  0.0425 — this renderer's resolve really is 20 % softer. `--taa-sharpness 0.2` and `0.5` raise it to
  0.0367 and 0.0386, closing about half the gap, while `mean|d|` stays put (0.1361). So the softness
  is real and separable, but it is not where the metric lives: the remaining difference is
  distributed across the tone curve, the material response and the geometry/AA edges — visible in a
  difference image as the floor's tile pattern and every silhouette, not as one pass's error.

Comparison-only switches added along the way: `--fov`, `--exposure`, `--hide-ui`,
`--taa-sharpness`, `--no-indirect`, `--no-denoise`, `--no-sky`.




Caveats when reading captures:

- PIX logs `WinPixGpuCapturer.dll error: Invalid/Uninitialized descriptor used in command list
  event` repeatedly while capturing this renderer, and still writes a usable `.wpix`. The events and
  their names survive; do not read the capture as proof a descriptor was actually unset.
- Captures are large: one Bistro frame here is 846 MB.


