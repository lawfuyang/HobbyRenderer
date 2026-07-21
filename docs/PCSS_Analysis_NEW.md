# PCSS — Percentage-Closer Soft Shadows
## Analysis & Implementation Plan for HobbyRenderer

---

## 1. OVERVIEW

### Feature / System

**Percentage-Closer Soft Shadows (PCSS)** is a real-time shadow filtering technique that produces physically plausible soft shadow penumbrae by estimating the average depth of occluders in a neighbourhood of the shadow map, then using that estimate to scale a variable-radius PCF kernel. The result is contact-hardening: shadows are sharp where a surface is close to its caster and soft where it is far away.

The implementation extends the existing `ComputeCSMShadow` path in `CommonShadow.hlsli` with a multi-stage pipeline: shadow-map-min-reduction-gated blocker search with wave-level reduction and dual robust estimation, adaptive PCF driven by a blue-noise Poisson disc texture, and a dedicated temporal shadow history buffer for stable accumulation independent of FSR3 TAA.

### Objective

**Problem solved:** The current `Compute3x3PCF` kernel is fixed at 3×3 texels. It produces uniformly soft shadows regardless of receiver–caster distance, which looks wrong: a wall touching the ground should cast a hard shadow, while a tree canopy 10 m above should cast a wide, blurry penumbra.

**Expected outcomes:**
- Contact-hardening: penumbra width scales with `(receiverDepth − blockerDepth) / blockerDepth × lightRadius`.
- Visually plausible soft shadows for the sun disc (default `m_AngularSize = 0.533°`).
- No regression in fully-lit or fully-shadowed regions.
- Temporal stability via a dedicated shadow history buffer (independent of FSR3 TAA).
- Cascade 3 falls back to fixed 3×3 PCF — PCSS is only applied to cascades 0–2 where it is statistically effective.

### Scope

**Assumptions:**
- Only the `NormalBasic` rendering mode uses CSM shadows; PCSS targets that path exclusively.
- The sun angular radius is available via `Scene::Light::m_AngularSize` and passed to the shader through `ShadowMaskConstants`.
- Motion vectors (`g_RG_GBufferMotionVectors`) are already in the render graph.
- Shadow map resolution stays at `kShadowMapResolution = 2048` per cascade.
- SM 6.x wave intrinsics (`WaveActiveSum`, `WaveActiveMin`) are available on the target hardware.

**Constraints:**
- PCSS runs as a single compute dispatch (same as the current `ShadowMask_CSMain`). The temporal resolve is a second dispatch in the same `ShadowMaskRenderer::Render()` call.
- Must not break the `CASCADE_BLEND` permutation.
- Shader permutation system requires entries in `shaders.cfg`; a `PCSS=1` define is the toggle. The four permutations `{PCSS=0,1} × {CASCADE_BLEND=0,1}` are already present in `shaders.cfg`.
- The blue-noise Poisson disc texture is loaded from `external/LDR_RG01_0.png` at app load via `CommonResources::Initialize()`, using the same `LoadTexture`/`UploadTexture` pattern as BRDF LUT and atmosphere textures.

---

## 2. BACKGROUND

### What is PCSS?

PCSS was introduced by Fernando (NVIDIA, 2005) as an extension of Percentage-Closer Filtering (PCF). Standard PCF averages binary shadow comparisons over a fixed kernel, producing uniformly blurry shadows. PCSS makes the kernel radius a function of the estimated blocker depth, so the penumbra width varies spatially.

The core insight is the **similar-triangles penumbra equation**:

```
penumbraWidth = (receiverDepth - avgBlockerDepth) / avgBlockerDepth * lightRadius
```

This is physically motivated: a larger light source or a more distant occluder produces a wider penumbra.

### Why does it exist?

Fixed-radius PCF is cheap but unconvincing. Hard shadows (no PCF) are fast but aliased and physically wrong for area lights. PCSS sits between the two: it is more expensive than fixed PCF but produces the contact-hardening behaviour that makes shadows feel grounded.

### How is it commonly used?

- **Game engines (Unreal, Unity HDRP, Frostbite):** PCSS or a variant (DPCF, SPCSS) is the default for directional lights with area penumbra.
- **Film/offline:** Used as a fast approximation before path-traced shadows became practical.
- **Research extensions:** VSSM (Variance Soft Shadow Maps), SAVSM (Summed-Area VSM), MSMSM (Moment Shadow Maps) all attempt to reduce the sample count of the blocker search.

### Core concepts

| Concept | Description |
|---|---|
| **Blocker search** | Sample the shadow map in a disc around the receiver; collect depths shallower than the receiver. |
| **Robust blocker estimation** | Nearest-biased weighted average + Welford outlier rejection to stabilise multi-layer scenes. |
| **Penumbra estimation** | Apply the similar-triangles formula to get a world-space penumbra radius. |
| **Adaptive PCF** | Run a second sampling pass with the estimated radius using a blue-noise Poisson disc. |
| **Contact hardening** | When `avgBlockerDepth ≈ receiverDepth`, penumbra → 0 (hard shadow). |
| **Shadow map min-reduction early-out** | Query the shadow map's own min-reduction mip chain at a mip level corresponding to the search radius; skip full blocker search if the region is fully lit. |
| **Wave-level reduction** | Share blocker search results across a 2×2 pixel quad using SM 6.x wave intrinsics. |
| **Temporal history** | Dedicated R8_UNORM persistent texture; reprojected with motion vectors; neighbourhood-clamped. |
| **Precomputed search radius** | Per-cascade UV search radius computed on the CPU once per frame and passed in the CB. |

### Key terminology

- **Receiver depth:** The shadow-map depth of the pixel being shaded.
- **Blocker depth:** The shadow-map depth of a sample that is in front of the receiver (occluding it).
- **Search radius (UV):** The shadow-map UV radius for the blocker search; precomputed per cascade on the CPU.
- **PCF radius (UV):** The shadow-map UV radius for the final visibility filter; equals the penumbra width in UV space.
- **Texel size:** `1.0 / kShadowMapResolution = 1/2048 ≈ 0.000488`.
- **Blue-noise Poisson disc:** A 64×64 R8G8_UNORM texture where each texel stores a 2D sample offset in `[0,1]²`, remapped to `[-1,1]²` in the shader. Pre-baked as a binary asset (void-and-cluster generation takes 2–5 seconds at 64×64 — not suitable for runtime).

### Existing Solutions

**Current implementation (`CommonShadow.hlsli`):**
- `Compute3x3PCF`: 9-tap fixed 3×3 grid, hardware bilinear comparison.
- `ComputeCSMShadow`: cascade selection, normal-offset bias, optional cascade blend.
- No blocker search, no variable penumbra.

**Alternative approaches:**

| Technique | Quality | Cost | Notes |
|---|---|---|---|
| Fixed 3×3 PCF (current) | Low | ~9 taps | No penumbra variation |
| Fixed large PCF (e.g. 5×5) | Medium | ~25 taps | Uniform blur, no contact hardening |
| **PCSS (this implementation)** | High | ~28 taps + temporal | Contact hardening, early-out-gated, wave-reduced |
| VSSM / VSM | Medium | 1 tap + mip | Leaks on thin geometry, no contact hardening |
| MSMSM (Moment SM) | High | 1 tap + mip | Complex, requires 4-channel shadow map |
| Ray-traced shadows | Reference | 1+ rays/pixel | Already available in `Normal` mode |
| DPCF (Unreal) | High | ~16 taps | Penumbra mask + PCF, similar to PCSS |

**Industry standards:**
- Unreal Engine 5 uses PCSS by default for directional lights (Lumen path uses ray tracing).
- Unity HDRP uses PCSS with a configurable sample count.
- Frostbite uses a variant called SPCSS (Stochastic PCSS) with temporal accumulation — the same pipeline implemented here.

---

## 3. ARCHITECTURE

### High-Level Design

```
ShadowMaskRenderer::Render()
    │
    ├── Dispatch 1: ShadowMask_CSMain  (PCSS=1)
    │       │
    │       ├── Per-pixel: reconstruct worldPos, worldNormal, viewDepth
    │       │
    │       ├── Cascade 0–2: ComputePCSSShadow()
│       │       ├── Shadow map min-reduction early-out (mip query → skip if fully lit)
    │       │       ├── Wave-level blocker search (12 taps, raw SampleLevel)
    │       │       │       ├── WaveActiveMin  → nearestBlocker
    │       │       │       └── WaveActiveSum  → blockerSum, blockerCount
    │       │       ├── Robust blocker estimation
    │       │       │       ├── Nearest-biased weighted average
    │       │       │       └── Welford outlier rejection
    │       │       ├── Penumbra estimation (similar-triangles)
    │       │       └── Adaptive PCF (16 taps, blue-noise disc, SampleCmpLevelZero)
    │       │
    │       ├── Cascade 3: Compute3x3PCF() fallback
    │       │
    │       └── Write g_RWShadowMask (raw stochastic result)
    │
    └── Dispatch 2: ShadowMaskTemporal_CSMain
            ├── Reproject via g_GBufferMotionVectors
            ├── Sample g_ShadowHistory (previous frame)
            ├── Neighbourhood min/max clamp (3×3 of current frame)
            ├── Blend: lerp(current, clampedHistory, α=0.9)
            └── Write g_RWShadowMask (final) + g_RWShadowHistory (for next frame)
```

### Major Components

| Component | File | Role |
|---|---|---|
| `ComputePCSSShadow` | `CommonShadow.hlsli` | Full PCSS function, PCSS=1 path |
| `Compute3x3PCF` | `CommonShadow.hlsli` | Retained; used for cascade 3 fallback |
| `ShadowMask_CSMain` | `ShadowMask.hlsl` | Stage 1 compute entry point |
| `ShadowMaskTemporal_CSMain` | `ShadowMask.hlsl` | Stage 2 temporal resolve entry point |
| `ShadowMaskConstants` | `ShadowMask.sr` | Extended CB: search radii, frame index, light radius |
| `ShadowMaskInputs` | `ShadowMask.sr` | Extended: blue-noise tex, motion vectors, history, point sampler |
| `ShadowMaskRenderer` | `ShadowMaskRenderer.cpp` | Two dispatches; persistent history texture; CB fill |
| `CommonResources` | `CommonResources.h/.cpp` | `ShadowSamplerPoint` + `BlueNoiseTex` |
| `shaders.cfg` | `shaders.cfg` | Already has `{PCSS=0,1} × {CASCADE_BLEND=0,1}` permutations |

### Component Relationships

```
CommonResources::Initialize()
    ├── Creates ShadowSamplerPoint (point, clamp-to-border white)
    └── Loads BlueNoiseTex from external/LDR_RG01_0.png (64×64 R8G8_UNORM)

ShadowMaskRenderer::Setup()
    ├── DeclareTexture(ShadowMask, R8_UNORM, UAV)          ← transient, written by stage 1
    └── DeclarePersistentTexture(ShadowHistory, R8_UNORM)  ← survives across frames

ShadowMaskRenderer::Render()
    ├── Fills ShadowMaskConstants
    │       ├── m_SearchRadii[4]      ← precomputed per-cascade on CPU
    │       ├── m_LightAngularRadius  ← from Scene::Light::m_AngularSize
    │       └── m_FrameIndex          ← from g_Renderer.m_FrameNumber
    │
    ├── Dispatch 1: ShadowMask_CSMain (PCSS=1)
│       Inputs:  Depth, GBufferNormals, CSMShadowMap, BlueNoiseTex
    │       Outputs: RWShadowMask (raw)
    │
    └── Dispatch 2: ShadowMaskTemporal_CSMain
            Inputs:  RWShadowMask (raw), ShadowHistory, GBufferMotionVectors, Depth
            Outputs: RWShadowMask (final), RWShadowHistory
```

### Data Ownership

- `ShadowMaskRenderer` owns the `ShadowMaskConstants` CB (volatile, recreated each frame).
- `ShadowMaskRenderer` owns the persistent `g_RG_ShadowHistory` texture (declared via `DeclarePersistentTexture`).
- `CommonShadow.hlsli` owns the PCSS algorithm; it is stateless (no persistent GPU state).
- `CommonResources` owns `BlueNoiseTex` and `ShadowSamplerPoint` (created once at app init, never freed during normal operation).

---

## 4. TECHNICAL ANALYSIS

### Precomputed Search Radius (CPU)

The blocker search radius in UV space is derived from the light angular radius and the cascade's world-space footprint. For an orthographic CSM projection:

```
worldTexelSize[i] = 2.0f / (kShadowMapResolution * |VP_row0[i]|)
lightRadius_UV[i] = tan(m_LightAngularRadius) * kSearchScale / kShadowMapResolution
```

This is computed once per cascade per frame in `ShadowMaskRenderer::Render()` and stored in `m_SearchRadii[4]` (a `float4` in the CB). The shader reads `m_SearchRadii[cascadeIndex]` directly — no per-pixel division required.

```cpp
// CPU — ShadowMaskRenderer::Render()
for (uint32_t i = 0; i < 3; ++i)  // cascades 0-2 only; cascade 3 uses fixed PCF
{
    const float3 vpRow0 = /* extract from m_CSMCascades[i].m_ViewProj */;
    const float worldTexelSize = 2.0f / (kShadowMapResolution * length(vpRow0));
    const float lightRadiusWorld = tanf(lightAngularRadius) * kSearchScale;
    searchRadii[i] = lightRadiusWorld * worldTexelSize;  // UV units
}
searchRadii[3] = 0.0f;  // unused; cascade 3 falls back to fixed PCF
cb.SetSearchRadii(searchRadii);
```

### Blue-Noise Poisson Disc Texture

A 64×64 R8G8_UNORM texture where each texel stores a 2D sample offset in `[0, 255]` (remapped to `[-1, 1]²` in the shader). The texture wraps in both dimensions.

**Loading:** The texture `external/LDR_RG01_0.png` is a pre-generated 64×64 two-channel blue-noise texture. It is loaded by `CommonResources::Initialize()` using the existing `LoadTexture` + `UploadTexture` path, the same way BRDF LUT and atmosphere textures are loaded. The path is resolved relative to the exe directory (same convention as other assets).

The shader indexes it as:

```hlsl
// frameIndex rotates the base tile offset each frame (temporal decorrelation)
uint2 tileOffset = uint2(frameIndex * 7u, frameIndex * 13u) & 63u;
uint2 texel      = (pixelPos + tileOffset + uint2(sampleIndex * 5u, sampleIndex * 3u)) & 63u;
float2 xi        = g_BlueNoiseTex.Load(uint3(texel, 0)).rg * (2.0f / 255.0f) - 1.0f;
```

The prime-stride offsets (`5`, `3`) ensure consecutive samples within the same pixel do not alias to the same tile row/column. The per-frame tile shift provides temporal decorrelation without a per-pixel RNG.

**Why a texture over a static Poisson array?**
- A static array produces the same spatial pattern every frame at every pixel. Even with rotation, the pattern is structured and resolves slowly with temporal accumulation.
- A blue-noise texture provides spatially uncorrelated, spectrally optimal sample distributions. The noise resolves faster with temporal accumulation and produces less visible banding.

### Stage 1 — Shadow Map Min-Reduction Early-Out

Before running the full 12-tap blocker search, the shader queries a **min-reduction mip chain of the shadow map itself** at a mip level corresponding to the search radius.

The shadow map (`Texture2DArray<float>`, `D32_FLOAT`) is sampled with the existing `MinReductionClamp` sampler from `CommonResources`:

```hlsl
// Compute the mip level that covers the search disc in shadow-map texel space
float shadowMip = log2(searchRadiusUV * kShadowMapResolution);
shadowMip = clamp(shadowMip, 0.0f, (float)(SHADOW_MAP_MIP_COUNT - 1));

// Sample shadow map with min-reduction: shallowest (nearest-to-light) depth in the search region
float minBlockerDepth = g_CSMShadowMap.SampleLevel(g_SamplerMinReduction,
    float3(shadowUV.xy, slice), shadowMip).r;

// If even the shallowest depth in the region is behind the receiver, no blockers exist
if (minBlockerDepth >= receiverDepth - bias)
    return 1.0f;  // fully lit — skip blocker search entirely
```

**Requirement:** The shadow map must have a full min-reduction mip chain. This is generated by `ShadowRenderer` after the depth pass using the existing `GenerateMipsUsingSPD` function with `SPD_REDUCTION_MIN` — the same mechanism already used for the scene depth pyramid. `ShadowRenderer` must be extended to: (1) create the shadow map with `mipLevels > 1`, (2) declare an SPD atomic counter buffer, and (3) call `g_Renderer.GenerateMipsUsingSPD(shadowMap, spdCounter, commandList, "ShadowMapMips", srrhi::CommonConsts::SPD_REDUCTION_MIN)` after the depth pass.

This early-out eliminates the blocker search for all fully-lit pixels, which is the majority of the screen in typical scenes. This is the single largest performance win in the pipeline.

### Stage 2 — Wave-Level Blocker Search

The blocker search runs 12 taps per pixel. Because adjacent pixels in the same wave sample very similar shadow-map regions, `WaveActiveSum` and `WaveActiveMin` pool the per-pixel blocker statistics across all active lanes in the wave:

```hlsl
float blockerSum    = 0.0f;
float blockerCount  = 0.0f;
float nearestBlocker = 1e10f;

[unroll]
for (int i = 0; i < BLOCKER_SAMPLES; ++i)
{
    float2 xi     = SampleBlueNoise(pixelPos, i, frameIndex);
    float2 offset = xi * searchRadiusUV;
    float  z      = g_CSMShadowMap.SampleLevel(g_ShadowSamplerPoint,
                        float3(shadowUV.xy + offset, slice), 0).r;

    if (z < receiverDepth - bias)
    {
        blockerSum    += z;
        blockerCount  += 1.0f;
        nearestBlocker = min(nearestBlocker, z);
    }
}

// Wave-level reduction: pool results across all active lanes in the wave
blockerSum     = WaveActiveSum(blockerSum);
blockerCount   = WaveActiveSum(blockerCount);
nearestBlocker = WaveActiveMin(nearestBlocker);

if (blockerCount == 0.0f) return 1.0f;  // fully lit
```

**What this achieves:** Each pixel still performs 12 texture fetches. The wave reduction aggregates the blocker statistics from all lanes in the wave (32 lanes on NVIDIA, 32 or 64 on AMD RDNA). Because each pixel uses a different blue-noise tile offset, the 12 samples per pixel are spatially decorrelated from neighbouring pixels. Pooling across a 32-lane wave gives an effective **384-sample** blocker estimate (32 × 12) at the cost of 12 fetches per pixel. The wave reduction itself is register-to-register with no memory traffic.

**Workgroup layout:** With `[numthreads(8, 8, 1)]`, threads are mapped row-major (X-first) into the wave. On a 32-lane wave, threads `(0,0)–(7,0)` and `(0,1)–(7,1)` (two full rows) form one wave. All these pixels share very similar shadow UVs (differing by at most a few texels), making the pooled blocker estimate coherent.

**Correctness note:** `WaveActiveSum` and `WaveActiveMin` are available in all shader stages in SM 6.0 (confirmed by Microsoft docs). They operate on all *active* lanes, so divergent flow control (e.g., the cascade-3 early-out) correctly excludes those lanes from the reduction.
A plain average of blocker depths is unstable when the search disc straddles multiple depth layers (e.g., a thin branch in front of a wall). Both robust estimators are applied in sequence:

#### Estimator A — Nearest-Biased Weighted Average

```hlsl
// Weight each blocker by proximity to the nearest blocker.
// k controls how sharply the weight falls off with depth distance.
static const float kBlockerWeightK = 50.0f;

float weightedSum   = 0.0f;
float weightedTotal = 0.0f;

[unroll]
for (int i = 0; i < BLOCKER_SAMPLES; ++i)
{
    if (blockerDepths[i] < receiverDepth - bias)
    {
        float w = exp(-abs(blockerDepths[i] - nearestBlocker) * kBlockerWeightK);
        weightedSum   += blockerDepths[i] * w;
        weightedTotal += w;
    }
}

float avgBlockerDepth = (weightedTotal > 0.0f) ? weightedSum / weightedTotal : nearestBlocker;
```

This makes the nearest occluder dominate, which is physically correct: the nearest blocker determines the umbra boundary. Distant co-occluders (e.g., a wall behind a branch) are suppressed exponentially.

#### Estimator B — Welford Outlier Rejection

After the weighted average, a second pass rejects samples that deviate more than 2σ from the mean. This uses Welford's online algorithm, which computes mean and variance in a single pass without storing all samples:

```hlsl
// Welford online mean/variance over the wave-pooled blocker set
float wMean = 0.0f, wM2 = 0.0f;
float wCount = 0.0f;

[unroll]
for (int i = 0; i < BLOCKER_SAMPLES; ++i)
{
    if (blockerDepths[i] < receiverDepth - bias)
    {
        wCount += 1.0f;
        float delta  = blockerDepths[i] - wMean;
        wMean  += delta / wCount;
        float delta2 = blockerDepths[i] - wMean;
        wM2    += delta * delta2;
    }
}

float variance = (wCount > 1.0f) ? wM2 / (wCount - 1.0f) : 0.0f;
float stddev   = sqrt(variance);

// Second pass: reject outliers beyond 2σ, recompute weighted average
float filteredSum = 0.0f, filteredWeight = 0.0f;
[unroll]
for (int i = 0; i < BLOCKER_SAMPLES; ++i)
{
    if (blockerDepths[i] < receiverDepth - bias
        && abs(blockerDepths[i] - wMean) <= 2.0f * stddev + 1e-5f)
    {
        float w = exp(-abs(blockerDepths[i] - nearestBlocker) * kBlockerWeightK);
        filteredSum    += blockerDepths[i] * w;
        filteredWeight += w;
    }
}

float robustBlockerDepth = (filteredWeight > 0.0f)
    ? filteredSum / filteredWeight
    : avgBlockerDepth;  // fallback to estimator A if all samples rejected
```

**Why both?** Estimator A (nearest-biased) handles the common case of a dominant near occluder with distant co-occluders. Estimator B (Welford) handles pathological cases where the nearest occluder is itself an outlier (e.g., a single stray texel from a thin wire). Together they cover the full range of multi-layer blocker configurations.

**Storage:** Both estimators operate on the `blockerDepths[BLOCKER_SAMPLES]` array, which is a local register array. At 12 samples × 4 bytes = 48 bytes, this fits comfortably in the register file without spilling on SM 6.x. Note that the Welford pass and the nearest-biased pass both iterate over this same stored array — this is two `[unroll]` loops over 12 elements, which is standard HLSL and compiles to straight-line ALU code.

### Stage 4 — Penumbra Estimation

```hlsl
float penumbraUV = (receiverDepth - robustBlockerDepth) / robustBlockerDepth
                 * m_LightAngularRadius
                 * kPenumbraScale;

penumbraUV = clamp(penumbraUV, kMinPenumbra * texelSize, kMaxPenumbra * texelSize);
```

Typical values:
- `kMinPenumbra = 1.0` — at least 1 texel; prevents degenerate 0-radius kernel at the hard shadow edge.
- `kMaxPenumbra = 64.0` — caps at 64 texels to avoid over-blurring.
- `kPenumbraScale = 1.0` — tunable multiplier.

### Stage 5 — Adaptive PCF with Blue-Noise Disc

```hlsl
float visibility = 0.0f;

[unroll]
for (int i = 0; i < PCF_SAMPLES; ++i)
{
    float2 xi     = SampleBlueNoise(pixelPos, i + BLOCKER_SAMPLES, frameIndex);
    float2 offset = xi * penumbraUV;
    visibility += g_CSMShadowMap.SampleCmpLevelZero(g_ShadowSampler,
                      float3(shadowUV.xy + offset, slice), receiverDepth);
}

return visibility / (float)PCF_SAMPLES;
```

The blue-noise texture index is offset by `BLOCKER_SAMPLES` so that the PCF samples are drawn from a different region of the texture than the blocker search samples, avoiding correlation between the two stages.

### Stage 6 — Cascade 3 Fallback

```hlsl
// In ComputeCSMShadow / ComputePCSSShadow dispatch:
if (cascadeIndex == 3u)
{
    // Cascade 3 covers kilometers; PCSS blocker search is statistically
    // ineffective at this scale. Fall back to fixed 3×3 PCF.
    return Compute3x3PCF(shadowMap, shadowSampler,
                         float3(shadowUV.xy, 3.0f),
                         compareDepth, texelSize);
}
```

This saves approximately 28 taps for all pixels in cascade 3, which covers the majority of the screen in large open-world scenes.

### Stage 7 — Temporal Shadow History

A dedicated `R8_UNORM` persistent texture (`g_RG_ShadowHistory`) stores the previous frame's resolved shadow mask. A second compute dispatch (`ShadowMaskTemporal_CSMain`) performs:

```hlsl
// 1. Reproject current pixel to previous frame using motion vectors
float2 motionVec = g_MotionVectors.Load(uint3(pixelPos, 0)).rg;
float2 prevUV    = (float2(pixelPos) + 0.5f) / outputSize + motionVec;

// 2. Sample history
float historyVal = g_ShadowHistory.SampleLevel(g_LinearClamp, prevUV, 0).r;

// 3. Neighbourhood min/max clamp (3×3 of current raw shadow mask)
float neighbourMin = 1.0f, neighbourMax = 0.0f;
[unroll]
for (int x = -1; x <= 1; ++x)
[unroll]
for (int y = -1; y <= 1; ++y)
{
    float s = g_RawShadowMask.Load(uint3(pixelPos + int2(x,y), 0)).r;
    neighbourMin = min(neighbourMin, s);
    neighbourMax = max(neighbourMax, s);
}
historyVal = clamp(historyVal, neighbourMin, neighbourMax);

// 4. Blend
float current = g_RawShadowMask.Load(uint3(pixelPos, 0)).r;
float blended = lerp(current, historyVal, kTemporalAlpha);  // α = 0.9

// 5. Write final mask and update history
g_RWShadowMask[pixelPos]   = blended;
g_RWShadowHistory[pixelPos] = blended;
```

**Why a dedicated history buffer instead of FSR3 TAA?**
- FSR3 TAA accumulates the full HDR colour buffer. Shadow noise is only one of many noise sources it must handle; its neighbourhood clamp is tuned for colour, not for a binary shadow signal.
- A dedicated shadow history allows a tighter neighbourhood clamp (shadow values are in `[0,1]`, not HDR), a higher blend factor (α = 0.9 vs. TAA's ~0.85), and disocclusion handling tuned specifically for shadow boundaries.
- The shadow history resolves in 3–5 frames for a static camera vs. 8–10 frames through FSR3 TAA.

**Disocclusion:** When `prevUV` is out of bounds or the depth difference between current and reprojected pixel exceeds a threshold, `α` is reduced to 0 (no history blend), forcing the current raw sample to be used directly. This prevents ghosting at newly revealed surfaces.

### Data Structures

**New fields in `ShadowMaskConstants` (`ShadowMask.sr`):**

```hlsl
cbuffer ShadowMaskConstants
{
    float4x4 m_ShadowViewProj[4];   // 256 bytes (existing)
    float4x4 m_ClipToWorld;         //  64 bytes (existing)
    float4x4 m_WorldToView;         //  64 bytes (existing)
    float4   m_CascadeSplits;       //  16 bytes (existing)
    float4   m_SearchRadii;         //  16 bytes (NEW) — per-cascade UV search radius
    float2   m_OutputSize;          //   8 bytes (existing)
    float    m_NormalBias;          //   4 bytes (existing)
    uint     m_EnableCascadeBlend;  //   4 bytes (existing)
    float    m_LightAngularRadius;  //   4 bytes (NEW) — half-angle in radians
    uint     m_FrameIndex;          //   4 bytes (NEW) — for blue-noise tile offset
    float2   m_Padding;             //   8 bytes (NEW) — align to 16 bytes
};
```

**New resources in `ShadowMaskInputs` (`ShadowMask.sr`):**

```hlsl
srinput ShadowMaskInputs
{
    ShadowMaskConstants m_CB;

    Texture2D<float>      Depth;              // t0 (existing)
    Texture2D<float2>     GBufferNormals;     // t1 (existing)
    Texture2DArray<float> CSMShadowMap;       // t2 (existing)
    Texture2D<float2>     BlueNoiseTex;      // t3 (NEW) — 64×64 R8G8_UNORM (external/LDR_RG01_0.png)
    Texture2D<float4>     MotionVectors;     // t4 (NEW) — RGBA16_FLOAT GBuffer motion
    Texture2D<float>      ShadowHistory;     // t5 (NEW) — previous frame R8_UNORM

    RWTexture2D<float>    RWShadowMask;      // u0 (existing)
    RWTexture2D<float>    RWShadowHistory;   // u1 (NEW) — written by temporal pass

    SamplerComparisonState ShadowSampler;    // s0 (existing)
    SamplerState           ShadowSamplerPoint; // s1 (NEW) — point, border=white, for raw depth fetch
};
```

### Resource Management

| Resource | Type | Owner | Lifetime |
|---|---|---|---|
| `g_RG_ShadowMask` | R8_UNORM UAV | `ShadowMaskRenderer` | Transient (per-frame) |
| `g_RG_ShadowHistory` | R8_UNORM SRV+UAV | `ShadowMaskRenderer` | **Persistent** (cross-frame) |
| `BlueNoiseTex` | R8G8_UNORM SRV | `CommonResources` | App lifetime (loaded from `external/LDR_RG01_0.png`) |
| `ShadowSamplerPoint` | Sampler | `CommonResources` | App lifetime |
| `ShadowMaskConstants` CB | Volatile CB | `ShadowMaskRenderer` | Per-frame |

### Execution Model

- **Dispatch 1** (`ShadowMask_CSMain`): `[numthreads(8,8,1)]`, full screen resolution. Writes raw stochastic shadow mask to `g_RWShadowMask`.
- **Dispatch 2** (`ShadowMaskTemporal_CSMain`): `[numthreads(8,8,1)]`, full screen resolution. Reads raw mask + history, writes final mask + updated history.
- Both dispatches share the same binding set (same `ShadowMaskInputs`). The temporal pass reads `g_RWShadowMask` as an SRV (via a separate `Texture2D` binding) and writes the final result back to the same UAV slot.

---

## 5. INTEGRATION

### Affected Systems

| System | Change |
|---|---|
| `CommonShadow.hlsli` | Add `ComputePCSSShadow()`, `SampleBlueNoise()`, wave-level blocker search, both robust estimators, cascade-3 fallback, shadow map min-reduction early-out |
| `ShadowMask.hlsl` | `#if PCSS` branch; add `ShadowMaskTemporal_CSMain` entry point |
| `ShadowMask.sr` | Add `m_SearchRadii`, `m_LightAngularRadius`, `m_FrameIndex` to CB; add BlueNoiseTex, MotionVectors, ShadowHistory, RWShadowHistory, ShadowSamplerPoint |
| `ShadowMaskRenderer.cpp` | Two dispatches; persistent history texture; fill new CB fields; bind new resources |
| `CommonResources.h/.cpp` | Add `ShadowSamplerPoint` sampler; add `BlueNoiseTex` texture loaded from `external/LDR_RG01_0.png` |
| `Renderer.h` | Add `bool m_EnablePCSS = false` |
| `shaders.cfg` | Already has all 4 permutations — no changes needed |

### Required Modifications

1. **`ShadowMask.sr`** — extend `ShadowMaskConstants`; extend `ShadowMaskInputs` with 3 new textures, 1 new UAV, 1 new sampler.
2. **`CommonShadow.hlsli`** — add `ComputePCSSShadow(...)` guarded by `#if PCSS`; add `SampleBlueNoise()` helper.
3. **`ShadowMask.hlsl`** — add `#if PCSS` dispatch branch; add `ShadowMaskTemporal_CSMain`.
4. **`ShadowMaskRenderer.cpp`** — declare `g_RG_ShadowHistory` as persistent; add `g_RG_GBufferMotionVectors` read; fill new CB fields; bind new resources; add second dispatch.
5. **`CommonResources.h`** — add `ShadowSamplerPoint` and `BlueNoiseTex` fields.
6. **`CommonResources.cpp`** — create `ShadowSamplerPoint`; load `BlueNoiseTex` from `external/LDR_RG01_0.png` via `LoadTexture`/`UploadTexture`.
7. **`Renderer.h`** — add `bool m_EnablePCSS = false`.
8. **ImGui** — add checkbox for `m_EnablePCSS` in the shadow settings panel.

### New Components

- `ShadowMaskTemporal_CSMain` — new compute entry point in `ShadowMask.hlsl`.
- `SampleBlueNoise()` — helper function in `CommonShadow.hlsli`.

### Dependencies

| Dependency | Source | Notes |
|---|---|---|
| Shadow map mip chain | `ShadowRenderer` (extended) | Create shadow map with `mipLevels > 1`; call `GenerateMipsUsingSPD(..., SPD_REDUCTION_MIN)` after depth pass |
| `g_RG_GBufferMotionVectors` | `OpaqueRenderer` | Already in render graph |
| `g_RG_DepthTexture` | `OpaqueRenderer` | Already read by `ShadowMaskRenderer` |
| `CommonResources::ShadowSamplerPoint` | `CommonResources::Initialize()` | New |
| `CommonResources::BlueNoiseTex` | `CommonResources::Initialize()` | Loaded from `external/LDR_RG01_0.png` |
| `Scene::Light::m_AngularSize` | Scene data | Already populated |

### Compatibility

- The `PCSS=0` path is identical to the current implementation; no regression risk.
- The `CASCADE_BLEND` permutation is orthogonal; both dimensions combine freely.
- `Normal` mode (ray-traced shadows) is unaffected.
- The temporal pass is only dispatched when `m_EnablePCSS = true`; the history texture is declared persistent but only written when PCSS is active.

---

## 6. PERFORMANCE

### Resource Usage

**GPU — Texture samples per pixel (PCSS path, cascades 0–2):**

| Stage | Taps | Sampler type | Notes |
|---|---|---|---|
| Shadow map min-reduction early-out | 1 | Min-reduction | Skips all remaining taps if fully lit |
| Blue-noise fetch (blocker) | 12 | Point (texture) | 1 texel per sample from 64×64 BN tex |
| Blocker search | 12 | Point (shadow map) | Raw depth, no comparison |
| Blue-noise fetch (PCF) | 16 | Point (texture) | |
| Adaptive PCF | 16 | Comparison | `SampleCmpLevelZero` |
| **Total (worst case)** | **57** | | Early-out hit eliminates 56 of these |
| **Total (early-out miss)** | **57** | | Full path |
| **Effective (typical scene)** | **~15** | | ~70% of pixels hit early-out |

**Cascade 3 (fixed PCF fallback):** 9 taps, same as current.

**Temporal pass:** 9 taps (3×3 neighbourhood clamp) + 1 history sample + 1 motion vector fetch = 11 taps per pixel, full screen.

**Wave-level reduction:** 4 lanes pool 12 blocker taps each → effective 48-sample estimate at 12 taps/pixel cost. No additional texture fetches.

**GPU — ALU:** Welford variance (~30 ops), nearest-biased weighting (~24 ops), penumbra formula (~10 ops), blue-noise indexing (~15 ops). Total ~80 ALU instructions per pixel on the PCSS path. Negligible vs. texture fetch latency.

**CPU:** Per-frame: 3 `sqrt` + 3 `tan` + 3 multiplies for search radius precomputation. Negligible.

**Memory:**
- `g_RG_ShadowHistory`: 1920×1080 × 1 byte = ~2 MB. Persistent.
- `BlueNoiseTex`: 64×64 × 2 bytes = 8 KB. App lifetime.
- No other new allocations.

**Bandwidth:** At 1080p, the temporal pass reads ~2 MB (history) + ~2 MB (raw mask) + ~8 MB (motion vectors) = ~12 MB/frame. The PCSS pass reads ~57 taps × 4 bytes × 2.07M pixels × (1 - early_out_rate) shadow map texels. With 70% early-out rate, effective shadow map bandwidth ≈ 57 × 4 × 0.62M ≈ 141 MB/frame worst case; in practice the shadow map is hot in L2 cache.

### Scalability

**Small scenes (Sponza, ~100m × 100m):**
- Cascade 0 covers ~5m; search radius ≈ 2–4 texels. Min-reduction early-out rate very high (>90%).
- Expected overhead vs. current: +0.2–0.4 ms at 1080p.

**Medium scenes (Bistro, ~200m × 200m):**
- Cascades 1–2 active for most geometry. Penumbra radii 4–16 texels. Min-reduction early-out rate ~70%.
- Expected overhead: +0.5–1.0 ms at 1080p.

**Large scenes (Caldera / open-world, ≥ 8 km × 8 km):**
- Cascade 3 (kilometers) uses fixed 3×3 PCF fallback — no PCSS cost.
- Cascades 0–2 cover near geometry; min-reduction early-out rate high for distant terrain.
- Expected overhead: +0.4–0.8 ms at 1080p (cascade 3 fallback saves ~30% vs. full PCSS).

---

## 7. QUALITY

### Improvements

- **Contact hardening:** Shadows sharpen near casters, soften with distance.
- **Physically motivated penumbra:** Width scales with sun angular size.
- **Robust thin-geometry handling:** Nearest-biased weighting + Welford rejection stabilise fences, railings, and foliage.
- **Blue-noise sampling:** Less structured noise than a static Poisson disc; resolves faster with temporal accumulation.
- **Dedicated temporal accumulation:** Shadow-specific neighbourhood clamp and blend factor; resolves in 3–5 frames vs. 8–10 through FSR3 TAA.
- **No cascade 3 degradation:** Large-scale cascade uses fixed PCF, avoiding the statistical failure mode of PCSS at kilometer scale.

### Regressions

- **Noise on disocclusion:** When a pixel is newly revealed (disocclusion), the history is invalid and the raw stochastic sample is used directly. This produces a single-frame noise spike at disocclusion boundaries.
- **Bias sensitivity in penumbra:** The blocker search uses raw depth fetches; the normal-offset bias must be applied consistently to `receiverDepth` in both the blocker comparison and the PCF comparison. Mismatch causes acne in the penumbra region.
- **Wave intrinsic requirement:** The wave-level blocker search requires SM 6.x and a wave size ≥ 4. On hardware with wave size < 4 (rare), the `WaveActive*` calls still compile but may not pool across a full 2×2 quad.

### Reliability

Main failure modes and mitigations:

| Failure | Mitigation |
|---|---|
| Blocker search returns 0 samples | Early-out to `1.0` (fully lit) |
| Penumbra radius = 0 | Clamped to `kMinPenumbra * texelSize` |
| Penumbra radius too large | Clamped to `kMaxPenumbra * texelSize` |
| All Welford samples rejected | Fallback to estimator A result |
| History out of bounds | `α = 0` (no blend); use raw current sample |
| Shadow map mip chain missing | Shadow map created without mips: skip early-out, run full blocker search |

---

## 8. DEBUGGING

### Visualization (HIGH PRIORITY)

The existing `CSMDebugRenderer` should be extended with PCSS-specific debug modes. Proposed additions to `CSMDebugMode`:

| Mode | Output | Purpose |
|---|---|---|
| `CSM_DEBUG_BLOCKER_DEPTH` | Grayscale: avg blocker depth (normalised) | Verify blocker search is finding occluders |
| `CSM_DEBUG_BLOCKER_COUNT` | Heatmap: 0 (blue) → 48 effective (red) | Identify regions where blocker search is sparse |
| `CSM_DEBUG_PENUMBRA_RADIUS` | Heatmap: 0 (blue) → kMaxPenumbra (red) | Verify penumbra scaling is correct |
| `CSM_DEBUG_SEARCH_RADIUS` | Heatmap: search radius in texels | Verify precomputed search radius is correct |
| `CSM_DEBUG_EARLYOUT` | Binary: green=early-out, red=full search | Measure min-reduction early-out effectiveness |
| `CSM_DEBUG_TEMPORAL_BLEND` | Grayscale: blend weight α | Identify disocclusion regions |
| `CSM_DEBUG_RAW_SHADOW` | Raw stochastic output before temporal | Inspect noise level before accumulation |

**Implementation:** Pass debug values out of `ComputePCSSShadow` via `out` parameters. Write to `g_RWShadowMask` in debug mode (shadow mask is not used for lighting when debug overlay is active).

### Logging

- Log `m_LightAngularRadius` (in degrees) and `m_SearchRadii[0..2]` (in texels) to the ImGui shadow panel.
- Log min-reduction early-out rate (percentage of pixels that skipped blocker search) via a GPU readback counter (optional, debug-only).

### Metrics

- GPU timer around `ShadowMaskRenderer` (already present via `PROFILE_GPU_SCOPED`).
- Separate timers for dispatch 1 (PCSS) and dispatch 2 (temporal) to isolate costs.
- Compare frame time with `m_EnablePCSS = false` vs. `true`.

### Profiling

- Use PIX or RenderDoc to capture a frame and inspect:
  - `g_RWShadowMask` after dispatch 1 (raw stochastic noise).
  - `g_RWShadowMask` after dispatch 2 (temporally resolved).
  - `g_RG_ShadowHistory` (should match previous frame's final mask).
- Check register pressure of the PCSS shader: `blockerDepths[12]` array = 48 bytes; Welford state = 12 bytes; total ~60 bytes of local state. Should not spill on SM 6.x.

### Troubleshooting

**Common failure cases:**

| Symptom | Likely cause | Fix |
|---|---|---|
| Shadow acne in penumbra | Bias not applied to blocker comparison depth | Apply `normalBias * worldTexelSize` to `receiverDepth` in blocker search |
| Penumbra too wide everywhere | `m_SearchRadii` too large or `kPenumbraScale` too high | Check CPU search radius formula; reduce `kPenumbraScale` |
| No soft shadows (hard edge) | `m_LightAngularRadius = 0` | Verify `m_AngularSize` is non-zero; check CB upload |
| Ghosting on moving shadows | Temporal α too high or neighbourhood clamp too loose | Reduce α to 0.85; tighten clamp range |
| Flickering noise | History not initialised on first frame | Clear `g_RG_ShadowHistory` to 1.0 on `m_ShadowHistoryIsNew` |
| Black shadows at cascade edges | Border sampler returning 0 | Verify `ShadowComparison` and `ShadowSamplerPoint` use `clamp-to-border-white` |
| Early-out never fires | Shadow map has no mip chain, or wrong mip level | Verify shadow map is created with `mipLevels > 1`; verify `MinReductionClamp` sampler is bound |
| Wave reduction incorrect | Wave size < 4 on target hardware | Add `[WaveSize(4)]` attribute or fallback path |
| Welford rejects all samples | `stddev` near zero with outlier present | Ensure `+ 1e-5f` epsilon in rejection threshold |

**Debug workflow:**
1. Enable `CSM_DEBUG_EARLYOUT` — verify min-reduction early-out is firing for lit regions.
2. Enable `CSM_DEBUG_BLOCKER_DEPTH` — verify occluders are detected in shadow regions.
3. Enable `CSM_DEBUG_PENUMBRA_RADIUS` — verify penumbra scales with distance from caster.
4. Enable `CSM_DEBUG_RAW_SHADOW` — inspect stochastic noise before temporal resolve.
5. Disable PCSS (`m_EnablePCSS = false`) — verify 3×3 PCF baseline is unchanged.
6. Re-enable PCSS — verify temporal resolve converges within 5 frames (static camera).

---

## 9. PROS & CONS

### Advantages

- **Physically motivated:** Penumbra width derived from actual light angular size and receiver–caster geometry.
- **Contact hardening:** Shadows sharp at contact, soft at distance.
- **Shadow map min-reduction early-out (via SPD mip chain):** Eliminates ~70% of blocker search cost in typical scenes.
- **Wave-level reduction:** Up to 384× effective blocker sample count (32 lanes × 12 taps) at no additional texture fetch cost.
- **Dual robust estimation:** Handles thin geometry, multi-layer scenes, and outlier blockers.
- **Blue-noise sampling:** Faster temporal convergence than static Poisson disc.
- **Dedicated temporal history:** Shadow-specific accumulation; resolves in 3–5 frames.
- **Cascade 3 fallback:** No quality/performance degradation at large scales.
- **Incremental:** `PCSS=0` path is unchanged; clean opt-in.

### Disadvantages

- **Two compute dispatches:** The temporal pass adds a second dispatch and a persistent texture.
- **SM 6.x wave intrinsics required:** Not available on very old hardware (pre-DX12 level).
- **Blue-noise texture is an external asset:** `external/LDR_RG01_0.png` must be present at the expected path relative to the exe.
- **History ghosting on disocclusion:** Single-frame noise spike at newly revealed surfaces.
- **Bias sensitivity:** Both blocker search and PCF must use the same bias; mismatch causes acne.
- **Shadow map mip chain dependency:** The min-reduction early-out requires the shadow map to have a full mip chain generated with min-reduction downsampling. This is an additional requirement on `ShadowRenderer` that does not currently exist.

### Trade-offs

| Trade-off | Decision |
|---|---|
| Sample count vs. quality | 12 blocker + 16 PCF = 28 taps; wave reduction gives effective 48-sample blocker estimate |
| Temporal stability vs. responsiveness | α = 0.9; disocclusion drops to α = 0 for immediate response |
| Accuracy vs. robustness | Both estimators applied in sequence; Welford fallback to estimator A if all samples rejected |
| PCSS on all cascades vs. near only | Cascades 0–2 use PCSS; cascade 3 uses fixed PCF |
| Dedicated history vs. FSR3 TAA | Dedicated history: faster convergence, shadow-tuned clamp, no coupling to colour TAA |

---

## 10. RISKS

### Technical Risks

| Risk | Severity | Probability | Mitigation |
|---|---|---|---|
| Shadow acne in penumbra | High | Medium | Consistent bias in blocker search and PCF |
| Temporal ghosting on fast shadow movement | Medium | Medium | Neighbourhood clamp + disocclusion α=0 |
| Wave intrinsics unavailable | Medium | Low | Fallback: remove `WaveActive*` calls, use per-pixel blocker sum |
| Shadow map mip chain not generated | Medium | Medium | Extend `ShadowRenderer`: create with `mipLevels > 1`, call `GenerateMipsUsingSPD(..., SPD_REDUCTION_MIN)` |
| Blue-noise texture missing | Low | Low | Assert on load failure in `CommonResources::Initialize()`; same pattern as BRDF LUT |
| History texture not cleared on first frame | Medium | High | Check `m_ShadowHistoryIsNew` flag; clear to 1.0 on first use |
| Welford instability with < 2 samples | Low | Medium | Guard `wCount > 1.0f` before computing variance |

### Maintenance Risks

- **CB layout changes:** Adding fields to `ShadowMaskConstants` requires regenerating `.sr`-derived headers. Handled by the existing build system.
- **Blue-noise texture path:** `external/LDR_RG01_0.png` must be present. Loaded via `LoadTexture` — same failure mode as missing BRDF LUT or atmosphere textures.
- **Two-dispatch coupling:** The temporal pass assumes the raw shadow mask is in `g_RWShadowMask` after dispatch 1. If the resource layout changes, both dispatches must be updated together.
- **History texture format:** R8_UNORM gives 1/255 ≈ 0.004 precision. For very subtle penumbra gradients this may quantise. If needed, upgrade to R16_UNORM (doubles memory to ~4 MB).

### Mitigation Strategies

1. **Acne:** Apply `normalBias * worldTexelSize` to `receiverDepth` before the blocker comparison, not just to the world position.
2. **Ghosting:** Neighbourhood clamp with a small epsilon (`±0.05`) around the current frame's min/max. Reduce α to 0.85 if ghosting is visible.
3. **Wave intrinsics:** Wrap `WaveActive*` calls in `#if WAVE_INTRINSICS` define; add a scalar fallback path.
4. **First-frame history:** In `ShadowMaskRenderer::Render()`, if `m_ShadowHistoryIsNew`, clear `g_RG_ShadowHistory` to 1.0 before dispatch 2.

### Alternative Solutions

If the full pipeline proves too expensive or complex:

1. **Drop temporal history, rely on FSR3 TAA:** Simpler (no second dispatch, no persistent texture), but slower convergence and less control over shadow-specific artefacts.
2. **Drop wave reduction:** Reduces effective sample count from 48 to 12 but simplifies the shader significantly.
3. **Drop Welford, keep only nearest-biased:** Covers 95% of cases; Welford only helps with pathological outlier blockers.
4. **VSM / MSMSM:** Prefilter shadow map into variance/moment representation. 1-tap lookup, no noise, no temporal pass — but leaks on thin geometry and requires a different shadow map format.

---

## 11. FUTURE IMPROVEMENTS

### Possible Enhancements

1. **Checkerboard PCSS:** Run PCSS at half resolution (checkerboard pattern), reconstruct with a bilateral upsample. Halves the sample count; temporal history resolves the checkerboard pattern.

2. **DPCF hybrid:** Use PCSS for the penumbra radius estimation, but apply a separable bilateral filter (depth + normal weighted) instead of a stochastic PCF kernel. Eliminates noise entirely at the cost of a second pass.

3. **Per-material shadow softness:** Allow materials to override `kPenumbraScale` (e.g., translucent leaves could use a larger penumbra to simulate sub-surface scattering of shadow).

4. **Light radius from atmosphere:** The sun angular radius currently comes from `m_AngularSize`. A future improvement would derive it from the atmosphere renderer's sun disc size for consistency.

5. **NRD SIGMA integration:** The penumbra radius estimate from PCSS could seed NVIDIA's SIGMA shadow denoiser for ray-traced shadows, improving denoiser quality in penumbra regions.

6. **History texture upgrade to R16_UNORM:** Doubles memory (2 MB → 4 MB) but eliminates quantisation artefacts in subtle penumbra gradients.

### Optimization Opportunities

1. **Async compute for temporal pass:** Dispatch 2 (temporal resolve) has no dependency on the rasterisation pipeline and could run on the async compute queue, overlapping with the next frame's geometry passes.

2. **Async shadow map mip generation:** The SPD pass that generates the shadow map min-reduction mip chain could be moved to the async compute queue, overlapping with the geometry passes of the next frame.

3. **Temporal α from motion magnitude:** Reduce α proportionally to the magnitude of the motion vector. Fast-moving shadows get less history blend (less ghosting); static shadows get full blend (maximum quality).

### Future Extensions

1. **Point/spot light PCSS:** The same algorithm applies to perspective shadow maps. The search radius formula changes (perspective depth is non-linear), but the structure is identical.

2. **Machine-learning shadow filtering:** Replace the stochastic PCF with a neural network denoiser (e.g., NVIDIA DLSS Ray Reconstruction applied to shadow maps). Long-term research direction.

---

## Appendix: Implementation Checklist

```
Phase 1 — Resources & CB
  [ ] Add m_SearchRadii, m_LightAngularRadius, m_FrameIndex, m_Padding to ShadowMaskConstants (ShadowMask.sr)
  [ ] Add BlueNoiseTex, MotionVectors, ShadowHistory to ShadowMaskInputs (ShadowMask.sr)
  [ ] Add RWShadowHistory UAV to ShadowMaskInputs (ShadowMask.sr)
  [ ] Add ShadowSamplerPoint to ShadowMaskInputs (ShadowMask.sr)
  [ ] Add ShadowSamplerPoint to CommonResources.h + CommonResources.cpp
  [ ] Add BlueNoiseTex to CommonResources.h + CommonResources.cpp (load from pre-baked binary asset)

Phase 2 — Shader (PCSS=0 path unchanged)
  [ ] Add SampleBlueNoise() helper to CommonShadow.hlsli
  [ ] Add ComputePCSSShadow() to CommonShadow.hlsli (#if PCSS guard)
      [ ] Shadow map min-reduction early-out
      [ ] Wave-level blocker search (WaveActiveSum, WaveActiveMin)
      [ ] Nearest-biased weighted average (estimator A)
      [ ] Welford outlier rejection (estimator B)
      [ ] Penumbra estimation
      [ ] Adaptive PCF with blue-noise disc
      [ ] Cascade 3 fallback to Compute3x3PCF
  [ ] Add ShadowMaskTemporal_CSMain to ShadowMask.hlsl
      [ ] Motion vector reprojection
      [ ] 3×3 neighbourhood min/max clamp
      [ ] Temporal blend (α=0.9, disocclusion α=0)
      [ ] Write RWShadowMask + RWShadowHistory
  [ ] Add #if PCSS branch in ShadowMask_CSMain (ShadowMask.hlsl)
  [ ] Verify PCSS=0 path compiles and produces identical output to current

Phase 3 — CPU integration
  [ ] Add bool m_EnablePCSS = false to Renderer.h
  [ ] Declare g_RG_ShadowHistory as persistent in ShadowMaskRenderer::Setup()
  [ ] Read g_RG_GBufferMotionVectors in ShadowMaskRenderer::Setup()
  [ ] Fill m_SearchRadii (precomputed per-cascade) in ShadowMaskRenderer::Render()
  [ ] Fill m_LightAngularRadius from Scene::Light::m_AngularSize
  [ ] Fill m_FrameIndex from g_Renderer.m_FrameNumber
  [ ] Bind BlueNoiseTex (external/LDR_RG01_0.png), MotionVectors, ShadowHistory, ShadowSamplerPoint, MinReductionClamp sampler
  [ ] Add second dispatch (ShadowMaskTemporal_CSMain)
  [ ] Clear g_RG_ShadowHistory to 1.0 on m_ShadowHistoryIsNew
  [ ] Select correct ShaderID based on m_EnablePCSS × m_EnableCascadeBlend
  [ ] Add ImGui checkbox for m_EnablePCSS

Phase 4 — Debug visualization
  [ ] Add CSM_DEBUG_EARLYOUT, CSM_DEBUG_BLOCKER_DEPTH, CSM_DEBUG_PENUMBRA_RADIUS,
      CSM_DEBUG_RAW_SHADOW, CSM_DEBUG_TEMPORAL_BLEND modes to CSMDebugMode
  [ ] Extend CSMDebugRenderer to handle new modes

Phase 5 — Tuning & validation
  [ ] Test in Sponza: verify contact hardening at column bases
  [ ] Test in Bistro: verify penumbra width scales with tree height
  [ ] Verify cascade 3 uses fixed PCF (no PCSS artefacts at large scale)
  [ ] Profile: measure GPU time delta for dispatch 1 and dispatch 2 separately
  [ ] Verify min-reduction early-out rate > 60% in typical scene (debug mode)
  [ ] Verify temporal history converges within 5 frames (static camera)
  [ ] Verify no acne regression vs. current 3×3 PCF
  [ ] Verify no ghosting on fast camera pan (disocclusion α=0 fires correctly)
```
