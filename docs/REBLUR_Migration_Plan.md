# REBLUR Migration Plan — RELAX → REBLUR_DIFFUSE_SPECULAR

## Summary

Switch the ReSTIR DI denoising pipeline from `nrd::Denoiser::RELAX_DIFFUSE_SPECULAR` to `nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR`.

**Expected performance:** ~1.9ms on RTX 3080 (down from ~2.4ms), approximately 22% faster based on NRD v4.17.3 published benchmarks.

**Key insight:** The codebase was already pre-wired for REBLUR. The `DENOISER_MODE_REBLUR = 1` constant, `reblurHitDistParams` cbuffer field, and commented-out REBLUR pack/unpack code already exist. The actual migration is un-commenting and activating existing scaffolding.

---

## Architecture Overview

### Current Data Flow (RELAX)

```
 ShadeSamples / FinalShading / BrdfRayTracing
     │
     ▼
 StoreShadingOutput()  [ShadingHelpers.hlsli]
     │  RELAX_FrontEnd_PackRadianceAndHitDist(radiance, hitDist)
     │  → float4(RGB, hitDist)  stored in IN_DIFF_RADIANCE_HITDIST / IN_SPEC_RADIANCE_HITDIST
     ▼
 NrdIntegration::RunDenoiserPasses()  [NrdIntegration.cpp]
     │  nrd::GetComputeDispatches() → multiple CS dispatches
     │  → OUT_DIFF_RADIANCE_HITDIST / OUT_SPEC_RADIANCE_HITDIST
     ▼
 CompositingPass (pixel shader)  [CompositingPass.hlsl]
     │  RELAX_BackEnd_UnpackRadiance()  → passthrough (identity)
     │  → remodulate: radiance × albedo/F0 + emissive
     ▼
 Final HDR output
```

### Target Data Flow (REBLUR)

```
 ShadeSamples / FinalShading / BrdfRayTracing
     │
     ▼
 StoreShadingOutput()  [ShadingHelpers.hlsli]
     │  REBLUR_FrontEnd_GetNormHitDist(hitDist, viewZ, hitDistParams, roughness)
     │  REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDist)
     │  → YCoCg-encoded float4(YCoCg, normHitDist)  stored in IN_DIFF_RADIANCE_HITDIST
     ▼
 NrdIntegration::RunDenoiserPasses()  [NrdIntegration.cpp]  ← same path, different shaders
     ▼
 CompositingPass (pixel shader)  [CompositingPass.hlsl]
     │  REBLUR_BackEnd_UnpackRadianceAndNormHitDist()  → YCoCg→RGB decode
     │  → remodulate: radiance × albedo/F0 + emissive
     ▼
 Final HDR output
```

**Critical difference:** REBLUR expects normalized hit distance (not raw), YCoCg color encoding, and the `.w` channel of the output carries AO/SO (ambient/specular occlusion), not raw hit distance.

---

## File-by-File Change Plan

### File 1: `src/Renderer.h`

**Lines to change:** ~262

| What | Old | New |
|------|-----|-----|
| Bool flag | `bool m_EnableReSTIRDIRelaxDenoising = true;` | Rename to `m_EnableReSTIRDenoising` (generic name)

---

### File 2: `src/RTXDIRenderer.cpp`

This is the main host-side file. Changes are listed in execution order:

#### 2a. Member variable (line 526)

```cpp
// Before:
nrd::RelaxSettings m_NRDRelaxSettings;

// After:
nrd::ReblurSettings m_NRDReblurSettings;
```

#### 2b. Denoiser creation (line 607)

```cpp
// Before:
m_NrdIntegration = std::make_unique<NrdIntegration>(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);

// After:
m_NrdIntegration = std::make_unique<NrdIntegration>(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);
```

#### 2c. Settings initialization (line 610)

```cpp
// Before:
m_NRDRelaxSettings.enableAntiFirefly = true;

// After:
m_NRDReblurSettings.enableAntiFirefly = true;
```

#### 2d. Denoiser mode flag (line 1212)

```cpp
// Before:
g_Const.SetDenoiserMode(g_Renderer.m_EnableReSTIRDenoising
    ? srrhi::RTXDIConstants::DENOISER_MODE_RELAX : srrhi::RTXDIConstants::DENOISER_MODE_OFF);

// After:
g_Const.SetDenoiserMode(g_Renderer.m_EnableReSTIRDenoising
    ? srrhi::RTXDIConstants::DENOISER_MODE_REBLUR : srrhi::RTXDIConstants::DENOISER_MODE_OFF);
```

#### 2e. ReblurHitDistanceParams → cbuffer (NEW — add after line 1213)

The `reblurHitDistParams` cbuffer field already exists in `RTXDI.sr` but is never written. REBLUR needs it to reconstruct actual hit distances from normalized values. Add:

```cpp
// ---- Reblur hit distance params (used by REBLUR_FrontEnd_GetNormHitDist) ----
{
    // NRD default values. See ReblurHitDistanceParameters in NRDSettings.h
    // normHitDist = saturate(hitDist / (A + viewZ * B) * lerp(C, 1.0, smc))
    //   where smc = F(roughness)
    g_Const.SetReblurHitDistParams(nrd::float4{3.0f, 0.1f, 20.0f, 0.0f});
}
```

*Note:* The srrhi codegen will produce `SetReblurHitDistParams()` from the `float4 reblurHitDistParams;` cbuffer member in `RTXDI.sr`. If the generated name differs, adjust accordingly.

#### 2f. Denoiser settings struct passed to RunDenoiserPasses (line 1975)

```cpp
// Before:
&m_NRDRelaxSettings);

// After:
&m_NRDReblurSettings);
```

#### 2g. ImGui label (line 95)

```cpp
// Before:
ImGui::Checkbox("Enable RELAX Denoising (NRD)", &g_Renderer.m_EnableReSTIRDenoising);

// After:
ImGui::Checkbox("Enable REBLUR Denoising (NRD)", &g_Renderer.m_EnableReSTIRDenoising);
```

#### 2h. ImGui tooltip (line 97)

```cpp
// Before:
ImGui::TextDisabled("  Diffuse + specular denoised separately via RELAX D+S.");

// After:
ImGui::TextDisabled("  Diffuse + specular denoised separately via REBLUR D+S.");
```

#### 2i. Comments referencing "RELAX" (lines 729, 1265, 1955)

Update code comments that mention "RELAX" to say "REBLUR" or "NRD":
- Line ~729: `// RELAX denoising output textures` → `// NRD denoising output textures`
- Line ~1265: `// When RELAX denoising is active...` → `// When NRD denoising is active...`
- Line ~1955: `// RELAX denoising` → `// REBLUR denoising`

---

### File 3: `src/shaders/rtxdi/LightingPasses/ShadingHelpers.hlsli`

This file has **two** overloads of `StoreShadingOutput` (at lines 126 and 209) — both contain identical NRD packing logic that must be updated.

#### 3a. First overload (line ~183-197)

Replace the `#if WITH_NRD` block:

```hlsl
// BEFORE (lines 180-197):
#if WITH_NRD
    if(g_Const.denoiserMode != DENOISER_MODE_OFF && isLastPass)
    {
        const bool useReLAX = (g_Const.denoiserMode == DENOISER_MODE_RELAX);
        const bool sanitize = true;

        // if (useReLAX)
        // {
            u_DiffuseLighting[lightingTexturePos] = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse, diffuseHitT, sanitize);
            u_SpecularLighting[lightingTexturePos] = RELAX_FrontEnd_PackRadianceAndHitDist(specular, specularHitT, sanitize);
        // }
        // else
        // {
        //     float diffNormDist = REBLUR_FrontEnd_GetNormHitDist(diffuseHitT, viewDepth, g_Const.reblurHitDistParams, 1.0);
        //     u_DiffuseLighting[lightingTexturePos] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse, diffNormDist, sanitize);
        //
        //     float specNormDist = REBLUR_FrontEnd_GetNormHitDist(specularHitT, viewDepth, g_Const.reblurHitDistParams, roughness);
        //     u_SpecularLighting[lightingTexturePos] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular, specNormDist, sanitize);
        // }
    }

// AFTER:
#if WITH_NRD
    if(g_Const.denoiserMode != DENOISER_MODE_OFF && isLastPass)
    {
        const bool sanitize = true;

        if (g_Const.denoiserMode == DENOISER_MODE_REBLUR)
        {
            float diffNormDist = REBLUR_FrontEnd_GetNormHitDist(diffuseHitT, viewDepth, g_Const.reblurHitDistParams, 1.0);
            u_DiffuseLighting[lightingTexturePos] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse, diffNormDist, sanitize);

            float specNormDist = REBLUR_FrontEnd_GetNormHitDist(specularHitT, viewDepth, g_Const.reblurHitDistParams, roughness);
            u_SpecularLighting[lightingTexturePos] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular, specNormDist, sanitize);
        }
        else // DENOISER_MODE_RELAX — keep as fallback
        {
            u_DiffuseLighting[lightingTexturePos] = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse, diffuseHitT, sanitize);
            u_SpecularLighting[lightingTexturePos] = RELAX_FrontEnd_PackRadianceAndHitDist(specular, specularHitT, sanitize);
        }
    }
```

#### 3b. Second overload (line ~266-280)

Identical change — replace the same block in the second StoreShadingOutput overload. The `viewDepth` and `roughness` parameters are available in both overloads.

#### 3c. Checkerboard path (lines 165, 248)

Lines 165 and 248 check `g_Const.denoiserMode == DENOISER_MODE_RELAX || g_Const.enableAccumulation`. These should be widened to also accept REBLUR:

```hlsl
// Before:
if (g_Const.denoiserMode == DENOISER_MODE_RELAX || g_Const.enableAccumulation)

// After:
if (g_Const.denoiserMode == DENOISER_MODE_RELAX || g_Const.denoiserMode == DENOISER_MODE_REBLUR || g_Const.enableAccumulation)
```

Or better, check for any non-OFF mode:

```hlsl
if (g_Const.denoiserMode != DENOISER_MODE_OFF || g_Const.enableAccumulation)
```

---

### File 4: `src/shaders/rtxdi/CompositingPass.hlsl`

**Lines to change:** 33-58

#### 4a. Add REBLUR mode alias (line 33)

```hlsl
// Before:
#define DENOISER_MODE_RELAX      srrhi::RTXDIConstants::DENOISER_MODE_RELAX

// After:
#define DENOISER_MODE_RELAX      srrhi::RTXDIConstants::DENOISER_MODE_RELAX
#define DENOISER_MODE_REBLUR     srrhi::RTXDIConstants::DENOISER_MODE_REBLUR
```

#### 4b. Unpacking logic (line 55)

```hlsl
// BEFORE:
#ifdef WITH_NRD
    if (g_Const.denoiserMode == DENOISER_MODE_RELAX)
    {
        diffuse_illumination  = RELAX_BackEnd_UnpackRadiance(diffuse_illumination);
        specular_illumination = RELAX_BackEnd_UnpackRadiance(specular_illumination);
    }
#endif

// AFTER:
#ifdef WITH_NRD
    if (g_Const.denoiserMode == DENOISER_MODE_REBLUR)
    {
        diffuse_illumination  = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffuse_illumination);
        specular_illumination = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specular_illumination);
    }
    else if (g_Const.denoiserMode == DENOISER_MODE_RELAX)
    {
        diffuse_illumination  = RELAX_BackEnd_UnpackRadiance(diffuse_illumination);
        specular_illumination = RELAX_BackEnd_UnpackRadiance(specular_illumination);
    }
#endif
```

**Note:** `REBLUR_BackEnd_UnpackRadianceAndNormHitDist` decodes YCoCg→RGB. The `.w` channel of the output contains AO/SO (ambient/specular occlusion = normalized hit distance). This is fine — the compositing pass only reads `.rgb` for radiance remodulation.

---

### File 5: `src/shaders/RTXDI.sr` — NO CHANGES

Already contains:
- `DENOISER_MODE_REBLUR = 1` (line 170)
- `float4 reblurHitDistParams;` in `ResamplingConstants` cbuffer (line 123)

---

### File 6: `src/NrdIntegration.cpp` / `src/NrdIntegration.h` — NO CHANGES

The `NrdIntegration` class is denoiser-agnostic. It accepts any `nrd::Denoiser` enum and handles pipeline creation generically. The `RunDenoiserPasses` signature uses `const void* denoiserSettings` — compatible with both `nrd::RelaxSettings*` and `nrd::ReblurSettings*`.

---

### File 7: `src/shaders/rtxdi/LightingPasses/RtxdiApplicationBridge/RAB_Buffers.hlsli` — NO CHANGES

Already has `#define DENOISER_MODE_REBLUR  srrhi::RTXDIConstants::DENOISER_MODE_REBLUR` (line 93).

---

## REBLUR vs RELAX: Data Format Differences

| Aspect | RELAX | REBLUR |
|--------|-------|--------|
| **Hit distance in input** | Raw world-space distance | Normalized (saturated to [0,1] using hitDistParams) |
| **Input packing** | `float4(RGB, hitDist)` — raw radiance + raw hitDist | `float4(YCoCg, normHitDist)` — YCoCg color + normalized hitDist |
| **Output unpack** | Identity passthrough | YCoCg → RGB decode |
| **`.w` channel** | Hit distance | AO (diffuse) / SO (specular) — normalized hit distance |
| **Settings struct** | `nrd::RelaxSettings` | `nrd::ReblurSettings` |
| **A-trous iterations** | Configurable (default 5) | N/A (recurrent blur) |
| **Color encoding** | Raw RGB | YCoCg (luminance-chrominance) |

---

## REBLUR Settings at a Glance

Key tunable parameters (from [NRDSettings.h](d:\Workspace\HobbyRenderer\external\NRD\NRD-4.17.3\Include\NRDSettings.h)):

| Parameter | Default | Notes |
|-----------|---------|-------|
| `maxAccumulatedFrameNum` | 30 | Max frames of temporal history |
| `maxFastAccumulatedFrameNum` | 6 | Max frames for "fast" (responsive) history |
| `maxStabilizedFrameNum` | 63 | Max frames for stabilized output pass |
| `historyFixFrameNum` | 3 | Frames to reconstruct after history reset |
| `minBlurRadius` | 1.0 (pixels) | Min spatial denoising radius (converged) |
| `maxBlurRadius` | 30.0 (pixels) | Base (max) spatial denoising radius |
| `enableAntiFirefly` | true | Firefly suppression (cheap, recommended) |
| `hitDistanceParameters.A` | 3.0 | Constant term for hit distance normalization |
| `hitDistanceParameters.B` | 0.1 | ViewZ-based linear scale |
| `hitDistanceParameters.C` | 20.0 | Roughness-based scale clamp |
| `diffusePrepassBlurRadius` | 30.0 | Pre-accumulation spatial reuse (probabilistic only) |
| `specularPrepassBlurRadius` | 50.0 | Pre-accumulation spatial reuse (probabilistic only) |
| `checkerboardMode` | OFF | Not needed if using probabilistic lobe selection |
| `hitDistanceReconstructionMode` | OFF | Enable only for probabilistic sampling |

---

## Verification Checklist

After making all changes:

1. [ ] **Build:** Project compiles without errors
2. [ ] **Runtime:** No NRD assertion failures or `NRD_CHECK` failures on startup
3. [ ] **Visual:** Denoised output is stable — no flickering, no NaN/inf pixels
4. [ ] **Visual:** Diffuse denoising quality comparable or better than RELAX
5. [ ] **Visual:** Specular denoising quality comparable or better than RELAX
6. [ ] **Visual:** Disocclusion handling — camera pan reveals clean until history catches up
7. [ ] **Visual:** Checkerboard/VRR — no artifacts if active
8. [ ] **Performance:** GPU profiler shows ~1.9ms (down from ~2.4ms), ~22% improvement
9. [ ] **Toggle:** ImGui "Enable REBLUR Denoising" checkbox correctly enables/disables denoising
10. [ ] **Fallback:** If RELAX fallback path is kept in shader, toggle DENOISER_MODE_RELAX still works
11. [ ] **GI path:** If ReSTIR GI is enabled, GI FinalShading output (via StoreShadingOutput) is correctly packed for REBLUR
12. [ ] **BRDF indirect:** BrdfRayTracing lighting output is correctly packed for REBLUR

---

## Rollback Plan

To revert, reverse the changes:
1. Switch `nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR` back to `nrd::Denoiser::RELAX_DIFFUSE_SPECULAR`
2. Switch `nrd::ReblurSettings` back to `nrd::RelaxSettings`
3. Switch `DENOISER_MODE_REBLUR` back to `DENOISER_MODE_RELAX` in SetDenoiserMode()
4. Restore the shader pack/unpack functions to RELAX variants
5. Remove the `SetReblurHitDistParams()` call

The RELAX fallback code preserved in `ShadingHelpers.hlsli` and `CompositingPass.hlsl` makes rollback trivial — just change the C++ denoiser mode flag.

---

## Estimated Effort

| Area | Complexity | Files | Lines Changed |
|------|-----------|-------|---------------|
| C++ host (denoiser enum + settings) | Low | 1 | ~15 |
| C++ host (hit dist params) | Low | 1 | ~5 (new) |
| C++ host (comments/labels) | Trivial | 1 | ~8 |
| Shader (ShadingHelpers.hlsli) | Medium | 1 | ~40 |
| Shader (CompositingPass.hlsl) | Low | 1 | ~12 |
| **Total** | | **5 files** | **~80 lines** |
