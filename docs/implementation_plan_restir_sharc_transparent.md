# RIS-Guided Single Light + SHARC GI for Transparent Objects — Implementation Plan

> **Source**: Derived from [analysis_restir_di_transparent.md](analysis_restir_di_transparent.md)  
> **Total estimated effort**: 6–10 hours  
> **Risk level**: Low — additive changes, guarded by compile-time and runtime toggles  
> **Dependencies**: RTXDI presample passes must run before transparent pass (already true); SHARC Update+Resolve must run before transparent pass (already true in all `IndirectLightingMode` values)

---

## 0. Pre-requisites & Infrastructure

### 0.1 Promote RIS Buffers to Global Render-Graph Handles

The RIS buffers (`m_RG_RISBuffer`, `m_RG_RISLightDataBuffer`) are currently private members of `RTXDIRenderer`. They must be accessible from `BasePassRenderer.cpp` for the transparent pass.

**File**: [src/RTXDIRenderer.cpp](src/RTXDIRenderer.cpp)

1. Move the handle declarations from `private:` in class `RTXDIRenderer` to the global scope (top of file), matching the pattern used by SHARC handles in [src/SHARCRenderer.cpp](src/SHARCRenderer.cpp):

```cpp
// Promote from private class members to globals (before the class definition):
RGBufferHandle g_RG_RISBuffer;          // RIS tiles (128 × 1024, StructuredBuffer<uint2>)
RGBufferHandle g_RG_RISLightDataBuffer; // Compact light info per RIS entry
```

2. Change all uses in `RTXDIRenderer` from `m_RG_RISBuffer` → `g_RG_RISBuffer` and `m_RG_RISLightDataBuffer` → `g_RG_RISLightDataBuffer`.

3. Remove the member declarations from the `RTXDIRenderer` class.

**File**: [src/BasePassRenderer.cpp](src/BasePassRenderer.cpp)

4. Add `extern` declarations at the top of the file:

```cpp
extern RGBufferHandle g_RG_RISBuffer;           // from RTXDIRenderer
extern RGBufferHandle g_RG_RISLightDataBuffer;  // from RTXDIRenderer
extern RGBufferHandle g_RG_SHARCHashEntries;    // from SHARCRenderer
extern RGBufferHandle g_RG_SHARCResolved;       // from SHARCRenderer
extern RGBufferHandle g_RG_SHARCAccumulation;   // from SHARCRenderer
```

> **Note**: This step may already be partially done by the [SHARC + ReSTIR GI Integration Plan](docs/SHARC_ReSTIR_GI_Integration_Plan.md) if that work is completed first. Coordinate to avoid duplication.

### 0.2 Add New Constants to Common.sr

**File**: [src/shaders/Common.sr](src/shaders/Common.sr)

Add RTXDI RIS tile configuration constants (used by both the opaque RTXDI pipeline and the transparent pass):

```hlsl
srinput CommonConsts
{
    // ... existing constants ...
    static const uint RTXDI_TILE_COUNT       = 128;   // Number of RIS tiles
    static const uint RTXDI_TILE_SIZE        = 1024;  // Candidates per RIS tile
    static const uint RTXDI_TILE_PIXEL_WIDTH = 16;    // Screen-space tile dimension in pixels
};
```

---

## Phase 1: Plumbing — Host Side (~2 hours)

### Task 1.1: Add New Per-Frame Constants to BasePass CB

**File**: [src/shaders/BasePass.sr](src/shaders/BasePass.sr)

Add to the `BasePassConstants` cbuffer (before the closing `};`):

```hlsl
    uint m_EnableReSTIRDI;            // Toggle: RIS-guided direct light for transparent pass
    uint m_EnableSHARCForTransparent; // Toggle: SHARC GI cache lookup for transparent pass
    uint m_EnableRTShadowsTransparent; // Toggle: shadow rays for the RIS-selected light
```

**What changes for the generated C++ binding**: The `srrhi::BasePassConstants` struct gains 3 new `uint` fields. Update the C++ code that populates this CB.

### Task 1.2: Add RIS and SHARC Buffer Bindings to BasePass.sr

**File**: [src/shaders/BasePass.sr](src/shaders/BasePass.sr)

Add to the `BasePassInputs` srinput (after existing entries, before closing `};`):

```hlsl
    // ReSTIR DI RIS buffers (read-only, written by RTXDIRenderer presample passes)
    StructuredBuffer<uint2>  RISBuffer;         // RTXDI RIS tiles (128 × 1024 × uint2)
    StructuredBuffer<uint4>  RISLightDataBuffer; // Compact light info per RIS entry

    // SHARC radiance cache (read-only, written by SHARC Update/Resolve)
    RWStructuredBuffer<uint64_t>                SHARCHashEntries;
    RWStructuredBuffer<SharcPackedData>         SHARCResolved;
    RWStructuredBuffer<SharcAccumulationData>   SHARCAccumulation;
    SHARCConstants                              SHARCConst;
```

**What changes for the generated C++ binding**: `BasePassInputs` struct gains `SetRISBuffer()`, `SetRISLightDataBuffer()`, `SetSHARCHashEntries()`, `SetSHARCResolved()`, `SetSHARCAccumulation()`, and `SetSHARCConst()` methods. The generated HLSL bindings (`srrhi/hlsl/BasePass.hlsli`) gain `GetRISBuffer()`, `GetRISLightDataBuffer()`, etc.

### Task 1.3: Wire Up Render-Graph Read Access in TransparentPassRenderer::Setup()

**File**: [src/BasePassRenderer.cpp](src/BasePassRenderer.cpp), method `TransparentPassRenderer::Setup()`

Add before `return true;` at the end of the function:

```cpp
// ── ReSTIR DI RIS buffers ───────────────────────────────────────────
if (g_Renderer.m_EnableReSTIRDI)  // new member/Renderer field
{
    renderGraph.ReadBuffer(g_RG_RISBuffer);
    renderGraph.ReadBuffer(g_RG_RISLightDataBuffer);
}

// ── SHARC radiance cache ────────────────────────────────────────────
if (g_Renderer.m_EnableSHARCForTransparent)  // new member/Renderer field
{
    renderGraph.ReadBuffer(g_RG_SHARCHashEntries);
    renderGraph.ReadBuffer(g_RG_SHARCResolved);
    renderGraph.ReadBuffer(g_RG_SHARCAccumulation);
}
```

The render graph will automatically insert UAV→SRV barriers because these buffers are consumed with `Read` access (their producers declared them with `Write`).

### Task 1.4: Wire Up Bindings in TransparentPassRenderer::Render()

**File**: [src/BasePassRenderer.cpp](src/BasePassRenderer.cpp), method `TransparentPassRenderer::Render()`

After the existing binding setup for `BasePassInputs`, add:

```cpp
// ── ReSTIR DI RIS buffers ───────────────────────────────────────────
if (g_Renderer.m_EnableReSTIRDI) {
    nvrhi::BufferHandle risBuf = renderGraph.GetBuffer(g_RG_RISBuffer, RGResourceAccessMode::Read);
    nvrhi::BufferHandle risLightBuf = renderGraph.GetBuffer(g_RG_RISLightDataBuffer, RGResourceAccessMode::Read);
    inputs.SetRISBuffer(risBuf);
    inputs.SetRISLightDataBuffer(risLightBuf);
}

// ── SHARC radiance cache ────────────────────────────────────────────
if (g_Renderer.m_EnableSHARCForTransparent) {
    nvrhi::BufferHandle hashEntries = renderGraph.GetBuffer(g_RG_SHARCHashEntries, RGResourceAccessMode::Read);
    nvrhi::BufferHandle resolved    = renderGraph.GetBuffer(g_RG_SHARCResolved, RGResourceAccessMode::Read);
    nvrhi::BufferHandle accum       = renderGraph.GetBuffer(g_RG_SHARCAccumulation, RGResourceAccessMode::Read);
    inputs.SetSHARCHashEntries(hashEntries);
    inputs.SetSHARCResolved(resolved);
    inputs.SetSHARCAccumulation(accum);
    inputs.SetSHARCConst(/* existing SHARC constant buffer */);  // see note below
}
```

> **Note on SHARCConst**: The SHARC constant buffer (`SHARCConstants`) is already populated by `SHARCRenderer`/`RTXDIRenderer`. It contains `m_CameraPosition`, `m_FrameIndex`, `m_ViewportSize`, `m_MatClipToWorld`, etc. The transparent pass needs this same data to build `SharcParameters` and compute `HashGridGetLevel()`. Either reuse the same CB handle or duplicate the relevant fields into `BasePassConstants`.

**Simpler approach — duplicate only what's needed into BasePassConstants**:
Instead of binding a separate SHARC constant buffer, add `m_CameraPosition` (already as `m_CameraPos`) and `m_FrameIndex` (new) to `BasePassConstants`. The hash-grid config parameters (`HASH_GRID_SCENE_SCALE`, `HASH_GRID_LOGARITHM_BASE`, etc.) are compile-time constants in `SHARCConsts` and don't need per-frame upload. Add to `BasePassConstants`:

```hlsl
    uint m_FrameIndex;  // For SHARC cache key generation
```

And populate it in the C++ code where `BasePassConstants` is filled.

### Task 1.5: Populate New Per-Frame Constants

**File**: [src/BasePassRenderer.cpp](src/BasePassRenderer.cpp), where `BasePassConstants` is populated before draw calls

Add:

```cpp
cb.m_EnableReSTIRDI            = g_Renderer.m_EnableReSTIRDI ? 1u : 0u;
cb.m_EnableSHARCForTransparent = g_Renderer.m_EnableSHARCForTransparent ? 1u : 0u;
cb.m_EnableRTShadowsTransparent = g_Renderer.m_EnableRTShadowsTransparent ? 1u : 0u;
cb.m_FrameIndex                = g_Renderer.m_FrameIndex;
```

### Task 1.6: Add ImGui Toggles

**File**: The ImGui rendering section (likely in a UI helper file or `Renderer.cpp`)

Add under the "Transparent" or "Forward Rendering" section:

```cpp
if (ImGui::TreeNode("Transparent Objects")) {
    ImGui::Checkbox("RIS-Guided Direct Light", &m_EnableReSTIRDI);
    ImGui::SameLine(); ShowHelpMarker(
        "Use RTXDI RIS presampled light candidates for single-light importance "
        "sampling in the transparent forward pass. Replaces brute-force O(N) "
        "loop with O(1) per pixel. Best for scenes with >4 lights. "
        "Toggle off to use the legacy brute-force path.");
    if (m_EnableReSTIRDI) {
        ImGui::Indent();
        ImGui::Checkbox("Shadow Rays", &m_EnableRTShadowsTransparent);
        ImGui::SameLine(); ShowHelpMarker(
            "Trace one RT shadow ray for the RIS-selected light. "
            "Increase quality at the cost of 1 RT traversal per transparent pixel.");
        ImGui::Unindent();
    }

    ImGui::Checkbox("SHARC GI (Indirect Lighting)", &m_EnableSHARCForTransparent);
    ImGui::SameLine(); ShowHelpMarker(
        "Query the SHARC world-space radiance cache for 2-4 bounce diffuse "
        "indirect lighting on transparent surfaces. Requires the opaque "
        "IndirectLightingMode to be set to SHARC or ReSTIR GI + SHARC "
        "(modes 2 or 3). No extra rays — pure cache lookup.");

    ImGui::TreePop();
}
```

Add the corresponding member variables to the `Renderer` class.

---

## Phase 2: Shader — RIS Direct Light (~3 hours)

### Task 2.1: Add RTXDI Include Headers and New Resource Declarations

**File**: [src/shaders/BasePass.hlsl](src/shaders/BasePass.hlsl)

After the existing `#include` block (around line 7), conditionally include RTXDI and SHARC headers:

```hlsl
#if defined(FORWARD_TRANSPARENT)
#include "SharcCommon.h"          // SHARC SDK: SharcGetCachedRadiance, SharcParameters, etc.
#include "srrhi/hlsl/SHARC.hlsli" // Generated SHARC bindings
#include "SharcHelpers.hlsli"     // BuildSharcParameters helper
#include "Rtxdi/DI/RISBuffer.hlsli"         // RTXDI RIS tile utilities
#include "Rtxdi/DI/LocalLightSelection.hlsli" // RTXDI light candidate selection
#endif
```

> **Note**: Verify that `RISBuffer.hlsli` and `LocalLightSelection.hlsli` exist at the expected paths under `external/rtxdi/Include/Rtxdi/DI/`. If the RTXDI headers are at a different include path, adjust accordingly. The `RtxdiApplicationBridge` already uses these — follow the same include pattern.

Add new resource declarations after the existing `g_Lights` declaration (around line 23):

```hlsl
#if defined(FORWARD_TRANSPARENT)
static const StructuredBuffer<uint2>                  g_RISBuffer         = srrhi::BasePassInputs::GetRISBuffer();
static const StructuredBuffer<uint4>                  g_RISLightDataBuffer = srrhi::BasePassInputs::GetRISLightDataBuffer();
static const RWStructuredBuffer<uint64_t>              g_SHARCHashEntries   = srrhi::BasePassInputs::GetSHARCHashEntries();
static const RWStructuredBuffer<SharcPackedData>       g_SHARCResolved      = srrhi::BasePassInputs::GetSHARCResolved();
static const RWStructuredBuffer<SharcAccumulationData> g_SHARCAccumulation  = srrhi::BasePassInputs::GetSHARCAccumulation();
static const srrhi::SHARCConstants                    g_SHARCConst         = srrhi::BasePassInputs::GetSHARCConst();
#endif
```

### Task 2.2: Implement RIS-Guided Single Light in Forward_PSMain

**File**: [src/shaders/BasePass.hlsl](src/shaders/BasePass.hlsl), inside the `#if defined(FORWARD_TRANSPARENT)` block, where `AccumulateDirectLighting` is currently called.

**Step A** — Locate the existing direct lighting code (approximately lines 400–416):

```hlsl
    else
    {
        float3 p_atmo = GetAtmospherePos(input.worldPos);

        if (g_PerFrame.m_EnableSky)
        {
            lightingInputs.sunRadiance = GetAtmosphereSunRadiance(p_atmo, g_PerFrame.m_SunDirection, g_Lights[0].m_Intensity);
            lightingInputs.sunShadow = CalculateRTShadow(lightingInputs, lightingInputs.sunDirection, 1e10f);
            lightingInputs.useSunRadiance = true;
        }

        LightingComponents directLighting = AccumulateDirectLighting(lightingInputs, g_PerFrame.m_LightCount);
        float3 directDiffuse = directLighting.diffuse;
        directSpecular = directLighting.specular;

        PrepareLightingByproducts(lightingInputs);

        color = directDiffuse + directSpecular;
    }
```

**Step B** — Replace with the RIS-guided path, gated by the toggle:

```hlsl
    else
    {
        float3 p_atmo = GetAtmospherePos(input.worldPos);

        if (g_PerFrame.m_EnableSky)
        {
            lightingInputs.sunRadiance = GetAtmosphereSunRadiance(p_atmo, g_PerFrame.m_SunDirection, g_Lights[0].m_Intensity);
            lightingInputs.sunShadow = CalculateRTShadow(lightingInputs, lightingInputs.sunDirection, 1e10f);
            lightingInputs.useSunRadiance = true;
        }

        if (g_PerFrame.m_EnableReSTIRDI != 0)
        {
            // ── RIS-Guided Single Light ─────────────────────────────
            // Select a RIS tile using coherent tile-space RNG
            uint2 tilePixel = uint2(input.Position.xy) / srrhi::CommonConsts::RTXDI_TILE_PIXEL_WIDTH;
            RTXDI_RandomSamplerState tileRng = RTXDI_InitRandomSampler(
                tilePixel, g_PerFrame.m_FrameIndex, 42);

            RTXDI_RISBufferSegmentParameters risParams;
            risParams.tileCount    = srrhi::CommonConsts::RTXDI_TILE_COUNT;
            risParams.tileSize     = srrhi::CommonConsts::RTXDI_TILE_SIZE;
            risParams.bufferOffset = 0;

            RTXDI_RISTileInfo tile = RTXDI_RandomlySelectRISTile(
                tileRng, g_RISBuffer, risParams);

            // Draw one candidate from the selected tile
            RNG rng = InitRNG(uint2(input.Position.xy), g_PerFrame.m_FrameIndex);
            uint2 tileData;
            uint  risBufferPtr;
            float rnd = NextFloat(rng);
            RTXDI_RandomlySelectLightDataFromRISTile(
                rnd, tile, g_RISBuffer, g_RISLightDataBuffer, tileData, risBufferPtr);

            // Unpack the light info
            RAB_LightInfo lightInfo;
            uint lightIndex;
            float invSourcePdf;
            RTXDI_UnpackLocalLightFromRISLightData(
                tileData, g_RISLightDataBuffer, risBufferPtr,
                lightInfo, lightIndex, invSourcePdf);

            // Evaluate BRDF with the selected light
            float3 L = normalize(lightInfo.position - input.worldPos);
            float  NdotL = saturate(dot(N, L));

            float3 directDiffuse = 0;
            directSpecular = 0;

            if (NdotL > 1e-5f)
            {
                float dist = length(lightInfo.position - input.worldPos);

                // Optional shadow ray
                float shadow = 1.0;
                if (g_PerFrame.m_EnableRTShadowsTransparent != 0)
                {
                    lightingInputs.L = L;
                    shadow = CalculateRTShadow(lightingInputs, L, dist);
                }

                // Simple radiance (point-light approximation for the candidate)
                // For more accurate light-type-aware evaluation, use
                // RAB_SamplePolymorphicLight() for area/rect/disc lights
                float3 radiance = lightInfo.color * lightInfo.intensity * shadow;

                lightingInputs.L = L;
                PrepareLightingByproducts(lightingInputs);

                LightingComponents di = EvaluateDirectionalLight(
                    lightingInputs, L, radiance);
                directDiffuse  = di.diffuse;
                directSpecular = di.specular;
            }

            color = directDiffuse + directSpecular;
        }
        else
        {
            // Fallback: existing brute-force path
            LightingComponents directLighting = AccumulateDirectLighting(
                lightingInputs, g_PerFrame.m_LightCount);
            float3 directDiffuse = directLighting.diffuse;
            directSpecular = directLighting.specular;

            PrepareLightingByproducts(lightingInputs);

            color = directDiffuse + directSpecular;
        }
    }
```

> **API Verification**: The exact function signatures for `RTXDI_RandomlySelectRISTile`, `RTXDI_RandomlySelectLightDataFromRISTile`, and `RTXDI_UnpackLocalLightFromRISLightData` must be verified against the RTXDI SDK headers in `external/rtxdi/Include/Rtxdi/DI/`. The signatures shown above are based on the documented RTXDI API. Adjust parameter names and types as needed.
>
> Also verify whether `RAB_LightInfo` and `RAB_SamplePolymorphicLight` are accessible from a pixel shader (not just compute). They may need `RAB_` helper functions from the `RtxdiApplicationBridge` headers, which are already included in your codebase. If not available, use the simplified point-light evaluation shown above.
>
> Verify that the `NextFloat(rng)` / `InitRNG` helpers from `Common.hlsli` or `CommonLighting.hlsli` are available in `FORWARD_TRANSPARENT` scope.

---

## Phase 3: Shader — SHARC GI Cache Lookup (~1.5 hours)

### Task 3.1: Implement SHARC Cache Query in Forward_PSMain

**File**: [src/shaders/BasePass.hlsl](src/shaders/BasePass.hlsl), inside the same `#if defined(FORWARD_TRANSPARENT)` block.

**Step A** — Add SHARC indirect lighting after direct lighting, before refraction logic:

Locate the line where `color` is finalized before the refraction block (approximately after the direct lighting `else` block closes). Insert:

```hlsl
    // ==================================================================
    // INDIRECT LIGHTING — SHARC Cache Lookup
    // ==================================================================
    if (g_PerFrame.m_EnableSHARCForTransparent != 0)
    {
        SharcParameters sharcParams = BuildSharcParameters(
            g_SHARCConst, g_SHARCHashEntries,
            g_SHARCResolved, g_SHARCAccumulation);

        SharcHitData hitData;
        hitData.positionWorld = input.worldPos;
        hitData.normalWorld   = normalize(input.normal);  // geometric normal (SHARC convention)

        float3 indirectGI = 0;
        if (SharcGetCachedRadiance(sharcParams, hitData, indirectGI, false))
        {
            // SHARC stores BSDF-modulated exitance at each voxel.
            // Apply primary surface throughput (diffuse albedo × (1 - metallic)).
            float3 throughput = baseColor * (1.0 - metallic);
            color += indirectGI * throughput;
        }
        // Else: cache miss — silently skip. Transparent objects are
        //       primarily direct-lit; zero GI on miss is acceptable.
    }
```

### Task 3.2: Optional — Environment Map Fallback for Cache Miss

For improved quality on cache misses, add after the `if (SharcGetCachedRadiance(...))` block:

```hlsl
        else
        {
            // Fallback: sample prefiltered environment map
            // Cost: ~1 texture sample
            // Uncomment to enable:
            // indirectGI = SampleEnvMap(N, roughness, g_PerFrame.m_RadianceMipCount);
            // color += indirectGI * baseColor * (1.0 - metallic);
        }
```

---

## Phase 4: Polish & Testing (~2 hours)

### Task 4.1: Code Review Checklist

- [ ] Verify all `#include` paths resolve (RTXDI headers may need path adjustment for PS compilation vs CS compilation)
- [ ] Verify generated `srrhi/hlsl/BasePass.hlsli` contains all new `Get*()` methods
- [ ] Verify `BasePassInputs` C++ struct has all new `Set*()` methods
- [ ] Verify `FORWARD_TRANSPARENT` permutation compiles without errors (PS, not CS — RTXDI functions may need PS-specific handling)
- [ ] Verify `RAB_LightInfo`, `RAB_SamplePolymorphicLight`, `RTXDI_*` functions are accessible from pixel shader scope (some RTXDI helpers are CS-only)
- [ ] Verify `SharcPackedData`, `SharcAccumulationData`, `SHARCConstants` types are declared (they come from SharcCommon.h + SHARC.sr)
- [ ] Verify `BuildSharcParameters` exists and compiles

### Task 4.2: Edge Cases

| Edge Case | Expected Behavior | Test |
|-----------|------------------|------|
| No lights in scene | `g_PerFrame.m_LightCount == 0` → skip RIS, `color = 0` | Load empty scene |
| RIS buffer uninitialized (first frame) | RIS tile data is zero → zero radiance → black | Frame 0 test |
| SHARC cache cold (first frames) | `SharcGetCachedRadiance` returns false → zero GI | Cold start |
| RIS disabled + SHARC disabled (both toggles off) | Falls through to existing brute-force path | Default config |
| Sky pixel seen through transparent (depth == FAR) | Existing sky handling applies; SHARC/RTXDI should not execute for sky | Look at sky through glass |
| Back-facing triangles | `dot(N, V) < 0` → `N = -N` (existing code) → correct | Back-face test |
| Multiple transparent layers (alpha-sorted) | Each layer independently evaluates RIS + SHARC → correct | Stacked glass panes |
| Transmission active (`m_TransmissionFactor > 0`) | Refraction logic runs after RIS+SHARC → correct | Glass material |

### Task 4.3: Performance Validation

1. **Profile with brute-force path disabled, RIS enabled**:
   - Scene with 4 lights: RIS should be ~2× faster than brute-force
   - Scene with 100 lights: RIS should be ~10–30× faster
   - Scene with 1 light: RIS may be slightly slower (RIS overhead without benefit)

2. **Profile with SHARC enabled**:
   - Indoor scene: verify SHARC hit rate >80% via debug visualization
   - Outdoor isolated transparent: verify SHARC gracefully returns 0

3. **GPU timer** (use `PROFILE_GPU_SCOPED` around transparent draw calls):
   - Compare: brute-force vs RIS-only vs RIS+SHARC vs brute-force+SHARC

### Task 4.4: Debug Visualizations (Optional, Nice-to-Have)

Add temporary debug overlays behind a debug mode toggle:

1. **RIS tile heatmap**: Color each 16×16 tile block differently
2. **RIS-selected light index**: Encode light index as a hash-color
3. **SHARC cache hit/miss**: Green = hit, red = miss

### Task 4.5: Quality Presets

Add an ImGui combo for quick A/B testing:

```cpp
enum TransparentDirectLightingMode {
    TRANSPARENT_DI_BRUTE_FORCE = 0,  // Original O(N) path
    TRANSPARENT_DI_RIS         = 1,  // RIS-guided single light (new)
};

enum TransparentGIMode {
    TRANSPARENT_GI_NONE        = 0,  // No indirect
    TRANSPARENT_GI_SHARC       = 1,  // SHARC cache lookup (new)
};
```

---

## 5. Files Modified Summary

| File | What Changes |
|------|-------------|
| [src/RTXDIRenderer.cpp](src/RTXDIRenderer.cpp) | Promote `m_RG_RISBuffer` and `m_RG_RISLightDataBuffer` to global scope; rename usages |
| [src/shaders/Common.sr](src/shaders/Common.sr) | Add `RTXDI_TILE_COUNT`, `RTXDI_TILE_SIZE`, `RTXDI_TILE_PIXEL_WIDTH` constants |
| [src/shaders/BasePass.sr](src/shaders/BasePass.sr) | Add 3 new constants to `BasePassConstants`; add 6 new resources to `BasePassInputs` |
| [src/shaders/BasePass.hlsl](src/shaders/BasePass.hlsl) | Add includes, resource declarations, RIS direct-light code, SHARC GI code (in `Forward_PSMain`) |
| [src/BasePassRenderer.cpp](src/BasePassRenderer.cpp) | Add `extern` handles; add `ReadBuffer`/`ReadTexture` in `Setup()`; wire bindings in `Render()`; populate new CB fields |
| [src/Renderer.cpp](src/Renderer.cpp) | Add ImGui toggles; add member variables for `m_EnableReSTIRDI`, `m_EnableSHARCForTransparent`, `m_EnableRTShadowsTransparent` |

---

## 6. Build & Test Sequence

### Step 1: Build plumbing changes (no functional change yet)
```
1. Promote RIS buffers to globals in RTXDIRenderer.cpp
2. Add Common.sr constants
3. Add BasePass.sr constants + resources (no shader code yet)
4. Build → should compile (unused resources may warn)
```

### Step 2: Build shader with includes only (no logic)
```
5. Add #include statements to BasePass.hlsl
6. Add resource declarations
7. Build FORWARD_TRANSPARENT permutation → should compile
```

### Step 3: Add RIS code, test with toggle OFF
```
8. Add RIS direct-light code behind g_PerFrame.m_EnableReSTIRDI check
9. Wire host-side bindings
10. Add ImGui toggles (default OFF)
11. Build & run → verify transparent pass renders identically to before
```

### Step 4: Enable RIS (no shadow), test
```
12. Toggle EnableReSTIRDI ON via ImGui
13. Verify transparent objects receive direct lighting
14. Compare A/B with brute-force path
15. Fix any compilation/linkage issues with RTXDI functions in PS scope
```

### Step 5: Add SHARC GI code, test
```
16. Add SHARC cache lookup code
17. Build & run → verify GI appears on transparent objects (when opaque mode is SHARC or combined)
18. Toggle EnableSHARCForTransparent ON/OFF → verify
```

### Step 6: Polish
```
19. Add shadow ray toggle
20. Add debug visualizations (optional)
21. Profile & optimize
22. Final edge-case testing
```

---

## 7. Known Risks & Mitigations During Implementation

| Risk | Mitigation |
|------|-----------|
| RTXDI functions (`RTXDI_InitRandomSampler`, `RTXDI_RandomlySelectRISTile`, etc.) are CS-only | Check SDK headers for PS-compatible alternatives; fall back to simplified direct buffer reads if needed |
| `RAB_LightInfo` / `RAB_SamplePolymorphicLight` unavailable in PS scope | Use simple point-light evaluation (color × intensity / dist² × shadow) — sufficient for single-candidate case |
| PSO compilation fails due to new bindings | Verify `BasePass.sr` srinput is the one used by the transparent permutation; check descriptor table layout |
| Generated `srrhi` code breaks due to large srinput | srrhi should handle up to ~32 bindings; BasePass currently has 13 |
| `SharcPackedData` type not found in BasePass scope | Ensure `SharcCommon.h` is included BEFORE `srrhi/hlsl/SHARC.hlsli` (required by SHARC.sr extern declarations) |
| Memory/performance regression | All paths gated by runtime toggles (default OFF); zero overhead when disabled |
