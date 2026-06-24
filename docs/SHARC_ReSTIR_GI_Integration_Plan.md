# SHARC + ReSTIR GI Integration Plan

## 1. Overview

Combine the **SHARC** (Spatial Hash Radiance Cache) world-space radiance caching with
**ReSTIR GI** (Reservoir-based Spatiotemporal Importance Resampling for Global
Illumination) to obtain the strengths of both:

| Feature | ReSTIR GI alone | SHARC alone | Combined |
|---------|:--------------:|:-----------:|:--------:|
| Bounce depth | 1 indirect bounce | 2–4 bounces (back-propagation) | 2–4 bounces |
| Per-sample cost | High (light sampling + shadow rays) | Low (hash-table read) | Low (cache hit) / High (cache miss fallback) |
| Temporal stability | Reservoir temporal resampling | 60-frame EMA temporal accumulation | Both |
| Spatial reuse | Spatial resampling across neighbors | None (random-walk dithering) | Both |
| Camera-cut resilience | Lost reservoirs | Cache persists | Cache persists |
| Noise reduction | NRD denoiser | No denoiser | NRD denoiser |

The combination lets SHARC provide cheap multi-bounce radiance at secondary hit
positions, while ReSTIR GI's reservoir resampling propagates good samples
spatially/temporally and NRD provides final denoising.

---

## 2. Architecture: Current vs Proposed

### 2.1 Current Architecture (mutually exclusive)

```
IndirectLightingMode = 1 (ReSTIR GI):
  1. BrdfRayTracing        — 1 BRDF ray/px → secondary GBuffer
  2. ShadeSecondarySurfaces — direct lighting + light sampling → seed GI reservoirs
  3. GI Temporal Resampling — reuse reservoirs across frames
  4. GI Spatial Resampling  — reuse across neighbors
  5. GI FinalShading        — visibility ray, MIS blend, BRDF eval → write to DI output

IndirectLightingMode = 2 (SHARC):
  1. SHARC Update   — sparse (4%) multi-bounce path trace → accumulation cache
  2. SHARC Resolve  — temporal EMA blend + stale eviction
  3. SHARC Query    — 1 random bounce + cache lookup → g_RG_SHARCIndirect
  * DeferredLighting reads g_RG_SHARCIndirect, adds directly to output
```

### 2.2 Proposed Architecture (combined mode)

```
IndirectLightingMode = 3 (ReSTIR GI + SHARC):
  1. SHARC Update   — sparse (4%) multi-bounce path trace → accumulation cache
  2. SHARC Resolve  — temporal EMA blend + stale eviction
  3. BrdfRayTracing — 1 BRDF ray/px → secondary GBuffer  (unchanged)
  4. ShadeSecondarySurfaces ─┐
     ├─ Query SHARC cache     ← CHEAP (replaces direct lighting eval)
     ├─ On MISS: fallback to   ← original ReSTIR GI direct lighting
     │  RTXDI_SampleLightsForSurface()
     └─ Seed GI reservoirs    ← (unchanged)
  5. GI Temporal Resampling   ← (unchanged)
  6. GI Spatial Resampling    ← (unchanged)
  7. GI FinalShading          ← (unchanged)
  8. NRD denoising            ← (unchanged)

  * DeferredLighting: no change — ReSTIR GI FinalShading already additively
    blends into the DI output texture that DeferredLighting reads.
```

**Key insight**: The combined mode is a *drop-in replacement* for the
`ShadeSecondarySurfaces` radiance evaluation. The rest of the ReSTIR GI pipeline
(temporal, spatial, final shading, NRD) remains completely unchanged. This means
no changes to `BrdfRayTracing.hlsl`, `GI/TemporalResampling.hlsl`,
`GI/SpatialResampling.hlsl`, `GI/FinalShading.hlsl`, or the NRD integration.

---

## 3. New `IndirectLightingMode` Value

| Constant | Value | Pipeline |
|----------|-------|----------|
| `INDIRECT_LIGHTING_MODE_NONE` | 0 | No indirect |
| `INDIRECT_LIGHTING_MODE_RESTIR_GI` | 1 | ReSTIR GI only |
| `INDIRECT_LIGHTING_MODE_SHARC` | 2 | SHARC only |
| `INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC` | 3 | **Combined** (new) |

This allows runtime A/B comparison between all four modes via the ImGui radio
buttons in [`ImGuiLayer.cpp`](../../src/ImGuiLayer.cpp).

---

## 4. File-by-File Implementation Checklist

### 4.1 `src/shaders/Common.sr` — Add new mode constant

```hlsl
srinput IndirectLightingMode
{
    static const uint INDIRECT_LIGHTING_MODE_NONE             = 0;
    static const uint INDIRECT_LIGHTING_MODE_RESTIR_GI        = 1;
    static const uint INDIRECT_LIGHTING_MODE_SHARC            = 2;
    static const uint INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC  = 3;  // NEW
};
```

### 4.2 `src/shaders/SHARC.sr` — Add srinput for SHARC cache access from ReSTIR GI passes

Add a new `srinput` (or reuse `SHARCQueryInputs`) to make the SHARC resolved
buffer and hash entries buffer available in the ReSTIR GI rendering path:

```hlsl
// New: SHARC cache binding for ReSTIR GI integration
// This provides read-only access to the resolved cache from
// ShadeSecondarySurfaces and FinalShading passes.
srinput SHARCGICacheInputs
{
    // Read-only SHARC cache
    RWStructuredBuffer<uint64_t>          m_HashEntries;   // RW for SDK compatibility
    RWStructuredBuffer<SharcPackedData>   m_Resolved;      // read-only in practice
    RWStructuredBuffer<SharcAccumulationData> m_Accumulation; // read-only in practice

    SHARCConstants m_Const;
};
```

**Alternative (simpler)**: Instead of a new srinput, add the SHARC cache buffers
directly to the existing `RTXDI` binding set in the RTXDIConstants CB. The
resolved buffer is only ~64 MB and can be bound as a UAV read-only or SRV.

### 4.3 `src/shaders/ShaderIDs.h` — Add new shader variant

No new shader needed. The `ShadeSecondarySurfaces` already has two variants
(ReGIR enabled/disabled). The SHARC integration adds a conditional code path
within the same shader — no new compilation variant required. The SHARC cache
binding will be present in the binding set when the combined mode is active.

### 4.4 `src/RTXDIRenderer.cpp` — Host-side changes

#### 4.4.1 SHARC Renderer interop

When `m_IndirectLightingTechnique == RESTIR_GI_SHARC`:

1. Run SHARC Update + Resolve passes **before** the ReSTIR GI passes.
2. The SHARC cache buffers (`g_RG_SHARCHashEntries`, `g_RG_SHARCResolved`,
   `g_RG_SHARCAccumulation`) must be declared in the render graph and resolved
   with `Read` access so they can be bound as SRV in the ReSTIR GI passes.
3. The SHARC Query pass is **not** run in combined mode (the query happens
   inside `ShadeSecondarySurfaces` instead).

```cpp
// Pseudo-code for combined mode dispatch order in RTXDIRenderer::Render():

bool bCombined = (g_Renderer.m_IndirectLightingTechnique ==
                  srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC);

// Run SHARC Update + Resolve before ReSTIR GI
if (bCombined) {
    // 1. SHARC Update
    // 2. SHARC Resolve
    // Bind resolved cache as UAV→SRV (read-only) for subsequent passes
}

// Existing ReSTIR GI passes (ShadeSecondarySurfaces modified internally)
// ...

// NRD denoising (unchanged)
```

#### 4.4.2 Binding set for ShadeSecondarySurfaces in combined mode

Extend the existing `bset` (binding set for ReSTIR GI) to include the SHARC
cache buffers when in combined mode. The SHARC constants CB must also be bound.

```
Existing bset for ShadeSecondarySurfaces:
  - RTXDI constant buffer
  - TLAS
  - Lights
  - Instance/Mesh/Material/Vertex/Index data
  - Secondary GBuffer (RW)
  - GI Reservoir buffer (RW)
  - DI output textures (RW)
  - Motion vectors
  - GBuffer textures

Additional for combined mode:
  - g_RG_SHARCHashEntries   (SRV)
  - g_RG_SHARCResolved      (SRV)
  - g_RG_SHARCAccumulation  (SRV)
  - SHARC constants CB      (or embed in RTXDI CB)
```

#### 4.4.3 ReSTIR GI Quality Mode Presets

Mirror the existing ReSTIR DI quality mode (`HighPerformance` / `HighQuality`) with
an equivalent GI quality mode that toggles between a fast low-noise preset and a
slow high-quality preset. These presets are modeled after the RTXDI full sample
application at [NVIDIA-RTX/RTXDI](https://github.com/NVIDIA-RTX/RTXDI).

**New enum, globals, and ImGui control:**

```cpp
// Quality mode presets for ReSTIR GI
enum class ReSTIRGI_QualityMode { HighPerformance, HighQuality };

static ReSTIRGI_QualityMode g_ReSTIRGI_QualityMode = ReSTIRGI_QualityMode::HighPerformance;

static void ApplyHighPerfGIPreset()
{
    // --- Resampling mode ---
    g_ReSTIRGI_ResamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;

    // --- Temporal resampling ---
    g_ReSTIRGI_TemporalParams = rtxdi::GetDefaultReSTIRGITemporalResamplingParams();
    g_ReSTIRGI_TemporalParams.maxHistoryLength       = 4;
    g_ReSTIRGI_TemporalParams.maxReservoirAge        = 15;
    g_ReSTIRGI_TemporalParams.enableFallbackSampling  = 1u;
    g_ReSTIRGI_TemporalParams.enablePermutationSampling = 0u;
    g_ReSTIRGI_TemporalParams.biasCorrectionMode     = RTXDI_GIBiasCorrectionMode::Off;
    g_ReSTIRGI_TemporalParams.depthThreshold         = 0.1f;
    g_ReSTIRGI_TemporalParams.normalThreshold        = 0.6f;

    // --- Boiling filter (suppress noise cheaply) ---
    g_ReSTIRGI_BoilingParams.enableBoilingFilter    = 1u;
    g_ReSTIRGI_BoilingParams.boilingFilterStrength  = 0.2f;

    // --- Spatial resampling ---
    g_ReSTIRGI_SpatialParams = rtxdi::GetDefaultReSTIRGISpatialResamplingParams();
    g_ReSTIRGI_SpatialParams.numSamples             = 1;
    g_ReSTIRGI_SpatialParams.samplingRadius         = 16.0f;
    g_ReSTIRGI_SpatialParams.biasCorrectionMode     = RTXDI_GIBiasCorrectionMode::Off;
    g_ReSTIRGI_SpatialParams.depthThreshold         = 0.1f;
    g_ReSTIRGI_SpatialParams.normalThreshold        = 0.6f;

    // --- Final shading ---
    g_ReSTIRGI_FinalShadingParams = rtxdi::GetDefaultReSTIRGIFinalShadingParams();
    g_ReSTIRGI_FinalShadingParams.enableFinalMIS       = 0u;  // cheaper: skip MIS
    g_ReSTIRGI_FinalShadingParams.enableFinalVisibility = 1u;
}

static void ApplyHighQualityGIPreset()
{
    // --- Resampling mode ---
    g_ReSTIRGI_ResamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;

    // --- Temporal resampling ---
    g_ReSTIRGI_TemporalParams = rtxdi::GetDefaultReSTIRGITemporalResamplingParams();
    g_ReSTIRGI_TemporalParams.maxHistoryLength       = 16;
    g_ReSTIRGI_TemporalParams.maxReservoirAge        = 60;
    g_ReSTIRGI_TemporalParams.enableFallbackSampling  = 1u;
    g_ReSTIRGI_TemporalParams.enablePermutationSampling = 1u;
    g_ReSTIRGI_TemporalParams.biasCorrectionMode     = RTXDI_GIBiasCorrectionMode::Raytraced;
    g_ReSTIRGI_TemporalParams.depthThreshold         = 0.1f;
    g_ReSTIRGI_TemporalParams.normalThreshold        = 0.9f;

    // --- Boiling filter (off — let NRD handle noise) ---
    g_ReSTIRGI_BoilingParams.enableBoilingFilter    = 0u;
    g_ReSTIRGI_BoilingParams.boilingFilterStrength  = 0.0f;

    // --- Spatial resampling ---
    g_ReSTIRGI_SpatialParams = rtxdi::GetDefaultReSTIRGISpatialResamplingParams();
    g_ReSTIRGI_SpatialParams.numSamples             = 4;
    g_ReSTIRGI_SpatialParams.samplingRadius         = 48.0f;
    g_ReSTIRGI_SpatialParams.biasCorrectionMode     = RTXDI_GIBiasCorrectionMode::Raytraced;
    g_ReSTIRGI_SpatialParams.depthThreshold         = 0.1f;
    g_ReSTIRGI_SpatialParams.normalThreshold        = 0.9f;

    // --- Final shading ---
    g_ReSTIRGI_FinalShadingParams = rtxdi::GetDefaultReSTIRGIFinalShadingParams();
    g_ReSTIRGI_FinalShadingParams.enableFinalMIS       = 1u;  // MIS reduces bias
    g_ReSTIRGI_FinalShadingParams.enableFinalVisibility = 1u;
}
```

**ImGui control** — add alongside the existing DI quality mode (in `RTXDIIMGUISettings()`):

```cpp
// ---- GI Quality mode preset ------------------------------------------------
if (ImGui::Combo("GI Quality Mode", reinterpret_cast<int*>(&g_ReSTIRGI_QualityMode),
        "High Performance\0"
        "High Quality\0"))
{
    if (g_ReSTIRGI_QualityMode == ReSTIRGI_QualityMode::HighPerformance)
        ApplyHighPerfGIPreset();
    else
        ApplyHighQualityGIPreset();
}
```

**Initialization** — call `ApplyHighPerfGIPreset()` at startup (in the existing init block
near line 625 where `g_ReSTIRGI_*` are initialized), then let `RTXDIIMGUISettings()`
apply the selected preset every frame via the ImGui combo.

**Key differences between HighPerformance and HighQuality for ReSTIR GI:**

| Parameter | High Performance | High Quality | Effect |
|-----------|:---------------:|:------------:|--------|
| `maxHistoryLength` | 4 | 16 | Temporal stability vs reaction speed |
| `maxReservoirAge` | 15 | 60 | How long a reservoir can live |
| `biasCorrectionMode` (temporal) | Off | Raytraced | Extra shadow rays for unbiased reuse |
| `biasCorrectionMode` (spatial) | Off | Raytraced | Extra shadow rays for unbiased reuse |
| `enablePermutationSampling` | Off | On | Jitters temporal samples for denoiser |
| `normalThreshold` | 0.6 | 0.9 | Stricter surface similarity test |
| `enableBoilingFilter` | On (0.2) | Off | Cheap noise suppression vs clean signal |
| `spatial.numSamples` | 1 | 4 | Neighbor count for spatial reuse |
| `spatial.samplingRadius` | 16 px | 48 px | Screen-space search radius |
| `enableFinalMIS` | Off | On | MIS reduces bias at cost of extra eval |
| `enableFinalVisibility` | On | On | Both modes trace final visibility ray |



### 4.5 `src/shaders/rtxdi/LightingPasses/ShadeSecondarySurfaces.hlsl` — **Primary integration point**

This is the **only shader file that needs algorithmic changes**.

#### 4.5.1 Current flow (pseudocode)

```hlsl
[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, ...)]
void main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    // ... unpack secondary GBuffer data ...
    RAB_Surface secondarySurface = /* from secondary GBuffer */;

    // === EXPENSIVE: direct lighting evaluation ===
    if (isValidSecondarySurface && !isEnvironmentMap)
    {
        RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(
            rng, tileRng, secondarySurface,
            g_Const.restirDI.initialSamplingParams,
            g_Const.lightBufferParams,
            /* presampling params */,
            lightSample);

        ShadeSurfaceWithLightSample(reservoir, secondarySurface,
            g_Const.restirDI.shadingParams, lightSample, ...);
    }

    // ... seed GI reservoir with radiance ...
}
```

#### 4.5.2 Modified flow for combined mode

```hlsl
#include "SharcCommon.h"           // SHARC SDK
#include "srrhi/hlsl/SHARC.hlsli"  // Generated bindings
#include "SharcHelpers.hlsli"      // BuildSharcParameters

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, ...)]
void main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    // ... unpack secondary GBuffer data ...
    RAB_Surface secondarySurface = /* from secondary GBuffer */;

    bool usedCache = false;

    if (isValidSecondarySurface && !isEnvironmentMap)
    {
#if RESTIR_GI_SHARC_MODE  // compile-time define or runtime branch
        // ---- Step 1: Try SHARC cache lookup ----
        SharcParameters sharcParams = BuildSharcParameters(
            g_SHARCConst, g_HashEntries, g_Resolved, g_Accumulation);

        SharcHitData sharcHitData;
        sharcHitData.positionWorld = secondarySurface.worldPos;
        sharcHitData.normalWorld   = secondarySurface.normal;

        float3 cachedRadiance;
        if (SharcGetCachedRadiance(sharcParams, sharcHitData, cachedRadiance, false))
        {
            // Cache HIT — use cached radiance directly
            radiance += cachedRadiance;
            usedCache = true;
        }
#endif

        if (!usedCache)
        {
            // ---- Step 2: Fallback — original direct lighting path ----
            RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(
                rng, tileRng, secondarySurface,
                g_Const.restirDI.initialSamplingParams,
                g_Const.lightBufferParams,
                /* presampling params */,
                lightSample);

            ShadeSurfaceWithLightSample(reservoir, secondarySurface,
                g_Const.restirDI.shadingParams, lightSample, ...);

            // Firefly suppression
            float indirectLuminance = calcLuminance(radiance);
            if (indirectLuminance > c_MaxIndirectRadiance)
                radiance *= c_MaxIndirectRadiance / indirectLuminance;
        }
    }

    // ... seed GI reservoir (unchanged) ...
}
```

#### 4.5.3 Shader compilation

**Runtime branch**

Use a constant buffer flag (e.g., `g_Const.restirGI.useSharcCache`) to
conditionally take the cache lookup path. The SHARC headers are always
included but the code is conditionally executed. Downside: 1-2% overhead
from the branch in non-combined modes.

### 4.6 `src/ImGuiLayer.cpp` — UI toggle

Add a new radio button for the combined mode:

```cpp
ImGui::SameLine();
if (ImGui::RadioButton("ReSTIR GI + SHARC", &technique,
    static_cast<int>(srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC)))
    g_Renderer.m_IndirectLightingTechnique =
        srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC;
```

### 4.7 `src/shaders/DeferredLighting.hlsl` — No changes needed

The ReSTIR GI FinalShading pass already additively blends its output into the
DI output textures (`u_DiffuseLighting` / `u_SpecularLighting`). The
DeferredLighting pass reads `g_RTXDIDIComposited` which includes both DI and
GI contributions. No change required.

### 4.8 `src/SHARCRenderer.cpp` — Conditional execution

In `SHARCRenderer::Setup()` and `SHARCRenderer::Render()`, extend the condition
to also run when the combined mode is selected:

```cpp
bool Setup(RenderGraph& renderGraph) override
{
    if (g_Renderer.m_IndirectLightingTechnique !=
            srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_SHARC &&
        g_Renderer.m_IndirectLightingTechnique !=
            srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC)
        return false;
    // ... declare SHARC buffers ...
}
```

---

## 5. SHARC Cache Lookup in ShadeSecondarySurfaces — Detailed Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│              ShadeSecondarySurfaces (combined mode)                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. Unpack SecondaryGBufferData from BrdfRayTracing output:         │
│     - worldPos (secondary hit position from BRDF ray)               │
│     - normal (geometry normal at hit, oct-encoded)                  │
│     - throughput (attenuation from primary surface BSDF)            │
│     - diffuseAlbedo, specularF0, roughness (secondary surface BRDF) │
│                                                                     │
│  2. Build SHARC hit data:                                           │
│     sharcHitData.positionWorld = secondarySurface.worldPos;         │
│     sharcHitData.normalWorld   = secondarySurface.normal;           │
│                                                                     │
│  3. SharcGetCachedRadiance(sharcParams, sharcHitData, radiance):    │
│     a) HashGridFindEntry → compute hash key from (pos, normal)      │
│        - Levels chosen by distance to camera (logarithmic LOD)      │
│        - Voxel size = logBase^level / (sceneScale * logBase^bias)   │
│     b) HashGridFind → linear probe in hash bucket (8 slots)          │
│     c) If found: unpack resolved radiance (float16_t4 → float3)     │
│        Check accumulatedSampleNum > threshold (default: 0)          │
│     d) Return cached radiance                                       │
│                                                                     │
│  4. On CACHE HIT:                                                   │
│     radiance = cachedRadiance;                                      │
│     // Apply firefly clamp (same as original)                       │
│                                                                     │
│  5. On CACHE MISS:                                                  │
│     // Fallback to original direct lighting evaluation              │
│     RTXDI_SampleLightsForSurface(...)                               │
│     ShadeSurfaceWithLightSample(...)                                │
│                                                                     │
│  6. Seed GI reservoir with radiance (unchanged)                     │
│     RTXDI_MakeGIReservoir(pos, normal, radiance, pdf)               │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Specular Energy Loss — Analysis and Solution

### 6.1 Why `SHARC_ROUGHNESS_MIN = 0.4` exists

In [`SHARCUpdate.hlsl`](../../src/shaders/SHARCUpdate.hlsl), the roughness is
clamped during direct lighting evaluation at secondary hits:

```hlsl
float3 Li = EvaluateDirectLightingAtHit(
    attr.m_WorldPos, hitN, V,
    pbr.baseColor,
    max(pbr.roughness, srrhi::SHARCConsts::SHARC_ROUGHNESS_MIN), // ← clamp
    pbr.metallic, rng) + pbr.emissive;
```

**Reason**: SHARC paths are always cosine-weighted (purely diffuse) from the
previous surface. If a secondary surface is glossy (roughness=0.1), computing
its specular BRDF in the direction of a random cosine-sampled ray would produce
a noisy, high-variance specular highlight at an essentially random angle.
Caching this would create incorrect specular reflections when queried from
different incoming directions in the Query pass.

### 6.2 Why NOT to simply lower `SHARC_ROUGHNESS_MIN` to 0.1

Lowering `SHARC_ROUGHNESS_MIN` to match `ROUGHNESS_THRESHOLD` would:

- **Allow sharp specular highlights to be cached** at random incoming angles
- **Create directional artifacts**: When a Query pixel queries the cache from
  a different direction than the Update pixel that populated it, the cached
  specular radiance would be incorrect for that query direction
- **Increase cache noise**: Sharp specular samples have extreme variance,
  requiring many more samples for convergence in the 60-frame EMA window

### 6.3 Correct solution for the combined system

In the combined system, **specular energy preservation comes from the ReSTIR GI
BRDF evaluation, not from the SHARC cache**. Here's why it works:

```
Primary surface (roughness = 0.2, glossy):
  │
  │ BrdfRayTracing samples a BRDF ray (can be specular or diffuse lobe)
  │
  ▼
Secondary surface (some roughness):
  │
  │ SHARC cache provides INCIDENT IRRADIANCE at secondary hit
  │ (computed with roughness≥0.4 — diffuse-dominant, correct for indirect)
  │
  ▼
GI FinalShading:
  │ EvaluateBrdf(primarySurface, reservoir.position)
  │ Uses primary surface's ACTUAL roughness (0.2)
  │ → Specular energy arrives through the primary BRDF lobe
  │
  ▼
Output: correct specular + diffuse contribution
```

**The key insight**: SHARC stores the indirect irradiance at the *secondary*
surface — it represents "what light arrives at the secondary hit point." The
primary surface's specular lobe is evaluated separately in FinalShading using
the *actual* roughness. The SHARC roughness clamp only affects how that
irradiance is computed for caching purposes (avoiding noisy specular samples),
not how the final image looks.

### 6.4 Recommendation

**Keep `SHARC_ROUGHNESS_MIN = 0.4` unchanged.** No changes to SHARC.sr
roughness constants are needed for the combined system.

For surfaces with roughness between `ROUGHNESS_THRESHOLD` (0.1) and
`SHARC_ROUGHNESS_MIN` (0.4):

- They still participate in SHARC Update (roughness ≥ 0.1 passes the threshold)
- They still produce valid cached irradiance (clamped to roughness 0.4 during
  direct lighting eval at the secondary hit)
- Their primary surface BRDF (with actual roughness 0.1–0.4) is correctly
  evaluated in ReSTIR GI's FinalShading

If a specific use case requires sharper indirect specular from secondary
surfaces, consider enabling `SHARC_ENABLE_SH_ENCODING` (spherical harmonics
encoding) in the future, which can store directional radiance and reconstruct
it for any query direction.

---

## 7. Voxel Grid Pattern Analysis

### 7.1 Why the current SHARC Query produces no grid patterns

The current SHARC-only mode shows no visible voxel grid patterns due to
multiple factors working together:

#### 7.1.1 Random-walk dithering

[`SHARCQuery.hlsl`](../../src/shaders/SHARCQuery.hlsl) traces a random diffuse
walk (`TraceIndirectRay`):

```hlsl
float3 rayDir = SampleHemisphereCosine(NextFloat2(rng), normal);
// ↑ Different random direction for EVERY pixel, EVERY frame
```

Each primary pixel shoots a **different random ray**. Even two adjacent pixels
on a flat wall will trace rays in different directions, hit different secondary
positions, and land in different hash-grid voxels. This effectively dithers the
cache lookup positions, preventing visible grid-aligned boundaries.

#### 7.1.2 Logarithmic LOD system

The hash grid uses a level-of-detail system based on distance from camera
([`HashGridCommon.h`](../../external/SHARC/include/HashGridCommon.h)):

```cpp
// Level = log_base(distance²) / 2 + levelBias
uint GetLevel(float3 samplePosition, HashGridParameters params) {
    float distance2 = dot(cameraPos - samplePosition, ...);
    return uint(clamp(0.5 * LogBase(distance2, logBase) + levelBias,
                      1.0, LEVEL_BIT_MASK));
}

// Voxel size grows with level
float GetVoxelSize(uint level, ...) {
    return pow(logBase, level) / (sceneScale * pow(logBase, levelBias));
}
```

With `logBase=2.0` and `sceneScale=50.0`:

| Distance | Level | Voxel Size |
|----------|-------|-----------|
| 1 m | 1 | ~0.02 m |
| 4 m | 2 | ~0.04 m |
| 16 m | 3 | ~0.08 m |
| 64 m | 4 | ~0.16 m |
| 256 m | 5 | ~0.32 m |

Voxels grow with the square root of distance — the grid adapts to perspective,
making boundaries invisible in screen space at typical distances.

#### 7.1.3 Normal octant encoding

The hash key includes a 3-bit normal octant encoding
(`HASH_GRID_USE_NORMALS = 1`):

```cpp
uint normalBits =
    (sampleNormal.x + BIAS >= 0 ? 0 : 1) +
    (sampleNormal.y + BIAS >= 0 ? 0 : 2) +
    (sampleNormal.z + BIAS >= 0 ? 0 : 4);
hashKey |= (normalBits << NORMAL_BIT_OFFSET);
```

Two hits at the same world position but on differently-oriented surfaces
(e.g., floor vs wall at a corner) get different hash keys and different cache
entries. This prevents bleeding between surfaces of different orientations.

#### 7.1.4 Hash scattering

The Jenkins32 hash function maps hash keys pseudo-randomly across the bucket
array. The linear probe (`SHARC_LINEAR_PROBE_WINDOW_SIZE = 8`) further spreads
entries, and `SHARC_BLEND_ADJACENT_LEVELS = 1` blends data between levels
during camera movement, smoothing level transitions.

#### 7.1.5 60-frame temporal accumulation

The Resolve pass uses a 60-frame EMA (`ACCUMULATION_FRAME_NUM = 60`). Each
frame's sparse samples (4% coverage) contribute to a slowly-converging
estimate. The temporal blending naturally smooths any residual spatial
discontinuities at voxel boundaries.

### 7.2 Risk of grid patterns in the combined system

In the combined system, the **cache query position is no longer random-walk
dithered**. Instead, `ShadeSecondarySurfaces` queries the cache at the exact
secondary hit position from `BrdfRayTracing`:

```hlsl
// BrdfRayTracing determines the secondary hit via BRDF importance sampling
// (GGX VNDF for specular, cosine hemisphere for diffuse)
secondarySurface.position = ray.Origin + ray.Direction * payload.committedRayT;
```

This is **more deterministic** than the random walk. On a large flat wall
facing the camera, adjacent primary pixels with similar GGX samples will hit
the wall at similar positions, potentially landing in the same voxel.

### 7.3 Mitigations (already present)

Despite the more deterministic query positions, grid patterns are unlikely to
be visible in practice:

1. **BRDF ray randomization**: `BrdfRayTracing` uses `RTXDI_InitRandomSampler`
   with per-pixel and per-frame seeds. Each pixel gets different GGX samples
   and different cosine-hemisphere samples. Adjacent pixels trace rays in
   slightly different directions → different secondary hit positions → different
   voxels. This provides inherent spatial jitter.

2. **ReSTIR GI spatial resampling**: Adjacent pixels with different reservoir
   samples exchange information. A pixel that happens to land in a low-quality
   voxel can "borrow" a higher-quality sample from a neighbor. This smooths
   any remaining quantization artifacts.

3. **ReSTIR GI temporal resampling**: Reservoirs accumulate across frames with
   different BRDF ray directions each frame (different random seeds). Over
   time, each pixel samples multiple voxels, converging to a smooth average.

4. **NRD denoising**: The final NRD pass applies spatial filtering that
   eliminates any residual grid-structured noise.

5. **Logarithmic LOD**: The voxel grid adapts to perspective — far surfaces
   have larger voxels but also subtend fewer screen pixels. The spatial
   frequency of grid artifacts is below the Nyquist limit at typical viewing
   distances.

### 7.4 Additional mitigation (if needed)

If grid patterns become visible (e.g., on perfectly flat surfaces at grazing
angles), an optional jitter can be added to the SHARC query position in
`ShadeSecondarySurfaces`:

```hlsl
// Optional: sub-voxel jitter for query position
float  level  = HashGridGetLevel(sharcHitData.positionWorld, sharcParams.hashGridParameters);
float  voxel  = HashGridGetVoxelSize(level, sharcParams.hashGridParameters);
float3 jitter = (float3(RTXDI_GetNextRandom(rng),
                        RTXDI_GetNextRandom(rng),
                        RTXDI_GetNextRandom(rng)) - 0.5f) * voxel * 0.1f;
sharcHitData.positionWorld += jitter;
```

This is listed as an optional future enhancement. The analysis above suggests
it will not be needed for typical scenes.

---

## 8. Cache Miss Fallback — Detailed Design

### 8.1 When cache misses occur

A cache miss in `SharcGetCachedRadiance` happens when:

1. **Hash entry not found**: The hash grid has no entry at the queried
   `(position, normal, level)` combination. Hash collisions can cause false
   misses even if the data exists elsewhere.
2. **Insufficient samples**: The entry exists but `accumulatedSampleNum ≤
   SHARC_SAMPLE_NUM_THRESHOLD` (default: 0). This happens in newly populated
   regions or areas with sparse Update coverage.
3. **Stale entry evicted**: The entry was evicted due to
   `staleFrameNum ≥ STALE_FRAME_NUM_MAX` (64 frames without new samples).

### 8.2 Expected cache hit rates

- **Static camera, diffuse scene**: 85-95% after warmup (60 frames)
- **Moving camera, new areas**: 30-50% initially, rising to 70-80% after a
  few seconds
- **Camera cut**: 40-60% (cache persists, but camera may see new areas)
- **Dynamic lighting**: 60-80% (stale entries with old lighting may be
  pre-evicted)

The SHARC sparse Update (4% coverage per frame) means ~92% of the scene is
repopulated within 30 frames (approximately 1 second at 60 FPS).

### 8.3 Fallback implementation

```
ShadeSecondarySurfaces (combined mode):
  ┌──────────────────────────────────────┐
  │ Try SHARC cache lookup               │
  │   ↓                                  │
  │ HIT → use cached radiance (cheap)    │
  │   ↓                                  │
  │ MISS → fallback to original code:    │
  │   RTXDI_SampleLightsForSurface()     │
  │   + ShadeSurfaceWithLightSample()    │
  │   + firefly clamp                    │
  │   ↓                                  │
  │ Seed GI reservoir                    │
  └──────────────────────────────────────┘
```

**The fallback is the exact same code as the current (non-SHARC)
`ShadeSecondarySurfaces` path** — no new code needed. The `if (!usedCache)`
branch simply executes the existing direct lighting evaluation.

### 8.4 Why the fallback is effective with ReSTIR GI

Cache misses are not catastrophic because:

1. **Spatial resampling**: A pixel with a cache miss can borrow a valid
   (cache-hit) sample from a neighbor during spatial resampling.
2. **Temporal resampling**: Even if a pixel gets a miss this frame, it may
   retain a valid sample from a previous frame via temporal resampling.
3. **ReSTIR GI's MIS**: The final shading pass evaluates both the resampled
   and initial reservoirs with MIS weighting, giving more weight to whichever
   has better BRDF alignment.
4. **NRD**: The denoiser fills remaining gaps.

In the worst case (100% miss rate due to empty cache on first frame), the
system gracefully degrades to **standard ReSTIR GI** behavior — no worse than
the current mode.

---

## 9. Performance Analysis

### 9.1 Cost comparison per ShadeSecondarySurfaces invocation

| Operation | Current (ReSTIR GI only) | Combined (cache hit) | Combined (cache miss) |
|-----------|:----------------------:|:--------------------:|:---------------------:|
| `HashGridFindEntry` (hash computation + linear probe) | — | ~5 scattered reads | ~5 scattered reads |
| `SharcGetVoxelData` (unpack radiance) | — | ~1 read | ~1 read |
| `RTXDI_SampleLightsForSurface` (RIS + light sampling) | ~50-200 reads | — | ~50-200 reads |
| `ShadeSurfaceWithLightSample` (shadow rays) | 1-2 rays | — | 1-2 rays |
| **Total per sample** | **~50-200 reads + 1-2 rays** | **~6 reads** | **~55-205 reads + 1-2 rays** |

### 9.2 Amortized SHARC Update cost

SHARC Update traces 4% of pixels (1 per 5×5 block) with 2-4 bounces each.
Each bounce evaluates direct lighting with all lights (equivalent to one
`ShadeSecondarySurfaces` invocation). Net cost per frame:
- ~4% × 3 bounces × 1 direct-lighting-eval = ~12% of a full-screen direct
  lighting evaluation

The SHARC Resolve pass is a trivial linear scan over cache entries (256 threads
per group, 16K groups for 4M entries). This is < 0.1ms even at 4M entries.

### 9.3 Estimated net performance

Assuming `ShadeSecondarySurfaces` is ~40% of the ReSTIR GI frame time:

- Cache hit rate: 80%
- Savings per hit: ~100% of `ShadeSecondarySurfaces` cost
- Savings per frame: 80% × 40% = **32%** of GI pipeline cost
- Added cost (SHARC Update + Resolve): ~12% + 0.1ms
- **Net savings: ~20%** of GI pipeline time

This translates to roughly **8-12%** of total frame time saved (depending on
scene complexity and GI weight in the frame budget).

### 9.4 Memory overhead

SHARC buffers are ~160 MB total at 2^22 entries:
- Hash entries: 32 MB (uint64_t × 4M)
- Accumulation: 64 MB (uint4 × 4M)
- Resolved: 64 MB (float16_t4 + 2×uint × 4M)

These are already allocated when SHARC mode is active. The combined mode uses
the same buffers — no additional memory is needed beyond what SHARC alone
requires.

---

## 10. Threading and Group Size Considerations

### 10.1 ShadeSecondarySurfaces thread group

Current group size: `RTXDI_SCREEN_SPACE_GROUP_SIZE (typically 8×8 = 64 threads)`

The SHARC cache lookup adds ~6 scattered memory reads per thread. With 64
threads, this is ~384 reads per group — well within GPU memory subsystem
capabilities. No group size change is needed.

### 10.2 Pipeline ordering

```
Frame N:
  ┌────────────────────────────────────┐
  │ 1. SHARC Update (sparse, 4%)       │ ← populate cache
  │ 2. UAV barrier                     │
  │ 3. SHARC Resolve (linear scan)     │ ← temporal blend
  │ 4. UAV→SRV barrier                 │ ← transition SHARC buffers to read
  │ ─────── ReSTIR GI passes ────────  │
  │ 5. BrdfRayTracing                  │
  │ 6. ShadeSecondarySurfaces          │ ← queries SHARC cache (read-only)
  │ 7. GI Temporal Resampling           │
  │ 8. GI Spatial Resampling            │
  │ 9. GI FinalShading                 │
  │ 10. NRD denoising                  │
  └────────────────────────────────────┘
```

The SHARC Update writes to the cache in frame N. The ReSTIR GI passes in the
same frame N read from the resolved buffer (blended from frame N-1's
accumulation and older frames). The frame N accumulation is resolved in frame
N+1's Resolve pass. This means there's a **1-frame latency** between SHARC
updates and their availability to ReSTIR GI queries. This is acceptable since
SHARC has a 60-frame accumulation window — one frame of delay is negligible.

---

## 11. Testing Strategy

### 11.1 A/B comparison workflow

1. Start the application, load a test scene (e.g., Sponza, Cornell Box)
2. In the ImGui panel, toggle between the four modes:
   - **None**: Direct lighting only — baseline reference
   - **ReSTIR GI**: Current implementation — quality reference
   - **SHARC**: Current implementation — performance reference
   - **ReSTIR GI + SHARC**: Combined — test subject
3. Compare visual quality at matched positions:
   - Static camera (warm cache)
   - Moving camera (cache misses)
   - Camera cut (temporal reset)
   - Dynamic lighting scene (if available)

### 11.2 Key metrics to measure

| Metric | Tool | Expected |
|--------|------|----------|
| Frame time | GPU profiler (PIX/RenderDoc) | 8-12% faster than ReSTIR GI alone |
| Cache hit rate | Add debug counter in shader | >80% after warmup |
| PSNR vs reference PT | Image comparison | Within 1-2 dB of ReSTIR GI alone |
| Dark spots / artifacts | Visual inspection | None visible after NRD |
| Memory usage | Task Manager / PIX | +0 MB vs SHARC alone |

### 11.3 Debug visualization

Add a debug overlay mode to visualize cache hits/misses in combined mode:

```hlsl
// In ShadeSecondarySurfaces, when debug flag is set:
if (g_Const.restirGI.debugShowSharcHits)
{
    if (usedCache)
        debugColor = float3(0, 1, 0); // Green = cache hit
    else
        debugColor = float3(1, 0, 0); // Red = cache miss (fell back to DL)
    // Store debug color to a debug output texture
}
```

---

## 12. Implementation Order (Recommended)

| Phase | Tasks | Estimated Effort |
|-------|-------|:---------------:|
| **Phase 1: Plumbing** | Add `INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC` constant. Extend `SHARCRenderer::Setup/Render` to also run in combined mode. Add ImGui radio button. Bind SHARC cache buffers into ReSTIR GI binding set. Add ReSTIR GI HighPerformance/HighQuality presets (enum, ApplyHighPerfGIPreset, ApplyHighQualityGIPreset, ImGui combo) in `RTXDIRenderer.cpp`. | 3-4 hours |
| **Phase 2: Shader integration** | Include SHARC headers in `ShadeSecondarySurfaces.hlsl`. Add `#if RESTIR_GI_SHARC_MODE` blocks. Implement cache query + fallback. Add shader variant to `shaders.cfg`. | 3-4 hours |
| **Phase 3: Testing & Tuning** | A/B comparison with ReSTIR GI and SHARC standalone modes. Tune cache thresholds if needed. Verify no regressions in non-combined modes. | 2-3 hours |
| **Phase 4: Polish** | Add debug overlay. Profile and optimize if needed. Update README/documentation. | 1-2 hours |

**Total estimated effort**: 9-13 hours

---

## 13. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|:---------:|:------:|------------|
| Cache hit rate too low in typical use | Low | Medium | Spatial resampling + temporal resampling + NRD handle misses gracefully. System degrades to ReSTIR GI behavior. |
| Voxel grid patterns visible on flat surfaces | Low | Medium | BRDF ray randomization provides natural jitter. If needed, add sub-voxel jitter to cache query. |
| Performance regression from SHARC overhead | Low | Low | SHARC Update is only 4% sparse — negligible cost. If Resolve is slow, reduce `SHARC_CACHE_ENTRIES` from 2^22 to 2^20. |
| SHARC SDK version incompatibility | Low | High | Pin SHARC SDK version. Test compilation with current SDK before starting. |
| Render graph resource state transitions | Medium | Medium | Ensure proper UAV→SRV barriers between SHARC Resolve and ReSTIR GI passes. |
| Binding set slot exhaustion | Low | Medium | SHARC cache adds 3-4 SRV bindings. Verify total binding count stays within GPU limits. |

---

## 14. References

- [RTXDI GitHub](https://github.com/NVIDIA-RTX/RTXDI) — ReSTIR DI & GI reference implementation
- [RTXGI GitHub](https://github.com/NVIDIA-RTX/RTXGI) — RTXGI (includes SHARC SDK v1.8)
- [`ShadeSecondarySurfaces.hlsl`](../../src/shaders/rtxdi/LightingPasses/ShadeSecondarySurfaces.hlsl) — Primary integration point
- [`SHARCUpdate.hlsl`](../../src/shaders/SHARCUpdate.hlsl) — Cache population pass
- [`SHARCQuery.hlsl`](../../src/shaders/SHARCQuery.hlsl) — Cache query pass (current standalone mode)
- [`BrdfRayTracing.hlsl`](../../src/shaders/rtxdi/LightingPasses/BrdfRayTracing.hlsl) — BRDF ray tracing for GI initial samples
- [`GI/FinalShading.hlsl`](../../src/shaders/rtxdi/LightingPasses/GI/FinalShading.hlsl) — Final GI shading with BRDF evaluation
- [`SharcCommon.h`](../../external/SHARC/include/SharcCommon.h) — SHARC SDK core (v1.8)
- [`HashGridCommon.h`](../../external/SHARC/include/HashGridCommon.h) — Hash grid implementation
- [`SHARC.sr`](../../src/shaders/SHARC.sr) — SHARC constants and resource declarations
- [`DeferredLighting.hlsl`](../../src/shaders/DeferredLighting.hlsl) — Final compositing
- [`RTXDIRenderer.cpp`](../../src/RTXDIRenderer.cpp) — ReSTIR GI host-side dispatch
- [`SHARCRenderer.cpp`](../../src/SHARCRenderer.cpp) — SHARC host-side dispatch
- [`Common.sr`](../../src/shaders/Common.sr) — Shared constants including IndirectLightingMode
