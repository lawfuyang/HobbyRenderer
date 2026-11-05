# The RTXDI GPU path costs more than the rest of the frame

Measured on 2026-11-05, Bistro (`D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf`), 1920×1080,
RTX 3080 / driver 616.92, Debug build, frame 90, OMM off (it is off by default and not involved here).

Solutions A (half-resolution DI) and C (half-resolution denoising) from section 4 are implemented;
section 7 has the measured result. Sections 1-6 are the analysis they came from.

## 1. How this was measured

Two captures of the same frame, plus the renderer's own GPU timers:

```powershell
# PIX, one frame from acquire to present
.\bin\HobbyRenderer.exe --scene "D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf" --pix-capture-frame 90
$PXT = 'C:\Program Files\Microsoft PIX\2610.28\pixtool.exe'
& $PXT --output=quiet open-capture bin\captures\bistrox-frame90.wpix `
      save-event-list bin\ev_rtxdi.csv --counters=gpu__* `
      --counters=smsp__inst_executed.sum --counters=l1tex__t_sectors.sum --counters=dram__bytes.sum

# RenderDoc, same frame
.\bin\HobbyRenderer.exe --scene "..." --renderdoc --renderdoc-capture-frame 90

# the renderer's own timer queries (MicroProfile), 62 steady-state frames
.\bin\HobbyRenderer.exe --scene "..." --hitch-dump 1        # writes bin/profiler_dump.html
```

Reading notes that matter for the numbers below:

- `gpu__time_duration.sum` collected under hardware counters is **inflated** by the replay
  instrumentation (the whole event list sums to 87 ms for a ~20 ms frame), so the PIX table is
  used for *ranking* only. `smsp__inst_executed.sum`, `l1tex__t_sectors.sum` and `dram__bytes.sum`
  reproduce to 0.1 % between runs and are the right A/B metrics.
- The renderer's own numbers (`profiler_dump.html`, average of 62 frames) are timer-query GPU
  durations and are the absolute reference below.
- `D:\Workspace\rdc-tools` could not read the Bistro capture: its LZ4 section decoder takes `int`
  sizes and this capture's frame-capture section expands to 2 224 834 944 bytes
  (*"section too large for the LZ4 decoder"*). The pass structure below therefore comes from the
  smaller Cornell capture (`bin\captures\cornell_capture.rdc`, 308 MB), which runs the same pass
  list. Until that limit is lifted, use RenderDoc captures at a reduced resolution for structural
  questions.

## 2. Where the frame goes

Renderer's own GPU timers, average of 62 frames:

| pass (GPU timer name) | ms | share |
|---|---|---|
| **RTXDIRenderer (whole scope)** | **13.20** | **~65 % of a 20 ms frame** |
| ↳ `SharcIndirectQuery_CSMain` | 3.74 | |
| ↳ `ShadeSamples_main` (ReGIR onion) | 1.77 | |
| ↳ `GenerateInitialSamples_main` (ReGIR onion) | 1.70 | |
| ↳ `SpatialResampling_main` | 0.84 | |
| ↳ remaining RTXDI internals (view-Z, prepare lights, presample, compact, temporal resampling, ReGIR build) | ~5.15 | |
| Base Pass / Opaque Renderer | 5.04 / 5.23 | |
| NRD Denoise (REBLUR) | 3.43 | |
| SHARC update (`SHARCUpdate_CSMain`) + resolve | 1.33 + ~0.2 | |
| FSR3 TAA | 0.93 | |
| TLAS update + patch | ~0.4 | |

PIX ranks the same passes the same way (inflated but ordered): `RTXDIRenderer` marker 20.4 ms
taken, `Opaque Renderer` 8.7, `Base Pass (Phase 1) - Opaque` 4.2, `SharcIndirectQuery` 3.06,
`NRD Denoise` 2.58, `SHARCUpdate` 0.83. Per-event instruction counts put
`SharcIndirectQuery_CSMain` in front by a wide margin (150.8 M instructions, 340 M `l1tex` sectors
for one dispatch), which is what a per-pixel hash-cache query against a 12-SRV binding looks like.

So "the RTXDI GPU path is ~10 ms" understates it: it is **~13 ms**, and with SHARC update and NRD
counted as the cost of the indirect-lighting chain it is closer to **18 ms of a 20 ms frame**.

## 3. What the capture shows about the structure

From `rdc_analysis.py draws` on the Cornell capture — one frame issues, in this order:

```
GenerateViewZ_main                 240x135 groups  = 1920x1080  (full res)
PrepareLights_main                   1x1
DownsampleTextureToPow2            128x128
PresampleEnvironmentMap_main         4x128       (env light PDF)
PresampleLights_main                 1x1
GenerateInitialSamples (REGIR_ONION) 240x135     = full res
TemporalResampling_main              240x135     = full res
SpatialResampling_main               240x135     = full res
ShadeSamples (REGIR_ONION)           240x135     = full res
SharcIndirectQuery_CSMain            240x135     = full res
Classify tiles (NRD)                 120x68      = half res
```

Three facts fall out of that, and they are the three biggest levers:

1. **Every DI pass runs at full resolution.** Initial sampling, temporal and spatial resampling,
   shading and the SHARC query are all 1920×1080 dispatches. RTXDI is designed to run at half
   resolution with temporal upsampling; that is a 4× reduction in the per-pixel work of five passes.
2. **Every feature is on, by default.** `Renderer::m_IndirectLightingTechnique` defaults to `3`
   = `INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC`, and the local-light sampling mode defaults to
   `ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS`. So one frame runs, simultaneously:
   ReGIR grid build + ReGIR RIS presampling for the direct pass, ReSTIR DI at full res, ReSTIR GI
   with the SHARC cache for secondary surfaces (`ShadeSecondarySurfaces` +
   `SharcIndirectQuery` + `SHARCUpdate` + `SharcResolve`), NRD REBLUR, and FSR3 TAA. In
   `RESTIR_GI_SHARC` the SHARC query exists only to accelerate the secondary-surface radiance
   lookup — it is not carrying the frame on its own, yet it is the single most expensive dispatch
   in the capture.
3. **Each RTXDI dispatch binds `t0..t11` plus a 14-entry SRV array** (`RTXDI_NeighborOffsets` and
   neighbours). That binding set is rebuilt per dispatch through the descriptor heap; they are
   cheap compared with the shading, but 46 dispatches per frame (PIX: 12 ms of `Dispatch` rows,
   inflated) still costs a serial submission tail.

## 4. Solutions, in the order I would try them

Each item names the knob and how to prove the win. Use the same pose and
`--hitch-log 0.1` averages, or the PIX instruction counters for a noise-free A/B.

### A. Run DI at half resolution (biggest single win)

The RTXDI context is created at the swapchain extent (`m_RHI->m_SwapchainExtent`), so everything
downstream is full res. Creating it at half extent and letting the temporal resampling upsample back
makes the five full-res dispatches 120×68 groups. The renderer already has a half-width path for
secondary surfaces (`activeCheckerboardField != 0` halves `dispatchWidth` around line 1563 of
`RTXDIRenderer.cpp`), so the plumbing for "not every pixel every frame" exists.

- Expected: the 3.74 + 1.77 + 1.70 + 0.84 ms of per-pixel DI work drops to roughly a quarter
  (~2 ms), plus a cheap upsample — **~4-5 ms saved**.
- Risk: more noise at edges and on thin geometry, partly recovered by the existing temporal
  resampling. Verify with the same screenshot comparison used for other A/Bs.

### B. Prove SHARC is earning its ~5 ms, or drop it from the default

`SharcIndirectQuery` (3.74 ms) + `SHARCUpdate` (1.33 ms) + `SharcResolve` ≈ 5.2 ms, and in
`INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC` its only job is the secondary-surface radiance cache used
by `ShadeSecondarySurfaces`. Two experiments, both a one-line change of
`m_IndirectLightingTechnique` in the ImGui panel:

- **Mode 1 (`INDIRECT_LIGHTING_MODE_RESTIR_GI`)**: no SHARC dispatch at all. If the image is
  visually unchanged apart from a little more secondary noise (which REBLUR already absorbs), the
  cache costs 5 ms for nothing and the default should be mode 1.
- **Mode 3 but half-res query**: `SharcIndirectQuery` runs at 240×135 groups like every other DI
  pass; the cache is a coarse radiance estimate, so a half-resolution query costs a quarter.

Expected: **3-5 ms** for the mode change, more if the half-res query alone is enough.

### C. Denoise at half resolution

NRD's REBLUR is 3.43 ms. NRD takes its working extent from `nrd::CommonSettings::resolution`, so it
can be run on a half-extent with an upsample back, which also matches the half-res DI from (A) —
denoising full-resolution data that was produced at half resolution is paying twice.

- Expected: **~1.5-2 ms**.
- Do it together with (A); doing it alone still helps.

### D. Cut the DI sample budget

`GenerateInitialSamples` (1.70 ms) and `ShadeSamples` (1.77 ms) scale with the ReSTIR DI sampling
parameters — the panel already exposes `g_ReSTIRDI_NumLocalLightReGIRRISSamples` ("ReGIR RIS
Samples"), and `ShadeSecondarySurfaces` scales the same way. The defaults bias toward quality; with
temporal resampling the visible cost of one step down is small.

- Expected: **0.8-1.5 ms** for one sample step down.
- Verify visually against the current frame; this is the classic quality/perf dial and it is the
  one place where "10 ms" can be traded down without structural work.

### E. Skip per-frame setup that does not change

`GenerateViewZ`, the two presample passes and `PrepareLights` are each cheap, but `PrepareLights`
and the light PDF mips only change when the light set or the camera moves. Cache them and
re-dispatch only when their inputs change.

- Expected: **0.3-0.8 ms** and 3-4 fewer dispatches.
- Small, but it also shortens the submission tail.

### F. Make the defaults honest

Whatever combination survives should be the default preset, not something that only a
half-res+single-technique build reaches. Two concrete numbers to keep in the preset after the
changes: RTXDI ≤ 5 ms and the whole indirect chain (RTXDI + SHARC + NRD) ≤ 8 ms at 1080p on this
GPU. Today it is ~18 ms.

## 5. What was *not* the problem

- **OMM** is off and unrelated (it is opt-in since the feature was shelved).
- **The raster half** is fine: base pass 5.0 ms for 426 k triangles with bindless materials is in
  the expected range, and the TAA/bloom/exposure tail is under 2 ms combined.
- **Acceleration-structure work** is negligible in a steady frame (TLAS update + patch ~0.4 ms).
  The BLAS/OMM build costs are load-time only and are not in these captures.

## 6. Reproducing this analysis

| artefact | what it is |
|---|---|
| `bin/captures/bistrox-frame90.wpix` | 913 MB, the PIX capture of frame 90 |
| `bin/ev_rtxdi.csv` | 1514 rows, per-event counters (`gpu__*`, instructions, sectors, DRAM bytes) |
| `bin/captures/bistrox_capture.rdc` | 1.39 GB, the RenderDoc capture of the same frame (too large for `rdc_analysis.py`) |
| `bin/captures/cornell_capture.rdc` | 308 MB, used for the dispatch list above |
| `bin/profiler_dump.html` | the renderer's own timer dump (62 frames) |

The per-pass table from the dump can be re-extracted without the GUI:

```powershell
$s = Get-Content bin\profiler_dump.html -Raw
[regex]::Matches($s, 'MakeTimer\(\d+,\s*"([^"]+)"[^)]*\)') | ForEach-Object { $_.Groups[1].Value }
```

## 7. Implemented: half-resolution DI + denoising

### What the switch does

Half-resolution DI is the **default**; `--full-res-di` (or the **Half-Resolution DI** checkbox in the
RTXDI panel) turns it off, which is how the A/B pairs below were taken.

- the RTXDI contexts are created at half extent, so every screen-space pass - initial sampling,
  temporal and spatial resampling, shading, the ReGIR presample and the SHARC indirect query -
  dispatches a quarter of the pixels and indexes a quarter of the reservoir grid;
- the lighting outputs and every NRD pool texture are half extent, so REBLUR denoises the
  half-resolution signal it is actually given;
- the G-buffer, its history and the normal+roughness texture stay at full resolution: the
  screen-space passes address the top-left texel of the block they stand for
  (`ResamplingConstants::gbufferScale`, `RAB_GbufferTexelPos`);
- `DownsampleDIInputs` point-samples view depth, motion vectors and normal+roughness for NRD,
  which requires every input to have the resolution it denoises at;
- `CompositingPass` upsamples the lighting signal: it unpacks the four surrounding texels, weights
  them by the bilinear weight **times a depth weight** and blends afterwards. Packed REBLUR radiance
  is not linear, so unpacking has to happen before any interpolation.

The depth weight is `1 / (1 + relativeDepthDifference * 50)`, comparing each reduced-resolution
texel's view depth (`RTXDIHalfLinearDepth`) with the full-resolution pixel's (`RTXDILinearDepth`).
It exists so a tap on the far side of a silhouette contributes nothing instead of smearing its
lighting across the 2×2 block. When no tap shares the pixel's surface the plain bilinear weights are
kept as the fallback.

### Measured, back-to-back in one session (Bistro, 1080p, `--hitch-dump 1`)

| GPU timer | full | half | delta |
|---|---|---|---|
| **RTXDIRenderer** | **12.76 ms** | **4.76 ms** | **-8.0 ms** |
| - `SharcIndirectQuery_CSMain` | 3.65 | 1.01 | -2.6 |
| - `GenerateInitialSamples` (ReGIR onion) | ~1.6 | ~0.4 | -1.2 |
| - temporal + spatial resampling + shading | ~1.2 | ~0.5 | -0.7 |
| - `PresampleReGIR` | 0.31 | 0.31 | 0 (resolution independent) |
| **NRD Denoise** | **3.37 ms** | **1.11 ms** | **-2.3 ms** |
| `DownsampleDIInputs` (new) | - | 0.04 | +0.04 |
| `CompositingPass` (upsampling now) | 0.116 | 0.134 | +0.02 |

**~10.3 ms off the frame** for +0.02 ms in the compositing pass. GPU clocks drift by 1-2 ms between
runs on this machine, so only a back-to-back pair like the one above is comparable.

### The per-frame light setup is not skippable (attempted, reverted)

Skipping `PrepareLights`, the two light PDF mip chains and both presample dispatches when the
scene's lights and sky have not changed looks like free performance, and it is not:

- The render graph treats a declared resource as **undefined at the start of the frame**
  (`DeclareTexture`/`DeclareBuffer` immediately mark it written, "they start with undefined
  contents"), so `DeclarePersistent*` keeps a resource from being aliased but does **not**
  preserve its contents across frames. Skipping the writes leaves garbage, and the frame renders
  with emissive materials only - no direct light, no environment light.
- Measured cost of the whole setup, per frame: `BuildEnvLightPDF` 0.027, `PrepareLights` 0.021,
  local PDF mips 0.021, `PresampleLights` 0.062, env PDF mips 0.037, `PresampleEnvironmentMap`
  0.068 = **0.24 ms**. That is all the optimisation was ever worth.

A verified skip would need those resources owned outside the render graph (created on the device,
like the TLAS history) or a graph API that declares a preserved resource; neither exists today.

### Image comparison

Same frame (120), same pose, same session, 1920×1080, ImGui overlay excluded. Two consecutive
identical runs differ by 1.6 / 255, so that is the noise floor of the metric; a pose that is one
session apart differs by 80+ and cannot be compared at all.

| half-res upsample | mean absolute difference / channel | pixels differing by > 24 | mean luminance shift |
|---|---|---|---|
| bilinear (depth weight 0) | 8.35 / 255 (3.3 %) | 32.2 % | -1.05 % |
| depth weight 50 (**default**) | 8.22 / 255 (3.2 %) | 31.8 % | -0.94 % |
| depth weight 5000 (probe) | 8.10 / 255 (3.2 %) | 31.3 % | - |

Split by whether the reference pixel sits on an edge (gradient > 40) or on a flat area:

| | bilinear | depth weight 50 |
|---|---|---|
| edge pixels (16.6 % of the image) | 13.33 | 12.88 |
| flat pixels | 7.37 | 7.30 |

The trend with weight strength proves the filter runs and helps, but the achievable gain is small:
the depth weight fixes the *blending* at silhouettes, while most of the remaining difference is the
half-resolution radiance itself - fewer surface samples, a different NRD history, and shading at
block centres instead of per pixel. No upsampler recovers that; only more samples or a higher
denoiser resolution would. 50 was kept rather than the more aggressive values because it still
rejects a 10 % depth step by 5/6 while leaving the tent smoothing intact under motion.

The run-to-run control shows the renderer is otherwise deterministic, so the full-vs-half difference
is the real change. It is not a brightness or an alignment shift: the mean luminance moves by 1 %
and structures stay in place. The amplified difference image (`bin/diff_half_x6.png`) is an **edge
map** - silhouettes, railings, lamp posts, plants, paving seams - with no 2×2 blocking and no
ghosting.

What remains unfixed is the sample/denoiser resolution itself. If that trade is still too large, the
next lever is to keep only the expensive passes at half resolution (the SHARC query and the shading)
and run the resampling at full resolution, which costs some of the ~8 ms back.

### Verification status

These numbers come from the renderer's own GPU timers and screenshot pairs, not from RenderDoc or
PIX: no capture-based verification of the half-resolution frame has been done yet. For an A/B this
is sufficient for the timings (they are timer-query durations, not derived), but a capture is what
would prove the resource sizes and dispatch dimensions rather than infer them.

Both modes were run end to end with zero assertions; the earlier `NvrhiErrorCallback` assertion in
the full-resolution mode was the compositing pass binding a `RGBA8` dummy texture to the
single-channel `m_HalfViewDepth` slot, which the shader never reads on that path but the descriptor
still has to match.

### Reproducing

```powershell
# GPU timers for both settings, one after the other (half resolution is the default)
.\bin\HobbyRenderer.exe --scene "D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf" --hitch-dump 1 --full-res-di
.\bin\HobbyRenderer.exe --scene "D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf" --hitch-dump 1

# the comparison pair - both from the same session, or the pose differs and the numbers are meaningless
.\bin\HobbyRenderer.exe --scene "..." --screenshot-frame 120 --full-res-di
.\bin\HobbyRenderer.exe --scene "..." --screenshot-frame 120
```

`bin/dump_full_final.html` and `bin/dump_half_final.html` hold the two timer dumps these numbers
come from; `bin/pose_a.jpg` is the full-resolution frame, `bin\shot_half_depthaware.jpg` the
default one, `bin\shot_half_bilinear.jpg` the same frame with the depth weight disabled, and
`bin/diff_half_x6.png` their amplified difference.
