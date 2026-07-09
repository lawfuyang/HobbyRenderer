# Screen-Space Shadows — Codebase Analysis & Integration Plan

> **Phase:** SS Shadows (Screen-Space Contact Shadows) — complement to CSM
> **Goal:** Add compute-shader-based screen-space ray marching shadows to recover small-scale contact details lost by CSM bias/resolution
> **Prerequisite:** CSM Phase (NormalBasic mode must already be running with CSM shadow mask)
> **Constraint:** No HWRT — compute shaders only

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Current Codebase State](#2-current-codebase-state)
3. [SS Shadows Technique — Deep Dive](#3-ss-shadows-technique--deep-dive)
4. [Renderer Design](#4-renderer-design)
5. [Integration Points](#5-integration-points)
6. [Performance Analysis](#6-performance-analysis)
7. [Pros and Cons](#7-pros-and-cons)
8. [Feasibility Assessment](#8-feasibility-assessment)
9. [Implementation Roadmap](#9-implementation-roadmap)
- [Appendix A: File Index](#appendix-a-file-index)
- [Appendix B: Key Design Decisions](#appendix-b-key-design-decisions)
- [Appendix C: Reference Implementation (raymarch.hlsl)](#appendix-c-reference-implementation-raymarchhlsl)

---

## 1. Executive Summary

Screen-space shadows (SS shadows, also called **contact shadows**) are a compute-shader-based ray marching technique that traces short rays through the depth buffer toward the light. They complement CSM by recovering fine-scale contact details that shadow maps lose due to bias and limited texel resolution.

**In the HobbyRenderer codebase**, this is viable because:
- The depth buffer, GBuffer normals, camera matrices, and light direction are all already available
- The render graph architecture makes adding a new compute pass straightforward
- The existing `raymarch.hlsl` reference provides a production-tested depth-buffer ray marcher

**SS shadows are NOT a replacement for CSM** — they are strictly a complement. CSM provides large-scale directional shadows; SS shadows fill in the small contact details near occluder-receiver boundaries.

---

## 2. Current Codebase State

### 2.1 What Already Exists (Assets SS Shadows Can Leverage)

| Resource | Handle/Variable | Format | Status |
|---|---|---|---|
| **Depth buffer** | `g_RG_DepthTexture` | `D32_FLOAT` (reversed-Z) | ✅ Available after Opaque + Masked passes |
| **GBuffer Normals** | `g_RG_GBufferNormals` | `R16G16_FLOAT` | ✅ Written by Opaque + Masked passes |
| **HDR Color** | `g_RG_HDRColor` | `R16G16B16A16_FLOAT` | ✅ Written by DeferredRenderer |
| **Camera matrices** | `PlanarViewConstants` | N/A (CB) | ✅ Available in deferred lighting CB |
| **Sun direction** | `DeferredLightingConstants.m_SunDirection` | N/A (CB) | ✅ Available in deferred lighting CB |
| **Compute pass infra** | `AddComputePass()` | N/A | ✅ Proven pattern (SHARC, RTXDI, etc.) |
| **Bindless textures** | Global descriptor tables | N/A | ✅ Available to any shader |
| **CSM shadow map** | `g_RG_CSMShadowMap` (planned) | `D32_FLOAT` array | ⚠️ **Planned** — CSM must be implemented first |
| **Shadow mask** | `g_RG_ShadowMask` (planned) | `R8_UNORM` | ⚠️ **Planned** — CSM must be implemented first |
| **NormalBasic mode** | `RenderingMode::NormalBasic` (planned) | N/A | ⚠️ **Planned** — CSM phase adds this |

### 2.2 What Does NOT Yet Exist (CSM Phase Must Deliver First)

SS shadows depend on the CSM phase being completed:

1. `RenderingMode::NormalBasic` enum value in [Renderer.h](../src/Renderer.h) and [Common.sr](../src/shaders/Common.sr)
2. `NormalBasic` branch in `ScheduleAndRunAllRenderers()` in [Renderer.cpp](../src/Renderer.cpp)
3. CSM shadow map array (`g_RG_CSMShadowMap`) — depth-only raster pass
4. Shadow mask RT (`g_RG_ShadowMask`) — `R8_UNORM`, screen resolution, written by `ShadowMaskRenderer`
5. Deferred lighting shader reading `g_ShadowMask` instead of firing inline ray queries

### 2.3 Current Render Pass Order (Normal Mode)

```
ClearRenderer → TLASRenderer → OpaqueRenderer → MaskedPassRenderer
→ RTXDIRenderer → SHARCRenderer
→ DeferredRenderer → SkyRenderer → TransparentPassRenderer
→ TAARenderer → BloomRenderer → HDRRenderer → ImGuiRenderer
```

### 2.4 Planned Render Pass Order (NormalBasic Mode — CSM Phase)

```
ClearRenderer → OpaqueRenderer → MaskedPassRenderer
→ ShadowRenderer → ShadowMaskRenderer
→ DDGIRenderer → DeferredRenderer → SkyRenderer
→ TransparentPassRenderer → TAARenderer → BloomRenderer
→ HDRRenderer → ImGuiRenderer
```

---

## 3. SS Shadows Technique — Deep Dive

### 3.1 Algorithm Overview

For every pixel on screen:

1. Reconstruct world-space position from depth buffer
2. Shoot a ray from the world position **toward the light** (opposite direction for a directional sun)
3. March along the ray in clip space (or world space), taking N steps
4. At each step, project the sample point back to screen space
5. Compare the ray's depth against the scene depth buffer
6. If `sceneDepth < rayDepth - thickness`, an occluder is between the camera and the ray point → the pixel is shadowed

### 3.2 Clip-Space Marching (Tomasz Stachowiak's Approach)

March in clip space (post-projection) where steps correspond more naturally to screen pixels:

```
rayStart_cs = project(worldPos)     // w=1
rayEnd_cs   = project(worldPos + lightDir * maxDist)  // w=1
dir_cs      = rayEnd_cs - rayStart_cs

for i = 0..N:
    t = lerp(0, 1, pow(i/N, exponent))   // non-linear spacing
    sample_cs = rayStart_cs + dir_cs * t
    sampleDepth = 1.0 / depthTex.Sample(uv_from_cs(sample_cs))
    rayDepth = 1.0 / sample_cs.z
    if sampleDepth < rayDepth - thickness: hit!
```

- **Pros:** Steps naturally align with screen-space sampling; exponential spacing covers more distance with fewer steps
- **Cons:** Requires careful handling of perspective divide and near-plane clipping
- **Reference:** The `raymarch.hlsl` in [docs/raymarch.hlsl](raymarch.hlsl) implements this fully, including `HybridRootFinder` with linear march + optional bisection refinement + secant method

### 3.3 The Hybrid Root Finder (from raymarch.hlsl)

The reference implementation uses a **HybridRootFinder** with three phases:

```
Phase 1: Linear March (N steps, exponential spacing)
    - Steps are distributed with lerp(0, 1, pow(i/N, exponent))
    - exponent > 1.0 compresses early steps and expands later ones
    - Finds the interval [t_min, t_max] containing the intersection

Phase 2: Bisection Refinement (optional, K steps)
    - Binary search within the [t_min, t_max] interval
    - Each step halves the interval
    - Useful for SSR/SSGI where exact hit point matters for color sampling
    - NOT needed for contact shadows (binary visibility only)

Phase 3: Secant Method (optional)
    - Uses line-line intersection between ray approach rate and surface gradient
    - NOT needed for contact shadows
```

**For contact shadows:** Only Phase 1 (linear march) is needed. Bisection and secant add cost without benefit since we only need a binary hit/miss result.

### 3.4 Depth Comparison — The Dual-Sample Trick

The reference implementation uses a critical trick to avoid both false occlusion (acne) and false disocclusion (light leaking):

```hlsl
float linear_depth = 1.0 / depth_tex.SampleLevel(linearSampler, uv, 0);
float unfiltered_depth = 1.0 / depth_tex.SampleLevel(pointSampler, uv, 0);

float max_depth = max(linear_depth, unfiltered_depth);  // conservative occlusion
float min_depth = min(linear_depth, unfiltered_depth);  // conservative surface

// Occlusion test: ray is behind the conservative surface
res.distance = max_depth * (1.0 + bias) - ray_depth;

// Penetration test: ray hasn't gone too deep behind the conservative surface
res.penetration = ray_depth - min_depth;
```

The combination of bilinear and point-sampled depth ensures:
- **Bilinear** fixes aliasing/acne from discrete depth samples
- **Point** fixes false occlusion near object boundaries (where bilinear interpolation creates a "shrink-wrap" surface)

### 3.5 March-Behind-Surfaces

For contact shadows specifically, `march_behind_surfaces = true` allows the ray to pass through thin occluders and find valid occlusions beyond them. This is critical because:
- Without it, rays that penetrate a surface past the thickness threshold terminate early
- With it, rays continue past the surface to find deeper occluders
- Requires `depth_thickness` to be ≥ world-space step length, otherwise rays can skip through surfaces

### 3.6 Typical Parameters (Tuned for HobbyRenderer)

```
Max steps:            8-16 (tuned down by pixel-distance heuristic)
Max distance:         0.3-1.0 m (world-space; only contact details)
Thickness:            0.02-0.05 m (2-5 cm at typical world scale)
Linear march exponent: 2.0 (compress early steps, expand later)
March behind surfaces: true
Sloppy march:         false (needs both linear + point depth)
```

---

## 4. Renderer Design

### 4.1 New Renderer: `SSShadowRenderer`

A single new `IRenderer`-derived class that runs as a fullscreen compute pass.

```cpp
class SSShadowRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        // Read dependencies
        renderGraph.ReadTexture(g_RG_DepthTexture);          // scene depth
        renderGraph.ReadTexture(g_RG_GBufferNormals);        // optional: edge-aware blur
        renderGraph.ReadTexture(g_RG_ShadowMask);            // CSM shadow mask (R8_UNORM)

        // Write output: enhanced shadow mask
        renderGraph.WriteTexture(g_RG_ShadowMask);           // read-modify-write

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList,
                const RenderGraph& renderGraph) override
    {
        // Fullscreen compute dispatch:
        // 8x8 thread groups, screen_width/8 × screen_height/8
        g_Renderer.AddComputePass({ ... });
    }

private:
};
```

### 4.2 Resource Specifications

| Resource | Format | Resolution | Type | Purpose |
|---|---|---|---|---|
| **SS Shadow output** | `R8_UNORM` | Screen | Transient | Per-pixel contact shadow factor |
| **SS Shadow CB** | N/A (CB) | 256 bytes | Volatile | View-proj, light dir, parameters |

### 4.3 Shader Files

| File | Purpose |
|---|---|
| `src/shaders/SSShadow.hlsl` | Main compute shader entry point |
| `src/shaders/CommonSSShadow.hlsli` | Ray marcher, world-pos reconstruction, depth comparison |
| `src/shaders/SSShadow.sr` | srrhi resource bindings |

### 4.4 Shader Compilation Entry

In [shaders.cfg](../src/shaders/shaders.cfg):
```
SSShadow.hlsl -T cs -E SSShadow_CSMain -m 6_8
```

---

## 5. Integration Points

### 5.1 Render Graph Scheduling

SS shadows run **after the CSM shadow mask is computed** and **before deferred lighting**:

```
ClearRenderer → OpaqueRenderer → MaskedPassRenderer
→ ShadowRenderer → ShadowMaskRenderer
→ SSShadowRenderer           ← NEW: contact shadow pass (reads depth, CSM shadow mask)
→ DDGIRenderer (if present)
→ DeferredRenderer           ← reads enhanced shadow mask
→ SkyRenderer → TransparentPassRenderer → TAARenderer
→ BloomRenderer → HDRRenderer → ImGuiRenderer
```

**Placement rationale:**
- After `ShadowMaskRenderer`: CSM shadow mask is written, ready for read-modify-write
- Before `DeferredRenderer`: deferred lighting reads the final enhanced shadow mask

### 5.2 Shadow Mask Integration (Read-Modify-Write)

The CSM `ShadowMaskRenderer` writes a per-pixel `R8_UNORM` shadow factor (0.0 = fully shadowed, 1.0 = fully lit) into `g_RG_ShadowMask`.

The SS shadow pass then **reads and potentially darkens** this value:

```hlsl
// In SSShadow_CS:
float csmShadow = g_ShadowMask.Load(uint3(uvInt, 0));

// Ray march for contact shadows
float ssShadow = ComputeScreenSpaceShadow(worldPos, lightDir, ...);

// Combine: use min() to avoid double-darkening CSM-shadowed regions
float combinedShadow = min(csmShadow, ssShadow);

g_RWShadowMask[uvInt] = combinedShadow;
```

**Why `min()` and not multiply?** Because CSM already shadows a large area. Multiplying contact shadow (which also returns 0 in CSM-shadowed regions) would double-darken. Using `min()` means contact shadows only affect pixels where CSM reports them as lit.

#### 5.2.1 CSM Early-Exit Optimization

Since `min(0.0, anything) = 0.0`, pixels already fully shadowed by CSM cannot be darkened further. The entire ray march can be skipped for those pixels:

```hlsl
[numthreads(8, 8, 1)]
void SSShadow_CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 uvInt = dispatchThreadID.xy;

    // Early exit: CSM already fully shadows this pixel → nothing to add
    float csmShadow = g_ShadowMask.Load(uint3(uvInt, 0));
    if (csmShadow <= 0.0)
    {
        g_RWShadowMask[uvInt] = 0.0;
        return;
    }

    // Only ray march pixels that CSM considers lit
    float3 worldPos = ReconstructWorldPos(uvInt, depth);
    float ssShadow = ComputeScreenSpaceShadow(worldPos, lightDir, ...);
    g_RWShadowMask[uvInt] = min(csmShadow, ssShadow);
}
```

**Why this is correct:** The combination formula is `min(csmShadow, ssShadow)`. When `csmShadow = 0.0`, the result is always `0.0` regardless of `ssShadow`. Computing it would be wasted work.

**Cost:** One `Load()` from `R8_UNORM` (~1 byte of bandwidth) vs. the full ray march (~140 ALU + ~24 depth samples). The check is essentially free.

**Savings:** Proportional to the fraction of screen pixels fully in CSM shadow — typically 5–40% depending on scene and sun angle. In dense indoor or forest scenes, this can halve the SS shadow workload.

### 5.3 Deferred Lighting Integration

In [DeferredLighting.hlsl](../src/shaders/DeferredLighting.hlsl), the shadow factor is read with a single load — **no change needed** in the deferred lighting shader:

```hlsl
// Already planned for CSM phase:
lightingInputs.sunShadow = g_ShadowMask.Load(uint3(uvInt, 0));
// This now contains the min(CSM, SS) result
```

### 5.4 GBuffer Dependencies

SS shadows use these GBuffer channels:

| GBuffer | Purpose | Required |
|---|---|---|
| **Depth** (`g_RG_DepthTexture`) | Ray marching against scene geometry | ✅ Mandatory |
| **Normals** (`g_RG_GBufferNormals`) | Edge-aware bilateral filtering | ⚪ Recommended |

---

## 6. Performance Analysis

### 6.1 Compute Shader Cost Estimate

For a 1920×1080 resolution with fullscreen 8×8 thread groups:

```
Thread groups:    (1920/8) × (1080/8) = 240 × 135 = 32,400 groups
GPU invocations:  32,400 × 64 = 2,073,600 threads

Per-thread cost:
  - World position reconstruction:  ~15 ALU
  - Ray direction computation:      ~5 ALU
  - Ray march loop (12 steps):      ~120 ALU (each step: math + 2 depth samples)
  - Total:                          ~140 ALU

Estimated GPU time (RTX 3060-class, ~10 TFLOPS):
  ~2M threads × 140 ops / 10 TFLOPS ≈ 0.03 ms (raw ALU)
  + ~2 texture samples × 12 steps × 2M = ~48M tex reads ≈ 0.05-0.1 ms (bandwidth)
  Total: ~0.08-0.13 ms
```

### 6.2 Optimization Strategies

| Strategy | Speedup | Quality Impact | Complexity |
|---|---|---|---|
| **Early exit** | Variable (many rays exit early) | None | Low (already in algorithm) |
| **Exponential step spacing** | Covers more distance with fewer steps | May miss thin mid-range occluders | Low |
| **Pixel-distance culling** | Skip ray march when CSM already fully shadows (see §5.2.1) | None | Low |

### 6.3 Memory Cost

| Resource | Size (1080p) | Type |
|---|---|---|
| SS Shadow output | Shared with ShadowMask | Transient |
| CB | 256 bytes | Volatile |
| **Total additional** | **~256 bytes** | |

---

## 7. Pros and Cons

### Pros

| Aspect | Detail |
|---|---|
| **No new hardware requirements** | Compute shader only — runs on any D3D12-capable GPU |
| **Recovers CSM detail** | Fills in contact shadows that CSM bias forces you to lose |
| **Simple integration** | One new renderer class + one new shader; no changes to geometry pipeline |
| **Leverages existing infrastructure** | Depth, normals all already available |
| **Independent of scene complexity** | Cost scales with screen resolution, not scene triangle count |
| **Composability** | Works with any shadow technique (CSM, shadow maps, RT shadows) |
| **Incremental adoption** | Can be toggled on/off per frame; falls back gracefully to CSM-only |
| **Proven technique** | Used in Unreal Engine, Unity HDRP, Skyrim Community Shaders, and most AAA engines |
| **Reference implementation exists** | `raymarch.hlsl` provides a production-tested depth-buffer raymarcher |

### Cons

| Aspect | Detail |
|---|---|
| **Screen-space limitation** | Only visible geometry can cast shadows; off-screen occluders cannot shadow |
| **Depth buffer resolution** | Thin geometry (wires, grass blades) may be missed if sub-pixel |
| **Self-shadowing artifacts** | Incorrect thickness parameter causes acne or floating shadows |
| **Screen edges** | Rays leaving the framebuffer terminate early with no occlusion |
| **Requires CSM first** | Cannot be implemented standalone — depends on NormalBasic + CSM being complete |
| **Adds ~0.1-0.3ms GPU time** | Acceptable for the quality gain, but not free |
| **Tuning required** | Thickness, max distance, and step count need per-scene calibration |

---

## 8. Feasibility Assessment

### 8.1 Technical Feasibility: ✅ HIGH

All prerequisites exist or are planned:

| Prerequisite | Status |
|---|---|
| Depth buffer with reversed-Z | ✅ Present |
| Camera view/proj matrices | ✅ Present (`PlanarViewConstants`)|
| Light direction (sun) | ✅ Present (`DeferredLightingConstants.m_SunDirection`) |
| Compute shader dispatch infrastructure | ✅ Present (`AddComputePass`) |
| Render graph transient/persistent resources | ✅ Present |
| GBuffer normals | ✅ Present |
| CSM shadow mask (`g_RG_ShadowMask`) | ⚠️ Planned (CSM phase) |
| NormalBasic mode | ⚠️ Planned (CSM phase) |
| Reference raymarcher | ✅ Present (`docs/raymarch.hlsl`) |

### 8.2 Integration Complexity: LOW

- **One new renderer class** (`SSShadowRenderer` in `src/SSShadowRenderer.cpp`)
- **One new compute shader** (`src/shaders/SSShadow.hlsl`)
- **One new shared header** (`src/shaders/CommonSSShadow.hlsli`)
- **One new srrhi binding file** (`src/shaders/SSShadow.sr`)
- **One line added** to render pass scheduling
- **Minimal changes** to deferred lighting (none if using read-modify-write on shadow mask)

### 8.3 Risk Assessment

| Risk | Likelihood | Mitigation |
|---|---|---|
| Performance regression on low-end GPUs | Medium | Make SS shadows optional (ImGui toggle) |
| Tuning parameter sensitivity across scenes | Medium | Expose parameters in ImGui; auto-calibrate from scene bounds |
| Self-shadowing acne | Low | Dual-sample depth trick + thickness parameter |

---

## 9. Implementation Roadmap

### Phase 1: Basic Ray March (1-2 days)

1. Create `src/shaders/CommonSSShadow.hlsli` with:
   - World position reconstruction from depth
   - Exponential-spacing clip-space ray march (adapted from `raymarch.hlsl`)
   - Dual-sample depth comparison (bilinear + point)
   - Thickness-based self-shadow avoidance
2. Create `src/shaders/SSShadow.hlsl` — fullscreen compute entry point
3. Create `src/shaders/SSShadow.sr` — resource bindings
4. Add to [shaders.cfg](../src/shaders/shaders.cfg): `SSShadow.hlsl -T cs -E SSShadow_CSMain -m 6_8`
5. Create `src/SSShadowRenderer.h` / `src/SSShadowRenderer.cpp`:
   - `SSShadowRenderer` class extending `IRenderer`
   - `Setup()` declares read of depth, shadow mask; write of shadow mask
   - `Render()` dispatches fullscreen compute
6. Register with `REGISTER_RENDERER(SSShadowRenderer)`
7. Schedule in `ScheduleAndRunAllRenderers()` after `ShadowMaskRenderer`
8. Add ImGui toggle + parameter sliders (steps, max distance, thickness)

### Phase 2: Quality Improvements (1-2 days)

1. Implement CSM-shadowed region early exit in the compute shader entry point (see §5.2.1) — skip ray march when `g_ShadowMask` already reads `0.0`

### Phase 3: Polish (1 day)

1. Expose all parameters in ImGui with tooltips
2. Add debug visualization mode (raw SS shadow output, ray direction overlay)
3. Test with various scenes (outdoor, indoor, thin geometry, moving light)
4. Document parameter ranges and recommendations

---

## Appendix A: File Index

| File | Purpose |
|---|---|
| [Renderer.h](../src/Renderer.h) | `IRenderer` base class, `RenderingMode` enum |
| [Renderer.cpp](../src/Renderer.cpp) | `ScheduleAndRunAllRenderers()` — add SSShadowRenderer scheduling |
| [CommonRenderers.cpp](../src/CommonRenderers.cpp) | Global RG handle declarations (`g_RG_ShadowMask`, etc.) |
| [DeferredRenderer.cpp](../src/DeferredRenderer.cpp) | Deferred lighting — reads enhanced shadow mask (no change needed) |
| [DeferredLighting.hlsl](../src/shaders/DeferredLighting.hlsl) | Lighting shader — reads `g_ShadowMask` (no change needed) |
| [DeferredLighting.sr](../src/shaders/DeferredLighting.sr) | Resource bindings for deferred lighting |
| [shaders.cfg](../src/shaders/shaders.cfg) | Shader compilation config — add SSShadow entry |
| [docs/raymarch.hlsl](raymarch.hlsl) | Reference depth-buffer ray marcher by Tomasz Stachowiak |
| [docs/CSM_Analysis.md](CSM_Analysis.md) | CSM implementation plan — prerequisite phase |

### New Files to Create

| File | Purpose |
|---|---|
| `src/SSShadowRenderer.h` | SSShadowRenderer class declaration |
| `src/SSShadowRenderer.cpp` | SSShadowRenderer implementation |
| `src/shaders/SSShadow.hlsl` | Compute shader entry point |
| `src/shaders/SSShadow.sr` | srrhi resource bindings |
| `src/shaders/CommonSSShadow.hlsli` | Shared ray march + utility functions |

---

## Appendix B: Key Design Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Ray march space | Clip-space (exponential spacing) | Better screen-space coverage; reference implementation exists |
| March method | Hybrid linear march (Phase 1 only) | Bisection/secant add cost without benefit for binary visibility |
| Integration point | Read-modify-write on CSM shadow mask | Minimal deferred lighting changes; lower memory |
| Depth comparison | Dual-sample (bilinear + point) | Proven trick from reference implementation to avoid acne + leaking |
| Resolution | Full resolution only | Keeps implementation simple; no half-res variant |
| Self-shadowing | Thickness parameter + march-behind-surfaces | Required for correctness; reference implementation supports both |

---

## Appendix C: Reference Implementation (raymarch.hlsl)

The file [docs/raymarch.hlsl](raymarch.hlsl) contains Tomasz Stachowiak's production-tested depth-buffer ray marcher. Key structures we will adapt:

### DepthRayMarch

```hlsl
struct DepthRayMarch {
    uint   linear_steps;           // Number of linear march steps
    float  linear_march_exponent;  // Exponential spacing (2.0 = compress early)
    float  depth_thickness_linear_z;
    bool   march_behind_surfaces;  // true for contact shadows
    bool   use_sloppy_march;       // false for quality (dual-sample)
    float3 ray_start_cs;           // Clip-space start (w=1)
    float3 ray_end_cs;             // Clip-space end (w=1)
    Texture2D<float> depth_tex;
    float2 depth_tex_size;
};
```

### Usage for Contact Shadows (from the reference)

```hlsl
DepthRayMarchResult raymarch_result = DepthRayMarch
    ::new_from_depth(depth_tex, depth_tex_dims)
    .from_cs(vc.ray_hit_cs.xyz)          // clip-space position of pixel
    .to_ws(ray_hit_ws + direction_to_sun() * 0.3)  // march 0.3m toward sun
    .with_linear_steps(4)                // only 4 steps for contact!
    .with_depth_thickness(0.5)
    .with_march_behind_surfaces(true)
    .march();

if (raymarch_result.hit) {
    shadow = smoothstep(1.0, 0.5, raymarch_result.hit_penetration_frac);
}
```

**Note:** The reference uses only **4 linear steps** for contact shadows because:
1. The max distance is short (~0.3m)
2. Exponential spacing covers more at the end
3. March-behind-surfaces prevents false positives
4. The dual-sample depth trick provides reliable depth comparison even with few samples

For HobbyRenderer with potentially larger scenes, **8-12 steps** is a safer starting point, tunable via ImGui.

### Adaptation Notes

The reference shader depends on external includes that don't exist in HobbyRenderer:
- `frame_constants.hlsl` → Use `Common.hlsli` + `PlanarViewConstants`
- `uv.hlsl` → Simple `(clipPos.xy / clipPos.w) * 0.5 + 0.5`
- `samplers.hlsl` → Use existing bindless sampler indices

The `HybridRootFinder` template and `DepthRaymarchDistanceFn` struct are self-contained and can be copied directly into `CommonSSShadow.hlsli` with minimal adaptation.
