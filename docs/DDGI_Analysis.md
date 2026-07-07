# DDGI Implementation — Analysis & Plan

> **Phase:** DDGI (RTXGI Dynamic Diffuse Global Illumination) — standalone implementation phase  
> **Goal:** Integrate RTXGI-DDGI SDK for baked/live indirect diffuse lighting (3 ISVs, probe ray tracing, indirect query)

---

## Table of Contents

1. [Codebase Architecture Overview](#1-codebase-architecture-overview)
2. [RenderingMode Enum & CommonConsts Changes](#2-renderingmode-enum--commonconsts-changes)
3. [Render Pass Scheduling](#3-render-pass-scheduling)
4. [RTXGI-DDGI — Deep Dive](#4-rtxgi-ddgi--deep-dive)
5. [Feature Dependency Map & Disabled Features](#5-feature-dependency-map--disabled-features)
6. [DeferredRenderer Modifications](#6-deferredrenderer-modifications)
7. [Transparent Lighting in NormalBasic](#7-transparent-lighting-in-normalbasic)
8. [Implementation Roadmap](#8-implementation-roadmap)
- [Appendix A: File Index](#appendix-a-file-index)
- [Appendix B: Key Design Decisions](#appendix-b-key-design-decisions)

---

## 1. Codebase Architecture Overview

### 1.1 Rendering Pipeline

The current pipeline uses a modular render-graph architecture:

```
Per Frame:
  RenderGraph::Reset()
  RenderGraph::BeginSetup()
  for each IRenderer:
      IRenderer::Setup(RenderGraph)  ← declares resources
      RenderGraph::ScheduleRenderer()
  RenderGraph::EndSetup()
  RenderGraph::Compile()             ← resolves lifetimes, aliases resources
  TaskScheduler::ExecuteAllScheduledTasks()  ← parallel Render()
  RenderGraph::PostRender()
```

### 1.2 Current Render Pass Order (Normal Mode)

Found in [Renderer.cpp](../src/Renderer.cpp) ~lines 817-848:

```
ClearRenderer → TLASRenderer → OpaqueRenderer → MaskedPassRenderer
→ HZBGeneratorPhase2 → RTXDIRenderer → SHARCRenderer
→ DeferredRenderer → SkyRenderer → TransparentPassRenderer
→ TAARenderer → BloomRenderer → HDRRenderer → ImGuiRenderer
```

### 1.3 Key Feature Flags (Renderer struct)

From [Renderer.h](../src/Renderer.h) lines 221-310:

| Flag | Type | Default | Purpose |
|---|---|---|---|
| `m_Mode` | `RenderingMode` | `Normal` | Current rendering mode |
| `m_EnableRTShadows` | `bool` | `true` | RT shadows — **already replaced by CSM in earlier phase** |
| `m_EnableReSTIRDI` | `bool` | `true` | ReSTIR direct illumination — **disabled in NormalBasic** |
| `m_EnableReSTIRDenoising` | `bool` | `true` | NRD REBLUR denoising — **disabled in NormalBasic** |
| `m_IndirectLightingTechnique` | `uint32_t` | `2` (SHARC) | 0=None, 1=ReSTIR GI, 2=SHARC — **set to 0 in NormalBasic** |
| `m_EnableDDGI` | `bool` | `false` | **NEW** — controls DDGI probe RT + blending (bake mode when off) |
| `m_EnableOcclusionCulling` | `bool` | `true` | 2-phase HZB occlusion |

### 1.4 Dependencies

| Feature | Where Used | Dependency Chain |
|---|---|---|
| **ReSTIR DI** | `RTXDIRenderer`, `CompositingPass` | ReSTIR DI → NRD → Denoised output → DeferredRenderer |
| **ReSTIR GI** | `RTXDIRenderer` (GI pass) | ReSTIR GI → NRD → Denoised output |
| **SHARC** | `SHARCRenderer` (Update/Resolve/Query) | SHARC → g_RG_SHARCIndirect → DeferredRenderer |
| **OMM** | `RTXDIRenderer` (BLAS with OMM) | OMM → BLAS build (RT only) |
| **NRD** | `NrdIntegration` → `RTXDIRenderer` | NRD denoising for ReSTIR DI & GI outputs |
| **HWRT Shadows** | `DeferredRenderer` | **Already replaced by CSM** (ShadowRenderer + ShadowMaskRenderer) in prior phase |
| **TLAS** | `TLASRenderer`, `RTXDIRenderer`, `PathTracerRenderer` | **Still needed for DDGI probe rays** when `m_EnableDDGI=true`; not needed in bake mode |

**Key Insight:** DDGI is the _only_ remaining consumer of TLAS in NormalBasic. When `m_EnableDDGI=false` (bake mode), TLAS can be skipped entirely — pure raster pipeline.

---

## 2. RenderingMode Enum & CommonConsts Changes

### 2.1 Current State

**Host-side** ([Renderer.h](../src/Renderer.h) line 82):
```cpp
enum class RenderingMode : uint32_t
{
    Normal = srrhi::CommonConsts::RENDERING_MODE_NORMAL,          // = 0
    IBL = srrhi::CommonConsts::RENDERING_MODE_IBL,                // = 1
    ReferencePathTracer = srrhi::CommonConsts::RENDERING_MODE_PATH_TRACER  // = 2
};
```

**Shader-side** ([Common.sr](../src/shaders/Common.sr) line 85):
```hlsl
static const int RENDERING_MODE_NORMAL = 0;
static const int RENDERING_MODE_IBL = 1;
static const int RENDERING_MODE_PATH_TRACER = 2;
```

### 2.2 Required Changes

1. **Rename `RENDERING_MODE_NORMAL` → `RENDERING_MODE_NORMAL_ADVANCED`** (value stays 0)
2. **Add `RENDERING_MODE_NORMAL_BASIC = 3`** (new value)
3. Update both [Renderer.h](../src/Renderer.h) and [Common.sr](../src/shaders/Common.sr)
4. Update ImGui combo in [ImGuiLayer.cpp](../src/ImGuiLayer.cpp):
   ```cpp
   static const char* kRenderingModes[] = { "NormalAdvanced", "IBL", "Pathtracer", "NormalBasic" };
   ```

---

## 3. Render Pass Scheduling — Current vs NormalBasic

### 3.1 New Scheduling Logic (Pseudocode)

```cpp
// In Renderer::ScheduleAndRunAllRenderers()
if (m_Mode == RenderingMode::ReferencePathTracer)
{
    // ... existing path tracer path ...
}
else if (m_Mode == RenderingMode::NormalBasic)
{
    // NormalBasic: classic rasterization pipeline
    m_RenderGraph.ScheduleRenderer(g_OpaqueRenderer);
    m_RenderGraph.ScheduleRenderer(g_MaskedPassRenderer);
    m_RenderGraph.ScheduleRenderer(g_HZBGeneratorPhase2);
    // CSM shadows (already implemented in prior phase)
    m_RenderGraph.ScheduleRenderer(g_ShadowRenderer);       // CSM depth-only pass
    m_RenderGraph.ScheduleRenderer(g_ShadowMaskRenderer);   // fullscreen shadow mask compute
    // NEW — DDGI (this phase)
    if (g_Renderer.m_EnableDDGI)
        // Schedule TLAS if DDGI is active (probe rays need it)
        m_RenderGraph.ScheduleRenderer(g_TLASRenderer);
    m_RenderGraph.ScheduleRenderer(g_DDGIRenderer);          // probe RT+blend (if enabled) + indirect query (always)
    //
    m_RenderGraph.ScheduleRenderer(g_DeferredRenderer);
    m_RenderGraph.ScheduleRenderer(g_SkyRenderer);
    m_RenderGraph.ScheduleRenderer(g_TransparentPassRenderer);
    m_RenderGraph.ScheduleRenderer(g_TAARenderer);
    m_RenderGraph.ScheduleRenderer(g_BloomRenderer);
}
else  // NormalAdvanced (old Normal) + IBL
{
    // existing full pipeline with RTXDI, SHARC, TLAS
    ...
}
```

### 3.2 Pass Comparison Matrix

| Pass | NormalAdvanced | DDGI Phase (NormalBasic) | Notes |
|---|---|---|---|
| ClearRenderer | ✅ | ✅ | |
| TLASRenderer | ✅ | ✅ (conditional) | Only scheduled when `m_EnableDDGI=true` |
| OpaqueRenderer | ✅ | ✅ | |
| MaskedPassRenderer | ✅ | ✅ | |
| HZBGeneratorPhase2 | ✅ | ✅ | |
| **ShadowRenderer** | ❌ | ✅ | CSM depth-only pass (prior phase) |
| **ShadowMaskRenderer** | ❌ | ✅ | Fullscreen shadow mask compute (prior phase) |
| **DDGIRenderer** | ❌ | ✅ | **NEW** — probe RT + blend + indirect query |
| RTXDIRenderer | ✅ | ❌ | Disabled |
| SHARCRenderer | ✅ (if enabled) | ❌ | Disabled |
| DeferredRenderer | ✅ (RT shadows) | ✅ (shadow mask + DDGI indirect) | Reads shadow mask + DDGI output |
| SkyRenderer | ✅ | ✅ | |
| TransparentPassRenderer | ✅ (with TLV) | ✅ (simplified) | No TLV; simplified forward with DDGI |
| TAARenderer | ✅ | ✅ | |
| BloomRenderer | ✅ | ✅ | |
| HDRRenderer | ✅ | ✅ | |
| ImGuiRenderer | ✅ | ✅ | |

---

## 4. RTXGI-DDGI — Deep Dive

### 4.1 How DDGI Works (From Reference Study)

DDGI (Dynamic Diffuse Global Illumination) uses a **3D grid of probes** placed in world space. Each probe stores:
- **Irradiance** (octahedral map — sphere unwrapped to 2D)
- **Distance** (octahedral map — mean & variance of distance to surfaces)

**Per-frame pipeline:**
1. **Update Constants** — random rotation for probe rays, scroll offsets
2. **Trace Probe Rays** — ray trace from each probe, gather radiance & distance (APPLICATION responsibility)
3. **Probe Blending** — blend new ray data into irradiance/distance textures (SDK handles)
4. **Probe Relocation** *(optional)* — move probes out of geometry (SDK handles)
5. **Probe Classification** *(optional)* — deactivate probes in empty space (SDK handles)
6. **Probe Variability** *(optional)* — measure convergence; stop when stable (SDK handles)
7. **Query Irradiance** — fullscreen pass gathers indirect lighting from probes (APPLICATION responsibility)

### 4.2 Reference Sample Analysis

The test harness ([DDGI_D3D12.cpp](../REFERENCES/RTXGI-DDGI/samples/test-harness/src/graphics/DDGI_D3D12.cpp)) shows:
- **Single large volume** covering the entire map (Cornell Box / Sponza)
- **Unmanaged resource mode** — application creates textures, SDK creates PSOs
- **Bindless resource access** — resources accessed via descriptor heap indices
- **Probe ray tracing** uses inline ray tracing (`RayQuery`) in a compute shader, not a ray generation shader. This avoids DXR hit groups and state objects entirely — all DDGI RT is done from a pure CS with a single `Dispatch()` call.
- **Indirect lighting query** in a fullscreen compute shader ([IndirectCS.hlsl](../REFERENCES/RTXGI-DDGI/samples/test-harness/shaders/IndirectCS.hlsl))

**Key SDK API calls:**
```
DDGIVolume::Create(desc, resources)
DDGIVolume::Update()                          // pre-trace: random rotation
GetRayDispatchDimensions()                    // get dispatch size for probe rays
rtxgi::d3d12::UpdateDDGIVolumeProbes()        // blend irradiance & distance
rtxgi::d3d12::RelocateDDGIVolumeProbes()      // optional: move probes
rtxgi::d3d12::ClassifyDDGIVolumeProbes()      // optional: deactivate probes
DDGIGetVolumeIrradiance()                     // shader function: query indirect
DDGIGetVolumeBlendWeight()                    // shader function: blend volumes
```

### 4.3 Texture Formats

DDGI uses these texture arrays:

| Resource | Format | Dimensions | Notes |
|---|---|---|---|
| Ray Data | RGBA32_FLOAT or RG32_FLOAT | `(probesPerPlane, numRays, numPlanes)` | Temporary, per-frame |
| Irradiance | R10G10B10A2_UNORM or RGBA16_FLOAT | `(probeTexels * probesX, probeTexels * probesZ, probesY)` | Persistent, gamma-encoded |
| Distance | RG16_FLOAT | `(probeTexels * probesX, probeTexels * probesZ, probesY)` | Persistent |
| Probe Data | RGBA16_FLOAT | `(probesX, probesZ, probesY)` | Offsets + classification |
| Variability | R16_FLOAT | Like irradiance (no border) | Per-texel coefficient of variation |
| Variability Avg | RG16_FLOAT | Reduction pyramid | Average across volume |

### 4.4 Volume Placement Strategy

Three camera-following Infinite Scrolling Volumes (ISV) with decreasing density and increasing size as distance grows. **All three volumes are updated every frame**, but each volume only processes a subset of its probe rows via row slicing — giving perfectly consistent per-frame ray dispatch.

```
          Near ISV              Medium ISV               Far ISV
         ┌─────────┐         ┌───────────────┐      ┌─────────────────────┐
         │ ████████ │         │ · · · · · · · │      │ ◦ ◦ ◦ ◦ ◦ ◦ ◦ ◦ ◦ ◦ │
         │ ──────── │         │ ───────────── │      │ ◦ ◦ ◦ ◦ ◦ ◦ ◦ ◦ ◦ ◦ │
         │ · · · ·  │ camera  │ · · · · · · · │      │ ───────────────────── │
         └─────────┘         └───────────────┘      │ · · · · · · · · · ·  │
     row slice ÷2:              row slice ÷2:       │ · · · · · · · · · ·  │
     █ = this frame             █ = this frame      └─────────────────────┘
     · = next frame             · = next frame    row slice ÷4:
                                                     █ = this frame
                                                     · = next 3 frames
```

| Property | Near ISV | Medium ISV | Far ISV |
|---|---|---|---|
| **Probe counts** | 20×12×20 ≈ 4,800 | 20×10×20 ≈ 4,000 | 20×10×20 ≈ 4,000 |
| **Probe spacing** | 1.5 m | 4.0 m | 12.0 m |
| **Volume extent** | 30×18×30 m | 80×40×80 m | 240×120×240 m |
| **Rays per probe** | 256 | 128 | 64 |
| **Total rays (full volume)** | ~1,228,800 | ~512,000 | ~256,000 |
| **Row slice** | ÷ 2 | ÷ 2 | ÷ 4 |
| **Rows per frame** | 6 of 12 | 5 of 10 | 2–3 of 10 |
| **Probes per frame** | 2,400 | 2,000 | 800–1,200 |
| **Rays per frame** | ~614,400 | ~256,000 | ~51,200–76,800 |
| **Irradiance texels** | 10×10 (R10G10B10A2) | 8×8 (R10G10B10A2) | 6×6 (R10G10B10A2) |
| **GPU memory** | ~19 MB | ~10 MB | ~7 MB |
| **Total rays/frame (consistent): ~922K–947K** ||||

**Row slicing mechanics:**
- Each volume tracks a `currentRowOffset` that advances by `probeRows / rowSliceDivisor` each frame
- Near: offset advances by 6 rows/frame, wraps at row 0 after 2 frames → each probe updated every 2nd frame
- Medium: offset advances by 5 rows/frame, wraps after 2 frames → each probe updated every 2nd frame
- Far: offset advances by 2 or 3 rows/frame (alternating), wraps after 4 frames → each probe updated every 4th frame
- Probes scrolled in via ISV movement get updated immediately in their first slice (special-case at the scroll edge)

**Design rationale:**
- **Near ISV** provides high-quality indirect lighting for surfaces close to the camera where detail is most visible. Small volume keeps probe count manageable despite tight spacing. Halving the rows means temporal latency of only 1 frame — imperceptible.
- **Medium ISV** covers the mid-range where indirect lighting still matters but at less demanding fidelity. 2-frame update latency is fine for mid-distance.
- **Far ISV** provides low-frequency ambient indirect for distant geometry. Coarse probe grid + fewer rays + 4-frame latency keeps cost negligible. The large extent ensures outdoor scenes have indirect coverage at distance.
- **Consistent ~930K rays/frame** means predictable GPU budget, no frame-to-frame variance.

All three volumes use `EDDGIVolumeMovementType::Scrolling` and follow the camera via `SetScrollAnchor(cameraWorldPos)`. Frustum-cull each volume: if a volume's AABB is completely outside the frustum, skip its probe update for that frame.

### 4.5 Probe Update Strategies

With row slicing, all 3 ISVs are updated every frame at a perfectly consistent workload:

```
Frame 0 ─────────────────────────────────────────────────────
  Near:  rows 0-5   (6 of 12)    ████████████········
  Med:   rows 0-4   (5 of 10)    ██████████··········
  Far:   rows 0-1   (2 of 10)    ████················
  Rays:  614K + 256K + 51K = ~922K

Frame 1 ─────────────────────────────────────────────────────
  Near:  rows 6-11  (6 of 12)    ············████████████
  Med:   rows 5-9   (5 of 10)    ··········██████████····
  Far:   rows 2-3   (2 of 10)    ····████················
  Rays:  614K + 256K + 51K = ~922K

Frame 2 ─────────────────────────────────────────────────────
  Near:  rows 0-5   (6 of 12)    ████████████········  (wraps)
  Med:   rows 0-4   (5 of 10)    ██████████··········  (wraps)
  Far:   rows 4-6   (3 of 10)    ········██████········
  Rays:  614K + 256K + 77K = ~947K

Frame 3 ─────────────────────────────────────────────────────
  Near:  rows 6-11  (6 of 12)    ············████████████
  Med:   rows 5-9   (5 of 10)    ··········██████████····
  Far:   rows 7-9   (3 of 10)    ··············██████····
  Rays:  614K + 256K + 77K = ~947K

... repeats every 4 frames
```

**Key properties:**
- **Consistent ray count** — between 922K and 947K rays every frame (3% variance max)
- **No bursts** — unlike staggered-volume approaches where frame 0 does 2M rays
- **Latency proportional to distance** — near probes have 1-frame latency, far probes 4-frame
- **Scroll-edge handling** — new probes introduced by ISV scrolling are force-updated in their first slice, ensuring moving camera doesn't cause stale indirect at scroll boundaries

All RTXGI SDK calls (`UpdateDDGIVolumeProbes`, `RelocateDDGIVolumeProbes`, `ClassifyDDGIVolumeProbes`) operate on the row slice only, not the entire volume.

### 4.6 Volume Culling

**Q: Frustum culling? Occlusion culling for volumes?**

**A:** 
- **Frustum culling:** `DDGIVolume::GetAxisAlignedBoundingBox()` → test against camera frustum. If volume is outside frustum, skip probe updates for that frame.
- **Occlusion culling:** More complex. Can use the HZB from the main camera pass. If all probes in a volume are behind occluders (as determined by HZB), skip the volume. **Not recommended for initial implementation** — frustum culling is sufficient.

### 4.7 Variability & Convergence

**Q: When to stop dispatching rays? When probe results are "stable"?**

**A:** The SDK's `CalculateDDGIVolumeVariability()` and `ReadbackDDGIVolumeVariability()` provide a **coefficient of variation** across all probes. When this drops below a threshold (e.g., 0.01-0.05), the volume has "converged enough."

```
if (variability < 0.03 && !sceneChanged)
    skip_probe_updates = true;
```

However, in practice for a real-time game:
- Probes **never fully converge** because the camera moves
- With ISV, new probes are constantly being introduced at scroll edges
- **Recommendation:** Always update probes for the active ISV; only use variability for optional stationary volumes

### 4.8 Indirect Diffuse vs Specular

**Q: Is DDGI only for indirect diffuse? What about indirect specular?**

**A:** DDGI is **diffuse-only**. It stores irradiance (hemispherical integral of incoming radiance weighted by cos θ). For specular indirect:
- Screen-space reflections (SSR) — cheap, works in NormalBasic
- Ray-traced reflections — requires RT support (not in NormalBasic scope)
- **Recommendation:** DDGI for indirect diffuse + SSR (if desired) for indirect specular

### 4.9 Non-Volume Areas (Far Distance)

**Q: What about indirect lighting for areas outside volumes?**

**A:** The SDK provides `DDGIGetVolumeBlendWeight()` which returns 1.0 inside the volume and decays to 0 outside based on distance. Use this to blend with:
- A constant ambient color (simple)
- A sky-derived ambient term (better — sample sky irradiance)
- An IBL probe (best but needs environment map)

For NormalBasic: blend with sky ambient outside DDGI volume. This is what the test harness does.

### 4.10 DDGI Integration Architecture for HobbyRenderer

```
class DDGIRenderer : public IRenderer
{
    // Per-volume state
    struct DDGIVolumeState
    {
        rtxgi::d3d12::DDGIVolume volume;
        rtxgi::d3d12::DDGIVolumeResources resources;
        rtxgi::DDGIVolumeDesc desc;
        
        // HobbyRenderer-specific texture handles (for render graph integration)
        RGTextureHandle irradianceTex;
        RGTextureHandle distanceTex;
        RGTextureHandle probeDataTex;
        RGTextureHandle rayDataTex;         // transient
    };
    
    std::vector<DDGIVolumeState> m_Volumes;
    
    // Shader bytecode for DDGI SDK shaders
    std::vector<uint8_t> m_ProbeBlendingIrradianceCS;
    std::vector<uint8_t> m_ProbeBlendingDistanceCS;
    // ... etc.
    
    // Render graph output: indirect lighting texture
    RGTextureHandle m_IndirectOutput;
};
```

**Per-frame flow:** The DDGIRenderer operates in two modes controlled by `m_EnableDDGI`:

```
// LIVE MODE (m_EnableDDGI = true) — probe RT + blending + indirect query
Setup():
  Declare DDGI textures (persistent: irradiance, distance, probe data)
  Declare ray data texture (transient)
  Declare indirect output texture
  Read GBuffer depth/normals
  Write indirect output

Render():
  1. Update() each volume (scroll, random rotation)
  2. Upload constants to GPU
  3. Dispatch probe ray tracing (compute shader with inline `RayQuery` → writes ray data)
  4. UpdateDDGIVolumeProbes() (blend new ray data into persistent textures)
  5. DDGIGetVolumeIrradiance() in fullscreen CS → writes indirect output

// BAKE MODE (m_EnableDDGI = false) — indirect query only, no RT
Setup():
  Declare indirect output texture only
  Read GBuffer depth/normals
  Write indirect output

Render():
  1. DDGIGetVolumeIrradiance() in fullscreen CS → reads baked persistent textures → writes indirect output
```

In bake mode, steps 1-4 are skipped entirely. The persistent irradiance/distance/probe data textures survive across frames (they are long-lived GPU resources). The indirect query shader (`DDGIGetVolumeIrradiance()`) simply samples the pre-converged probe data — it has no dependency on ray tracing. This means:
- **Zero RT cost per frame** in bake mode
- **Zero transient allocations** for ray data
- **TLAS not needed** in bake mode (see §5.1)

### 4.11 Ray Tracing Requirement for DDGI

**Critical note:** DDGI requires GPU ray tracing for probe ray tracing. The probe rays need to trace against the scene's TLAS/BLAS. This means:

- **TLAS is still needed** — but only for DDGI probes, not for shadows
- **BLAS must be built** — they already are (Scene.cpp builds them for meshlet LOD)
- **DXR 1.1 support is required** — DDGI probe rays use inline `RayQuery` in a **compute shader** (no raygen, no any-hit, no hit groups). This keeps everything as a pure `Dispatch()` call with a single compute PSO.

This is a key trade-off: NormalBasic removes ReSTIR DI/GI/SHARC but adds DDGI which still requires RT support. However, DDGI's RT cost is much lower:
- Fewer rays (probeCount × raysPerProbe vs per-pixel rays for ReSTIR)
- Inline RT is simpler and lighter than the DXR hit-group pipeline — no shader tables, no state objects for hit groups
- Can be amortized over multiple frames

### 4.12 DDGI Bake Mode — Use HWRT to Converge, Then Disable RT

The primary use case for DDGI in NormalBasic is **baking** — not real-time updating. The workflow:

```
1. Enable DDGI (m_EnableDDGI = true) → probes ray-trace each frame, converge to steady state
2. Wait for convergence (variability drops below threshold, or manual bake timer)
3. Disable DDGI (m_EnableDDGI = false) → probes stop updating, RT cost drops to zero
4. Enjoy baked GI: indirect query reads the persistent converged probe textures
```

**Feature flag and ImGui control:**

```cpp
// In Renderer.h
bool m_EnableDDGI = false;  // Enable DDGI probe ray tracing + blending (for baking)
```

```cpp
// In ImGuiLayer.cpp — "DDGI" section
if (ImGui::CollapsingHeader("DDGI (Global Illumination)"))
{
    ImGui::Checkbox("Enable DDGI (Probe RT)", &g_Renderer.m_EnableDDGI);
    if (g_Renderer.m_EnableDDGI)
    {
        ImGui::TextColored(ImVec4(1,1,0,1), "Probes converging... (RT active)");
        ImGui::SliderFloat("Convergence Threshold", &g_Renderer.m_DDGIConvergenceThreshold, 0.001f, 0.1f);
        if (ImGui::Button("Stop When Converged"))
            g_Renderer.m_DDGIAutoStop = true;
    }
    else
    {
        ImGui::TextColored(ImVec4(0,1,0,1), "Baked GI (no RT cost)");
    }
}
```

**What stays and what goes:**

| Component | `m_EnableDDGI = true` | `m_EnableDDGI = false` |
|---|---|---|
| Probe ray tracing (inline RT in CS) | ✅ Runs each frame | ❌ Skipped |
| `UpdateDDGIVolumeProbes()` blending | ✅ Blends new rays | ❌ Skipped |
| Irradiance/distance/probe data textures | ✅ Updated (persistent) | ✅ Preserved (persistent, baked) |
| Indirect query (`DDGIGetVolumeIrradiance`) | ✅ Runs | ✅ Runs (reads baked data) |
| TLAS requirement | ✅ Needed | ❌ Not needed |
| RT cost per frame | ~930K rays | **0** |
| GPU memory | ~36 MB (3 volumes) + ray data transient | ~36 MB (3 volumes, persistent only) |

**Key design points:**

- **Persistent textures survive disabling:** The irradiance, distance, and probe data textures are allocated as long-lived GPU resources (not per-frame transient). When `m_EnableDDGI` is toggled off, these textures retain their last converged state — the indirect query shader simply reads from them as before.
- **DDGI SDK state is preserved:** The SDK's `DDGIVolume` objects and their internal state (probe offsets, classifications) remain in host memory. The renderer just stops calling `Update()` and `UpdateDDGIVolumeProbes()`.
- **No hot-reload needed:** Switching between bake and live modes is a single checkbox — no scene reload, no probe data loss. Re-enabling DDGI resumes probe updates from the current converged state.
- **Scrolling still works:** If the camera moves after baking, probes don't follow (ISV scrolling is part of `Update()`, which is skipped). For static-camera scenes this is perfect. For moving cameras, re-enable DDGI briefly to re-converge at the new position.
- **TLAS is freed in bake mode:** When `m_EnableDDGI = false`, `TLASRenderer` can be skipped entirely (see §5.1). No BLAS rebuild, no TLAS update — pure raster pipeline.
- **Convergence detection:** The SDK provides `CalculateDDGIVolumeVariability()` and `ReadbackDDGIVolumeVariability()` (see §4.9). When variability drops below threshold across all volumes, probes are considered "converged enough." This can be used for automatic bake stop.

**Bake workflow example:**

```
User workflow:
  Load scene → Enable DDGI checkbox → wait 2-5 seconds (probes converge)
  → Variability drops below 0.02 → auto-stop or manual uncheck
  → "Baked GI (no RT cost)" shown in UI
  → Enjoy indirect lighting at zero RT cost for the rest of the session

Developer workflow (for shipping):
  1. Enable DDGI, place camera at key positions
  2. Let probes converge at each position
  3. Serialize converged probe textures to disk
  4. Ship game with baked probe data
  5. At runtime: load baked textures, run indirect query only (m_EnableDDGI = false)

---

## 5. Feature Dependency Map & Disabled Features

### 5.1 Feature Dependency Map

```
NormalAdvanced (current Normal):
  TLAS ────────┬──→ RTXDIRenderer (ReSTIR DI + GI)
               │       └── NRD (REBLUR denoising)
               │       └── OMM (opacity micromaps)
               ├──→ SHARCRenderer (spatial hash radiance cache)
               │       └── requires TLAS for ray queries
               └──→ DeferredRenderer (HWRT shadows via inline ray queries)   ← REMOVED in NormalBasic

NormalBasic (m_EnableDDGI = true):
  ───→ HWRT shadows REMOVED — replaced by CSM (ShadowRenderer + ShadowMaskRenderer) [prior phase]
  ───→ TLAS still needed for DDGI probe rays only
  ───→ ShadowRenderer (depth-only raster, NO TLAS needed)
  ───→ DDGIRenderer (probe rays NEED TLAS)
  ───→ NO ReSTIR DI, NO ReSTIR GI, NO SHARC, NO NRD, NO OMM, NO TLV
  ───→ TransparentPassRenderer (simplified forward — sun + DDGI indirect only; no TLV)

NormalBasic (m_EnableDDGI = false) — baked GI mode:
  ───→ HWRT shadows via CSM (ShadowRenderer + ShadowMaskRenderer) [prior phase]
  ───→ ShadowRenderer (depth-only raster)
  ───→ DDGIRenderer (indirect query only, NO TLAS)
  ───→ TransparentPassRenderer (simplified forward — sun + DDGI indirect only; no TLV)
  ───→ NO TLAS, NO ReSTIR DI, NO ReSTIR GI, NO SHARC, NO NRD, NO OMM, NO TLV
  ───→ Pure raster pipeline. Zero RT cost. Zero HWRT shadows.
```

### 5.2 What Actually Gets Skipped

| Component | What Happens | Impact |
|---|---|---|
| `TLASRenderer` | **Conditional** — scheduled only when `m_EnableDDGI=true` (DDGI probe ray tracing). Skipped entirely in bake mode (`m_EnableDDGI=false`) — pure raster pipeline, zero RT cost. | TLAS is the _only_ RT dependency remaining in NormalBasic. |
| `RTXDIRenderer` | Entire renderer skipped (`Setup()` returns false when `m_EnableReSTIRDI=false`) | Saves: RIS buffer alloc, presampling, temporal resampling, spatial resampling, compositing, NRD denoising passes |
| `SHARCRenderer` | Entire renderer skipped | Saves: Update, Resolve, Query passes |
| `NrdIntegration` | Not instantiated | Saves: REBLUR denoiser, permanent/transient pools, PackNormalRoughness pass |
| OMM | Not built for BLAS (or built with `AllowOMM=false`) | Saves: OMM build time, OMM memory |
| HWRT Shadows (`m_EnableRTShadows`) | **Already removed in prior phase** — replaced by CSM (ShadowRenderer + ShadowMaskRenderer) | DeferredRenderer reads R8 shadow mask instead of firing inline ray queries |
| TLV (Translucency Lighting Volume) | Entirely skipped — TLV injection reads ReSTIR DI RIS buffers + SHARC resolved buffers, both unavailable | Transparent objects use simplified forward lighting (sun + DDGI indirect only); see §7 |

### 5.3 DDGI-Specific Feature Flags

| Flag | Type | Default | Purpose |
|---|---|---|---|
| `m_EnableDDGI` | `bool` | `false` | When `true`: probe ray tracing (needs TLAS) + blending runs each frame. When `false`: only indirect query runs from persistent baked probe textures (no RT, no TLAS). |

---

## 6. DeferredRenderer Modifications

The `DeferredRenderer` currently has these conditional inputs:
```cpp
// ReSTIR DI composited output
if (g_Renderer.m_EnableReSTIRDI) → reads g_RG_RTXDIDIComposited

// SHARC indirect output
if (g_Renderer.m_IndirectLightingTechnique == SHARC) → reads g_RG_SHARCIndirect
```

For NormalBasic (DDGI phase), modify to:
```cpp
// DI source: shadow mask (precomputed per-pixel visibility) — prior CSM phase
// Indirect source: DDGI indirect output (this phase)
if (g_Renderer.m_Mode == RenderingMode::NormalBasic)
{
    renderGraph.ReadTexture(g_RG_ShadowMask);     // Read precomputed shadow mask (R8_UNORM)
    renderGraph.ReadTexture(g_RG_DDGIIndirect);   // Read DDGI indirect output
}
```

The deferred lighting shader bindings:
```hlsl
Texture2D<float>             g_ShadowMask;    // R8_UNORM, screen resolution — from CSM phase
Texture2D<float4>            g_DDGIIndirect;  // Indirect irradiance — from DDGI phase
```

In `DeferredLighting_PSMain`, the shadow factor and indirect GI are read with single loads:
```hlsl
lightingInputs.sunShadow = g_ShadowMask.Load(uint3(uvInt, 0));
float3 ddgiIndirect = g_DDGIIndirect.Load(uint3(uvInt, 0)).rgb;
```

---

## 7. Transparent Lighting in NormalBasic — No TLV

The [Translucency Lighting Volume (TLV)](implementation_plan_restir_sharc_transparent.md) is an **advanced-only feature** that bakes ReSTIR DI stochastic direct lighting and SHARC indirect GI into a 3D volume grid. It is **excluded from NormalBasic**:

| Dependency | TLV Requirement | NormalBasic Status |
|---|---|---|
| ReSTIR DI RIS buffers | TLV injection pass reads RIS candidate lights | ❌ RTXDIRenderer skipped — no RIS buffers |
| SHARC resolved radiance cache | TLV injection pass queries SHARC for indirect | ❌ SHARCRenderer skipped — no radiance cache |
| RTXDI + SHARC render passes | Must run before TLV injection | ❌ Both disabled |

The transparent forward pass falls back to a simplified path using DDGI:

```hlsl
// NormalBasic transparent forward lighting:
// ── Direct: sun + CSM shadow mask ──
float sunShadow = g_ShadowMask.SampleLevel(linearSampler, screenUV, 0).r;
float3 directDiffuse = EvaluateSunLight(baseColor, normal, sunDirection) * sunShadow;

// ── Indirect: DDGI probe query ──
float3 ddgiIndirect = DDGIGetVolumeIrradiance(worldPos, surfaceBias, normal, volumes);
float3 indirectGI = ddgiIndirect * baseColor * (1.0 - metallic);

float3 color = directDiffuse + indirectGI;
```

| Aspect | NormalAdvanced (TLV) | NormalBasic (DDGI indirect) |
|---|---|---|
| **Direct light** | ReSTIR DI RIS from all scene lights | Single directional sun (analytic) |
| **Direct shadows** | Baked into TLV from stochastic samples | CSM shadow mask (R8 screen-space) — prior phase |
| **Indirect GI** | SHARC 2–4 bounce diffuse GI | DDGI probe-based indirect (baked or live) — this phase |
| **Light count scaling** | O(1) | O(1) — always 1 sun |
| **Per-pixel cost** | 2 trilinear samples (~16 taps) | 1 shadow mask sample + 1 DDGI probe query |

---

## 8. Implementation Roadmap

### Phase 1: Enum & Skeleton (shared — likely already done from CSM phase)

1. Add `RENDERING_MODE_NORMAL_BASIC = 3` to [Common.sr](../src/shaders/Common.sr)
2. Rename `RENDERING_MODE_NORMAL` → `RENDERING_MODE_NORMAL_ADVANCED`
3. Update `RenderingMode` enum in [Renderer.h](../src/Renderer.h)
4. Add `NormalBasic` branch in `ScheduleAndRunAllRenderers()`
5. Update ImGui combo in [ImGuiLayer.cpp](../src/ImGuiLayer.cpp)
6. Set `m_EnableReSTIRDI = false`, `m_EnableRTShadows = false`, `m_IndirectLightingTechnique = 0` in NormalBasic

### Phase 2: CSM Integration (prior phase — already present)

- CSM shadow maps + shadow mask already implemented and wired into `DeferredRenderer`
*(Full details in [CSM_Analysis.md](CSM_Analysis.md))*

### Phase 3: DDGIRenderer (this phase)

1. Integrate RTXGI-DDGI SDK (`rtxgi-sdk/include`, `rtxgi-sdk/shaders`, `rtxgi-sdk/src`)
2. Create `src/DDGIRenderer.h` / `src/DDGIRenderer.cpp`
3. Compile DDGI SDK shaders with `RTXGI_DDGI_RESOURCE_MANAGEMENT=0` (unmanaged), bindless mode
4. Create 3 camera-following ISVs (near / medium / far) with row slicing:
   - Near: 20×12×20, 1.5m spacing, 256 rays/probe, row slice ÷2
   - Medium: 20×10×20, 4.0m spacing, 128 rays/probe, row slice ÷2
   - Far: 20×10×20, 12.0m spacing, 64 rays/probe, row slice ÷4
5. Implement probe ray tracing (compute shader with inline `RayQuery` using existing TLAS)
6. Call SDK: `UpdateDDGIVolumeProbes()`, `RelocateDDGIVolumeProbes()`, `ClassifyDDGIVolumeProbes()`
7. Fullscreen CS pass calling `DDGIGetVolumeIrradiance()` → writes `g_RG_DDGIIndirect`
8. Wire `g_RG_DDGIIndirect` into `DeferredRenderer`
9. Add `m_EnableDDGI` flag (default `false`) in `Renderer.h` — controls probe RT + blending
10. Implement bake mode: when `m_EnableDDGI = false`, skip probe RT/blending but still run indirect query
11. Conditionally schedule `TLASRenderer`: only when `m_EnableDDGI = true`
12. Add ImGui checkbox for `m_EnableDDGI` + convergence indicators in `ImGuiLayer.cpp`
13. Implement convergence detection: `CalculateDDGIVolumeVariability()` → auto-stop when below threshold

### Phase 4: Integration & Polish

1. Modify `DeferredRenderer` to conditionally use CSM + DDGI in NormalBasic mode
2. Add ImGui controls for DDGI:
   - DDGI enable checkbox + convergence threshold + auto-stop
   - DDGI probe density presets (Low/Medium/High)
   - Variability readout
3. Profile and tune 3-ISV probe density, row slice divisors, ray counts per probe
4. Test DDGI bake workflow: enable → wait for convergence → disable → verify zero RT cost + correct indirect
5. Test corner cases: moving camera with baked probes, thin geometry, outdoor scenes, scroll-edge probe updates
6. Verify TLAS is not scheduled in bake mode (`m_EnableDDGI = false`)

---

## Appendix A: File Index

| File | Purpose |
|---|---|
| [Renderer.h](../src/Renderer.h) | `IRenderer` base, `RenderingMode` enum, `Renderer` struct with `m_EnableDDGI` flag |
| [Renderer.cpp](../src/Renderer.cpp) | `ScheduleAndRunAllRenderers()` — pass scheduling with conditional TLAS |
| [Common.sr](../src/shaders/Common.sr) | `CommonConsts` HLSL constants including RENDERING_MODE_* |
| [DeferredRenderer.cpp](../src/DeferredRenderer.cpp) | Deferred lighting pass — conditionally reads DDGI output |
| [DeferredLighting.hlsl](../src/shaders/DeferredLighting.hlsl) | Lighting shader — reads DDGI indirect irradiance |
| [DeferredLighting.sr](../src/shaders/DeferredLighting.sr) | Resource bindings for deferred lighting |
| [BasePass.hlsl](../src/shaders/BasePass.hlsl) | Transparent forward shader — DDGI probe query for indirect |
| [RTXDIRenderer.cpp](../src/RTXDIRenderer.cpp) | ReSTIR DI + GI — to be skipped in NormalBasic |
| [SHARCRenderer.cpp](../src/SHARCRenderer.cpp) | SHARC indirect — to be skipped in NormalBasic |
| [TLASRenderer.cpp](../src/TLASRenderer.cpp) | TLAS build — conditionally scheduled (only when `m_EnableDDGI=true`) |
| [NrdIntegration.h](../src/NrdIntegration.h) | NRD denoiser — to be skipped in NormalBasic |
| [ImGuiLayer.cpp](../src/ImGuiLayer.cpp) | UI controls — RenderingMode combo + DDGI enable checkbox |
| [DDGIVolume.h](../REFERENCES/RTXGI-DDGI/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h) | DDGI volume API |
| [Integration.md](../REFERENCES/RTXGI-DDGI/docs/Integration.md) | DDGI integration guide |
| [DDGIVolume.md](../REFERENCES/RTXGI-DDGI/docs/DDGIVolume.md) | DDGI volume reference |
| [DDGI_D3D12.cpp](../REFERENCES/RTXGI-DDGI/samples/test-harness/src/graphics/DDGI_D3D12.cpp) | DDGI test harness reference implementation |
| [IndirectCS.hlsl](../REFERENCES/RTXGI-DDGI/samples/test-harness/shaders/IndirectCS.hlsl) | Reference indirect query shader |

## Appendix B: Key Design Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Shadow technique | CSM (4 cascades) — prior phase | Well-understood, no RT hardware needed for shadows |
| DDGI volume count | 3 ISVs (near/medium/far, camera-following) | High detail near, ambient far; consistent ~930K rays/frame |
| DDGI near probe density | 20×12×20, 1.5m spacing | High-quality indirect for surfaces close to camera |
| DDGI medium probe density | 20×10×20, 4.0m spacing | Balanced mid-range coverage |
| DDGI far probe density | 20×10×20, 12.0m spacing | Low-frequency ambient for distant geometry |
| DDGI update strategy | Row slicing: near÷2, med÷2, far÷4 | All volumes updated every frame, consistent ray count, no bursts |
| DDGI volume culling | Frustum culling only | Simple, sufficient; skip volumes fully outside frustum |
| DDGI + far distance | Blend with sky ambient | No complex falloff needed |
| Indirect specular | Not in scope | SSR can be added later if needed |
| DDGI operation mode | `m_EnableDDGI` flag: bake (default off) vs live (on) | Primary use case is baking: converge probes with HWRT, then disable RT; indirect query always runs from persistent textures |
| TLAS | Only needed when `m_EnableDDGI = true` | DDGI probe rays need TLAS; when disabled (bake mode), TLASRenderer is skipped entirely |
| DDGI bake convergence | SDK variability readback + auto-stop | Probes converge in 2-5 seconds; variability < 0.02 indicates "done" |
| Transparent lighting | Simplified forward (sun + DDGI indirect, no TLV) | TLV depends on ReSTIR + SHARC which are disabled |
```
