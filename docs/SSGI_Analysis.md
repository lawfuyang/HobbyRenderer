# SSGI Implementation — Analysis & Plan

> **Phase:** SSGI (Screen-Space Global Illumination) — integration into NormalBasic rendering mode  
> **Goal:** Add screen-space diffuse + specular indirect lighting to the rasterized (non-RT) pipeline  
> **Reference:** [0beqz/realism-effects](https://github.com/0beqz/realism-effects) (poisson-recursive branch)

---

## Table of Contents

1. [Feasibility Assessment](#1-feasibility-assessment)
2. [Algorithm Overview](#2-algorithm-overview)
3. [realism-effects poisson-recursive — Implementation Details](#3-realism-effects-poisson-recursive--implementation-details)
4. [Pros and Cons](#4-pros-and-cons)
5. [Image Quality Considerations](#5-image-quality-considerations)
6. [Denoising — Necessity & Options](#6-denoising--necessity--options)
7. [Performance Analysis](#7-performance-analysis)
8. [Integration into HobbyRenderer (NormalBasic)](#8-integration-into-hobbyrenderer-normalbasic)
9. [Half vs Full Resolution](#9-half-vs-full-resolution)
10. [Hi-Z Tracing](#10-hi-z-tracing)
11. [Bent Normals](#11-bent-normals)
12. [Debug Rendering Modes & ImGui Controls](#12-debug-rendering-modes--imgui-controls)
13. [Suggestions for Improvements](#13-suggestions-for-improvements)
14. [Implementation Roadmap](#14-implementation-roadmap)

---

## 1. Feasibility Assessment

### ✅ Fully Feasible — No Hardware RT Dependency

SSGI is entirely screen-space: ray-march against the depth buffer, sample from the direct-lighting buffer. Every piece of required input data already exists in the NormalBasic pipeline:

| Required Input | Available? | Source |
|---|---|---|
| Depth buffer | ✅ | GBuffer pass (already written) |
| Normals (world/view-space) | ✅ | GBuffer pass |
| Albedo | ✅ | GBuffer pass |
| Roughness / Metalness | ✅ | GBuffer pass (ORM texture) |
| Direct lighting buffer | ✅ | DeferredRenderer output (before SSGI) |
| Motion vectors | ✅ | GBuffer pass (motion vector RT) |
| Camera matrices (view/proj/inv) | ✅ | `Scene::m_View`, standard CB |
| Hi-Z buffer | ✅ | `HZBGeneratorPhase2` (already generated) |

### ⚠️ Risks

- **Screen-space visibility limitation**: SSGI only sees what's on screen — off-screen / occluded geometry contributes no indirect light. Mitigated by temporal accumulation; future DDGI integration will fill in off-screen contributions.
- **Integration complexity**: Must slot into the RenderGraph between `DeferredRenderer` (direct lighting) and `TAARenderer` (post-processing). The RenderGraph architecture supports this cleanly.
- **Shader compilation pipeline**: New `.hlsl` compute shaders must be registered in the SRRHI/ShaderID system.

---

## 2. Algorithm Overview

SSGI estimates indirect lighting by ray-marching in screen space. The realism-effects implementation handles **both** diffuse and specular indirect in a single unified effect — a hybrid of traditional SSGI (diffuse) and SSR (specular). We adopt this hybrid approach.

> **Note:** Traditional SSGI as defined by engines like Unity HDRP is diffuse-only, with specular indirect handled by a separate SSR pass. The realism-effects library bundles both into one `SSGIEffect` for convenience: a GGX VNDF specular ray is traced every frame, and a Fresnel-weighted Russian roulette additionally picks a diffuse (cosine hemisphere) ray ~50% of the time. This dual-channel design gives us specular GI "for free" since the GGX ray is already the base sampling strategy. We keep this architecture.

### Algorithm

```
For each pixel (at half or full res):
  1. Reconstruct world-space position P, normal N from GBuffer
  2. Generate GGX VNDF specular ray (always traced)
  3. Fresnel-weighted Russian roulette: diffuse ray (cosine hemisphere) ~50% of frames
  4. Ray-march both rays through depth buffer using Hi-Z acceleration
  5. On hit: sample direct-lighting buffer at hit UV for outgoing radiance
  6. Accumulate into diffuse and specular indirect buffers
  7. Apply spatial denoiser (Poisson bilateral, dual-channel)
  8. Temporally reproject with history rejection (dual-channel)
  9. Composite: final = direct + diffuseIndirect × albedo × (1-F) + specularIndirect × F
```

### Pass Breakdown

```
Depth Prepass ──> GBuffer Pass ──> Shadow Mask ──> Deferred Lighting ──> SSGI Trace (compute)
                                                                              │
                                                                              ▼
                                                                     Spatial Denoiser
                                                                              │
                                                                              ▼
                                                                     Temporal Reprojection
                                                                              │
                                                                              ▼
                                                                     SSGI Composite ──> TAA ──> Bloom ──> HDR
```

---

## 3. realism-effects poisson-recursive — Implementation Details

Our reference implementation. All shader logic below is from the `poisson-recursive` branch.

### 3.1 Architecture

| Component | Detail |
|---|---|
| Ray tracing shader | `ssgi.frag` + `ssgi_utils.frag` |
| Spatial denoiser | `PoissionDenoisePass` — dynamic Poisson disk generated via `PoissonUtils` |
| Denoise compose | BRDF-aware Fresnel reconstruction in `denoise_compose.frag` |
| Temporal reproject targets | `HalfFloatType` |
| Temporal blend | Explicit `blend` parameter (default 0.9) |
| Samples per pixel | Configurable `spp` (1, 2, 4…) |
| Neighborhood clamp | Disabled by default |
| Denoiser depth metric | `distToPlane()` — distance from neighbor to center plane |

### 3.2 Denoiser Details

```glsl
// Physically meaningful plane-distance metric
float distToPlane(worldPos, neighborWorldPos, worldNormal) {
    return abs(dot(worldPos - neighborWorldPos, worldNormal));
}

// Dynamically generated Poisson disk (not hardcoded)
const vec2 poissonDisk[8] = POISSON_DISK_SAMPLES;  // generated at runtime

// Age-aware aggressiveness (younger pixels = more aggressive denoising)
w = 1.0 / sqrt(accumulatedAge + 1.0);
wBasic = mix(wBasic, exp(-normalDiff * 10.0), w);
wFinal = w * pow(wBasic * exp(-lumaDiff * lumaPhi), phi / w);
```

### 3.3 Denoiser Compose — Fresnel-aware Reconstruction

```glsl
// Approximate multi-frame accumulated Fresnel
vec3 H = SampleGGXVNDF(V, roughness, roughness, 0.25, 0.25);
float VoH = max(EPSILON, dot(v, h));
VoH = pow(VoH, 0.875);  // multi-frame accumulation approximation

vec3 f0 = mix(vec3(0.04), diffuse, metalness);
vec3 F = F_Schlick(f0, VoH);

vec3 diffuseComponent  = diffuse * (1.0 - metalness) * (1.0 - F) * diffuseLightingColor;
vec3 specularComponent = specularLightingColor * F;
```

### 3.4 Memory Usage

| Buffer | Format |
|---|---|
| Temporal reproject (diffuse + specular) | `HalfFloatType` × 2 |
| Poisson denoise ping-pong (diffuse + specular) | `FloatType` × 4 |
| SSGI trace output | `FloatType` × 2 |

### 3.5 Key Strengths

1. **Clean architecture**: Separated denoise functions, `denoise_compose_functions.frag` for BRDF logic.
2. **Physically correct depth metric**: `distToPlane()` handles sloped surfaces correctly — neighbors on the same plane are weighted correctly regardless of raw depth difference.
3. **Lower memory**: HalfFloatType for temporal history (~2× reduction vs FloatType).
4. **Configurable `spp`**: Trace 1-4 rays/pixel, accumulated temporally.
5. **Dual-channel (diffuse + specular)**: GGX specular ray traced every frame; diffuse ray added via Fresnel-weighted Russian roulette ~50% of frames. Specular GI comes essentially "for free" since the GGX ray is the base sampling strategy.
6. **Author's own assessment**: The main-branch README states "poisson-recursive provides far better performance, quality and memory usage."

---

## 4. Pros and Cons

### Pros

| Pro | Detail |
|---|---|
| **No hardware RT needed** | Works entirely on rasterized GBuffer + depth. Fits NormalBasic perfectly. |
| **Reuses existing data** | Depth, normals, albedo, roughness, motion vectors, direct lighting — all already rendered. |
| **Low incremental cost** | A few compute dispatches + a few render targets. No new geometry passes. |
| **Handles dynamic scenes** | No precomputation. Works with animated geometry, moving lights. |
| **Temporal stability** | Converges to a clean result over ~8-16 frames when camera is still. |
| **Reference implementation available** | realism-effects poisson-recursive is well-structured GLSL → HLSL. |
| **Complements DDGI** | SSGI handles near-field detail; DDGI (future) handles far-field / off-screen. |

### Cons

| Con | Detail | Mitigation |
|---|---|---|
| **Screen-space only** | No GI from off-screen or occluded surfaces | Future DDGI integration for far-field contributions |
| **Noise under motion** | Camera movement causes disocclusion → noise spike | Robust temporal reprojection with history rejection |
| **Light leaking** | Depth discontinuities can cause false positives | `distToPlane()` depth metric; thickness heuristics |
| **Ray-march cost** | Expensive at long distances | Hi-Z acceleration (always on); half-res tracing |
| **Thin geometry** | Thin walls missed by ray marcher | Thickness parameter |

---

## 5. Image Quality Considerations

### 5.1 Expected Quality Level

```
SSAO ────────── SSGI ────────── DDGI/Probes ────────── RTGI (path traced)
(occlusion      (screen-space       (world-space          (full light
 only)           diffuse indirect)    probes)               transport)
```

SSGI provides:
- **Color-bleeding**: Red wall bounces red light onto adjacent white floor ✅
- **Diffuse indirect shadows**: Dark corners get darker ✅
- **Specular indirect**: Glossy surfaces reflect nearby colored objects ✅ (noisy but present, converges temporally)
- **Sky occlusion**: Ceilings block sky contribution ✅

### 5.2 Known Artifacts

| Artifact | Cause | Fix |
|---|---|---|
| Screen-edge darkening | Rays march off-screen, get no hit | Edge fade (clamp to screen border) |
| Boiling/flickering | Under-converged temporal accumulation | Increase `blend`; variance-guided adaptive blending |
| Ghosting on camera motion | Stale history reprojected to wrong pixels | History rejection (depth + normal + velocity) |
| Specular smearing | Specular GI inherently noisier | Higher `specularPhi`; separate specular temporal blend |
| Dark halos at depth edges | Depth discontinuity | `distToPlane()` specifically designed to fix this |

---

## 6. Denoising — Necessity & Options

### 6.1 Is Denoising Necessary?

**Yes.** Raw 1-spp SSGI has SNR ~1-2. Temporal accumulation alone needs 64+ frames.

### 6.2 Built-in Denoiser (realism-effects Poisson)

The poisson-recursive variant includes a **spatio-temporal denoiser**:
1. **Spatial**: Poisson-disk bilateral filter (8 samples, 1-2 iterations), edge-aware via depth/normal/roughness/luminance
2. **Temporal**: Motion-vector reprojection with neighborhood clamp + history rejection

**Verdict:** Good enough for NormalBasic. Lightweight (~0.15ms), handles edges, temporally stable.

### 6.3 NRD — Optional Upgrade Path

Your project already has NRD (`NrdIntegration.cpp`).

| NRD Denoiser | Use Case | Cost |
|---|---|---|
| **ReBLUR** | Diffuse + specular GI | ~1-2 ms @ 1080p |
| **Relax** | Lightweight diffuse-only | ~0.3-0.5 ms @ 1080p |

**Recommendation:** Start with built-in Poisson. NRD's `Relax` is available as an optional upgrade (~0.3ms extra).

### 6.4 Other Options

| Option | Cost | Quality |
|---|---|---|
| **À-Trous (ASVGF)** | Low-Medium | Wavelet-based, edge-preserving |
| **SVGF** (Falcor) | Medium | Production quality |
| **Bilateral blur** | Very Low | Poor — blurs edges |

À-Trous is the best lightweight alternative (~30-50% more than Poisson, better edge preservation).

---

## 7. Performance Analysis

### 7.1 Cost Breakdown (Estimated, RTX 3060-class @ 1080p, half-res)

| Pass | Threads | Est. GPU Time |
|---|---|---|
| SSGI Trace (1 ray diffuse + 1 specular, Hi-Z) | 960×540 | 0.2-0.3 ms |
| SSGI Trace (1 ray, linear) | 960×540 | 0.4-0.6 ms |
| Poisson Denoise (2 iter, dual-channel) | 960×540 | 0.1-0.15 ms |
| Temporal Reproject (dual-channel) | 1920×1080 | 0.15-0.2 ms |
| Denoiser Compose | 1920×1080 | 0.1-0.15 ms |
| **Total (Hi-Z)** | — | **~0.55-0.8 ms** |
| **Total (linear)** | — | **~0.75-1.1 ms** |

### 7.2 Scaling

| Resolution | Half-Res SSGI Time |
|---|---|
| 1080p | ~0.6-0.8 ms |
| 1440p | ~1.0-1.3 ms |
| 4K | ~2.0-2.5 ms |

### 7.3 Cost Reduction Options

1. **Half-res tracing** (default): ~2× vs full-res
2. **Hi-Z acceleration**: Always enabled — ~2× speedup for the ray-march step at no quality cost
3. **Lower step count**: `steps: 12 + refineSteps: 3` instead of `20 + 5`

---

## 8. Integration into HobbyRenderer (NormalBasic)

### 8.1 Pipeline Insertion Point

Current NormalBasic pass order (from `Renderer.cpp::ScheduleAndRunAllRenderers()`, CSM_Analysis.md):

```
ClearRenderer → OpaqueRenderer → MaskedPassRenderer → HZBGeneratorPhase2
→ ShadowRenderer → ShadowMaskRenderer → DDGIRenderer
→ DeferredRenderer → SkyRenderer → TransparentPassRenderer
→ TAARenderer → BloomRenderer → HDRRenderer → ImGuiRenderer
```

**SSGI insertion (after DeferredRenderer, before SkyRenderer):**

```
ClearRenderer → OpaqueRenderer → MaskedPassRenderer → HZBGeneratorPhase2
→ ShadowRenderer → ShadowMaskRenderer → DDGIRenderer
→ DeferredRenderer → [SSGIRenderer] ← NEW
→ SkyRenderer → TransparentPassRenderer
→ TAARenderer → BloomRenderer → HDRRenderer → ImGuiRenderer
```

**Rationale:**
- Needs direct-lighting buffer from `DeferredRenderer` as radiance source
- Must run before TAA (TAA provides additional temporal stability)
- Before SkyRenderer so sky pixels are excluded from SSGI

### 8.2 New Renderer Class

```cpp
// src/SSGIRenderer.h
class SSGIRenderer : public IRenderer
{
    // Resources (dual-channel: diffuse + specular)
    nvrhi::TextureHandle m_SSGIRawDiffuse;     // Raw traced diffuse GI
    nvrhi::TextureHandle m_SSGIRawSpecular;    // Raw traced specular GI
    nvrhi::TextureHandle m_SSGIDenoisedA[2];   // Ping-pong denoised (diffuse, specular)
    nvrhi::TextureHandle m_SSGIDenoisedB[2];
    nvrhi::TextureHandle m_SSGIHistory[2];     // Temporal history (diffuse, specular)
    nvrhi::TextureHandle m_SSGIComposited;     // Final composited output

    // Passes
    void TracePass(nvrhi::CommandListHandle cmd, const RenderGraph& rg);
    void SpatialDenoisePass(nvrhi::CommandListHandle cmd, const RenderGraph& rg);
    void TemporalReprojectPass(nvrhi::CommandListHandle cmd, const RenderGraph& rg);
    void CompositePass(nvrhi::CommandListHandle cmd, const RenderGraph& rg);

    // Settings
    float m_RayDistance = 10.0f;
    float m_Thickness = 10.0f;
    int   m_Steps = 20;
    int   m_RefineSteps = 5;
    int   m_Spp = 1;
    float m_Blend = 0.9f;
    float m_ResolutionScale = 0.5f;
    bool  m_EnableBentNormals = false;
    int   m_DenoiseIterations = 1;
    float m_DenoiseRadius = 3.0f;
    float m_DepthPhi = 2.0f;
    float m_NormalPhi = 50.0f;
    float m_LumaPhi = 5.0f;
    float m_SpecularPhi = 50.0f;
    int   m_DebugMode = 0;
};
```

### 8.3 Zero RT Dependencies

No TLAS, RTXDI, SHARC, OMM, or NRD needed. Pure compute dispatches on rasterized GBuffer + depth data.

---

## 9. Half vs Full Resolution

### 9.1 Industry Standard

| Engine | SSGI Resolution |
|---|---|
| Unreal Engine 5 (Lumen SSGI) | Half-res default |
| Unity HDRP | Half-res default |
| Call of Duty MW2019 | Half-res + checkerboard |

**Consensus:** Half-res is standard. Full-res is a quality luxury.

### 9.2 Recommendation: Half-Res Default

```
Default: 0.5 (half-res)
Quality: 1.0 (full-res) — screenshots / ultra
```

~90% visual quality at ~50% cost. Hi-Z acceleration is always enabled (strict 5-10× speedup with no quality tradeoff).

### 9.3 ImGui Control

```cpp
ImGui::Combo("SSGI Resolution", &resScaleIdx, "Quarter (0.25)\0Half (0.5)\0Full (1.0)\0");
```

---

## 10. Hi-Z Tracing

### 10.1 What It Is

Uses a mip-chain of the depth buffer to accelerate ray-marching. Coarser mips = larger steps. Refine at finer mips on potential hit.

### 10.2 Performance Impact

| Method | Steps for 500px | Performance |
|---|---|---|
| Linear (1px step) | 500 | 1× |
| Hi-Z | ~log₂(500) ≈ 9-12 | ~5-10× faster |

Hi-Z is the **single biggest optimization** for SSGI. It is always enabled — there is no scenario where linear march is preferable.

### 10.3 Integration

Your project already generates Hi-Z via `HZBGeneratorPhase2`:

```cpp
renderGraph.Import(g_HZBTexture, "HZB");
```

If HZB stores max-depth (occlusion culling convention), invert the comparison in the SSGI trace shader — trivial one-line change, no extra dispatch needed.

### 10.4 ImGui Control

```cpp

ImGui::SliderInt("SSGI Max Steps", &m_SSGISteps, 4, 40);
ImGui::SliderInt("SSGI Refine Steps", &m_SSGIRefineSteps, 0, 8);
```

---

## 11. Bent Normals

### 11.1 What Are Bent Normals?

The average unoccluded direction from a surface point, "bending" away from nearby occluders:

```
Geometric normal:         Bent normal:
      ↑                      ↗
   ┌──┴──┐               ┌──┴──┐
   │     │               │  █  │  ← occluder
   └─────┘               └─────┘
```

Computed via hemisphere visibility sampling: `BentNormal = normalize(∫ V(ω)·ω dω)`

### 11.2 How They Help SSGI

| Without | With |
|---|---|
| Rays sampled around geometric normal | Rays around bent normal |
| Rays go into walls → miss | Rays avoid occluded directions |
| Light leaking at edges | Cleaner occlusion |
| More rays needed | Fewer wasted rays |

### 11.3 Implementation

Implemented as a dedicated compute pass (~0.1-0.2ms). **Disabled by default** — toggle via ImGui. When enabled, replaces the geometric normal with the bent normal for hemisphere sampling in the SSGI trace pass, improving ray efficiency and reducing light leaking.

### 11.4 ImGui Control

```cpp
ImGui::Checkbox("SSGI Bent Normals", &m_SSGIEnableBentNormals);
ImGui::SliderFloat("Radius", &m_SSGIBentNormalRadius, 0.1f, 2.0f);
ImGui::SliderInt("Samples", &m_SSGIBentNormalSamples, 4, 32);
```

---

## 12. Debug Rendering Modes & ImGui Controls

### 12.1 Debug Visualization Modes

| Mode | What It Shows | Purpose |
|---|---|---|
| `SSGI_FINAL` | Full composite (direct + diffuse + specular) | Default |
| `SSGI_OFF` | Direct only (no SSGI) | Baseline A/B comparison |
| `SSGI_RAW_DIFFUSE` | Raw diffuse GI before denoising | Check diffuse ray-march quality |
| `SSGI_RAW_SPECULAR` | Raw specular GI before denoising | Check specular ray quality |
| `SSGI_DENOISED_DIFFUSE` | After spatial denoiser (diffuse) | Check denoiser effectiveness |
| `SSGI_DENOISED_SPECULAR` | After spatial denoiser (specular) | Check specular denoising |
| `SSGI_DIFFUSE_ONLY` | Final composite, diffuse GI only | Isolate diffuse contribution |
| `SSGI_SPECULAR_ONLY` | Final composite, specular GI only | Isolate specular contribution |
| `SSGI_INDIRECT_ONLY` | Indirect only (no direct light) | See exactly what SSGI adds |
| `SSGI_TEMPORAL_AGE` | Heatmap: red=young, green=converged | Debug temporal convergence |
| `SSGI_HIT_DISTANCE` | Heatmap of ray hit distances | Debug ray distance settings |
| `SSGI_RAY_MISS` | Red where rays miss, green where they hit | Debug ray-march efficiency |

### 12.2 ImGui UI Layout

```cpp
if (ImGui::CollapsingHeader("SSGI (Screen-Space GI)"))
{
    // Quality
    ImGui::Text("Quality");
    ImGui::Combo("Resolution", &m_SSGIResScaleIdx, "Quarter\0Half\0Full\0");
    ImGui::Checkbox("Bent Normals", &m_SSGIEnableBentNormals);
    ImGui::SliderInt("Samples/Pixel", &m_SSGISpp, 1, 4);
    ImGui::SliderInt("Max Steps", &m_SSGISteps, 4, 40);
    ImGui::SliderInt("Refine Steps", &m_SSGIRefineSteps, 0, 8);

    // Ray March
    ImGui::Text("Ray March");
    ImGui::SliderFloat("Distance", &m_SSGIRayDistance, 1.0f, 50.0f);
    ImGui::SliderFloat("Thickness", &m_SSGIThickness, 0.1f, 20.0f);

    // Temporal
    ImGui::Text("Temporal");
    ImGui::SliderFloat("Blend", &m_SSGIBlend, 0.5f, 1.0f);
    ImGui::Checkbox("Neighborhood Clamp", &m_SSGINeighborhoodClamp);

    // Denoiser
    ImGui::Text("Denoiser");
    ImGui::SliderInt("Iterations", &m_SSGIDenoiseIterations, 0, 4);
    ImGui::SliderFloat("Radius", &m_SSGIDenoiseRadius, 0.5f, 10.0f);
    ImGui::SliderFloat("Depth Phi", &m_SSGIDepthPhi, 0.1f, 20.0f);
    ImGui::SliderFloat("Normal Phi", &m_SSGINormalPhi, 1.0f, 100.0f);
    ImGui::SliderFloat("Luma Phi", &m_SSGILumaPhi, 0.5f, 20.0f);
    ImGui::SliderFloat("Specular Phi", &m_SSGISpecularPhi, 1.0f, 100.0f);

    // Debug
    ImGui::Text("Debug View");
    ImGui::Combo("Display Mode", &m_SSGIDebugMode,
        "Final\0Off\0Raw Diffuse\0Raw Specular\0"
        "Denoised Diffuse\0Denoised Specular\0"
        "Diffuse Only\0Specular Only\0"
        "Indirect Only\0Temporal Age\0Hit Distance\0Ray Miss\0");

    // NRD (future)
    ImGui::Checkbox("Use NRD Denoiser", &m_SSGIUseNRD);
}
```

---

## 13. Suggestions for Improvements

### 13.1 Short-Term

1. **Hi-Z from day one**: Already have HZB. Do it right the first time.
2. **Use `distToPlane()`**: The plane-distance metric is physically correct vs `abs(depthDiff) * 10000`.
3. **Half-res tracing**: Quality difference barely visible after denoising, performance doubles.

### 13.2 Medium-Term

4. **Variance-guided temporal blending**: Adaptive `blend` based on per-pixel variance reduces ghosting.
5. **Multi-bounce approximation**: Feed previous frame's SSGI as extra radiance source (clamped).
6. **À-Trous denoiser**: If Poisson shows edge-softening, swap to wavelet denoiser.

### 13.3 Long-Term

7. **Combine with DDGI**: SSGI near-field + DDGI far-field, blended by distance.
8. **Blue-noise sampling**: Better frequency characteristics, faster convergence.

---

## 14. Implementation Roadmap

### Phase 1: Core Infrastructure (est. 2-3 sessions)
```
1. Create SSGIRenderer class skeleton
   → verify: compiles, in renderer list
2. Register shader IDs (SSGITrace, SSGIDenoise, SSGITemporal, SSGIComposite)
   → verify: shaders load
3. RenderGraph resource declarations
   → verify: no validation errors
4. Wire into ScheduleAndRunAllRenderers for NormalBasic
   → verify: Setup/Render called each frame
```

### Phase 2: Ray March (est. 2-3 sessions)
```
5. Port SSGI trace shader from realism-effects (HLSL, dual-channel: diffuse + specular)
   → verify: raw diffuse and specular outputs are non-zero on surfaces
6. Implement Hi-Z tracing (leverage existing HZB)
   → verify: <10 steps average
7. Test different scenes, validate correctness
   → verify: "Raw" debug mode shows color-bleeding
```

### Phase 3: Denoising (est. 2-3 sessions)
```
8. Port Poisson spatial denoiser (poisson-recursive, dual-channel)
   → verify: <5% variance vs raw
9. Port temporal reprojection
   → verify: static camera noise-free in ~8 frames
10. Port denoiser compose
    → verify: "Final" mode shows plausible indirect lighting
```

### Phase 4: Polish & UI (est. 1-2 sessions)
```
11. Add ImGui controls
    → verify: sliders affect output in real-time
12. Add debug visualization modes
    → verify: each mode shows correct buffer
13. Bent normals compute pass + ImGui toggle (disabled by default)
    → verify: toggle produces visible difference in corners/crevices
14. Performance profiling
    → verify: <0.8ms total SSGI cost at 1080p half-res (excluding bent normals)
```

### Phase 5: Optional Enhancements (future)
```
15. Multi-bounce approximation
16. Variance-guided adaptive blending
17. NRD Relax integration
18. Blue-noise sampling
```

---

## Appendix A: Resources Required (Dual-Channel: Diffuse + Specular)

| Resource | Format | Resolution | Persistent? |
|---|---|---|---|
| SSGI Raw Diffuse | R11G11B10_FLOAT | traceRes × traceRes | No (per-frame) |
| SSGI Raw Specular | R11G11B10_FLOAT | traceRes × traceRes | No |
| SSGI Denoised A | R11G11B10_FLOAT × 2 | traceRes × traceRes | Yes (ping-pong) |
| SSGI Denoised B | R11G11B10_FLOAT × 2 | traceRes × traceRes | Yes |
| SSGI History Diffuse | R11G11B10_FLOAT | traceRes × traceRes | Yes (temporal) |
| SSGI History Specular | R11G11B10_FLOAT | traceRes × traceRes | Yes |
| SSGI Composited | R11G11B10_FLOAT | fullRes × fullRes | No |

**Total persistent memory (half-res 1080p):** ~12 MB  
**Total transient memory:** ~6 MB

---

## Appendix B: Key Design Decisions

| Decision | Choice | Rationale |
|---|---|---|
| GI type | Hybrid diffuse + specular | realism-effects traces both; specular comes "for free" via GGX base ray |
| Reference | realism-effects poisson-recursive | Better denoiser, `distToPlane()`, HalfFloat history |
| Default resolution | Half-res (0.5) | Industry standard; 2× faster |
| Hi-Z | Always enabled | Already have HZB; 5-10× speedup, no quality tradeoff |
| Denoiser | Built-in Poisson | Lightweight, no NRD dependency |
| Bent normals | Implemented, disabled by default | ~5-10% quality improvement; toggle via ImGui |
| NRD | Optional future upgrade | Relax denoiser if Poisson insufficient |

---

## Appendix C: File Index

| File | Purpose |
|---|---|
| `src/SSGIRenderer.h` | New renderer class |
| `src/SSGIRenderer.cpp` | Renderer implementation |
| `src/shaders/SSGITrace.hlsl` | Screen-space ray march (compute, dual-channel) |
| `src/shaders/SSGIDenoise.hlsl` | Poisson spatial denoiser (compute, dual-channel) |
| `src/shaders/SSGITemporal.hlsl` | Temporal reprojection (compute, dual-channel) |
| `src/shaders/SSGIComposite.hlsl` | Fresnel-aware BRDF composite (compute) |
| `src/shaders/SSGIUtils.hlsli` | Shared helpers (reconstruction, BRDF) |
| `REFERENCES/realism-effects-poisson-recursive/src/ssgi/` | Primary reference |
| `REFERENCES/realism-effects-poisson-recursive/src/denoise/` | Denoiser reference |
