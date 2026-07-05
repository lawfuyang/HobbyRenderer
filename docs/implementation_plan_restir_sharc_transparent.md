# Translucency Lighting Volume (TLV) for ReSTIR Direct Light + SHARC GI — Architecture Analysis

> **Status**: Analysis document — no implementation plan yet.
> **Approach**: World-space 3D volume grid baked from ReSTIR DI RIS samples + SHARC radiance cache, sampled in the transparent forward pass.
> **Inspiration**: UE5 MegaLights Translucency Lighting Volume (TLV) — but extended with SHARC indirect lighting (unique contribution).
> **SDK Versions**: SHARC v1.8 at [`external/SHARC/`](../external/SHARC), RTXDI SDK at [`external/rtxdi/`](../external/rtxdi).

---

## 1. Executive Summary

This document proposes replacing the current brute-force O(N) direct lighting in the transparent forward pass with a **Translucency Lighting Volume (TLV)** — a world-space 3D volume grid that bakes both **ReSTIR DI stochastic direct lighting** and **SHARC indirect (GI) radiance** into spatial cells. Transparent objects sample this volume by world position in the forward pixel shader, achieving:

- **O(1) lighting cost** per transparent pixel regardless of light count (vs. current O(N) brute-force loop)
- **Stochastic many-light quality** for transparent surfaces — a capability currently absent
- **Indirect/GI lighting** for transparent surfaces — a capability no mainstream engine ships
- **Low noise** — volume data is temporally and spatially smooth (unlike per-pixel RIS which is inherently noisy)
- **No depth discrepancy** — world-space sampling is accurate regardless of transparent surface depth

This is architecturally modeled after UE5 MegaLights' Translucency Lighting Volume, but extended with SHARC indirect radiance caching — a genuine innovation since UE5's TLV handles direct lighting only.

---

## 2. How It Works — Architecture Overview

### 2.1 Frame-Level Pipeline

The TLV is injected and resolved between the existing RTXDI and SHARC passes and the transparent forward pass:

```
Frame N:
  ClearRenderer → TLASRenderer → OpaqueRenderer → MaskedPassRenderer
  → HZBGeneratorPhase2
  → RTXDIRenderer          ← ReSTIR DI presamples build RIS buffers
  → SHARCRenderer           ← SHARC Update + Resolve populate radiance cache
  → [NEW] TLVInjectionPass  ← Compute: inject RIS samples + SHARC radiance into 3D volume
  → [NEW] TLVResolvePass    ← Compute: temporal blend + stale cell eviction
  → DeferredRenderer        ← Uses RTXDI+SHARC for opaque deferred lighting
  → SkyRenderer
  → TransparentPassRenderer ← Forward PS: trilinearly samples TLV for direct+indirect
  → TAARenderer → BloomRenderer → HDRRenderer → ImGuiRenderer
```

**Key insight**: The RTXDI presample passes and SHARC Update+Resolve already run for opaque geometry. The TLV injection pass is a small additional compute dispatch that reads their already-produced buffers and injects into the volume. No new ray tracing passes are needed.

### 2.2 TLV Injection Pass (New Compute Shader)

**Inputs** (already produced by RTXDI + SHARC):
- `g_RG_RISBuffer` — 128 tiles × 1024 candidates of light indices + weights (StructuredBuffer<uint2>)
- `g_RG_RISLightDataBuffer` — compact light info per RIS entry (StructuredBuffer<uint4>)
- `g_LightData` / `g_Lights` — full light buffer (PolymorphicLightInfo / GPULight)
- `g_RG_SHARCResolved` — SHARC resolved radiance cache (StructuredBuffer<SharcPackedData>)
- `g_RG_SHARCHashEntries` — SHARC hash entries (StructuredBuffer<uint64_t>)

**Outputs** (new, appended to render graph):
- `g_RG_TLVDirectLighting` — RGBA16F 3D texture, 64³ (2 MB)
- `g_RG_TLVIndirectLighting` — RGBA16F 3D texture, 64³ (2 MB)

**Algorithm** — per-cell compute dispatch (64³ = 262,144 cells, ~1024 thread groups of 256):

```
For each volume cell:
  1. Compute cell world-space center from cell index
  2. Sample RIS tiles:
     a. Select a RIS tile randomly based on cell spatial hash
     b. Draw M candidate lights from the tile (M = 4–16)
     c. For each candidate: evaluate light contribution at cell center
     d. Average into direct lighting value
  3. Query SHARC cache at cell center:
     a. HashGridFind at cell center with geometric "up" normal
     b. SharcGetVoxelData → accumulateSampleNum > threshold?
     c. If hit: decode radiance, write to indirect cell
     d. If miss: write 0 (temporal blending will fill over time)
  4. Atomic-add results to 3D texture UAV
```

Alternatively, a **simpler injection strategy** that avoids per-cell RIS:
- Use the screen-space RTXDI DI composited output (`g_RG_RTXDIDIComposited`) 
- Reproject each pixel's direct lighting into the TLV via world position
- This reuses the already-composited opaque direct lighting result — zero extra light evaluation

### 2.3 TLV Resolve Pass (New Compute Shader)

Temporal blending per cell, identical in spirit to SHARC's own Resolve pass:

```
For each volume cell:
  1. Read current frame's direct_curr, indirect_curr from injection textures
  2. Read previous frame's direct_prev, indirect_prev from persistent textures
  3. EMA blend: direct_new = lerp(direct_prev, direct_curr, alpha)
     indirect_new = lerp(indirect_prev, indirect_curr, alpha)
     where alpha = 1.0 / min(frameCount, MAX_ACCUMULATION_FRAMES)
  4. Stale frame tracking: if a cell received zero new samples this frame,
     increment stale counter; if stale > STALE_THRESHOLD, fade to black
  5. Write blended results to persistent resolved volume textures
```

### 2.4 Transparent Forward Pass Sampling

In `BasePass.hlsl` `Forward_PSMain`, replace the current brute-force `AccumulateDirectLighting` call:

```hlsl
// ── TLV Direct Lighting ──────────────────────────────────
float3 tlvDirect = g_TLVResolvedDirect.SampleLevel(
    trilinearSampler, worldPosToTLVUv(input.worldPos), 0).rgb;

// ── TLV Indirect Lighting ────────────────────────────────
float3 tlvIndirect = g_TLVResolvedIndirect.SampleLevel(
    trilinearSampler, worldPosToTLVUv(input.worldPos), 0).rgb;

// Apply BRDF modulation for indirect (SHARC stores BSDF-modulated exitance)
float3 throughput = baseColor * (1.0 - metallic);
float3 color = tlvDirect + tlvIndirect * throughput;
```

**Cost**: 2 trilinear texture samples per transparent pixel. This replaces the current O(N) brute-force loop that evaluates N lights with per-light BRDF, distance attenuation, spot cone tests, and optional RT shadow rays.

---

## 3. GPU Resource Analysis

### 3.1 Memory Budget

| Resource | Format | Resolution | Size | Notes |
|---|---|---|---|---|
| `g_RG_TLVInjectionDirect` | RGBA16F | 64³ × 8 B | 2 MB | Current-frame direct injection |
| `g_RG_TLVInjectionIndirect` | RGBA16F | 64³ × 8 B | 2 MB | Current-frame indirect injection |
| `g_RG_TLVResolvedDirect` | RGBA16F | 64³ × 8 B | 2 MB | Temporally blended direct (persistent) |
| `g_RG_TLVResolvedIndirect` | RGBA16F | 64³ × 8 B | 2 MB | Temporally blended indirect (persistent) |
| `g_RG_TLVStaleCounters` | R16UI | 64³ × 2 B | 0.5 MB | Stale frame counters per cell |
| **Total TLV** | | | **8.5 MB** | |

Comparison with existing SHARC memory:
- SHARC hash entries (2^22 × 8 B): **32 MB**
- SHARC accumulation (2^22 × 16 B): **64 MB**
- SHARC resolved (2^22 × 16 B): **64 MB**
- SHARC total: **160 MB**

**TLV at 8.5 MB is negligible compared to the existing SHARC cache (160 MB).** Even at 128³ (×8 cells = 68 MB), the TLV would still be modest.

### 3.2 Volume Resolution Trade-offs

| Resolution | Cells | Memory (4×RGBA16F) | Spatial Precision (at 100m scene) | Injection Cost |
|---|---|---|---|---|
| 32³ | 32,768 | 1 MB | 3.1 m/cell | Very cheap |
| 64³ | 262,144 | 8.5 MB | 1.6 m/cell | Cheap (~1024 thread groups) |
| 128³ | 2,097,152 | 68 MB | 0.8 m/cell | Moderate (~8192 thread groups) |
| 256³ | 16,777,216 | 544 MB | 0.4 m/cell | Expensive |

**Recommendation**: 64³ is the sweet spot. At a 100m scene extent, each cell covers ~1.6m³ — sufficient for smooth lighting transitions on glass surfaces. Higher resolutions improve shadow detail fidelity but cost significantly more injection compute.

### 3.3 Bandwidth Analysis

**TLV Injection Pass** (64³ cells, 256 threads/group = 1024 groups):
- Reads: RIS buffer samples (~1 KB), SHARC cache probes (~64 KB)
- Writes: 2 × RGBA16F × 64³ = 512 KB (atomic)
- Total bandwidth: ~1–2 MB per frame — trivial

**TLV Resolve Pass** (64³ cells):
- Reads: 2 × RGBA16F injection (512 KB) + 2 × RGBA16F previous resolved (512 KB) + stale counters (128 KB)
- Writes: 2 × RGBA16F new resolved (512 KB) + stale counters (128 KB)
- Total bandwidth: ~2 MB per frame — trivial

**Transparent Forward Pass Sampling**:
- 2 trilinear samples per pixel (8 taps each = 16 total texture reads)
- At 1920×1080: ~2M pixels × 16 taps × 8 bytes = ~256 MB
- This replaces the current O(N) brute-force loop which reads N × GPULight structs from memory

---

## 4. How the Forward Lighting Renderer & Shader Change

### 4.1 Host-Side Changes (BasePassRenderer.cpp)

**TransparentPassRenderer::Setup()** — declare TLV resources:
```cpp
// Declare injection textures (written by TLV injection compute pass)
renderGraph.DeclareTexture(tlvDirectInjectionDesc, g_RG_TLVInjectionDirect);
renderGraph.DeclareTexture(tlvIndirectInjectionDesc, g_RG_TLVInjectionIndirect);

// Declare resolved textures (persistent, read by transparent pass)
renderGraph.DeclarePersistentTexture(tlvResolvedDirectDesc, g_RG_TLVResolvedDirect);
renderGraph.DeclarePersistentTexture(tlvResolvedIndirectDesc, g_RG_TLVResolvedIndirect);

// Read resolved volumes in transparent pass
renderGraph.ReadTexture(g_RG_TLVResolvedDirect);
renderGraph.ReadTexture(g_RG_TLVResolvedIndirect);
```

**TransparentPassRenderer::Render()** — bind TLV textures:
```cpp
inputs.SetTLVDirect(renderGraph.GetTexture(g_RG_TLVResolvedDirect, Read));
inputs.SetTLVIndirect(renderGraph.GetTexture(g_RG_TLVResolvedIndirect, Read));
```

### 4.2 Shader-Side Changes (BasePass.hlsl)

**Minimal changes**. The only modification to `Forward_PSMain` is replacing the brute-force direct lighting block (lines ~401–419) with TLV sampling:

```hlsl
// BEFORE (current code):
LightingComponents directLighting = AccumulateDirectLighting(
    lightingInputs, g_PerFrame.m_LightCount);
float3 directDiffuse = directLighting.diffuse;
directSpecular = directLighting.specular;
color = directDiffuse + directSpecular;

// AFTER (TLV approach):
float3 uvw = WorldPosToTLVUv(input.worldPos, g_PerFrame.m_TLVOrigin, g_PerFrame.m_TLVExtent);
float3 tlvDirect = g_TLVResolvedDirect.SampleLevel(g_SamplerTrilinearClamp, uvw, 0).rgb;
float3 tlvIndirect = g_TLVResolvedIndirect.SampleLevel(g_SamplerTrilinearClamp, uvw, 0).rgb;

// Apply indirect throughput (SHARC stores BSDF-modulated exitance)
float3 indirectGI = tlvIndirect * baseColor * (1.0 - metallic);

color = tlvDirect + indirectGI;
```

**What stays the same**: Sun/sky handling, `RENDERING_MODE_IBL` path, refraction logic, debug visualization, emission — all unchanged.

**What's removed**: The entire brute-force `AccumulateDirectLighting` call and its O(N) per-pixel loop.

**BasePass.sr changes**: Add 2 new texture bindings (`TLVResolvedDirect`, `TLVResolvedIndirect`) and 2 new CB fields (`m_TLVOrigin`, `m_TLVExtent`). Total: +4 lines.

### 4.3 New Renderer: TLVInjectionRenderer

A new small renderer class (~200 lines) registered before `TransparentPassRenderer`:

```cpp
class TLVInjectionRenderer : public IRenderer {
    bool Setup(RenderGraph& rg) override {
        // Read RIS buffers (from RTXDIRenderer)
        rg.ReadBuffer(g_RG_RISBuffer);
        rg.ReadBuffer(g_RG_RISLightDataBuffer);
        rg.ReadBuffer(g_RG_RTXDILightReservoirBuffer);  // optional: for reservoir-guided injection
        // Read SHARC resolved (from SHARCRenderer)
        rg.ReadBuffer(g_RG_SHARCHashEntries);
        rg.ReadBuffer(g_RG_SHARCResolved);
        // Write injection volumes
        rg.WriteTexture(g_RG_TLVInjectionDirect);
        rg.WriteTexture(g_RG_TLVInjectionIndirect);
        // Read resolved volumes for temporal blending
        rg.ReadTexture(g_RG_TLVResolvedDirect);   // previous frame
        rg.ReadTexture(g_RG_TLVResolvedIndirect);
        // Write new resolved
        rg.WriteTexture(g_RG_TLVResolvedDirect);   // persistent
        rg.WriteTexture(g_RG_TLVResolvedIndirect);
    }
    void Render(...) override {
        // Dispatch compute: 64³ / 256 = 1024 groups
        // Injection shader reads RIS + SHARC, writes injection + resolved
    }
};
```

---

## 5. Image Quality Analysis

### 5.1 Strengths

| Quality Aspect | TLV Approach | Current Brute-Force |
|---|---|---|
| **Light count** | Scales to hundreds/thousands of lights — limited only by volume resolution, not per-pixel cost | Capped at scene light count; O(N) becomes prohibitive beyond ~20 lights |
| **Stochastic quality** | RIS importance sampling across all lights — correct energy distribution | Nearest-N brute force — biased toward nearest lights, misses distant important lights |
| **Indirect lighting** | SHARC 2–4 bounce diffuse GI included | None — transparents are direct-lit only |
| **Noise** | Very low — volume data is temporally blended across 30–60 frames | None (deterministic) — but wrong (biased) for many-light scenes |
| **Temporal stability** | Excellent — EMA blending, no flickering | Deterministic (same lights every frame) but can pop when lights enter/leave the capped-N list |
| **Spatial coherence** | Smooth trilinear interpolation between cells | Per-pixel independent evaluation — coherent by nature of light grid |

### 5.2 Weaknesses & Mitigations

| Weakness | Severity | Mitigation |
|---|---|---|
| **Lag on camera movement** — TLV needs 2–4 frames to repopulate after a cut or fast pan | Medium | Use the brute-force fallback for the first N frames after a discontinuity; fade TLV in gradually |
| **Shadow detail loss** — 1.6m³ cells cannot capture fine shadow boundaries | Medium-High | This is the fundamental TLV trade-off. For critical transparent objects (hero glass), optionally trace 1 RT shadow ray for the dominant light direction derived from the TLV. For most objects, the temporally smoothed TLV shadow approximation is acceptable. |
| **Volume edge artifacts** — cells at the volume boundary may have incorrect lighting | Low | Clamp UV to [0.01, 0.99]; extend volume extent 10% beyond scene bounds; fade to black at edges |
| **Multiple bounce transparency** — light passing through one transparent object to another | Low | TLV at world position handles this naturally — the volume cell holds the aggregate lighting at that 3D point. However, it does not model shadowing between transparent layers (which the brute-force path also doesn't). |
| **Color bleeding from opaque to transparent** — if injection uses opaque-surface composited radiance, transparents may inherit opaque albedo modulation | Low | Use light-sample injection (evaluate light at cell center) rather than composited-screen reprojection. Or demodulate albedo before injection. |
| **SHARC cache misses in empty regions** — outdoor transparent objects far from geometry | Medium | SHARC caches radiance at diffuse surface hits. Volumes in open air will have no cache entries. The indirect TLV cell will stay black. Acceptable since open-air transparents primarily need direct lighting. Optionally blend in an environment probe sample. |

### 5.3 Comparison: TLV vs. RIS-in-Forward-PS

| Aspect | TLV (this plan) | RIS-in-PS (previous plan, deleted) |
|---|---|---|
| **Per-pixel cost** | 2 trilinear texture samples (~16 taps) | RIS buffer read + polymorphic light decode + BRDF evaluation + optional RT shadow ray |
| **Noise** | Very low (temporally accumulated volume) | Moderate (per-pixel RIS is stochastic) |
| **Depth discrepancy** | None — world-space volume | **Fundamental problem** — RIS tiles built for opaque depth behind transparents |
| **Multiple transparent layers** | Consistent — all layers sample the same volume | Inconsistent — each layer independently draws RIS samples |
| **Indirect lighting** | SHARC GI baked into volume | SHARC hash grid query per pixel (works but adds cost) |
| **Memory** | ~8.5 MB (64³) | ~256 KB (RIS buffer reuse) |
| **Complexity** | New compute passes + volume management | ~100 lines of shader code in forward PS |

---

## 6. Performance Analysis

### 6.1 Transparent Forward Pass Cost

**Current brute-force path** (per transparent pixel):
- Loop over N lights: per-light branch (directional/point/spot), distance check, spot cone check, distance attenuation math, BRDF evaluation, optional RT shadow ray
- At 4 lights: ~50 ALU + 1–4 RT traversals
- At 64 lights: ~800 ALU + 1–64 RT traversals ← becomes GPU-limited

**TLV path** (per transparent pixel):
- 2 trilinear samples: 16 texture reads total
- 1 world-pos-to-UV conversion: ~5 ALU
- 1 indirect throughput multiply: ~3 ALU
- **Total: ~24 ALU, 0 RT traversals** ← constant regardless of light count

**Performance ratio**:
- At 1 light: brute-force ≈ TLV (neither is expensive)
- At 4 lights: TLV ≈ 1.2× faster (marginally)
- At 16 lights: TLV ≈ 3–5× faster
- At 64+ lights: TLV ≈ 10–30× faster

The TLV makes transparent rendering performance **independent of scene light count**.

### 6.2 TLV Injection Pass Cost

- 64³ cells × 4 light samples per cell × BRDF evaluation
- ~262K × 4 × ~20 ALU = ~21M ALU operations
- 1024 thread groups × 256 threads ≈ well within budget for a modern GPU
- Equivalent cost: roughly the same as rendering one fullscreen compute pass
- **Estimated GPU time: 0.1–0.3 ms on RTX 3060+ class hardware**

### 6.3 TLV Resolve Pass Cost

- 64³ cells × simple EMA blend
- ~262K × ~10 ALU = ~2.6M ALU
- Negligible (< 0.05 ms)

### 6.4 Total Overhead

| Component | GPU Time Estimate |
|---|---|
| TLV Injection (compute) | 0.1–0.3 ms |
| TLV Resolve (compute) | < 0.05 ms |
| Transparent forward pass speedup | **varies: 1×–30× faster depending on light count** |
| Additional memory | 8.5 MB (negligible vs. SHARC's 160 MB) |

**Net effect**: For scenes with >4 lights, the TLV approach is a performance win. For scenes with ≤4 lights, the performance is essentially identical (the brute-force path is already cheap at low N).

---

## 7. Comparison with Industry Standards

### 7.1 UE5 MegaLights TLV

UE5 MegaLights uses a Translucency Lighting Volume that is architecturally identical in concept:

| Aspect | UE5 MegaLights TLV | HobbyRenderer TLV (this plan) |
|---|---|---|
| **Light sampling source** | ReGIR (3D grid reservoirs) | RTXDI RIS (2D screen tiles) |
| **Injection mechanism** | ReGIR cell samples → volume grid | RIS tile samples → volume grid (or composited-screen reprojection) |
| **Volume resolution** | Not publicly documented (likely 64³–128³) | 64³ (configurable) |
| **Temporal blending** | Yes (exponential moving average) | Yes (EMA, 30–60 frame window) |
| **Direct lighting** | ✅ MegaLights stochastic direct light | ✅ ReSTIR DI stochastic direct light |
| **Indirect lighting** | ❌ **Not provided** (Lumen is opaque-only) | ✅ **SHARC GI baked into separate indirect volume** |
| **Production status** | Production-Ready (UE 5.8, June 2026) | Analysis phase |

**Key differentiator**: The HobbyRenderer TLV extends the concept with SHARC indirect radiance caching. UE5's TLV currently only handles direct lighting — transparent objects in UE5 receive no GI. This is a genuine innovation opportunity.

### 7.2 Other Engines

| Engine | Transparent Lighting | TLV-equivalent? |
|---|---|---|
| **Unity HDRP** | Forward clustered light grid, capped N lights | No |
| **Cyberpunk 2077 (REDengine)** | Forward pass, limited light list, no RIS/ReSTIR | No |
| **Alan Wake 2 (Northlight)** | Forward pass, nearest lights + probe | No (probe-based, not volume-baked stochastic) |
| **Frostbite (Battlefield)** | Forward+ with clustered light culling | No |
| **id Tech 7 (Doom Eternal)** | Forward pass, light grid, no stochastic sampling | No |

**Conclusion**: The TLV approach — baking stochastic light samples into a world-space volume for transparent forward-pass sampling — is currently only available in UE5 MegaLights. The HobbyRenderer plan matches the industry gold standard for direct lighting while adding indirect lighting, which no engine currently provides.

### 7.3 Online Games & Real-Time Applications

Most shipped games handle transparent lighting conservatively:
- Glass windows: environment map + single dominant directional light
- Particle effects: unlit or emissive-only
- Water surfaces: planar reflections + single sun
- Holograms/HUD: emissive

Stochastic transparent lighting is not yet common in shipped games. UE5 MegaLights is the first engine feature to make it accessible, and adoption is still early. The HobbyRenderer TLV would be at the leading edge of shipped-game capability.

---

## 8. Pros & Cons

### 8.1 Advantages

1. **Performance independence from light count** — transparent rendering cost is O(1) regardless of scene lights. A scene with 1000 ReSTIR-sampled lights renders transparent objects at the same cost as a scene with 4 lights.

2. **Stochastic many-light quality** — transparent surfaces receive ReSTIR DI-quality direct lighting. Light selection is RIS-driven (importance-sampled), not nearest-N biased.

3. **Indirect lighting on transparent surfaces** — SHARC GI baked into the TLV provides 2–4 bounce diffuse indirect. No other engine (including UE5 MegaLights) provides indirect/GI for transparent surfaces.

4. **Temporal stability** — volume data is inherently temporally accumulated (EMA), eliminating flicker and noise that would plague per-pixel stochastic approaches.

5. **No depth discrepancy** — world-space volume lookup is correct regardless of where the transparent surface is in depth. The fundamental flaw of the RIS-in-PS approach (RIS tiles built from opaque depth) is completely avoided.

6. **Consistent multi-layer lighting** — all transparent layers sample the same volume, producing coherent lighting rather than each layer independently drawing different RIS samples.

7. **Minimal forward shader changes** — the transparent pixel shader changes from an O(N) loop to 2 trilinear texture samples. The shader becomes simpler, not more complex.

8. **Synergy with existing infrastructure** — the TLV injection pass reuses RTXDI RIS buffers and SHARC resolved buffers that are already produced for opaque rendering. Zero new ray tracing passes.

### 8.2 Disadvantages

1. **New infrastructure required** — the TLV injection and resolve compute passes, volume texture management, and persistent resource handling are new code (~300–500 lines C++, ~200 lines HLSL). This is more total code than the RIS-in-PS approach (~100 lines shader).

2. **Lag on discontinuous camera movement** — after a camera cut or fast pan, the TLV may need 2–4 frames to repopulate. Mitigation: fade-in or fallback to brute-force for N frames.

3. **Spatial resolution trade-off** — 64³ cells at ~1.6m/cell cannot capture fine shadow detail. High-frequency shadow boundaries are smoothed out by the volume. Acceptable for most transparent objects; critical cases can add 1 optional RT shadow ray.

4. **Volume extent limits** — the TLV covers a finite world-space region. Transparent objects outside the volume receive zero lighting. Mitigation: extend volume 10–20% beyond the camera frustum far plane; fade to environment at boundaries.

5. **SHARC dependence for indirect** — indirect lighting in the TLV is only as good as the SHARC cache. In scenes where SHARC has low hit rates (open outdoor areas, newly visible regions), the indirect volume will be dark. Acceptable — transparents in open areas primarily need direct light, which the TLV provides.

6. **Memory overhead** — 8.5 MB at 64³ is modest, but engines targeting mobile/Switch may find even this significant. Mitigation: 32³ at 1 MB is viable for mobile.

7. **Not a drop-in replacement** — unlike RIS-in-PS which modified a single shader, TLV requires new render passes, new renderers, and pipeline ordering changes. Higher integration cost.

---

## 9. Risk Analysis

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| TLV injection compute pass is too expensive | Low | Medium | Reduce samples-per-cell (4→2); reduce volume resolution (64³→32³) |
| SHARC indirect cache misses make indirect volume mostly dark | Medium | Low | Blend in environment map sample for outdoor regions; acceptable for most scenes |
| Volume boundary artifacts visible on large transparent objects | Medium | Medium | Clamp UV to [0.02, 0.98]; fade to environment at edges; extend volume bounds |
| Temporal lag noticeable on fast camera pans | Medium | Medium | Reduce EMA alpha (faster response); fallback to brute-force for first 2 frames after large camera delta |
| Render graph ordering conflicts with existing RTXDI/SHARC passes | Low | High | TLV injection runs after RTXDI+SHARC but before Deferred+Transparent — this ordering already exists in the pipeline |
| 3D texture UAV support on target hardware | Low | High | RGBA16F 3D textures with UAV access require DX12 FL 12.0+ / Vulkan 1.2+. Already the minimum spec for RTXDI+SHARC. |

---

## 10. Open Questions

1. **Injection strategy**: Should TLV injection use RIS tile sampling per cell (more accurate, more expensive) or screen-space composited reprojection (cheaper, reuses existing result, but inherits albedo modulation from opaque surfaces)?

2. **Volume coordinate space**: Camera-relative (moves with camera, no repopulation on translation) or world-absolute (fixed, requires repopulation on camera movement beyond extent)? Camera-relative with a large enough extent is recommended.

3. **Multiple volumes**: One large volume or a cascaded set (like CSM cascades)? One volume at 64³ is likely sufficient for the transparent use case.

4. **Separate direct vs. indirect volumes**: The plan uses two separate 3D textures. Could combine into one RGBA16F (direct RGB + indirect intensity monochrome) to save memory, at the cost of lower indirect quality. Worth profiling both.

5. **Fallback behavior**: Should the TLV path be runtime-toggleable (like the RIS-in-PS plan), or should it fully replace the brute-force path? Toggle is safer for incremental integration.

6. **Light type support**: RIS tiles contain polymorphic lights (sphere, rect, disc, triangle, directional, environment). TLV injection at cell centers can handle all types via `RAB_SamplePolymorphicLight`. The forward pass only needs the resulting radiance, not the light type — type opacity is a key TLV advantage.

---

## 11. Summary

The Translucency Lighting Volume approach is the **architecturally correct solution** for bringing stochastic many-light quality and indirect GI to transparent surfaces. It aligns with the industry direction set by UE5 MegaLights while extending it with SHARC indirect lighting — a capability no engine currently ships.

**Key trade-off**: More infrastructure code (~500 lines) vs. the RIS-in-PS approach (~100 lines), but dramatically better image quality (no noise, no depth discrepancy, indirect lighting, consistent multi-layer results) and better performance scaling (O(1) vs. O(N) for brute-force, no per-pixel RNG overhead vs. RIS-in-PS).

**Recommended path**: Implement the TLV with a runtime toggle. Keep the existing brute-force path as the default until the TLV is validated. This mirrors the safe integration strategy from the original RIS-in-PS plan while targeting a superior architecture.
