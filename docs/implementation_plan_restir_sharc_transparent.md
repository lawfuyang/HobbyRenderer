# RIS-Guided Single Light + SHARC GI for Transparent Objects — Implementation Plan

> **Source**: Derived from [analysis_restir_di_transparent.md](analysis_restir_di_transparent.md)  
> **Total estimated effort**: 6–10 hours  
> **Risk level**: Low — additive changes, guarded by compile-time and runtime toggles  
> **Dependencies**: RTXDI presample passes must run before transparent pass (already true); SHARC Update+Resolve must run before transparent pass (already true in all `IndirectLightingMode` values)

> **SDK Version Context**: The active SHARC SDK is at [`external/SHARC/`](../external/SHARC)
> (v1.8), which uses `hashGridData` naming (not `hashMapData` from older SDK).
> `SharcGetCachedRadiance` uses a `skipResponsiveLighting` (bool) 4th parameter
> instead of the older `debug` parameter. The RTXDI SDK is at
> [`external/rtxdi/`](../external/rtxdi) and the RTXDI reference clone at
> [`REFERENCES/RTXDI/`](../REFERENCES/RTXDI). Note that `RAB_LightInfo` =
> `PolymorphicLightInfo` (a packed 3×uint4 structure); there are no simple
> `.color`/`.intensity`/`.position` fields — use `RAB_SamplePolymorphicLight()`.

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

    // Polymorphic light data (read-only) — needed for non-compact RIS entries
    StructuredBuffer<PolymorphicLightInfo> LightData;

    // SHARC radiance cache (read-only, written by SHARC Update/Resolve)
    RWStructuredBuffer<uint64_t>                SHARCHashEntries;
    RWStructuredBuffer<SharcPackedData>         SHARCResolved;
    RWStructuredBuffer<SharcAccumulationData>   SHARCAccumulation;
    SHARCConstants                              SHARCConst;
```

**What changes for the generated C++ binding**: `BasePassInputs` struct gains `SetRISBuffer()`, `SetRISLightDataBuffer()`, `SetLightData()`, `SetSHARCHashEntries()`, `SetSHARCResolved()`, `SetSHARCAccumulation()`, and `SetSHARCConst()` methods. The generated HLSL bindings (`srrhi/hlsl/BasePass.hlsli`) gain `GetRISBuffer()`, `GetRISLightDataBuffer()`, `GetLightData()`, etc.

> **Note on `PolymorphicLightInfo`**: This struct is already declared as `extern` in
> [`RTXDI.sr`](../../src/shaders/RTXDI.sr). Including `srrhi/hlsl/BasePass.hlsli` in the shader
> will **not** automatically bring `PolymorphicLightInfo` into scope — it must also be forward-declared
> or included. The simplest approach: add `extern PolymorphicLightInfo;` to [`BasePass.sr`](../../src/shaders/BasePass.sr)
> before the `srinput BasePassInputs` block, matching the pattern used in RTXDI.sr.

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

**File**: [src/ImGuiLayer.cpp](src/ImGuiLayer.cpp), in the "Transparent" or "Forward Rendering" UI section

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

Add the corresponding member variables (`m_EnableReSTIRDI`, `m_EnableSHARCForTransparent`, `m_EnableRTShadowsTransparent`) to the `Renderer` class in [`Renderer.h`](../../src/Renderer.h). These are read from ImGuiLayer.cpp via the global `g_Renderer` reference.

---

## Phase 2: Shader — RIS Direct Light (~3 hours)

### Task 2.1: Add RTXDI Include Headers and New Resource Declarations

**File**: [src/shaders/BasePass.hlsl](src/shaders/BasePass.hlsl)

After the existing `#include` block (around line 7), conditionally include RTXDI and SHARC headers:

```hlsl
#if defined(FORWARD_TRANSPARENT)

// SHARC preprocessor defines must precede SharcCommon.h
#define SHARC_ENABLE_64_BIT_ATOMICS     1
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1
#include "SharcCommon.h"          // SHARC SDK: SharcGetCachedRadiance, SharcParameters, etc.
#include "srrhi/hlsl/SHARC.hlsli" // Generated SHARC bindings
#include "SharcHelpers.hlsli"     // BuildSharcParameters helper

// RTXDI RIS — requires RTXDI_RIS_BUFFER global macro definition BEFORE includes.
// In BasePass.hlsl this must point to the RIS buffer bound by BasePassInputs.
#define RTXDI_RIS_BUFFER    g_RISBuffer
#include "Rtxdi/LightSampling/RISBuffer.hlsli"         // RTXDI RIS tile utilities
#include "Rtxdi/LightSampling/LocalLightSelection.hlsli" // RTXDI light candidate selection
#include "Rtxdi/LightSampling/RISBufferSegmentParameters.h"

// RNG for RIS tile random selection
#include "RNG.hlsli"

#endif
```

> **Note**: `#include "Rtxdi/LightSampling/RISBuffer.hlsli"` (not `DI/`) — these headers live
> under [`external/rtxdi/Include/Rtxdi/LightSampling/`](../../external/rtxdi/Include/Rtxdi/LightSampling/).
> The `RTXDI_RIS_BUFFER` macro must be defined before including `RISBuffer.hlsli` because
> `RTXDI_RandomlySelectLightDataFromRISTile` accesses the RIS buffer through this global,
> not through a function parameter. The SDK's application bridge pattern (see
> [RAB_Buffers.hlsli](../../src/shaders/rtxdi/LightingPasses/RtxdiApplicationBridge/RAB_Buffers.hlsli))
> uses the same approach. `SHARC_ENABLE_64_BIT_ATOMICS` and `HASH_GRID_ENABLE_64_BIT_ATOMICS`
> must be defined before `SharcCommon.h` since the transparent pass PS runs at shader model
> 6.0+ where these are not auto-detected.

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

> **Note**: `g_RISBuffer` must be declared as `StructuredBuffer<uint2>` (not `RW`) since
> it will alias the `RTXDI_RIS_BUFFER` macro, which in the BasePass HLSL reads only.
> The actual RIS buffer underlying handle is a `StructuredBuffer<uint2>` using `uint2` entries
> matching the RTXDI SDK convention (`tileData.x` = light index + flags, `tileData.y` = invSourcePdf).
> The `PolymorphicLightInfo` alias `RAB_LightInfo` is typedef'd to the packed
> 3×uint4 structure in [`RAB_LightInfo.hlsli`](../../src/shaders/rtxdi/LightingPasses/RtxdiApplicationBridge/RAB_LightInfo.hlsli) —
> its fields (`.center`, `.colorTypeAndFlags`, etc.) are packed bit-fields, not
> simple `.color` / `.intensity` floats. Use `RAB_SamplePolymorphicLight()` to decode.

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
            // The RTXDI RIS buffer is a StructuredBuffer<uint2> where:
            //   .x = light index | flags  (RTXDI_LIGHT_INDEX_MASK, RTXDI_LIGHT_COMPACT_BIT)
            //   .y = invSourcePdf as float
            //
            // We read tiles directly without the full LightSampling context
            // machinery, which requires the RAB_Buffers.hlsli bridge globals
            // (t_LightDataBuffer, u_RisLightDataBuffer, etc.).
            //
            // 1. Select a RIS tile using tile-space coherent RNG
            uint2 tilePixel = uint2(input.Position.xy) / 16u; // 16×16 pixel tiles
            uint  tileSeed  = tilePixel.x ^ (tilePixel.y << 11) ^ (g_PerFrame.m_FrameIndex * 0x9E3779B9u);
            uint  tileRnd   = PCGHash(tileSeed);
            uint  tileIndex = uint(float(tileRnd) * (1.0f / 4294967296.0f) * RTXDI_TILE_COUNT);
            uint  tileBase  = tileIndex * RTXDI_TILE_SIZE;

            // 2. Draw one random candidate from the selected tile
            RNG rng = InitRNG(uint2(input.Position.xy), g_PerFrame.m_FrameIndex);
            uint  sampleIdx = uint(NextFloat(rng) * RTXDI_TILE_SIZE);
            uint2 tileData  = g_RISBuffer[tileBase + sampleIdx];

            uint  lightIndex    = tileData.x & RTXDI_LIGHT_INDEX_MASK;
            bool  isCompact     = (tileData.x & RTXDI_LIGHT_COMPACT_BIT) != 0;
            float invSourcePdf  = asfloat(tileData.y);

            // 3. Decode light info — use compact path when available
            PolymorphicLightInfo lightInfo;
            if (isCompact)
            {
                uint risPtr = tileBase + sampleIdx;
                uint4 d1 = g_RISLightDataBuffer[risPtr * 2 + 0];
                uint4 d2 = g_RISLightDataBuffer[risPtr * 2 + 1];
                lightInfo = unpackCompactLightInfo(d1, d2);
            }
            else
            {
                lightInfo = g_LightData[lightIndex];
            }

            // 4. Sample the polymorphic light for this surface
            //    RAB_SamplePolymorphicLight handles sphere, rect, disc,
            //    triangle, directional, and environment light types.
            RAB_Surface rabSurface;
            rabSurface.worldPos = input.worldPos;
            rabSurface.normal   = N;
            rabSurface.geoNormal = normalize(input.normal);
            rabSurface.viewDir  = V;
            float2 lightUV = float2(NextFloat(rng), NextFloat(rng));

            RAB_LightSample ls = RAB_SamplePolymorphicLight(lightInfo, rabSurface, lightUV);

            // 5. Evaluate BRDF with the sampled light direction
            float3 L = normalize(ls.position - input.worldPos);
            float  NdotL = saturate(dot(N, L));

            float3 directDiffuse = 0;
            directSpecular = 0;

            if (NdotL > 1e-5f)
            {
                float dist = length(ls.position - input.worldPos);

                // Optional shadow ray
                float shadow = 1.0;
                if (g_PerFrame.m_EnableRTShadowsTransparent != 0)
                {
                    lightingInputs.L = L;
                    shadow = CalculateRTShadow(lightingInputs, L, min(dist, 1e10f));
                }

                // Use the radiance pre-computed by RAB_SamplePolymorphicLight
                float3 radiance = ls.radiance * shadow;

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

> **API verification notes**:
> - `RAB_LightInfo` = `PolymorphicLightInfo` — a tightly packed 3×uint4 struct defined in
>   [`RTXDI.sr`](../../src/shaders/RTXDI.sr). There are no `.color`, `.intensity`, or `.position`
>   float fields. Use `RAB_SamplePolymorphicLight()` or the lower-level helpers in
>   [`PolymorphicLight.hlsli`](../../src/shaders/rtxdi/PolymorphicLight.hlsli).
> - `RTXDI_LIGHT_INDEX_MASK` and `RTXDI_LIGHT_COMPACT_BIT` are defined in the RTXDI SDK
>   (pulled in via `RISBuffer.hlsli` → `RtxdiTypes.h`).
> - `unpackCompactLightInfo()` is defined in `PolymorphicLight.hlsli`, already included by
>   `RAB_LightInfo.hlsli`.
> - `RAB_LightSample` fields: `.position` (float3), `.radiance` (float3), `.solidAnglePdf` (float).
> - The RIS buffer tile size and count match the RTXDI presampling pass constants in
>   [`RTXDIRenderer.cpp`](../../src/RTXDIRenderer.cpp): `k_RISTileSize = 1024`, `k_RISTileCount = 128`.
> - `PCGHash()` and `InitRNG()` / `NextFloat()` are from [`RNG.hlsli`](../../src/shaders/RNG.hlsli).
> - `RAB_SamplePolymorphicLight()` / `RAB_Surface` require including
>   [`RAB_LightInfo.hlsli`](../../src/shaders/rtxdi/LightingPasses/RtxdiApplicationBridge/RAB_LightInfo.hlsli)
>   and its transitive includes (`PolymorphicLight.hlsli`, `RAB_Surface.hlsli`, `RAB_LightSample.hlsli`).
>   `RAB_LightInfo.hlsli` internally uses `u_RisLightDataBuffer` for compact loads — in the
>   transparent PS scope, this macro must be aliased to the BasePass binding.
> - `g_LightData` must be bound as a `StructuredBuffer<PolymorphicLightInfo>` — add to
>   `BasePass.sr` as a new resource (the existing `g_Lights` uses a different `GPULight` struct).
>   See [Task 1.2 additions](#task-12-add-ris-and-sharc-buffer-bindings-to-basepasssr).

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
        // v1.8 SDK: 4th param is 'skipResponsiveLighting' (false = include responsive)
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

- [ ] Verify all `#include` paths resolve: `Rtxdi/LightSampling/RISBuffer.hlsli` (not `DI/`)
- [ ] Verify `RTXDI_RIS_BUFFER` macro is defined before `RISBuffer.hlsli` include
- [ ] Verify `SHARC_ENABLE_64_BIT_ATOMICS=1` and `HASH_GRID_ENABLE_64_BIT_ATOMICS=1` are defined before `SharcCommon.h`
- [ ] Verify `RNG.hlsli` is included (needed for `InitRNG`/`NextFloat`/`PCGHash`)
- [ ] Verify `PolymorphicLightInfo` is declared as `extern` in `BasePass.sr` (or `PolymorphicLight.hlsli` is included)
- [ ] Verify `RAB_LightInfo.hlsli` and its transitive includes (`PolymorphicLight.hlsli`, `RAB_Surface.hlsli`, `RAB_LightSample.hlsli`) are included in `FORWARD_TRANSPARENT` scope
- [ ] Verify `u_RisLightDataBuffer` macro aliases the BasePass `g_RISLightDataBuffer` binding (required by `RAB_LoadCompactLightInfo` in `RAB_LightInfo.hlsli`)
- [ ] Verify generated `srrhi/hlsl/BasePass.hlsli` contains all new `Get*()` methods
- [ ] Verify `BasePassInputs` C++ struct has all new `Set*()` methods
- [ ] Verify `FORWARD_TRANSPARENT` permutation compiles without errors (PS, not CS)
- [ ] Verify `RAB_SamplePolymorphicLight`, `RAB_LightSample`, `RAB_Surface` compile correctly in PS scope
- [ ] Verify `SharcPackedData`, `SharcAccumulationData`, `SHARCConstants` types are declared (from SharcCommon.h + SHARC.sr)
- [ ] Verify `BuildSharcParameters` exists and compiles (from SharcHelpers.hlsli)

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
| [src/shaders/Common.sr](src/shaders/Common.sr) | Add `RTXDI_TILE_COUNT`, `RTXDI_TILE_SIZE` constants; add `extern PolymorphicLightInfo` |
| [src/shaders/BasePass.sr](src/shaders/BasePass.sr) | Add 3 new constants to `BasePassConstants`; add 7 new resources to `BasePassInputs` (RIS, LightData, SHARC) |
| [src/shaders/BasePass.hlsl](src/shaders/BasePass.hlsl) | Add includes (SHARC SDK, RTXDI LightSampling, RNG, RAB bridge), resource declarations, RIS direct-light code, SHARC GI code (in `Forward_PSMain`) |
| [src/BasePassRenderer.cpp](src/BasePassRenderer.cpp) | Add `extern` handles; add `ReadBuffer`/`ReadTexture` in `Setup()`; wire bindings in `Render()`; populate new CB fields |
| [src/ImGuiLayer.cpp](src/ImGuiLayer.cpp) | Add ImGui toggles for `m_EnableReSTIRDI`, `m_EnableSHARCForTransparent`, `m_EnableRTShadowsTransparent` |

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
| `RAB_LightInfo.hlsli` / `RAB_SamplePolymorphicLight` require bridge globals (`u_RisLightDataBuffer`, `t_LightDataBuffer`) not available in BasePass PS scope | Define `#define u_RisLightDataBuffer g_RISLightDataBuffer` and `#define t_LightDataBuffer g_LightData` before including `RAB_LightInfo.hlsli`. Alternatively, use the simplified RIS path that reads the buffer directly without the full bridge include chain — see the compact light decode code path above. |
| `PolymorphicLightInfo` struct not recognized in BasePass.hlsl (defined in RTXDI.sr, not BasePass.sr) | Add `extern PolymorphicLightInfo;` to `BasePass.sr` before the `srinput` block. This tells srrhi to use the existing global type from `RTXDI.sr`. |
| PSO compilation fails due to new bindings | Verify `BasePass.sr` srinput is the one used by the transparent permutation; BasePass currently has 13 bindings, adding ~7 more stays well within limits. Check descriptor table layout. |
| Generated `srrhi` code breaks due to large srinput | srrhi should handle up to ~32 bindings; BasePass currently has 13. The 7 additions are well within budget. |
| `SharcCommon.h` fails to compile in PS because `SHARC_ENABLE_64_BIT_ATOMICS` auto-detection doesn't trigger for PS shader models | Explicitly define `SHARC_ENABLE_64_BIT_ATOMICS=1` and `HASH_GRID_ENABLE_64_BIT_ATOMICS=1` before the `#include` (already done in the corrected includes above). |
| Memory/performance regression | All paths gated by runtime toggles (default OFF); zero overhead when disabled. RIS path reads 1-2 buffer elements per pixel (O(1)), vs original O(N) loop over lights. |
