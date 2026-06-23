# RTXDI Fast Preset Migration Plan

> **Goal:** Migrate from "Medium-like" defaults to RTXDI FullSample's **"Fast"** quality preset for maximum performance.
> **Principle:** Reuse existing `g_ReSTIRDI_*` globals — no new structs, no new UI plumbing.

---

## Reference: FullSample `QualityPreset::Fast`

Source: [`REFERENCES/RTXDI/Samples/FullSample/Source/UserInterface.cpp:131-152`](../REFERENCES/RTXDI/Samples/FullSample/Source/UserInterface.cpp)

```cpp
case QualityPreset::Fast:
    enableCheckerboardSampling = true;
    restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
    restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
    restirDI.neeLocalLightSampling.numLocalLightUniformSamples = 4;
    restirDI.neeLocalLightSampling.numLocalLightPowerRISSamples = 4;
    restirDI.neeLocalLightSampling.numLocalLightReGIRRISSamples = 4;
    restirDI.initialSamplingParams.numLocalLightSamples = restirDI.neeLocalLightSampling.numLocalLightPowerRISSamples;
    restirDI.initialSamplingParams.numBrdfSamples = 0;
    restirDI.initialSamplingParams.numInfiniteLightSamples = 1;
    restirDI.temporalResamplingParams.enableVisibilityShortcut = true;
    restirDI.boilingFilter.enableBoilingFilter = true;
    restirDI.boilingFilter.boilingFilterStrength = 0.2f;
    restirDI.temporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Off;
    restirDI.spatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Off;
    restirDI.spatialResamplingParams.numSamples = 1;
    restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 2;
    restirDI.shadingParams.reuseFinalVisibility = true;
    lightingSettings.brdfptParams.enableSecondaryResampling = false;
    lightingSettings.enableGradients = false;
```

---

## Changes Required

All changes are in [`src/RTXDIRenderer.cpp`](../src/RTXDIRenderer.cpp).

### Change 1 — Enable Checkerboard Sampling (Static Parameter)

**File:** `src/RTXDIRenderer.cpp` — function `CreateRTXDIContext()`  
**Location:** Line ~545 (inside the `ReSTIRDIStaticParameters` block)  
**What:** Set `CheckerboardSamplingMode = Black` on the DI static params.  
**Why:** Checkerboard sampling halves the number of pixels processed per frame (alternating black/white pixels). This is the single biggest performance win of the "Fast" preset — reduces all screen-space pass costs by ~50%.  
**⚠️ Static parameter:** Changing this forces a context recreation, which `CreateRTXDIContext()` already handles (it's called from `Initialize()` and can be called again at runtime).

**Current code (~line 541-545):**
```cpp
rtxdi::ReSTIRDIStaticParameters staticParams;
staticParams.RenderWidth = width;
staticParams.RenderHeight = height;
m_Context = std::make_unique<rtxdi::ReSTIRDIContext>(staticParams);
```

**New code:**
```cpp
rtxdi::ReSTIRDIStaticParameters staticParams;
staticParams.RenderWidth = width;
staticParams.RenderHeight = height;
staticParams.CheckerboardSamplingMode = rtxdi::CheckerboardMode::Black;
m_Context = std::make_unique<rtxdi::ReSTIRDIContext>(staticParams);
```

**Also apply to GI static params (~line 560-563)** for consistency:
```cpp
rtxdi::ReSTIRGIStaticParameters giStaticParams;
giStaticParams.RenderWidth  = width;
giStaticParams.RenderHeight = height;
giStaticParams.CheckerboardSamplingMode = rtxdi::CheckerboardMode::Black;
m_ReSTIRGIContext = std::make_unique<rtxdi::ReSTIRGIContext>(giStaticParams);
```

---

### Change 2 — Switch Local Light Sampling to Power_RIS

**File:** `src/RTXDIRenderer.cpp` — function `Initialize()`  
**Location:** Line ~577  
**What:** Change `localLightSamplingMode` from `ReGIR_RIS` to `Power_RIS`.  
**Why:** Power_RIS avoids the expensive ReGIR grid build/presample passes. It directly importance-samples lights using a power-weighted PDF, which is cheaper than the full ReGIR pipeline. The "Fast" preset favors this because grid management adds significant overhead.

**Current:**
```cpp
g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
```

**New:**
```cpp
g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
```

**Knock-on effect:** The ternary on lines 582-584 auto-derives `numLocalLightSamples` from the selected mode, so `numLocalLightSamples` will automatically become `g_ReSTIRDI_NumLocalLightPowerRISSamples` (= 4 after change 3 below).

---

### Change 3 — Reduce NEE Sample Counts to 4

**File:** `src/RTXDIRenderer.cpp` — function `Initialize()`  
**Location:** Lines ~579-581  
**What:** Reduce all three NEE sample counts from `8` to `4`.  
**Why:** Fewer local light samples = fewer visibility rays traced during initial sampling. Combined with checkerboard, this is 4× fewer rays than current.

**Current:**
```cpp
g_ReSTIRDI_NumLocalLightUniformSamples = 8;
g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
g_ReSTIRDI_NumLocalLightReGIRRISSamples = 8;
```

**New:**
```cpp
g_ReSTIRDI_NumLocalLightUniformSamples = 4;
g_ReSTIRDI_NumLocalLightPowerRISSamples = 4;
g_ReSTIRDI_NumLocalLightReGIRRISSamples = 4;
```

**Also update the ImGui sliders** (lines ~136, 139, 142) to reflect the new ranges if desired (optional — the sliders already support 0-32, so 4 is within range).

---

### Change 4 — Disable BRDF Samples During Initial Sampling

**File:** `src/RTXDIRenderer.cpp` — function `Initialize()`  
**Location:** Line ~585  
**What:** Change `numBrdfSamples` from `1` to `0`.  
**Why:** The "Fast" preset traces **zero BRDF rays** during initial sampling (pure NEE only). This removes one ray-traced bounce per pixel. The quality loss is partially recovered through temporal/spatial resampling.

**Current:**
```cpp
g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
```

**New:**
```cpp
g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 0;
```

---

### Change 5 — Disable Bias Correction (Temporal + Spatial)

**File:** `src/RTXDIRenderer.cpp` — function `Initialize()`  
**Location:** Lines ~590-591  
**What:** Change both temporal and spatial bias correction from `Raytraced` to `Off`.  
**Why:** Bias correction traces extra shadow rays (temporal: ~1/pixel, spatial: ~1/sample). The "Fast" preset disables it entirely. This is a significant perf win — removing these rays reduces RT overhead substantially.

**Current:**
```cpp
g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
```

**New:**
```cpp
g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Off;
g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Off;
```

---

### Change 6 — Reduce Disocclusion Boost Samples

**File:** `src/RTXDIRenderer.cpp` — function `Initialize()`  
**Location:** Line ~593  
**What:** Change `numDisocclusionBoostSamples` from `8` to `2`.  
**Why:** Disocclusion boost kicks in when there's not enough temporal history (newly visible areas). The "Fast" preset uses only 2 neighbor samples instead of 8 — fewer rays, faster spatial resampling in disocclusions.

**Current:**
```cpp
g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 8;
```

**New:**
```cpp
g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 2;
```

---

### Change 7 — Update ImGui Slider Ranges (Optional)

The ImGui slider for `Power RIS Samples` currently starts at 0. Since the new default is 4, the slider should be functional. No code change required unless you want to clamp the minimum — the existing `SliderInt` calls accept any value 0-32.

---

## Parameters That Do NOT Change

These are already aligned with the "Fast" preset and should be left as-is:

| Parameter | Current Value | Fast Preset | Status |
|---|---|---|---|
| `g_ReSTIRDI_ResamplingMode` | `TemporalAndSpatial` | `TemporalAndSpatial` | ✅ |
| `numInfiniteLightSamples` | `1` | `1` | ✅ |
| `enableVisibilityShortcut` | `1u` | `true` | ✅ |
| `enableBoilingFilter` | `1u` | `true` | ✅ |
| `boilingFilterStrength` | `0.2f` | `0.2f` | ✅ |
| `numSamples` (spatial) | `1` | `1` | ✅ |
| `reuseFinalVisibility` | `1u` | `true` | ✅ |
| `enablePermutationSampling` | `0u` (disabled) | `true` (SDK default) | ⚠️ kept disabled — user observed better quality |

---

## Parameters Not Applicable (Infrastructure Missing)

These "Fast" preset settings cannot be applied because the corresponding rendering pipelines don't exist in HobbyRenderer. They are noted here for completeness.

| Parameter | Fast Preset Value | Why N/A |
|---|---|---|
| `lightingSettings.brdfptParams.enableSecondaryResampling` | `false` | No BRDF path tracer integration (no secondary surface ReSTIR DI pass) |
| `lightingSettings.enableGradients` | `false` | No gradient computation/filtering pipeline (no `FilterGradientsPass` or `ConfidencePass`) |

---

## Summary Table: All Changes

| # | What | Where | Old | New | Perf Impact |
|---|---|---|---|---|---|
| 1 | Checkerboard sampling | `CreateRTXDIContext()` | `Off` | `Black` | 🔴 ~50% fewer pixels |
| 2 | Light sampling mode | `Initialize()` L577 | `ReGIR_RIS` | `Power_RIS` | 🔴 skips ReGIR grid build |
| 3 | NEE sample counts ×3 | `Initialize()` L579-581 | `8` | `4` | 🟡 2× fewer visibility rays |
| 4 | BRDF samples | `Initialize()` L585 | `1` | `0` | 🟡 removes 1 RT bounce |
| 5 | Temporal bias correction | `Initialize()` L590 | `Raytraced` | `Off` | 🟡 removes ~1 shadow ray/px |
| 6 | Spatial bias correction | `Initialize()` L591 | `Raytraced` | `Off` | 🟡 removes ~N shadow rays/px |
| 7 | Disocclusion boost | `Initialize()` L593 | `8` | `2` | 🟢 minor perf improvement |

---

## Implementation Order

1. **Change 1 first** — checkerboard affects all subsequent passes and requires context recreation. Test that rendering still works correctly with checkerboard (image should still look correct, just at half-resolution with alternating pixels).

2. **Changes 2-6 together** — they are all in `Initialize()` and are simple value swaps. Apply them as one batch and rebuild.

3. **Build & test** — verify:
   - Frame time decreases significantly (target: >40% reduction)
   - Image quality is acceptable (some noise increase is expected)
   - No visual artifacts from checkerboard (flickering, black pixels, etc.)
   - Denoiser (RELAX or REBLUR) still converges properly

---

## Rollback

If the "Fast" preset is too aggressive, the easiest rollback is to revert `Initialize()` to the "Medium-like" values:

```cpp
g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
g_ReSTIRDI_NumLocalLightUniformSamples = 8;
g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
g_ReSTIRDI_NumLocalLightReGIRRISSamples = 8;
g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 8;
```

And remove `CheckerboardSamplingMode = Black` from `CreateRTXDIContext()`.
