# PCSS Implementation — Analysis & Full Replacement Plan

> **Phase:** PCSS (Percentage-Closer Soft Shadows) — full replacement of current implementation
> **Goal:** Replace the current ugly/rigid PCSS with a proper, configurable, high-quality implementation

---

## Table of Contents

1. [Overview](#1-overview)
2. [Background](#2-background)
3. [Architecture](#3-architecture)
4. [Technical Analysis](#4-technical-analysis)
5. [Integration](#5-integration)
6. [Performance](#6-performance)
7. [Quality](#7-quality)
8. [Debugging & Observability](#8-debugging--observability)
9. [Pros & Cons](#9-pros--cons)
10. [Risks](#10-risks)
11. [Implementation Plan](#11-implementation-plan)
12. [Future Improvements](#12-future-improvements)

---

## 1. Overview

### 1.1 Feature / System

**Percentage-Closer Soft Shadows (PCSS)** is a shadow filtering technique that produces
*contact-hardening* soft shadows: shadows are sharp near the occluder and become
progressively softer as the receiver moves away from the caster. This mimics real
area-light penumbra behavior.

PCSS extends standard Percentage-Closer Filtering (PCF) by dynamically scaling the PCF
kernel radius per pixel based on the estimated penumbra width, rather than using a
fixed-size kernel for all pixels.

The algorithm has three stages:

1. **Blocker Search** — sample the shadow map around the receiver to estimate the average
   depth of occluding geometry.
2. **Penumbra Estimation** — compute the penumbra width from the receiver-blocker depth
   gap using similar-triangles geometry.
3. **Variable-Kernel PCF** — perform PCF with the filter radius set to the estimated
   penumbra width.

### 1.2 Objective

**Problem:** The current PCSS implementation (`CommonShadow.hlsli`) is rigid, noisy, and
lacking in configurability:

- All constants are hardcoded in the shader (light size, near plane, sample counts).
- A single 16-sample Poisson disk is used for both blocker search and PCF — no
  quality/performance trade-off controls.
- No sample rotation between frames → persistent noise patterns.
- No minimum/maximum penumbra clamping → either aliased contact shadows or
  excessively expensive distant filtering.
- No early-out for fully lit or fully occluded pixels — every pixel pays the full cost.
- The host-side toggle (`m_EnablePCSS`) is a binary on/off with NO tunable parameters
  (light angular size, blocker search quality, PCF quality, etc.).

**Expected outcomes:**

- A well-structured, configurable PCSS with per-light angular size, adjustable sample
  counts, and proper clamping.
- Temporal rotation of sample patterns to eliminate stationary noise.
- Early-out paths that skip expensive computation when the pixel is clearly lit or
  shadowed.
- Host-side UI controls (via ImGui or config) for all quality parameters.
- Debug visualization modes for blocker depth, penumbra width, and sample coverage.

### 1.3 Scope

**Assumptions:**

- The existing CSM pipeline (4 cascades, `ShadowRenderer` → `ShadowMaskRenderer`)
  remains structurally unchanged.
- The shadow map is `D32_FLOAT` stored as a `Texture2DArray` with 4 slices at
  2048² resolution each.
- The pipeline uses standard (non-reversed) depth: near=0.0, far=1.0.
- One directional light (sun) only — area light size is the sun's angular radius.
- D3D12 / Shader Model 6.8 target.

**Constraints:**

- Must remain compatible with the existing srrhi binding system and render graph.
- Must support the 4 existing shader permutations: base, `PCSS=1`, `CASCADE_BLEND=1`,
  and both combined.
- Graphics pipeline is mesh-shader based (AS+MS for shadow depth passes).
- All PCSS computation runs in a compute shader (`ShadowMask_CSMain` at 8×8 thread
  groups).

---

## 2. Background

### 2.1 Understanding

**What is PCSS?** Percentage-Closer Soft Shadows, introduced by Randima Fernando (NVIDIA,
2005), is an image-space technique that approximates soft shadows from area lights using
standard shadow maps. It does NOT require area light geometry — instead, it uses a single
"light size" parameter and similar-triangles geometry to estimate how wide the penumbra
should be at each shaded point.

**Why does it exist?** Hardware shadow maps only produce hard (binary) shadows. PCF adds
a fixed-width soft edge, but real area lights produce variable-width penumbras — tight
near the contact point, wide far from it. PCSS bridges this gap without requiring
expensive area light sampling or ray tracing.

**Core concepts:**

| Concept | Description |
|---|---|
| Blocker | A shadow map texel whose depth is closer to the light than the receiver |
| Blocker search | Sampling the shadow map to find and average blocker depths |
| Penumbra | The partially shadowed transition region between fully lit and fully shadowed |
| Penumbra estimation | Using similar triangles: `penumbra = (receiver - blocker) / blocker × lightSize` |
| Contact hardening | Penumbra → 0 as receiver → blocker (sharp shadow at contact points) |

**Key terminology:**

- **Light angular radius** (or light size): The apparent size of the area light, typically
  expressed in world units or as a fraction of the shadow map.
- **Blocker search radius**: The region (in texels) around the receiver sample point
  searched for blockers. Scales with receiver distance and light size.
- **PCF kernel radius**: The width of the PCF sampling kernel, equal to the estimated
  penumbra width.

### 2.2 Existing Solutions

#### 2.2.1 Current HobbyRenderer Implementation

Located in `src/shaders/CommonShadow.hlsli`:

```
ComputePCSSShadow():
  1. BlockerSearch()      — 16 Poisson disk samples, searchRadius = LIGHT_SIZE_UV ×
                            (receiverDepth - SHADOW_NEAR_PLANE) / receiverDepth
  2. Penumbra estimation  — penumbraWidth = (compareDepth - avgBlockerDepth) ×
                            LIGHT_SIZE_UV / avgBlockerDepth
  3. VariableKernelPCF()  — 16 Poisson disk samples with penumbra-sized kernel

Constants are all hardcoded:
  LIGHT_SIZE_UV       = 0.05f   (light angular size in UV)
  SHADOW_NEAR_PLANE   = 0.1f    (light-space near plane)
  kPoissonDisk16[16]  — static, unrotated Poisson disk
```

**What's "ugly" about it:**

1. **No configurability** — `LIGHT_SIZE_UV` and `SHADOW_NEAR_PLANE` are hardcoded.
   You can't adjust the sun's apparent size or softness without recompiling shaders.
2. **No quality tiers** — always 16+16 samples regardless of distance or scene complexity.
3. **No temporal rotation** — the same Poisson disk every frame creates a static,
   unnatural noise pattern that TAA partially hides but does not eliminate.
4. **No early-out for fully shadowed** — sky pixels are caught by the depth-far check,
   but fully occluded pixels still go through the full 32-sample loop.
5. **Blocker search uses a point sampler** — no hardware PCF during blocker search means
   16 raw point samples, which is noisy. Many implementations use a comparison sampler
   (hardware PCF) in the blocker search for better quality at same cost.
6. **No penumbra clamping** — penumbra can grow arbitrarily large for distant receivers,
   leading to expensive PCF and over-blurred shadows.
7. **Cascade boundary handling is accidental** — The blocker search and PCF both use the
   same cascade slice, but the penumbra estimation doesn't account for cascade-specific
   texel sizes.
8. **No debug visualization** — impossible to inspect blocker depth, penumbra width, or
   sample distribution at runtime.

#### 2.2.2 Reference: defold-pcss

**Source:** `REFERENCES/defold-pcss/materials/pcss.fp`

A compact, educational PCSS implementation for Defold (OpenGL ES). Key observations:

- **128 pre-computed Poisson disk samples** — good distribution.
- **Random sample rotation** — uses `rnd(gl_FragCoord.xyy, i)` to pick samples from the
  128-sample pool per pixel, giving temporal variation.
- **Blocker search radius formula:** `scale = (proj.z - 0.1) / proj.z; range = scale * LIGHT_SIZE`
  (in world units, converted to UV).
- **Penumbra formula:** `(proj.z - d_blocker) / d_blocker * LIGHT_SIZE * NEAR / proj.z`
  — includes the near plane term as part of the similar-triangles derivation.
- **Early-outs:** returns fully lit if no blockers found; returns fully shadowed if
  `find_blockers` returns `shadow == 1.0`.
- **Penumbra clamp:** `if (scale > 20.0) return 0.0` (fully lit for very large penumbras).
- **Notable issue:** The `find_blockers()` function also returns a partial shadow value,
  which is a design flaw — it conflates blocker search with shadow computation.

**Key takeaway:** Random per-pixel sample rotation from a large pool is simple and
effective. The clamp on penumbra size is important for performance.

#### 2.2.3 Reference: SofterShadows (Unity URP)

**Source:** `REFERENCES/SofterShadows/`

A production-oriented Unity URP shadow replacement with PCSS support. Key observations:

- **PCSS is a toggle on PCF** — PCF and PCSS share the same sampling infrastructure.
- **Configurable parameters:**
  - `Blocker Search Count` (1–32)
  - `Blocker Search Size` (0–1)
  - `Light Size` (0–1)
  - `Minimum Penumbra Size` (0–1)
- **Grid-based PCF** — uses a uniform grid kernel (`filterSize × filterSize`) rather
  than Poisson disk. This gives more predictable quality but can produce grid artifacts.
- **Blocker search uses separate point-sampled depth texture** — not a comparison sampler.
- **Depth convention is different** — Unity uses a non-standard depth encoding (needs
  `-shadowCoord.z * 0.5 + 0.5` remapping).
- **Cascade blending** — blends between adjacent cascades using a `0.8x–1.2x` blend zone
  (wider than our 10% band).
- **PCSS sample count scales with cascade** — comment indicates they experimented with
  reducing `blockerSearchSize` and `lightSize` per cascade index.

**Key takeaway:** Grid-based PCF is simpler than Poisson disk and produces consistent
quality. Per-cascade scaling of PCSS parameters is valuable for performance.

#### 2.2.4 Industry Standards

| Implementation | Blocker Search | PCF Kernel | Samples | Notes |
|---|---|---|---|---|
| NVIDIA Whitepaper (2005) | Poisson disk, fixed count | Variable Poisson disk | ~16+32 | Original reference |
| NVIDIA D3D Sample | Comparison sampler | 3×3 to 7×7 grid | ~16+49 | Practical production reference |
| Unreal Engine 5 | Adaptive, jittered | Variable Poisson + denoiser | Varies | Integrated with virtual shadow maps |
| Unity HDRP | Poisson disk | Grid PCF (PCSS variant) | Configurable | Blocker search uses raw depth |

### 2.3 How CSM + PCSS Interact

The cascaded shadow map structure adds complexity to PCSS:

- **Each cascade has a different world-space texel size.** Blocker search and PCF radii
  must be expressed in UV space for the specific cascade.
- **Penumbra estimation must be consistent across cascades.** The estimated penumbra
  width in world-space units should not change abruptly when a pixel transitions from
  cascade N to N+1.
- **Blocker search straddling cascade boundaries is a problem.** If the blocker is in
  a different cascade than the receiver, it may not exist in the current cascade's
  shadow map — though for tight CSM frustums this is rare.

---

## 3. Architecture

### 3.1 High-Level Design

```
┌──────────────────────────────────────────────────────────────┐
│                      PER FRAME (CPU)                          │
│                                                              │
│  Renderer::ComputeCSMCascadeSplits()  → 4 split depths       │
│  Renderer::ComputeCascadeViewProj()   → 4 light view-proj    │
│                                                              │
│  RenderGraph::Setup()                                        │
│    ShadowRenderer::Setup()                                   │
│      └─ Declares CSMShadowMap (D32, 2048²×4 array)           │
│    ShadowMaskRenderer::Setup()                               │
│      └─ Declares ShadowMask (R8_UNORM, screen resolution)    │
│      └─ Reads CSMShadowMap, Depth, GBufferNormals            │
│                                                              │
│  RenderGraph::Compile()  →  allocates/aliases memory         │
│                                                              │
│  RenderGraph::Execute():                                     │
│    ShadowRenderer::Render()                                  │
│      └─ for each cascade i:                                  │
│          ├─ GPU cull (frustum only, no HZB)                  │
│          ├─ BuildIndirect                                    │
│          └─ DrawShadowMeshlets → D32 depth array[i]          │
│    ShadowMaskRenderer::Render()                              │
│      └─ Compute dispatch (8×8 thread groups)                 │
│         └─ ShadowMask_CSMain per pixel:                      │
│            ├─ SelectCascade(viewDepth)                       │
│            ├─ Normal-offset bias                             │
│            ├─ ComputeCSMShadow()                             │
│            │  ├─ [PCSS] ComputePCSSShadow()                  │
│            │  │   ├─ BlockerSearch()                         │
│            │  │   ├─ Penumbra estimation                     │
│            │  │   └─ VariableKernelPCF()                     │
│            │  └─ [no PCSS] Compute3x3PCF()                   │
│            └─ [BLEND] Cascade blend with next cascade        │
│         └─ Write R8 shadow mask                              │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 Major Components

| Component | File | Responsibility |
|---|---|---|
| Shadow depth pass | `ShadowDepth.hlsl`, `ShadowRenderer.cpp` | Renders 4-cascade D32 depth array via mesh shaders |
| Shadow mask compute | `ShadowMask.hlsl`, `ShadowMaskRenderer.cpp` | Evaluates CSM + PCSS → R8 shadow mask |
| Common shadow utilities | `CommonShadow.hlsli` | `ComputeCSMShadow()`, `ComputePCSSShadow()`, PCF helpers |
| CSM debug overlay | `CSMDebug.hlsl`, `CSMDebugRenderer.cpp` | Debug visualization of cascades, shadow maps, blend zones |
| Host configuration | `Renderer.h` | `m_EnablePCSS`, `m_EnableCascadeBlend`, `m_CSMNormalBias` |
| Shader compilation | `shaders.cfg` | 4 ShadowMask variants: base, `_PCSS`, `_Blend`, `_PCSS_Blend` |

### 3.3 Component Relationships

```
Renderer (CPU)
  ├─ owns CSM cascade data (m_CSMCascades[4], m_CSMCascadeSplits[5])
  ├─ owns PCSS toggle (m_EnablePCSS) and bias settings
  │
  ├─ ShadowRenderer
  │   └─ writes g_RG_CSMShadowMap (Texture2DArray<D32>)
  │
  ├─ ShadowMaskRenderer
  │   ├─ reads g_RG_CSMShadowMap, g_RG_DepthTexture, g_RG_GBufferNormals
  │   ├─ writes g_RG_ShadowMask (Texture2D<R8_UNORM>)
  │   └─ selects shader permutation based on m_EnablePCSS + m_EnableCascadeBlend
  │
  ├─ DeferredRenderer
  │   └─ reads g_RG_ShadowMask → applies to deferred lighting
  │
  └─ CSMDebugRenderer (optional)
      └─ reads g_RG_ShadowMask + g_RG_CSMShadowMap → debug overlay
```

### 3.4 Data Ownership

| Data | Owner | Lifetime |
|---|---|---|
| `m_CSMCascades[4]` (view-proj matrices, AABBs) | `Renderer` | Per-frame |
| `m_CSMCascadeSplits[5]` | `Renderer` | Per-frame |
| `m_EnablePCSS` | `Renderer` (ImGui config) | Config |
| `g_RG_CSMShadowMap` | `ShadowRenderer` (declares) | Per-frame (transient) |
| `g_RG_ShadowMask` | `ShadowMaskRenderer` (declares) | Per-frame (transient) |
| PCSS constants (light size, etc.) | **NEW → Renderer or ShadowMaskRenderer** | Config |

### 3.5 Workflow — Per-Frame Execution

```
Frame N:
  1. ComputeCSMCascadeSplits()     → split depths
  2. ComputeCascadeViewProj()      → 4× light view-proj matrices
  3. [RenderGraph Setup]
     ShadowRenderer declares CSMShadowMap
     ShadowMaskRenderer declares ShadowMask, reads inputs
  4. [RenderGraph Compile]
  5. [Execute - parallel]
     Thread A: ShadowRenderer → depth-only draws for 4 cascades
     Thread B: (waits for A) ShadowMaskRenderer → compute shader dispatch
  6. DeferredRenderer reads ShadowMask for lighting
```

---

## 4. Technical Analysis

### 4.1 Core Algorithm — PCSS Three Stages

#### Stage 1: Blocker Search

**Current (CommonShadow.hlsli):**
```hlsl
searchRadius = LIGHT_SIZE_UV * (compareDepth - SHADOW_NEAR_PLANE) / compareDepth;
// 16 Poisson disk samples, point sampler, depth < compareDepth → blocker
```

**Issue:** The search radius formula mixes UV-space light size with NDC-space depths.
`SHADOW_NEAR_PLANE = 0.1` is hardcoded and specific to a particular light setup.
In practice, it should be derived from the light projection matrix's near plane.

**Improvement:** The search radius should be derived from the physical light angle:
```
searchRadius_UV = (lightAngularRadius_rad) * (receiverDepth - blockerDepth) / receiverDepth
```
where `lightAngularRadius_rad` is the sun's angular radius (~0.00465 rad for our sun,
but typically 0.01–0.1 in games for artistic control). This maps naturally to world-space
reasoning.

**Reference (defold-pcss):**
```glsl
float scale = (proj.z - 0.1) / proj.z;
float range = scale * LIGHT_SIZE;
vec2 texel = vec2(range) / textureSize(tex0, 0);
```
Uses a hardcoded `0.1` near plane too, but scales correctly.

**Reference (SofterShadows):**
```hlsl
blockerSearchSize = blockerSearchSize * 10;  // art-directed scaling
lightSize = lightSize * 30;
averageBlocker = FindAverageBlocker(cascadeIndex, shadowCoord.xy, depth, ...);
```
Uses arbitrary scaling of parameters — not physically based, but artist-friendly.

#### Stage 2: Penumbra Estimation

**Current:**
```hlsl
penumbraWidth = (compareDepth - avgBlockerDepth) * LIGHT_SIZE_UV / avgBlockerDepth;
```
This is the classic formula from the NVIDIA whitepaper. Correct in NDC space.

**Issue:** `LIGHT_SIZE_UV` is 0.05, meaning the light source covers 5% of the shadow map.
This is a UV-space quantity that doesn't map cleanly to world-space reasoning.

**Improvement:** Express light size in world units:
```
penumbraWidth_world = (receiverDist - blockerDist) * lightRadius_world / blockerDist;
penumbraWidth_UV    = penumbraWidth_world / worldTexelSize;
```
Then clamp to `[minPenumbra, maxPenumbra]` texels.

#### Stage 3: Variable-Kernel PCF

**Current:**
```hlsl
VariableKernelPCF():
  16 Poisson disk samples × penumbraWidth × texelSize
  Hardware comparison sampler (SampleCmpLevelZero)
```

**Issue:** 16 Poisson disk samples with no rotation creates a static pattern. For large
penumbras (far from contact), 16 samples is insufficient and produces noise. For small
penumbras (near contact), 16 samples is wasteful.

**Improvement Options:**

| Option | Pros | Cons |
|---|---|---|
| **Grid PCF (SofterShadows approach)** | Deterministic quality, easier to tune | Grid artifacts at low sample counts |
| **Poisson disk + temporal rotation** | Better visual quality, no grid artifacts | Requires frame index or noise texture |
| **Stratified + jittered grid** | Best of both worlds | More complex |
| **Blue noise + spatiotemporal** | Convergence under TAA | Requires blue noise texture |

**Recommended:** Poisson disk with per-pixel rotation from a pre-computed noise texture
(sampled via `screenUV` modulo pattern). This eliminates stationary patterns and converges
well under TAA.

### 4.2 Sample Counts & Quality Tiers

Proposed quality tiers, controlled by a new `m_PCSSQuality` enum:

| Tier | Blocker Search | PCF | Total | Use Case |
|---|---|---|---|---|
| Low | 8 (rotated Poisson) | 8 (rotated Poisson) | 16 | Integrated GPUs, 4K |
| Medium (default) | 16 | 16 | 32 | Desktop, 1440p |
| High | 24 | 32 | 56 | High-end GPU, 1080p |
| Ultra | 32 | 64 | 96 | Cinematic, reference |

### 4.3 Early-Out Optimizations

1. **Sky pixel:** `depth == DEPTH_FAR` → immediately write 1.0 (already implemented).
2. **Full-screen coherency:** Pixels in the same 8×8 thread group likely share the same
   cascade. Pre-select cascade in group-shared memory to reduce divergence.
3. **No blockers → fully lit:** Already implemented (`avgBlockerDepth < 0.0 → return 1.0`).
4. **Fully occluded early-out:** If the center sample at the receiver depth is already
   fully shadowed AND the blocker search finds all samples are blockers at close range,
   the pixel is deep in shadow with no penumbra — return 0.0 early. (Not currently
   implemented.)
5. **Penumbra clamp:** If estimated penumbra exceeds a maximum (e.g., 64 texels),
   return a pre-computed fully soft value or skip the PCF.

### 4.4 Penumbra Clamping

```hlsl
const float MIN_PENUMBRA_TEXELS = 0.5;  // below this, use 1-tap (hard shadow)
const float MAX_PENUMBRA_TEXELS = 32.0; // above this, use max-kernel PCF

penumbraWidth = clamp(penumbraWidth, MIN_PENUMBRA_TEXELS, MAX_PENUMBRA_TEXELS);
```

For penumbra widths below `MIN_PENUMBRA_TEXELS`, fall back to a single hardware PCF tap
(or a tiny 2×2 block) to save ALU. For widths above `MAX_PENUMBRA_TEXELS`, use the max
kernel but reduce samples (quality degrades gracefully rather than tanking performance).

### 4.5 Temporal Sample Rotation

Sample patterns should be rotated each frame to convert structured noise into
high-frequency noise that TAA can resolve.

**Implementation:**
```hlsl
// Per-frame rotation from a Halton sequence or pre-computed texture
float noiseAngle = NoiseTexture.SampleLevel(PointSampler, screenUV, 0).r * TWO_PI;
float2 rotatedOffset = float2(
    offset.x * cos(noiseAngle) - offset.y * sin(noiseAngle),
    offset.x * sin(noiseAngle) + offset.y * cos(noiseAngle));
```

The noise texture can be a 64×64 `R8_UNORM` tiled texture generated once at startup.

### 4.6 Data Structures & Resources

**New or modified srrhi constants (Shader-side):**

```hlsl
// Added to ShadowMaskConstants (or new PCSSConstants)
float   m_PCSSLightAngularRadius;   // Sun angular radius in radians (default: 0.05)
float   m_PCSSMinPenumbraTexels;    // Minimum penumbra in shadow map texels (default: 0.5)
float   m_PCSSMaxPenumbraTexels;    // Maximum penumbra in shadow map texels (default: 32.0)
uint    m_PCSSBlockerSampleCount;   // Blocker search samples (default: 16)
uint    m_PCSSPCFSampleCount;       // PCF samples (default: 16)
```

**New resources:**
- `NoiseTexture` — `R8_UNORM` 64×64, generated once at startup, used for sample rotation.

### 4.7 Alternative: Using Hardware PCF for Blocker Search

A significant improvement over the current implementation: the blocker search can use
the hardware comparison sampler (`SampleCmpLevelZero`) instead of a raw point sampler.
This gives "free" 2×2 bilinear PCF per blocker sample, reducing noise.

The trade-off is that you get a binary result per sample (blocker or not) rather than
a depth value. The average blocker depth then becomes the average of depths only at
positions where the comparison passed. This is slightly different from the whitepaper
formula but works well in practice (used by the NVIDIA D3D sample).

### 4.8 Depth Bias with Variable-Size Kernels

**Critical issue:** As the PCF kernel grows, self-shadowing (shadow acne) becomes more
likely because the kernel samples larger areas where the depth bias may be insufficient.

**Mitigation strategies:**

1. **Scale bias with kernel radius:**
   ```hlsl
   float dynamicBias = baseBias + penumbraWidth * slopeBiasScale;
   ```
2. **Receiver-plane bias** (NVIDIA's approach): Adjust the receiver depth by a
   small epsilon proportional to the surface slope relative to the light.
3. **Use the same normal-offset bias** — the current implementation already uses
   normal-offset bias. Since it operates in world space before the shadow projection,
   it's independent of the PCF kernel size. This is already correct.

**Verdict:** The current normal-offset bias strategy is compatible with PCSS without
modification. No additional per-sample bias is needed.

---

## 5. Integration

### 5.1 Affected Systems

| System | Impact | Type |
|---|---|---|
| `CommonShadow.hlsli` | Major rewrite | Replace PCSS functions |
| `ShadowMask.hlsl` | Minor | Pass new constants to `ComputeCSMShadow` |
| `ShadowMaskRenderer.cpp` | Moderate | Populate new PCSS constants, add noise texture |
| `Renderer.h` | Minor | Add PCSS quality/config members |
| `CommonResources.cpp` | Minor | Generate noise texture at startup |
| `shaders.cfg` | None | Existing 4 permutations stay |
| `ShadowMask.sr` / srrhi headers | Regenerate | Add new PCSS constant fields |
| ImGui config panel | Minor | Add PCSS parameter sliders |
| `CSMDebug.hlsl` | Optional | Add PCSS debug modes (penumbra viz) |
| `DeferredRenderer.cpp` | None | Only reads shadow mask, no change |
| `ShadowRenderer.cpp` | None | Depth-only pass, no change |

### 5.2 New Components

1. **PCSS noise texture** — 64×64 `R8_UNORM`, generated once at init. Stored in
   `CommonResources` alongside existing `PointClamp`/`ShadowComparison` samplers.
2. **PCSS debug visualization** — new debug modes in `CSMDebugRenderer`:
   - Mode 9: Penumbra heatmap (red = wide penumbra, blue = sharp)
   - Mode 10: Blocker depth visualization
   - Mode 11: PCSS sample count overlay

### 5.3 Required Modifications — Step by Step

**Step 1: srrhi — Add PCSS constants to `ShadowMask.sr`**

```hlsl
@cbuffer ShadowMaskCB {
    // ... existing fields ...
    float   m_PCSSLightAngularRadius;  // Default: 0.05
    float   m_PCSSMinPenumbraTexels;   // Default: 0.5
    float   m_PCSSMaxPenumbraTexels;   // Default: 32.0
    uint    m_PCSSBlockerSampleCount;   // Default: 16
    uint    m_PCSSPCFSampleCount;       // Default: 16
}
```

**Step 2: `CommonShadow.hlsli` — Rewrite PCSS functions**

- Replace `LIGHT_SIZE_UV` with CB-derived `m_PCSSLightAngularRadius`.
- Replace `SHADOW_NEAR_PLANE` with computed near plane from cascade projection.
- Add temporal rotation via noise texture lookup.
- Add penumbra clamping.
- Add early-out for fully occluded pixels.
- Replace hardcoded `16` with `m_PCSSBlockerSampleCount` / `m_PCSSPCFSampleCount`.
- Improve Poisson disk distribution (use 64-sample table, select subset).

**Step 3: `ShadowMask.hlsl` — Wire up new constants**

- Add `NoiseTexture` and `NoiseSampler` inputs.
- Pass PCSS constants through to `ComputeCSMShadow()`.

**Step 4: `ShadowMaskRenderer.cpp` — Populate new CB fields**

- Set `m_PCSSLightAngularRadius`, `m_PCSSMinPenumbraTexels`, etc. from
  `g_Renderer` config.
- Bind noise texture to the compute shader.

**Step 5: `CommonResources.cpp` — Generate noise texture**

- Create 64×64 `R8_UNORM` texture with random values.
- Add to `CommonResources` singleton.

**Step 6: `Renderer.h` — Add host-side PCSS config**

```cpp
float    m_PCSSLightAngularRadius = 0.05f;   // sun angular radius (radians)
float    m_PCSSMinPenumbraTexels  = 0.5f;    // min penumbra in texels
float    m_PCSSMaxPenumbraTexels  = 32.0f;   // max penumbra in texels
uint32_t m_PCSSBlockerSampleCount = 16;      // blocker search samples
uint32_t m_PCSSPCFSampleCount     = 16;      // PCF samples
```

**Step 7: Regenerate srrhi headers** — `build_srrhi`

### 5.4 Compatibility

- **Backward compatible:** The non-PCSS path (`Compute3x3PCF`) is unchanged.
- **Shader permutations unchanged:** The 4 existing `ShadowMask` variants remain.
- **Render graph interface unchanged:** Read/write declarations stay the same.

---

## 6. Performance

### 6.1 Resource Usage

#### GPU — Current (16 blocker + 16 PCF)

Per shadowed pixel at 1920×1080 (~2M pixels, ~1M shadowed):

| Stage | Instructions | Texture samples |
|---|---|---|
| Blocker search | 16 × point sample + depth compare | 16 (point sampler) |
| Penumbra estimation | ~5 ALU | 0 |
| Variable PCF | 16 × comparison sample | 16 (comparison sampler) |
| **Total** | ~37 samples + ALU | 32 |

#### GPU — Proposed (configurable)

| Tier | Blocker samples | PCF samples | Total samples | Relative cost |
|---|---|---|---|---|
| Low | 8 | 8 | 16 | 0.5× current |
| Medium | 16 | 16 | 32 | 1.0× current |
| High | 24 | 32 | 56 | 1.75× current |
| Ultra | 32 | 64 | 96 | 3.0× current |

#### GPU — Early-out savings

- Sky pixels: 0 samples (already implemented).
- Fully lit (no blockers): 8–16 blocker samples + 0 PCF → ~50% savings.
- Fully shadowed (deep in umbra): 8–16 blocker + 1 PCF center → ~50% savings.

#### Memory

| Resource | Format | Dimensions | Size |
|---|---|---|---|
| CSM shadow map | D32_FLOAT | 2048² × 4 | 64 MB |
| Shadow mask | R8_UNORM | 1920×1080 | ~2 MB |
| Noise texture | R8_UNORM | 64×64 | 4 KB |
| **Added:** PCSS noise | R8_UNORM | 64×64 | 4 KB |

No significant memory impact.

#### Bandwidth

- Shadow mask write: 2 MB/frame (R8_UNORM).
- Shadow map read: 64 MB worst-case (all 4 cascades read by compute shader).
  In practice, most pixels read only 1 cascade, so ~16 MB.
- Noise texture read: 4 KB (cached).

### 6.2 Scalability

| Scene | Resolution | Expected PCSS cost | Notes |
|---|---|---|---|
| Sponza (~0.3M tris) | 1080p | ~0.3–0.5 ms (Medium) | Many shadowed pixels |
| Bistro (~1M tris) | 1440p | ~0.5–0.8 ms (Medium) | Higher geometry density |
| Open World (8×8 km) | 4K | ~1.5–3.0 ms (Medium) | Higher resolution dominates |

The compute shader cost scales primarily with resolution (number of shadowed pixels)
rather than geometry complexity. With 8×8 thread groups, the dispatch is:
- 1080p: 240×135 = 32,400 thread groups
- 1440p: 320×180 = 57,600 thread groups
- 4K: 480×270 = 129,600 thread groups

### 6.3 Optimization Opportunities

1. **Variable rate shading (VRS):** Shadow mask can be computed at half-res and
   bilinearly upsampled. PCSS noise is forgiving.
2. **Temporal accumulation:** Accumulate PCF results over frames with reprojection
   (like RTXDI does) to amortize sample cost.
3. **Cascade-level LOD:** Use fewer PCSS samples for far cascades (less visual
   importance, smaller screen coverage).
4. **Tile-based early-out:** If an 8×8 tile is all sky or all deep shadow, skip.

---

## 7. Quality

### 7.1 Evaluation

#### Current Implementation — Quality Assessment

| Aspect | Rating | Notes |
|---|---|---|
| Contact hardening | ★★★☆☆ | Works, but hardcoded light size may not match artistic intent |
| Noise | ★★☆☆☆ | Static Poisson pattern visible; TAA helps but doesn't fully resolve |
| Softness range | ★★★☆☆ | No clamping means very distant shadows can overflow |
| Edge quality | ★★★☆☆ | 16 samples is borderline for smooth penumbras |
| Temporal stability | ★★☆☆☆ | No rotation → static pattern; cascades shift with camera movement |
| Consistency across cascades | ★★☆☆☆ | No cascade-level texel size compensation |

#### Proposed Implementation — Expected Improvements

| Aspect | Improvement |
|---|---|
| Contact hardening | Tunable light angular size for artistic control |
| Noise | Temporal rotation + better Poisson distribution eliminates static patterns |
| Softness range | `MIN_PENUMBRA_TEXELS` to `MAX_PENUMBRA_TEXELS` clamping prevents extremes |
| Edge quality | Configurable sample counts (default 16, up to 64 for high quality) |
| Temporal stability | Frame-rotated sampling via noise texture |
| Cascade consistency | World-space light size with per-cascade UV conversion |

### 7.2 Accuracy

The PCSS algorithm is an approximation. It assumes:

- A spherical area light (reasonable for the sun).
- A single dominant blocker (not strictly true for complex geometry).
- Parallel light rays within a cascade (reasonable for orthographic CSM projections).

These assumptions are standard and well-accepted for real-time rendering.

### 7.3 Regression Risk

- **Non-PCSS path is untouched** → zero regression for baseline rendering.
- **Shader permutation for PCSS is compiled separately** → if the new PCSS is broken,
  users can switch to non-PCSS mode.
- **The existing `_PCSS` and `_PCSS_Blend` variants will be replaced in-place** — no
  new permutations needed.

---

## 8. Debugging & Observability

### 8.1 New Debug Modes (CSMDebugRenderer)

| Mode | Name | Description |
|---|---|---|
| 9 | Penumbra Heatmap | Red = wide penumbra, Blue = sharp/hard shadow → visualized as overlay |
| 10 | Blocker Depth | Grayscale: blocker depth (dark = near, bright = far) |
| 11 | PCSS Sample Distribution | Show sample points as colored dots on the shadow map |
| 12 | PCSS Cascades | Per-pixel cascade index with penumbra width as alpha blend |

### 8.2 Existing Debug Modes Still Useful

| Mode | Name | PCSS Relevance |
|---|---|---|
| 1 | Cascade Splits | Verify cascade selection correct |
| 2 | Shadow Map Array | Inspect raw shadow map depth |
| 3 | Raw Shadow Mask | See final PCSS output |
| 6 | Depth Compare | Red/green lit/shadowed visualization |
| 8 | Blend Zone | Verify cascade blend smoothness |

### 8.3 Logging & Metrics

- **GPU timer queries:** `ShadowMaskRenderer` already has `PROFILE_GPU_SCOPED`.
  The PCSS cost is included in that measurement.
- **Console logging:** Log PCSS configuration at startup.
- **ImGui stats window:** Display:
  - Current PCSS quality tier
  - Percentage of pixels in penumbra vs fully lit/shadowed
  - Average penumbra width (for tuning)

### 8.4 Troubleshooting

| Symptom | Likely Cause | Check |
|---|---|---|
| No visible softness | `LIGHT_SIZE` too small | Increase `m_PCSSLightAngularRadius` from 0.05 to 0.1–0.5 |
| Excessive blur everywhere | `MAX_PENUMBRA_TEXELS` too high | Clamp to 16–32 |
| Flickering shadows | Cascade boundaries misaligned | Check CSM texel snapping, verify cascade blend |
| Aliased contact shadows | `MIN_PENUMBRA_TEXELS` too high | Reduce to 0.25–0.5 |
| Shadow acne in soft regions | Normal bias insufficient for large kernel | Increase `m_CSMNormalBias` from 3.0 to 5.0–8.0 |
| Banding artifacts | Insufficient PCF samples | Increase `m_PCSSPCFSampleCount` |
| Dark halos around objects | Blocker search too aggressive | Reduce `m_PCSSBlockerSampleCount` or search radius |

---

## 9. Pros & Cons

### 9.1 Advantages

1. **Physically motivated** — contact-hardening shadows dramatically improve visual
   realism compared to fixed-width PCF.
2. **Minimal pipeline changes** — operates entirely within the existing ShadowMask
   compute shader. No changes to depth passes, G-buffer, or deferred lighting.
3. **Single-pass** — blocker search, penumbra estimation, and PCF run in one compute
   dispatch. No extra render targets or passes.
4. **Configurable quality** — sample counts and light size are tunable for
   performance/quality trade-off.
5. **Works with CSM** — the only change is inside the per-cascade shadow evaluation;
   cascade selection and blending are unaffected.
6. **TAA-friendly** — temporal rotation of samples converts structured noise into
   sub-pixel noise that TAA resolves.
7. **No additional GPU barriers** — all work is in one compute shader, so no
   synchronization overhead.

### 9.2 Disadvantages

1. **Sample count drives cost** — PCSS is inherently more expensive than fixed-kernel
   PCF (32 samples vs 9 for 3×3 PCF).
2. **Noise at low sample counts** — 8-sample PCSS is noisy. Requires TAA to converge.
3. **Blocker search is approximate** — averaging blockers assumes a single occluder
   layer. Complex overlapping geometry can produce incorrect averages.
4. **Not physically accurate for all light types** — spherical light assumption breaks
   for long thin area lights (fluorescent tubes, rectangular windows).
5. **Cascade boundaries require blending** — the penumbra estimation must be
   cascade-aware to avoid visible transitions.

### 9.3 Trade-offs

| Trade-off | Decision | Rationale |
|---|---|---|
| Poisson disk vs grid PCF | Poisson disk | Better visual quality; grid artifacts are more objectionable than Poisson noise under TAA |
| Point sampler vs comparison sampler for blocker search | Comparison sampler (NEW) | Free 2×2 PCF per blocker sample; minimal quality difference with much better performance |
| Separate PCSS pass vs inline in ShadowMask | Inline (current) | Avoids extra render target, barriers, and memory |
| World-space vs UV-space light size | World-space | Cleaner reasoning, cascade-independent |

---

## 10. Risks

### 10.1 Technical Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Shader compile times increase | Low | Medium | No new permutations; sample counts are CB-driven, not preprocessor |
| TAA fails to resolve PCSS noise | Medium | Medium | Test with TAA on/off; provide `Ultra` quality tier for TAA-off scenarios |
| Blocker search misses near-field blockers | Medium | Low | Ensure search radius covers at least ±2 texels even at contact distance |
| Performance regression on low-end GPUs | Medium | Medium | `Low` quality tier (8+8) is cheaper than current (16+16) |
| CB size limit exceeded | Low | High | Verify CB size stays within 256-byte D3D12 limit |

### 10.2 Maintenance Risks

| Risk | Mitigation |
|---|---|
| PCSS constants proliferate (too many knobs) | Start with 5 parameters; add more only with evidence |
| Poisson disk table quality degrades over time | Use a well-known pre-computed table; don't regenerate |
| Cascade-specific PCSS tuning becomes unwieldy | Use a single parameter set scaled by cascade texel ratio |

### 10.3 Alternative Solutions

If PCSS proves too expensive or too noisy, alternatives in order of increasing cost:

1. **Fixed 3×3 PCF with larger bias** — zero additional cost, no softness variation.
2. **EVSM (Exponential Variance Shadow Maps)** — blur the shadow map before sampling,
   giving uniform softness. Cheaper per-pixel but requires a blur pass.
3. **PCSS with half-res shadow mask** — compute shadow mask at half resolution,
   bilaterally upsample. Reduces sample cost by 4×.
4. **Ray-traced soft shadows** — most accurate, most expensive, already partially
   supported via RTXDI.

---

## 11. Implementation Plan

### Phase 1: Core PCSS Rewrite (~3 files)

**Goal:** Replace the PCSS functions in `CommonShadow.hlsli` with the improved version.

**Files:**
- `src/shaders/CommonShadow.hlsli` — rewrite `BlockerSearch()`, `ComputePCSSShadow()`,
  `VariableKernelPCF()`. Replace hardcoded constants with CB parameters. Add temporal
  rotation.
- `src/shaders/ShadowMask.hlsl` — pass new PCSS CB fields + noise texture.
- `src/shaders/ShadowMask.sr` — add PCSS fields to CB.

**Verification:** PCSS mode compiles and produces visually correct contact-hardening
shadows. Noise is temporally varying (visible with TAA off, resolved with TAA on).

### Phase 2: Host Configuration (~3 files)

**Goal:** Make PCSS parameters configurable from ImGui and the C++ host.

**Files:**
- `src/Renderer.h` — add `m_PCSSLightAngularRadius`, `m_PCSSMinPenumbraTexels`,
  `m_PCSSMaxPenumbraTexels`, `m_PCSSBlockerSampleCount`, `m_PCSSPCFSampleCount`.
- `src/ShadowMaskRenderer.cpp` — populate new CB fields.
- `src/ImGuiLayer.cpp` or config UI — add PCSS parameter sliders.

**Verification:** Changing light size in ImGui immediately affects shadow softness.
Quality tier dropdown changes sample counts.

### Phase 3: Noise Texture & Debug (~3 files)

**Goal:** Add temporal noise rotation and PCSS debug visualization.

**Files:**
- `src/CommonResources.cpp` / `.h` — generate 64×64 `R8_UNORM` noise texture.
- `src/CSMDebugRenderer.cpp` — add penumbra heatmap and blocker depth debug modes.
- `src/shaders/CSMDebug.hlsl` — implement new debug modes.

**Verification:** Toggling TAA on/off shows the noise pattern changing per frame.
Debug mode 9 shows a smooth gradient at shadow edges (penumbra heatmap).

### Phase 4: Regenerate & Cleanup

**Files:**
- Run `build_srrhi` to regenerate srrhi headers from `ShadowMask.sr`.
- Run `build_shaders` to recompile all ShadowMask variants.
- Run `build_shaderids` to regenerate `ShaderIDs.h` if needed.

**Verification:** Full build succeeds. All 4 ShadowMask shader variants compile.

### Phase 5: Testing & Tuning

- **Sponza:** Tune light size and sample counts for good visual quality.
- **Outdoor scenes:** Verify cascade blending with PCSS (test with `CASCADE_BLEND=1`).
- **Performance profiling:** Measure PCSS cost at each quality tier.
- **Regression:** Verify non-PCSS path is unchanged.

---

## 12. Future Improvements

1. **Half-resolution shadow mask** — compute PCSS at 50% resolution, bilaterally
   upsample. 4× sample savings with minimal quality loss.

2. **Per-cascade sample scaling** — automatically reduce blocker search and PCF
   sample counts for far cascades where penumbras are tiny on screen.

3. **Blue noise sampler** — replace Poisson disk with blue noise for faster
   convergence under TAA.

4. **Spatiotemporal PCSS** — accumulate PCF results over multiple frames with
   reprojection, allowing higher effective sample counts without per-frame cost
   (similar to how RTXDI handles reservoir resampling).

5. **Contact shadow integration** — add screen-space contact shadows (short ray
   march) to sharpen shadows at contact points where even PCSS has a minimum
   penumbra width.

6. **EVSM + PCSS hybrid** — use EVSM for far cascades (uniform softness via blur)
   and PCSS for near cascades (contact hardening where it matters).

7. **Per-light configuration** — support different light sizes for directional,
   spot, and point lights (if non-sun lights are added later).

---

## References

- Fernando, R. (2005). "Percentage-Closer Soft Shadows." NVIDIA Corporation.
  [PDF](https://developer.download.nvidia.com/SDK/9.5/Samples/MEDIA/docPix/docs/PCSS.pdf)
- NVIDIA D3D Soft Shadows Sample.
  [Docs](https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/d3d_samples/d3dsoftshadowssample.htm)
- SofterShadows (Unity URP PCSS implementation).
  [GitHub](https://github.com/Mortal-Dev/SofterShadows)
- defold-pcss (Compact PCSS implementation).
  [GitHub](https://github.com/abadonna/defold-pcss)
