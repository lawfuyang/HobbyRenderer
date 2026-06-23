# SHARC Specular Indirect — Implementation Plan

## Problem

Current SHARC Query pass (`SHARCQuery.hlsl`) is **diffuse-only**:

- Traces a cosine-weighted random walk from the primary surface (`TraceIndirectRay`)
- Continues until accumulated path length ≥ voxel size
- Queries `SharcGetCachedRadiance` at the distant hit
- At primary: output = `cachedRadiance × albedo × (1 − metallic)`
- **No specular lobe evaluation. No BRDF. No GGX. No colored glossy reflections.**

ReSTIR GI (`BrdfRayTracing.hlsl`) does all of these. This plan brings specular indirect to SHARC.

---

## Related Files

| File | Role |
|------|------|
| `src/shaders/SHARCQuery.hlsl` | **Primary target** — full rewrite of the Query pass |
| `src/shaders/SHARC.sr` | Add `GPULight` buffer + `Atmosphere` constants to `SHARCQueryInputs` |
| `src/shaders/SHARCConsts.sr` | Define `SPECULAR_ROUGHNESS_THRESHOLD` (optional) |
| `src/shaders/SHARCUpdate.hlsl` | **No changes** — Update pass remains diffuse-only (correct per SHARC spec) |
| `src/shaders/SharcHelpers.hlsli` | **No changes** |
| `src/shaders/DeferredLighting.hlsl` | **No changes** — just adds `g_SHARCIndirect.rgb` to color |
| `src/SHARCRenderer.cpp` | Update `SHARCQuery` pass binding to include `GPULight` buffer + atmosphere constants |
| `src/shaders/CommonLighting.hlsli` | Already has all BRDF helpers needed |

---

## Design Decisions

### 1. Output format: single combined RGB

**Decision**: Keep the single `R11G11B10_FLOAT` output texture. Combine diffuse + specular indirect into one value. DeferredLighting adds it as-is (no change to compositing).

**Why not separate channels?** Denoising specular separately requires a denoiser pipeline (REBLUR/RELAX), which is a much larger change. Combined output is simpler, matches AGENTS.md §16 "Query pass writes the final output or denoiser input expected by the engine", and works immediately.

### 2. BRDF sampling: GGX VNDF + cosine hemisphere, MIS-weighted

Per [BrdfRayTracing.hlsl](src/shaders/rtxdi/LightingPasses/BrdfRayTracing.hlsl):
- Sample GGX VNDF → specular direction, compute `F × G₁` weight
- Sample cosine hemisphere → diffuse direction, weight `1.0`
- Luminance-based MIS: `specular_PDF = lum(F·G₁) / lum(F·G₁ + 1.0 × diffuseAlbedo)`
- Choose specular or diffuse via RNG

### 3. Cache eligibility: segment length + footprint gates

Per [AGENTS.md §14.8](external/SHARC/AGENTS.md):
```
segment_length > voxelSize × √3
footprint = segment_length × √(0.5α² / (1−α²)) > voxelSize  (where α = roughness²)
```
If either gate fails → fall back to `EvaluateDirectLightingAtHit` (same as Update pass).

### 4. Delta surfaces: bypass SHARC

If `roughness < ROUGHNESS_THRESHOLD` (0.1) → delta surface:
- Trace specular reflection ray
- Shade secondary with direct lighting (no cache query)
- Apply primary BRDF: `F₀ × G₁` specular weight
- Pure mirror — SHARC doesn't cache this

### 5. Cache miss fallback: evaluate direct lighting at secondary hit

If `SharcGetCachedRadiance` returns false → evaluate direct lighting at the secondary surface using the same `EvaluateDirectLightingAtHit` function from Update pass (copied or shared).

**Consequence**: Query pass now needs `GPULight` buffer + atmosphere data bound (currently absent). These must be added to `SHARCQueryInputs`.

---

## Step-by-Step Implementation

### Step 1: Update `SHARC.sr` — Add light & atmosphere bindings to Query

File: `src/shaders/SHARC.sr`

Add to `SHARCQueryInputs`:
```
// Scene lights (needed for direct-lighting fallback on cache miss)
StructuredBuffer<GPULight> m_Lights;

// Atmosphere data (needed for sun radiance evaluation)
Texture2D<float4> m_TransmittanceLUT;    // existing bruneton LUT
Texture3D<float4> m_ScatteringLUT;       // existing bruneton LUT
Texture2D<float4> m_IrradianceLUT;       // existing bruneton LUT
float             m_AtmosphereBottomRadius;
float             m_AtmosphereTopRadius;
```

### Step 2: Update `SHARCRenderer.cpp` — Bind light & atmosphere to Query pass

File: `src/SHARCRenderer.cpp`

In the Query pass setup (~line 243):
```cpp
// Add:
inputs.SetLights(lightBuffer);
inputs.SetTransmittanceLUT(transmittanceLUT);
// ... etc for atmosphere
```

### Step 3: Rewrite `SHARCQuery.hlsl` — Replace `TraceIndirectRay` with BRDF ray tracing

File: `src/shaders/SHARCQuery.hlsl`

#### 3a. Add includes and bindings

```hlsl
#include "Atmosphere.hlsli"   // ← NEW
#include "CommonLighting.hlsli"

// NEW bindings
static const StructuredBuffer<srrhi::GPULight> g_Lights = ...;
static const Texture2D<float4> g_TransmittanceLUT = ...;
// ... atmosphere textures
```

#### 3b. Add `EvaluateDirectLightingAtHit` (copied from Update)

```hlsl
float3 EvaluateDirectLightingAtHit(
    float3 worldPos, float3 N, float3 V,
    float3 baseColor, float roughness, float metallic,
    inout RNG rng);
```
Copy the function verbatim from `SHARCUpdate.hlsl`.

#### 3c. Replace `TraceIndirectRay` + main shader body with BRDF path

**Per-pixel logic (pseudocode)**:

```
[numthreads(8,8,1)]
void SHARCQuery_CSMain(uint2 pixel):
    1. Read GBuffer: worldPos, N, roughness, metallic, baseColor
    2. if (depth == FAR): output 0, return

    3. V = normalize(cameraPos - worldPos)

    4. // Delta surface (roughness < threshold): mirror reflection
       if (roughness < ROUGHNESS_THRESHOLD):
           if (metallic):
               specDir = reflect(-V, N)
               Trace ray, shade secondary with EvaluateDirectLightingAtHit
               output = secondaryRadiance × Schlick_Fresnel(F0, NoV) × G1
           else:
               output = 0  // dielectric delta — nothing to contribute
           return

    5. // Build ONB for BRDF sampling
       branchlessONB(N, tangent, bitangent)

    6. // Sample specular direction (GGX VNDF)
       Ve = (dot(V,T), dot(V,B), dot(V,N))
       He = sampleGGX_VNDF(Ve, roughness, Rand)
       H = normalize(He.x*T + He.y*B + He.z*N)
       specDir = reflect(-V, H)
       HoV = saturate(dot(H, V))
       NoV = saturate(dot(N, V))
       F = Schlick_Fresnel(F0_from_metallic(baseColor,metallic), HoV)
       G1 = G1_Smith(roughness, NoV)
       specular_BRDF_over_PDF = F × G1

    7. // Sample diffuse direction (cosine hemisphere)
       (diffuse_BRDF_over_PDF, diffuseDir) = SampleCosHemisphere(Rand); weight = 1.0
       diffuseDir = transform from local to world via ONB

    8. // MIS: choose specular or diffuse
       getReflectivity(metallic, baseColor, diffuseAlbedo, specularF0)
       specular_PDF = saturate(luminance(F×G1) / luminance(F×G1 + diffuseAlbedo))
       isSpecRay = NextFloat(rng) < specular_PDF

       if (isSpecRay):
           rayDir = specDir
           brdfWeight = specular_BRDF_over_PDF / specular_PDF
       else:
           rayDir = diffuseDir
           brdfWeight = diffuseAlbedo / (1.0 − specular_PDF)

    9. // Trace single BRDF ray
       ray = RayDesc(worldPos + N×1e-3, rayDir, 1e-3, 1e10)
       hit = TraceRayStandard(ray, rng, hitInfo, ...)

    10. if (!hit): output 0, return  // sky miss

    11. // Extract secondary surface attributes
        attr = GetFullHitAttributes(hitInfo, ray, inst, mesh, indices, vertices)
        pbr  = GetPBRAttributes(attr, mat)
        geoN = normalize(attr.m_WorldNormal)
        hitN = getBentNormal(geoN, pbr.normal, ray.Direction)

    12. // Compute hit distance (segment length)
        hitDist = length(attr.m_WorldPos − worldPos)

    13. // Cache eligibility gates (AGENTS.md §14.8)
        gridLevel  = HashGridGetLevel(attr.m_WorldPos, hashGridParams)
        voxelSize  = HashGridGetVoxelSize(gridLevel, hashGridParams)
        segValid   = hitDist > voxelSize × √3

        clampedRoughness = min(roughness, 0.99)
        alpha = clampedRoughness²
        footprint = hitDist × √(0.5α² / max(1−α², 1e-6))
        footprintValid = footprint > voxelSize

    14. // Try cache lookup
        if (segValid && footprintValid):
            SharcHitData hd = { attr.m_WorldPos, hitN }
            found = SharcGetCachedRadiance(sharcParams, hd, secondaryRadiance, false)

    15. // Cache miss: evaluate direct lighting at secondary hit
        if (!found):
            secondaryV = −rayDir
            secondaryRadiance = EvaluateDirectLightingAtHit(
                attr.m_WorldPos, hitN, secondaryV,
                pbr.baseColor, pbr.roughness, pbr.metallic, rng) + pbr.emissive

    16. // Evaluate primary BRDF
        getReflectivity(metallic, baseColor, diffuseAlbedo, specularF0)
        NoL = saturate(dot(N, rayDir))
        brdfFactor = brdfWeight × NoL  // This already includes MIS weighting

        diffuseIndirect  = isSpecRay ? 0 : secondaryRadiance × brdfFactor
        specularIndirect = isSpecRay ? secondaryRadiance × brdfFactor : 0

        // For specular, demodulate to extract highlight color
        specularIndirect = DemodulateSpecular(specularF0, specularIndirect)

        output = diffuseIndirect + specularIndirect
```

### Step 4: Update debug modes

- **VoxelColor** (mode 1): No change — works at primary surface
- **AccumulatedRadiance** (mode 2): No change — reads cache at primary
- **CacheHeatmap** (mode 3): Replace `TraceIndirectRay` → use the new single BRDF ray trace, then check `SharcGetCachedRadiance` at secondary hit

### Step 5: Verification checklist

1. Visual: colored specular reflections appear (blue/red/green bounces from colored surfaces)
2. Visual: rough surfaces show soft glossy indirect
3. Visual: metallic surfaces show colored specular reflections (roughness ≥ threshold)
4. Visual: mirror surfaces (roughness < threshold) show sharp reflections via direct-lighting fallback
5. Debug: CacheHeatmap shows green where cache hits, red where cache misses
6. Debug: VoxelColor still works (hits primary surface)
7. Performance: 1 ray/pixel (same cost as ReSTIR GI BrdfRayTracing) + optional direct-lighting on cache miss
8. No regression: diffuse indirect still works when isSpecRay=false

---

## Files to Modify (summary)

| File | Changes |
|------|---------|
| `src/shaders/SHARC.sr` | Add `StructuredBuffer<GPULight>`, atmosphere textures to `SHARCQueryInputs` |
| `src/shaders/SHARCQuery.hlsl` | Full rewrite: BRDF ray trace + footprint/segment gates + cache lookup + fallback direct lighting + primary BRDF eval |
| `src/SHARCRenderer.cpp` | Bind new light/atmosphere resources to Query pass |
| `src/CMakeLists.txt` | Verify `SHARCQuery` shader now needs `GPULight.sr` included (already present) |

### Files NOT modified

| File | Why |
|------|-----|
| `src/shaders/SHARCUpdate.hlsl` | Update pass correctly handles diffuse-only; BRDF at primary is Query's job |
| `src/shaders/SHARCResolve.hlsl` | Resolve is cache-internal; agnostic to what the Query pass does |
| `src/shaders/DeferredLighting.hlsl` | Just adds `g_SHARCIndirect.rgb` to final color |
| `src/shaders/SharcHelpers.hlsli` | Unchanged |
| `src/shaders/SHARCConsts.sr` | (Optional: add `SPECULAR_ROUGHNESS_THRESHOLD` constant) |

---

## Risks & Mitigations

| Risk | Mitigation |
|------|-----------|
| Specular noise (1 BRDF sample/pixel, no denoising) | Acceptable for initial implementation. SHARC cache values are temporally accumulated; the BRDF direction is fresh each frame but the radiance source is stable. Expect some noise on glossy surfaces. Future: add denoiser for specular channel. |
| Performance regression (0 rays → 1 ray/pixel) | Expected. Query pass goes from fullscreen compute to fullscreen ray trace. This is the correct SHARC Query pattern per AGENTS.md. Cost ≈ 1 RTXGI equivalent. |
| Cache miss → direct lighting fallback expensive | Only paid when cache misses. As cache populates over time, hit rate approaches ~90%+. Cache entries last up to 64 stale frames. |
| Footprint gate too conservative (rejects too many cache queries) | Tune `min(roughness, 0.99)` and voxel size via `HASH_GRID_SCENE_SCALE` |
