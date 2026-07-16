# CSM Implementation — Analysis & Plan

> **Phase:** CSM (Cascaded Shadow Maps) — standalone implementation phase  
> **Goal:** Remove HWRT shadows, replace with classic rasterized CSM (4 cascades + shadow mask)

---

## Table of Contents

1. [Codebase Architecture Overview](#1-codebase-architecture-overview)
2. [RenderingMode Enum & CommonConsts Changes](#2-renderingmode-enum--commonconsts-changes)
3. [Render Pass Scheduling](#3-render-pass-scheduling)
4. [Cascaded Shadow Maps (CSM) — Deep Dive](#4-cascaded-shadow-maps-csm--deep-dive)
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
| `m_UseMeshletRendering` | `bool` | `true` | GPU-driven meshlet pipeline |
| `m_EnableRTShadows` | `bool` | `true` | RT inline ray query shadows — **must be set to `false` for CSM** |
| `m_EnableReSTIRDI` | `bool` | `true` | ReSTIR direct illumination — **disabled in NormalBasic** |
| `m_IndirectLightingTechnique` | `uint32_t` | `2` (SHARC) | 0=None, 1=ReSTIR GI, 2=SHARC — **set to 0 in NormalBasic** |
| `m_EnableOcclusionCulling` | `bool` | `true` | 2-phase HZB occlusion |

### 1.4 Dependencies of Features to Disable

| Feature | Where Used | Dependency Chain |
|---|---|---|
| **ReSTIR DI** | `RTXDIRenderer`, `CompositingPass` | ReSTIR DI → NRD → Denoised output → DeferredRenderer |
| **ReSTIR GI** | `RTXDIRenderer` (GI pass) | ReSTIR GI → NRD → Denoised output |
| **SHARC** | `SHARCRenderer` (Update/Resolve/Query) | SHARC → g_RG_SHARCIndirect → DeferredRenderer |
| **OMM** | `RTXDIRenderer` (BLAS with OMM), nvrhi OMM paths | OMM → BLAS build (RT only; rasterizer doesn't need OMM) |
| **NRD** | `NrdIntegration` → `RTXDIRenderer` | NRD denoising for ReSTIR DI & GI outputs |
| **HWRT Shadows** | `DeferredRenderer` (inline ray queries) | **REMOVED in NormalBasic** — replaced by CSM (ShadowRenderer + ShadowMaskRenderer) |
| **TLAS** | `TLASRenderer`, `RTXDIRenderer`, `PathTracerRenderer` | Not needed for CSM shadows — only needed later for DDGI probe rays |

**Key Insight:** All disabled features are RT-dependent. None affect the rasterized G-Buffer pass, HZB, deferred lighting, sky, transparency, TAA, or post-processing passes. CSM adds new raster passes (ShadowRenderer, ShadowMaskRenderer) that are entirely RT-free.

---

## 2. RenderingMode Enum & CommonConsts Changes

### 2.1 Current State (NormalBasic implemented ✅)

**Host-side** ([Renderer.h](../src/Renderer.h) line 83):
```cpp
enum class RenderingMode : uint32_t
{
    Normal = srrhi::CommonConsts::RENDERING_MODE_NORMAL,                      // = 0
    IBL = srrhi::CommonConsts::RENDERING_MODE_IBL,                            // = 1
    ReferencePathTracer = srrhi::CommonConsts::RENDERING_MODE_PATH_TRACER,    // = 2
    NormalBasic = srrhi::CommonConsts::RENDERING_MODE_NORMAL_BASIC            // = 3
};
```

**Shader-side** ([Common.sr](../src/shaders/Common.sr) line 86):
```hlsl
static const int RENDERING_MODE_NORMAL = 0;
static const int RENDERING_MODE_IBL = 1;
static const int RENDERING_MODE_PATH_TRACER = 2;
static const int RENDERING_MODE_NORMAL_BASIC = 3;
```

### 2.2 Completed Changes ✅

1. ~~**Rename `RENDERING_MODE_NORMAL` → `RENDERING_MODE_NORMAL_ADVANCED`**~~ — **SKIPPED**: kept `Normal` as-is; NormalBasic is a separate new mode.
2. ✅ **Add `RENDERING_MODE_NORMAL_BASIC = 3`** — done in [Common.sr](../src/shaders/Common.sr), srrhi auto-regenerated both headers.
3. ✅ **Update [Renderer.h](../src/Renderer.h)** — `NormalBasic` added to `RenderingMode` enum.
4. ✅ **Update [ImGuiLayer.cpp](../src/ImGuiLayer.cpp)** — combo now has `"Normal", "Image Based Lighting", "Reference Pathtracer", "NormalBasic"`.
5. ✅ **`--normalbasic` CLI flag** — added in [Config.cpp](../src/Config.cpp); auto-disables RT feature flags.
6. ✅ **Skip BLAS/TLAS in NormalBasic** — [Scene.cpp](../src/Scene.cpp) skips `BuildAccelerationStructures`, creates empty placeholder TLAS.
7. ✅ **NormalBasic scheduling branch** — [Renderer.cpp](../src/Renderer.cpp) skips TLAS, RTXDI, SHARC.
8. ✅ **Mode-switch assert** — switching from NormalBasic to Normal/PathTracer asserts TLAS is built. IBL does not assert (no RT needed).

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
    // NEW — CSM shadows (this phase)
    m_RenderGraph.ScheduleRenderer(g_ShadowRenderer);       // CSM depth-only pass
    m_RenderGraph.ScheduleRenderer(g_ShadowMaskRenderer);   // fullscreen shadow mask compute
    // NEW — DDGI (next phase, placeholder; indirect query only, no RT)
    m_RenderGraph.ScheduleRenderer(g_DDGIRenderer);
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

| Pass | NormalAdvanced | CSM Phase (NormalBasic) | Notes |
|---|---|---|---|
| ClearRenderer | ✅ | ✅ | |
| TLASRenderer | ✅ | ❌ | RT-only; skipped (not needed for CSM) |
| OpaqueRenderer | ✅ | ✅ | |
| MaskedPassRenderer | ✅ | ✅ | |
| HZBGeneratorPhase2 | ✅ | ✅ | |
| **ShadowRenderer** | ❌ | ✅ | **NEW** — CSM: renders opaque (null PS) + masked (alpha-test PS) per cascade |
| **ShadowMaskRenderer** | ❌ | ✅ | **NEW** — fullscreen compute: evaluates CSM → writes R8 shadow mask |
| **CSMDebugRenderer** | ❌ | ✅ | **NEW** — debug overlay (cascade splits, shadow map viz, PCF footprint, etc.) |
| **DDGIRenderer** | ❌ | ✅ | **NEW** — DDGI indirect query (next phase; placeholder) |
| RTXDIRenderer | ✅ | ❌ | Disabled |
| SHARCRenderer | ✅ (if enabled) | ❌ | Disabled |
| DeferredRenderer | ✅ (RT shadows) | ✅ (shadow mask) | Modified — uses shadow mask instead of RT queries |
| SkyRenderer | ✅ | ✅ | |
| TransparentPassRenderer | ✅ (with TLV) | ✅ (simplified) | No TLV; simplified forward lighting |
| TAARenderer | ✅ | ✅ | |
| BloomRenderer | ✅ | ✅ | |
| HDRRenderer | ✅ | ✅ | |
| ImGuiRenderer | ✅ | ✅ | |

---

## 4. Cascaded Shadow Maps (CSM) — Deep Dive

### 4.1 Algorithm Overview

CSM splits the camera frustum into 4 sub-frustums, each rendered to a separate shadow map region (or atlas tile). This gives higher shadow resolution near the camera and lower resolution far away.

**Frame sequence:**
```
1. Compute 4 cascade split depths (logarithmic + uniform blend)
2. For each cascade i:
   a. Compute frustum corners in world space
   b. Transform corners to light space
   c. Compute tight AABB in light space
   d. Build orthographic view-proj matrix
   e. Cull & render meshlet depth-only pass
3. In DeferredLighting shader: select cascade, sample shadow map, apply PCF
```

### 4.2 Depth-Only Draw Calls for Meshlets

The existing `BasePassRenderer` infrastructure can be heavily reused. Key observations:

- **GPU Culling**: [GPUCulling.hlsl](../src/shaders/GPUCulling.hlsl) already performs frustum culling, LOD selection, visibility counting, and indirect argument generation. This can be reused with a shadow-specific view-proj matrix.
- **Meshlet Rendering**: [BasePassRenderer.cpp](../src/BasePassRenderer.cpp) lines 420-443 show the meshlet dispatch pipeline. A shadow variant would use a **null PS** (no pixel shader at all) — only the depth target is written via the rasterizer's depth output.
- **Approach**: Create a `ShadowRenderer` class that uses the same `BasePassResources` infrastructure but with:
  - Shadow view-proj matrix instead of camera view-proj
  - Depth-only render target (no color/G-Buffer outputs — null RTV, null PS)
  - Simplified vertex/mesh shader — position-only output, no material evaluation
  - Null pixel shader — depth is written automatically by the hardware rasterizer

### 4.3 Best-Fitting Orthographic View-Proj Matrix

For each cascade:

1. **Extract frustum corners** in world space for the cascade's near/far planes
2. **Transform corners to light space** using the light's view matrix
3. **Compute min/max AABB** in light space → sphere or AABB
4. **Ortho projection** = `ortho(left, right, bottom, top, near_light, far_light)`

The light's `near` and `far` planes must encompass the entire scene depth range, not just the cascade, because objects outside the cascade can still cast shadows into it.

**Best-fitting planes:**
- `near_light = min_z_light - ENLARGE_FACTOR`
- `far_light = max_z_light + ENLARGE_FACTOR`
- The AABB should be expanded to include all potential shadow casters (use scene AABB or a conservative extrapolation).

### 4.4 Cascade Split Distances

**Recommendation: Practical Split Scheme (λ-blend of logarithmic and uniform)**

```
z_i = λ * n * (f/n)^(i/N) + (1-λ) * (n + (i/N) * (f-n))
```
where λ ∈ [0.5, 0.9] (typical: 0.75).

For 4 cascades with λ=0.75, near=1.0, far=1000:
- Cascade 0: 1.0 — ~8.0  (near, high-detail)
- Cascade 1: ~8.0 — ~45.0
- Cascade 2: ~45.0 — ~250.0
- Cascade 3: ~250.0 — 1000.0 (far, low-detail)

### 4.5 Culling Strategy

**Use per-cascade light-frustum culling only.** The existing GPU culling infrastructure ([GPUCulling.hlsl](../src/shaders/GPUCulling.hlsl)) already performs frustum culling — simply pass the shadow VP matrix as `m_ViewProj` in the `CullingConstants`. No occlusion culling or HZB is needed for shadow passes; depth-only draws with a null PS are extremely fast and the additional overhead of HZB generation per cascade is not justified.

### 4.6 Shadow Map Layout

**Texture Array:**
- `Texture2DArray`, 4 slices, e.g., 2048×2048 per cascade
- D3D12 depth target, `D32_FLOAT` or `D24_UNORM_S8_UINT`
- Single draw call changes viewport + slice index per cascade
- Cleaner API, independent cascade resolution possible later

### 4.7 Texel Snapping for Shadow Stability

**Problem:** As the camera moves, the shadow view-proj matrix changes slightly each frame, causing the shadow map texels to "swim" relative to world geometry. This creates shimmering/flickering shadows.

**Solution: View-proj matrix quantization to shadow texel size.**

```cpp
// After computing the orthographic projection for a cascade:
float worldUnitsPerTexel = (right - left) / shadowMapWidth;

// Round/quantize the matrix translation to texel boundaries
matrix._41 = floor(matrix._41 / worldUnitsPerTexel) * worldUnitsPerTexel;
matrix._42 = floor(matrix._42 / worldUnitsPerTexel) * worldUnitsPerTexel;
// _43 (Z) does not need snapping for orthographic projection stability
```

This ensures that as the camera moves, the shadow map texels "snap" to discrete world positions, eliminating sub-texel swimming. **This is a critical quality improvement and should be implemented from the start.**

Reference: "Stabilized Cascaded Shadow Maps" technique used in virtually all modern engines (Unreal, Unity, etc.).

### 4.8 Shadow Filtering Algorithms

Three tiers of shadow filtering will be implemented. Hardware PCF and 3×3 PCF are **enabled by default**. PCSS is available but **disabled by default**.

### 4.8.1 Hardware PCF (4-tap) — ✅ Enabled by default

D3D12 hardware comparison samplers perform **4-tap bilinear PCF automatically** in a single `SampleCmpLevelZero` call:

```hlsl
SamplerComparisonState g_ShadowSampler : register(s1);
// Create with D3D12_COMPARISON_FILTER_COMPARISON_LINEAR

float shadow = g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, uv, depth);
```

This is the fastest option — zero extra ALU cost, fully hardware-accelerated.

### 4.8.2 3×3 PCF (9-tap) — ✅ Enabled by default

Wider kernel for softer, more stable shadows:

```hlsl
float shadow = 0;
for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++)
        shadow += g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, uv + offset, depth);
shadow /= 9.0;
```

Each tap is a hardware-accelerated comparison sample (4-tap bilinear underneath). The 9 outer taps produce a smooth falloff equivalent to a 6×6 sample footprint at minimal ALU cost. This is the default behavior for NormalBasic mode.

### 4.8.3 PCSS (Percentage-Closer Soft Shadows) — ❌ Disabled by default

PCSS produces variable penumbra sizes — shadows are sharp near the occluder and soft farther away, mimicking realistic light source size:

1. **Blocker search** — sample the shadow map in a region around the receiver to estimate average blocker depth
2. **Penumbra estimation** — compute penumbra width from blocker-receiver distance and light size
3. **Wider PCF** — use the estimated penumbra to drive a variable-width PCF kernel (e.g. 5×5 to 25×25)

PCSS is substantially more expensive than fixed-kernel PCF and is therefore **disabled by default**. It can be toggled via a `CommonConsts` flag for users who want cinematic-quality contact-hardening shadows.

### 4.9 Shadow Map Resource Specification

```
Format:     D32_FLOAT (or D24_UNORM_S8_UINT if stencil needed)
Dimensions: 2048×2048 × 4 slices (Texture2DArray)
Sampler:    COMPARISON_LINEAR, CLAMP_TO_BORDER (border=1.0 white)
```

### 4.10 Implementation Summary for CSM

```
Per Frame:
1. Compute cascade splits (λ=0.75 blend)
2. For each cascade (0..3):
   a. Extract frustum corners in world space
   b. Transform to light space (using sun direction as light view)
   c. Compute AABB → ortho projection matrix
   d. Apply texel snapping
   e. Cull meshlets against light frustum (reuse GPUCulling pipeline)
   f. Dispatch depth-only meshlet draw with null PS
3. Bind shadow texture array as SRV to DeferredLighting
4. In DeferredLighting PS: select cascade by pixel depth, sample shadow map, apply PCF
```

### 4.11 Alpha-Masked Geometry in Shadow Pass (Grass & Foliage)

### Problem

The depth-only shadow pass described in §4.2 uses a **null PS** (no pixel shader) — the hardware rasterizer writes depth automatically. This works perfectly for opaque geometry, but **alpha-masked geometry** (grass, foliage, fences, tree leaves, etc.) renders as a **solid opaque quad** in the shadow map. The result: grass and foliage cast full-rectangle shadows instead of respecting their alpha-masked silhouette.

In the existing NormalAdvanced pipeline this is not an issue because RT shadows (inline ray queries) use the `AnyHit` shader which performs alpha testing via `AlphaTest()` in [RaytracingCommon.hlsli](../src/shaders/RaytracingCommon.hlsli). For NormalBasic's rasterized CSM, we must handle this differently.

### Existing Infrastructure

The codebase already has all the pieces needed:

1. **Scene already separates instances by alpha mode** — `m_OpaqueBucket` and `m_MaskedBucket` in `Scene` struct. The `MaskedPassRenderer` renders instances from the masked bucket with `ALPHA_MODE_MASK`.

2. **Alpha-test pixel shader already exists** — `GBuffer_PSMain_AlphaTest` in [BasePass.hlsl](../src/shaders/BasePass.hlsl) (lines 284-286) samples the albedo texture, computes alpha, and calls `discard` when `alpha < mat.m_AlphaCutoff`:

```hlsl
float4 albedoSample = hasAlbedo
    ? SampleBindlessTexture(mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, input.uv)
    : float4(mat.m_BaseColor.xyz, mat.m_BaseColor.w);

float alpha = hasAlbedo ? (albedoSample.w * mat.m_BaseColor.w) : mat.m_BaseColor.w;

#if defined(ALPHA_TEST)
if (alpha < mat.m_AlphaCutoff)
{
    discard;
}
#endif
```

3. **Bindless texture system** — `SampleBindlessTexture()` reads any texture by index + sampler index, no descriptor table binding needed. This is already used in the G-Buffer alpha test.

### Solution: Two-Pass Shadow Rendering

Split the shadow depth pass into two draw batches per cascade:

```
For each cascade:
  1. Draw OPAQUE instances   → null PS (depth-only, no material lookups)
  2. Draw MASKED instances   → alpha-test PS (samples albedo, discards below cutoff)
```

This way opaque geometry stays fast (null PS, zero texture bandwidth), and only masked geometry pays the cost of texture sampling + alpha test.

### Shader Variant Strategy

For the shadow pass, create a minimal alpha-test pixel shader that only discards — no need to output anything other than allowing the hardware rasterizer to handle depth:

```hlsl
// ShadowAlphaTest_PS — depth-only with alpha discard
void ShadowAlphaTest_PS(VSOut input)
{
    srrhi::PerInstanceData inst = g_Instances[input.instanceID];
    srrhi::MaterialConstants mat = g_Materials[inst.m_MaterialIndex];

    float alpha = mat.m_BaseColor.w;
    if ((mat.m_TextureFlags & srrhi::CommonConsts::TEXFLAG_ALBEDO) != 0)
    {
        float4 albedoSample = SampleBindlessTexture(
            mat.m_AlbedoTextureIndex, mat.m_AlbedoSamplerIndex, input.uv);
        alpha *= albedoSample.w;
    }

    if (alpha < mat.m_AlphaCutoff)
        discard;
    // No SV_Target output — depth is written by the hardware rasterizer
}
```

The vertex/mesh shader outputs `SV_Position` and UV coordinates (same as the existing G-Buffer mesh shader but without material/normal outputs). Only masked instances need UVs; opaque instances can use a stripped position-only mesh shader.

### Performance Considerations

| Aspect | Impact |
|---|---|
| **Opaque instances** | Zero change — still null PS, no texture reads |
| **Masked instances** | Additional cost: albedo texture reads + alpha compare + `discard` |
| **Texture bandwidth** | Only paid for masked geometry in the shadow pass; these are typically a small fraction of total geometry |
| **Quad utilization** | `discard` in a pixel shader reduces quad utilization on GPUs (4-pixel quads); masked geometry already has poor utilization in the G-Buffer pass, so this is not a new regression |
| **Culling** | Masked instances still culled against the light frustum, so only visible masked geometry triggers the alpha-test shader |

### Design Decision

**Use the split-pass approach (opaque → null PS, masked → alpha-test PS).** This is the standard approach used by Unreal Engine, Unity, and virtually all production renderers with CSM. The alternative (no alpha testing in shadows → solid blocky shadows for foliage) produces unacceptable visual quality and is not a viable option for any scene with vegetation.

---

### 4.12 Shadow Bias Strategy

Shadow bias is the single most important tuning parameter for CSM quality. Too little bias → shadow acne (speckled dark spots on lit surfaces). Too much bias → Peter Panning (shadows detach from their casters).

**Decision: Normal-offset bias is the sole bias strategy.** This technique, introduced by Daniel Holbert (GDC 2011, "Saying Goodbye to Shadow Acne"), pushes the sample position outward along the surface normal before transforming to light space. It is now standard in Unreal Engine's VSM and traditional CSM paths. Unlike slope-scaled depth bias, the normal offset is inherently bounded (maximum = `normalBias × texelSize`) and does not explode at grazing angles.

#### 4.12.1 Implementation

Applied in `ComputeCSMShadow()` in `CommonShadow.hlsli`:

```hlsl
float ComputeCSMShadow(
    float3 worldPos, float3 worldNormal, float viewDepth,
    Texture2DArray<float> shadowMap, SamplerComparisonState shadowSampler,
    float4x4 shadowViewProj[4], float4 cascadeSplits,
    float normalBias,      // Default: 3.0 texels
    float texelSize)       // 1.0 / shadowMapResolution
{
    uint cascadeIndex = SelectCascade(viewDepth, cascadeSplits);

    // Push sample outward along world-space normal
    float3 offsetWorldPos = worldPos + worldNormal * normalBias * texelSize;
    float4 lightSpacePos = mul(float4(offsetWorldPos, 1.0f), shadowViewProj[cascadeIndex]);
    float3 shadowUV = lightSpacePos.xyz / lightSpacePos.w;
    shadowUV.xy = shadowUV.xy * float2(0.5f, -0.5f) + 0.5f;

    if (any(shadowUV.xy < 0.0f) || any(shadowUV.xy > 1.0f))
        return 1.0f;

    return Compute3x3PCF(shadowMap, shadowSampler,
        float3(shadowUV.xy, (float)cascadeIndex), shadowUV.z, texelSize);
}
```

**Why this works:**
- The normal offset naturally scales per cascade — the same world-space displacement maps to proportionally more texels in distant cascades. No per-cascade bias table needed.
- Maximum offset is bounded: `normalBias × texelSize`. No explosion at grazing angles.
- When `N·L ≈ 1.0` (light perpendicular to surface): normal points toward light → the offset moves the sample slightly forward in light space → effectively zero bias.
- When `N·L ≈ 0.0` (light parallel to surface): normal is orthogonal to light → maximum offset → prevents acne on steep slopes.

#### 4.12.2 Default Values & Tuning

| Parameter | Default | Range | When to Increase |
|---|---|---|---|
| **Normal Bias** | **3.0** texels | 0.5–10.0 | Speckled dark pixels on any surface type |

**Tuning methodology:**
```
1. Start: Normal Bias = 3.0.
2. Enable Debug Mode 6 (Depth Compare) — red=shadowed, green=lit.
3. Red speckles in green areas → increase Normal Bias by 1.0.
4. Shadows detached from casters (Peter Panning) → decrease Normal Bias by 0.5.
5. Repeat until clean.
```

#### 4.12.3 Rasterizer-Level Depth Bias (Hardware)

A small hardware bias is applied during shadow map rendering to prevent coplanar z-fighting:

```cpp
pipelineDesc.renderState.rasterState.depthBias            = 100;
pipelineDesc.renderState.rasterState.slopeScaledDepthBias = 1.5f;
pipelineDesc.renderState.rasterState.depthBiasClamp       = 0.0f;
```

This is independent of the shader-level normal-offset bias above.

#### 4.12.4 Constant Buffer Layout

```hlsl
cbuffer ShadowMaskCB
{
    // ... view-proj matrices, cascade splits, output size ...
    float m_NormalBias;   // Normal-offset in shadow-map texels (default: 3.0)
    float m_Padding[3];
};
```

#### 4.12.5 Summary

| What | Where | Default |
|---|---|---|
| **Normal-offset bias** | `CommonShadow.hlsli` — `ComputeCSMShadow()` | **3.0** texels, always active |
| Rasterizer bias (hardware) | PSO for shadow depth pass | 100 / 1.5 (fixed, not user-exposed) |

---

### 4.13 Shadow Mask (Screen-Space R8 Texture)

For the NormalBasic implementation with **1 directional light**, shadows are computed via a **screen-space shadow mask** — a separate fullscreen pass that evaluates CSM and writes per-pixel visibility to an `R8_UNORM` render target. The deferred lighting shader then reads this precomputed value with a single texture load.

### Pipeline

```
Frame:
  1. ShadowRenderer → writes CSM depth array (4 × 2048², D32_FLOAT)
  2. ShadowMaskRenderer → fullscreen compute/pixel pass:
       for each pixel:
         - read depth + GBuffer normal from GBuffer
         - reconstruct world position
         - select cascade by view-space depth
         - apply normal-offset bias per §4.12
         - SampleCmpLevelZero() with PCF kernel (3×3 = 9 taps)
         - write shadow factor (0..1) to R8_UNORM RT
  3. DeferredRenderer → fullscreen PS:
       shadow = g_ShadowMask.Load(uvInt).r;  // single R8 load, no CSM sampling
       color = lighting * shadow;
```

### ShadowMaskRenderer Design

```cpp
class ShadowMaskRenderer : public IRenderer
{
    RGTextureHandle m_ShadowMask;  // R8_UNORM, screen resolution

    bool Setup(RenderGraph& renderGraph) override
    {
        renderGraph.ReadTexture(g_RG_CSMShadowMap);   // CSM depth array
        renderGraph.ReadTexture(g_RG_DepthTexture);   // reconstruct world pos
        renderGraph.ReadTexture(g_RG_GBufferNormals); // for normal-offset bias
        renderGraph.WriteTexture(m_ShadowMask);       // output shadow factor
        return true;
    }
};
```

### Shadow Mask Compute Shader

```hlsl
// ShadowMask_CS — fullscreen compute: evaluate CSM, write R8
[numthreads(8, 8, 1)]
void ShadowMask_CSMain(uint3 dispatchID : SV_DispatchThreadID)
{
    uint2 uvInt = dispatchID.xy;
    float depth = g_Depth.Load(uint3(uvInt, 0));
    
    // Sky / no geometry → fully lit
    if (depth == 0.0f)
    {
        g_RWShadowMask[uvInt] = 1.0f;
        return;
    }
    
    // Reconstruct world position + normal
    float2 uv = (float2(uvInt) + 0.5f) / g_Constants.m_OutputSize;
    float4 clipPos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
    float4 worldPos4 = mul(clipPos, g_Constants.m_ClipToWorld);
    float3 worldPos = worldPos4.xyz / worldPos4.w;
    
    // Decode world-space normal from GBuffer (for normal-offset bias, §4.12)
    float3 normal = DecodeNormal(g_GBufferNormals.Load(uint3(uvInt, 0)));
    
    // Cascade selection + shadow evaluation (bias applied internally)
    float shadow = ComputeCSMShadow(
        worldPos, normal, viewDepth,
        g_CSMShadowMap, g_ShadowSampler,
        g_Constants.m_ShadowViewProj, g_Constants.m_CascadeSplits,
        g_ShadowBias);
    
    g_RWShadowMask[uvInt] = shadow;
}
```

### Advantages

| Aspect | Benefit |
|---|---|
| **Modularity** | Shadow evaluation is a separate, independently debuggable pass |
| **Deferred lighting simplicity** | Lighting shader reads 1× R8 instead of 9× depth comparison samples + cascade logic |
| **Filtering flexibility** | Shadow mask can be post-processed: PCSS with variable kernel sizes, temporal accumulation — without touching the lighting shader |
| **Cache behavior** | CSM depth reads are localized to the shadow mask pass; deferred lighting pass only reads GBuffer + R8 mask |
| **Multi-light ready** | When multiple shadow-casting lights are added, each contributes to the same mask or separate masks; lighting pass reads precomputed results |
| **Half-res option** | Shadow mask can be computed at half resolution and bilinearly upsampled for performance |
| **Debugging** | Trivially visualize the shadow mask RT (e.g., in ImGui) |

### Resource Specification

```
Shadow Mask RT:
  Format:     R8_UNORM
  Dimensions: screen resolution (e.g., 1920×1080)
  Memory:     ~2 MB @ 1080p, ~8 MB @ 4K
  Lifetime:   per-frame transient

Shadow Mask Constants CB:
  float4x4 m_ShadowViewProj[4];
  float4   m_CascadeSplits;
  float4x4 m_ClipToWorld;
  float2   m_OutputSize;
```

### Implementation Note

The CSM shadow evaluation logic lives in a shared HLSL header [CommonShadow.hlsli](../src/shaders/CommonShadow.hlsli) (new file), used by both `ShadowMask_CS` and any future passes that need CSM access:

```hlsl
// In CommonShadow.hlsli (new file) — shared CSM evaluation with normal-offset bias
float ComputeCSMShadow(
    float3 worldPos,
    float3 normalWS,          // world-space normal for normal-offset bias
    float  viewDepth,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler,
    float4x4 shadowViewProj[4],
    float4   cascadeSplits,
    float    normalBias,      // Normal-offset in shadow-map texels (default: 3.0)
    float    texelSize)       // 1.0 / shadowMapResolution
{
    uint cascadeIndex = SelectCascade(viewDepth, cascadeSplits);

    // ── Normal-offset bias ──
    // Push sample position along world-space normal before transforming to light space
    float3 offsetPos = worldPos + normalWS * normalBias * texelSize;

    float4 lightSpacePos = mul(float4(offsetPos, 1.0f), shadowViewProj[cascadeIndex]);
    float3 shadowUV = lightSpacePos.xyz / lightSpacePos.w;
    shadowUV.xy = shadowUV.xy * 0.5f + 0.5f;
    shadowUV.y = 1.0f - shadowUV.y;

    // Out of bounds → fully lit
    if (any(shadowUV.xy < 0.0f) || any(shadowUV.xy > 1.0f))
        return 1.0f;

    // ── 3×3 PCF (each tap is hardware-accelerated 4-tap bilinear comparison) ──
    float shadow = 0.0f;
    float2 texelSize2D = 1.0f / 2048.0f;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            shadow += shadowMap.SampleCmpLevelZero(
                shadowSampler,
                float3(shadowUV.xy + float2(x, y) * texelSize2D, cascadeIndex),
                shadowUV.z);
    return shadow / 9.0f;
}
```

For a detailed explanation of the bias strategy, constants layout, and tuning guide, see [§4.12 Shadow Bias Strategy](#412-shadow-bias-strategy).
### 4.14 CSM Debug View Modes

Debug visualization is **critical** for CSM development. Shadow artifacts are notoriously hard to diagnose without visual feedback. All debug modes should be wired to an ImGui dropdown (`CSM Debug View` combo) and implemented **early** — ideally right after the skeleton renderer produces its first shadow map.

Each debug mode is a distinct fullscreen pass or overlay that replaces (or composites atop) the final output. All modes use a single `uint m_CSMDebugMode` constant in the shader constant buffer.

---

#### Mode 1: Cascade Split Overlay

**Purpose:** Visualize which cascade each pixel falls into. This is the single most important debug view — wrong split positions cause cascade boundaries to be visible as hard seams or resolution transitions.

**Implementation:**
```hlsl
// In DeferredLighting PS or a dedicated debug pass:
static const float3 kCascadeColors[4] = {
    float3(1.0f, 0.2f, 0.2f),  // Red    — Cascade 0 (near)
    float3(0.2f, 1.0f, 0.2f),  // Green  — Cascade 1
    float3(0.2f, 0.2f, 1.0f),  // Blue   — Cascade 2
    float3(1.0f, 1.0f, 0.2f),  // Yellow — Cascade 3 (far)
};

uint cascadeIndex = SelectCascade(viewDepth, cascadeSplits);
return float4(kCascadeColors[cascadeIndex] * 0.5f + albedo * 0.5f, 1.0f);
```

**Diagnoses:** Split distance tuning, cascade overlapping, view-dependent split drift.

---

#### Mode 2: Shadow Map Array Visualization

**Purpose:** Display each cascade's depth map as a small 2D quad in screen corners. Shows what geometry each cascade captures and whether occluders are present.

**Implementation:** Render 4 quads in a fixed screen-space layout (e.g., 4-up horizontal strip at the bottom). Sample each slice of the `Texture2DArray` with a linear depth→grayscale conversion:

```hlsl
float depth = g_CSMShadowMap.SampleLevel(pointSampler, float3(uv, cascadeIndex), 0);
float gray = saturate(depth * DEPTH_SCALE);  // or: LinearizeDepth(depth) / farPlane
return float4(gray, gray, gray, 1.0f);
```

**Diagnoses:** Empty cascade frusta, shadow caster coverage, depth precision issues (near vs far), cascade AABB tightness.

---

#### Mode 3: Raw Shadow Mask

**Purpose:** Display the `R8_UNORM` shadow mask as a grayscale image. This is the final visibility signal consumed by the deferred lighting pass — any artifacts here propagate directly to the final image.

**Implementation:** Copy or overlay the shadow mask RT:
```hlsl
float shadow = g_ShadowMask.Load(uint3(uvInt, 0));
return float4(shadow, shadow, shadow, 1.0f);
```

**Diagnoses:** PCF quality, shadow acne (where mask should be 0 but is 1), Peter Panning (where mask should be 1 but is 0), cascade boundary seams, shadow bias tuning.

---

#### Mode 4: PCF Sample Footprint

**Purpose:** Visualize the PCF kernel sample density by modulating pixel brightness based on the number of PCF taps that pass the depth test. Identifies shadow edge quality issues.

**Implementation:** Compute shadow twice — once at full PCF quality (9-tap), once at 1-tap (hardware PCF only). The difference reveals where the PCF kernel is active:
```hlsl
float shadow1Tap = shadowMap.SampleCmpLevelZero(sampler, float3(uv, cascadeIndex), depth);
float shadow9Tap = Compute3x3PCF(...);
float kernelEffect = abs(shadow9Tap - shadow1Tap);  // 0 = hard edge, >0 = PCF active
return float4(kernelEffect, shadow9Tap, shadow1Tap, 1.0f);
// R: kernel influence, G: soft shadow, B: hard shadow
```

**Diagnoses:** PCF kernel width effectiveness, shadow edge softness falloff, identifying areas where PCF is over-blurring or under-blurring.

---

#### Mode 5: Alpha-Masked Overlay

**Purpose:** Highlight only pixels whose shadow evaluation went through the alpha-test path (masked geometry: grass, foliage, fences) vs the opaque fast path (null PS).

**Implementation:** Two-pass shadow mask: one pass for opaque (write 0.0 to a stencil or separate RT), one for masked (write 1.0). Then visualize:
```hlsl
bool bIsMasked = g_MaskedFlag.Load(uint3(uvInt, 0)).r > 0.5f;
float3 color = bIsMasked ? float3(1.0f, 0.5f, 0.0f) : float3(0.5f, 0.5f, 0.5f);
return float4(color, 1.0f);
```

**Diagnoses:** Verify alpha-masked geometry is correctly separated from opaque, ensure foliage casts proper silhouetted shadows, detect missing alpha-test coverage.

---

#### Mode 6: Light-Space Depth Compare

**Purpose:** Directly visualize the shadow depth comparison. Show red where the depth test fails (in shadow), green where it passes (lit).

**Implementation:**
```hlsl
float depthSM = shadowMap.SampleCmpLevelZero(sampler, float3(uv, cascadeIndex), depth);
float3 color = depthSM < 0.5f
    ? float3(0.8f, 0.1f, 0.1f)   // Red = shadowed
    : float3(0.1f, 0.8f, 0.1f);  // Green = lit
return float4(color, 1.0f);
```

**Diagnoses:** Shadow acne (speckled red in lit areas), Peter Panning (green gaps at contact points), normal bias calibration, false shadowing from out-of-frustum casters.

---

#### Mode 7: Split Frustum Wireframe (3D)

**Purpose:** Draw the 8 corners and 12 edges of each cascade's sub-frustum in the 3D view, color-coded per cascade. Requires a simple line-draw pass (e.g., ImGui 3D lines or a dedicated wireframe overlay).

**Implementation:** Compute frustum corner world positions on CPU, submit to an ImGui 3D line-drawing call or a dedicated debug line renderer:
```cpp
// CPU-side: compute frustum corners for each cascade
for (uint cascadeIdx = 0; cascadeIdx < 4; cascadeIdx++)
{
    float3 corners[8];
    ComputeFrustumCorners(camera, cascadeSplits[cascadeIdx], cascadeSplits[cascadeIdx+1], corners);
    DrawFrustumWireframe(corners, kCascadeColors[cascadeIdx]);
}
```

**Diagnoses:** Frustum AABB tightness, split plane placement in 3D, light direction vs cascade alignment, dueling frusta detection.

---

#### Mode 8: Cascade Transition Blend Zone

**Purpose:** Highlight the blend band between cascades where both shadow maps are sampled and interpolated. Reveals whether the blend band is too narrow (visible seam) or too wide (wasted performance).

**Implementation:** In the cascade selection logic, track whether the pixel falls in the blend zone:
```hlsl
float blendFactor = saturate((viewDepth - splitEnd - BLEND_BAND * 0.5f) / BLEND_BAND);
bool bInBlendZone = blendFactor > 0.0f && blendFactor < 1.0f;
float3 color = bInBlendZone ? float3(1.0f, 0.0f, 1.0f) : kCascadeColors[cascadeIndex];
return float4(color, 1.0f);
```

**Diagnoses:** Blend band width tuning, cascade seam visibility, dithering vs blending quality comparison.

---

#### Debug Mode Summary

| Mode | Name | Visual Output | Primary Diagnosis |
|---|---|---|---|
| 1 | Cascade Split Overlay | Color-per-cascade tint on scene | Split position tuning |
| 2 | Shadow Map Array Viz | 4-up depth maps in screen corners | Caster coverage, empty cascades |
| 3 | Raw Shadow Mask | Grayscale mask RT | PCF quality, bias, acne |
| 4 | PCF Sample Footprint | RGB: kernel influence + soft/hard edges | PCF kernel effectiveness |
| 5 | Alpha-Masked Overlay | Orange=alpha-tested, gray=opaque | Masked geometry separation |
| 6 | Light-Space Depth Compare | Red=shadowed, Green=lit | Bias calibration, false shadowing |
| 7 | Split Frustum Wireframe | 3D frustum edges color-coded | Frustum shape, light alignment |
| 8 | Cascade Blend Zone | Magenta=blend, cascade color=non-blend | Blend band width tuning |

#### Debug View Architecture

All debug modes share a single compute/pixel shader with a `uint m_DebugMode` branch:

```hlsl
// CSMDebug_PS or integrated into DeferredLighting PS debug path
float3 DebugOutput = float3(0, 0, 0);
switch (m_DebugMode)
{
    case CSM_DEBUG_CASCADE_SPLITS:    DebugOutput = DebugCascadeSplits(...); break;
    case CSM_DEBUG_SHADOW_MAP_VIZ:    DebugOutput = DebugShadowMapViz(...); break;
    case CSM_DEBUG_SHADOW_MASK:       DebugOutput = DebugShadowMask(...); break;
    case CSM_DEBUG_PCF_FOOTPRINT:     DebugOutput = DebugPCFFootprint(...); break;
    case CSM_DEBUG_ALPHA_MASKED:      DebugOutput = DebugAlphaMasked(...); break;
    case CSM_DEBUG_DEPTH_COMPARE:     DebugOutput = DebugDepthCompare(...); break;
    case CSM_DEBUG_BLEND_ZONE:        DebugOutput = DebugBlendZone(...); break;
    default:                          DebugOutput = finalLighting; break;
}
```

ImGui integration:
```cpp
static const char* kCSMDebugModes[] = {
    "Off", "Cascade Splits", "Shadow Map Array", "Raw Shadow Mask",
    "PCF Footprint", "Alpha-Masked Overlay", "Depth Compare",
    "Frustum Wireframe", "Blend Zone"
};
ImGui::Combo("CSM Debug", &g_Renderer.m_CSMDebugMode, kCSMDebugModes, IM_ARRAYSIZE(kCSMDebugModes));
```

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
  ───→ HWRT shadows REMOVED — replaced by CSM (ShadowRenderer + ShadowMaskRenderer)
  ───→ TLAS still needed for DDGI probe rays only
  ───→ ShadowRenderer (depth-only raster, NO TLAS needed)
  ───→ DDGIRenderer (probe rays NEED TLAS)
  ───→ NO ReSTIR DI, NO ReSTIR GI, NO SHARC, NO NRD, NO OMM, NO TLV
  ───→ TransparentPassRenderer (simplified forward — sun + DDGI indirect only; no TLV)

NormalBasic (m_EnableDDGI = false) — baked GI mode:
  ───→ HWRT shadows REMOVED — replaced by CSM (ShadowRenderer + ShadowMaskRenderer)
  ───→ ShadowRenderer (depth-only raster)
  ───→ DDGIRenderer (indirect query only, NO TLAS)
  ───→ TransparentPassRenderer (simplified forward — sun + DDGI indirect only; no TLV)
  ───→ NO TLAS, NO ReSTIR DI, NO ReSTIR GI, NO SHARC, NO NRD, NO OMM, NO TLV
  ───→ Pure raster pipeline. Zero RT cost. Zero HWRT shadows.
```

### 5.2 What Actually Gets Skipped

| Component | What Happens | Impact |
|---|---|---|
| `TLASRenderer` | **Conditional** — needed only when `m_EnableDDGI=true` (probe ray tracing). Skipped entirely in bake mode (`m_EnableDDGI=false`) — pure raster pipeline, zero RT cost | |
| `RTXDIRenderer` | Entire renderer skipped (`Setup()` returns false when `m_EnableReSTIRDI=false`) | Saves: RIS buffer alloc, presampling, temporal resampling, spatial resampling, compositing, NRD denoising passes |
| `SHARCRenderer` | Entire renderer skipped | Saves: Update, Resolve, Query passes |
| `NrdIntegration` | Not instantiated | Saves: REBLUR denoiser, permanent/transient pools, PackNormalRoughness pass |
| OMM | Not built for BLAS (or built with `AllowOMM=false`) | Saves: OMM build time, OMM memory. Already handled by `RTXDIRenderer` skipping build flags |
| HWRT Shadows (`m_EnableRTShadows`) | **REMOVED in NormalBasic** — `m_EnableRTShadows` set to `false`. The `DeferredRenderer` no longer fires inline ray queries (`RayQuery`) for per-pixel shadow evaluation. Replaced entirely by CSM (ShadowRenderer + ShadowMaskRenderer): a depth-only raster pass writes the shadow map array, and a fullscreen compute pass evaluates PCF into an R8 shadow mask that the deferred lighting shader reads with a single load. | This is the first step — HWRT shadows are removed before CSM is wired in. |
| TLV (Translucency Lighting Volume) | Entirely skipped — TLV injection reads ReSTIR DI RIS buffers + SHARC resolved buffers, both unavailable in NormalBasic | Transparent objects use simplified forward lighting (sun + DDGI indirect only); see §7 |

---

## 6. DeferredRenderer Modifications

The `DeferredRenderer` currently has these conditional inputs:
```cpp
// ReSTIR DI composited output
if (g_Renderer.m_EnableReSTIRDI) → reads g_RG_RTXDIDIComposited

// SHARC indirect output
if (g_Renderer.m_IndirectLightingTechnique == SHARC) → reads g_RG_SHARCIndirect
```

For NormalBasic, modify to:
```cpp
// DI source: shadow mask (precomputed per-pixel visibility)
// Indirect source: DDGI indirect output (new g_RG_DDGIIndirect)
if (g_Renderer.m_Mode == RenderingMode::NormalBasic)
{
    renderGraph.ReadTexture(g_RG_ShadowMask);     // Read precomputed shadow mask (R8_UNORM)
    renderGraph.ReadTexture(g_RG_DDGIIndirect);   // Read DDGI indirect output
}
```

The `DeferredLightingConstants` CB needs:
```hlsl
uint m_RenderingMode;         // existing
uint m_UseReSTIRDI;           // existing (will be 0 for NormalBasic)
uint m_IndirectLightingMode;  // existing (will be 0 = None, or new DDGI mode)
float3 m_SunDirection;        // existing — for sun lighting direction
// No CSM matrices needed here — shadow is precomputed in the shadow mask
```

The deferred lighting shader only needs one new resource binding for the shadow mask:
```hlsl
Texture2D<float>             g_ShadowMask;   // R8_UNORM, screen resolution
```

In `DeferredLighting_PSMain`, the shadow factor is read with a single load:
```hlsl
lightingInputs.sunShadow = g_ShadowMask.Load(uint3(uvInt, 0));
```

The `DeferredRenderer::Setup()` resource declaration:
```cpp
// In DeferredRenderer::Setup():
if (g_Renderer.m_Mode == RenderingMode::NormalBasic)
{
    renderGraph.ReadTexture(g_RG_ShadowMask);     // Read shadow mask (R8)
    renderGraph.ReadTexture(g_RG_DDGIIndirect);   // Read DDGI indirect output
}
```

---

## 7. Transparent Lighting in NormalBasic — No TLV

The [Translucency Lighting Volume (TLV)](implementation_plan_restir_sharc_transparent.md) is an **advanced-only feature** that bakes ReSTIR DI stochastic direct lighting and SHARC indirect GI into a 3D volume grid, which transparent objects trilinearly sample in the forward pass. It is **excluded from NormalBasic** for the following reasons:

| Dependency | TLV Requirement | NormalBasic Status |
|---|---|---|
| ReSTIR DI RIS buffers (`g_RG_RISBuffer`, `g_RG_RISLightDataBuffer`) | TLV injection pass reads RIS candidate lights | ❌ RTXDIRenderer skipped — no RIS buffers |
| SHARC resolved radiance cache (`g_RG_SHARCResolved`) | TLV injection pass queries SHARC for indirect | ❌ SHARCRenderer skipped — no radiance cache |
| RTXDI + SHARC render passes | Must run before TLV injection | ❌ Both disabled |

Since all TLV data sources are unavailable in NormalBasic, the injection and resolve compute passes cannot run. The transparent forward pass falls back to a simplified path:

```hlsl
// NormalBasic transparent forward lighting (conceptual):
// ── Direct lighting: sun direction + CSM shadow mask sample ──
float sunShadow = g_ShadowMask.SampleLevel(linearSampler, screenUV, 0).r;
float3 directDiffuse = EvaluateSunLight(baseColor, normal, sunDirection) * sunShadow;

// ── Indirect lighting: DDGI probe query ──
float3 ddgiIndirect = DDGIGetVolumeIrradiance(worldPos, surfaceBias, normal, volumes);
float3 indirectGI = ddgiIndirect * baseColor * (1.0 - metallic);

// ── Final ──
float3 color = directDiffuse + indirectGI;
```

**Summary of differences:**

| Aspect | NormalAdvanced (TLV) | NormalBasic (no TLV) |
|---|---|---|
| **Direct light source** | ReSTIR DI RIS importance-sampled from all scene lights | Single directional sun (analytic) |
| **Direct shadows** | Baked into TLV from stochastic samples | CSM shadow mask (R8 screen-space) |
| **Indirect GI** | SHARC 2–4 bounce diffuse GI baked into volume | DDGI probe-based indirect (baked or live) |
| **Light count scaling** | O(1) regardless of scene light count | O(1) — always 1 sun |
| **Per-pixel cost** | 2 trilinear texture samples (~16 taps) | 1 shadow mask sample + 1 DDGI probe query |
| **Quality** | Stochastic many-light + SHARC GI | Single sun + DDGI ambient — sufficient for a classic raster pipeline |
| **GPU memory** | +8.5 MB (64³ volume) | +0 MB (no TLV resources) |

**Design rationale:** NormalBasic targets a **classic rasterizer pipeline** (CSM + DDGI). The TLV is an advanced stochastic-lighting feature that depends on ReSTIR DI and SHARC — both of which are explicitly disabled in NormalBasic. Transparent objects in NormalBasic receive a simplified but correct lighting model: sun direct lighting (with CSM shadow) plus DDGI indirect ambient. This is consistent with the mode's philosophy of trading advanced stochastic quality for simplicity and deterministic performance.

> **Note:** The existing brute-force `AccumulateDirectLighting` loop in [BasePass.hlsl](../src/shaders/BasePass.hlsl) iterates over `g_PerFrame.m_LightCount` scene lights. In NormalBasic, this loop could still run but would evaluate at most the sun (1 light), making the O(N) cost trivial. Alternatively, the transparent PS could branch on `RENDERING_MODE_NORMAL_BASIC` to use the simplified sun-only path above, bypassing the polymorphic light loop entirely.

---

## 8. Implementation Roadmap

### Phase 1: Enum & Skeleton

1. Add `RENDERING_MODE_NORMAL_BASIC = 3` to [Common.sr](../src/shaders/Common.sr)
2. Rename `RENDERING_MODE_NORMAL` → `RENDERING_MODE_NORMAL_ADVANCED` in Common.sr
3. Update `RenderingMode` enum in [Renderer.h](../src/Renderer.h)
4. Add `NormalBasic` branch in `ScheduleAndRunAllRenderers()` in [Renderer.cpp](../src/Renderer.cpp)
5. Update ImGui combo in [ImGuiLayer.cpp](../src/ImGuiLayer.cpp)
6. Add feature flag `m_EnableReSTIRDI = false`, `m_EnableRTShadows = false`, `m_IndirectLightingTechnique = 0` when switching to NormalBasic

### Phase 1.5: CSM Debug View Modes 🔍

> **Rationale:** Debug views must be implemented **immediately after** the first shadow map is produced. Without visual feedback, shadow quality issues (bias, acne, cascade seams, PCF artifacts) are essentially undiagnosable. Every subsequent phase benefits from these diagnostics.

**Deliverable:** A single ImGui `CSM Debug` dropdown that cycles through 8 debug modes (see §4.13). This should be implemented as soon as `ShadowRenderer` produces its first depth map and `ShadowMaskRenderer` writes the first R8 mask — even before the deferred lighting shader consumes them.

1. Add `uint m_CSMDebugMode` to `CommonConsts` in [Common.sr](../src/shaders/Common.sr) — maps to `CSM_DEBUG_OFF = 0` through `CSM_DEBUG_BLEND_ZONE = 8`
2. Add `uint32_t m_CSMDebugMode = 0` to `Renderer` struct in [Renderer.h](../src/Renderer.h)
3. Create `src/shaders/CSMDebug.hlsli` with all 8 debug mode functions (uses `CommonShadow.hlsli` for CSM sampling)
4. Create `src/CSMDebugRenderer.h` / `src/CSMDebugRenderer.cpp` — a lightweight `IRenderer` that:
   - Reads `g_RG_CSMShadowMap` (depth array), `g_RG_ShadowMask` (R8), `g_RG_DepthTexture`, GBuffer textures
   - Writes a debug visualization to a temporary RGBA8 RT (or composites directly into the final output)
   - Returns early (`Setup()` returns false) when `m_CSMDebugMode == CSM_DEBUG_OFF`
5. Wire into `ScheduleAndRunAllRenderers()` — schedule `g_CSMDebugRenderer` **after** `ShadowMaskRenderer` and **before** `DeferredRenderer` (or as a post-process overlay after `DeferredRenderer`)
6. Add ImGui combo in [ImGuiLayer.cpp](../src/ImGuiLayer.cpp):
   ```cpp
   static const char* kCSMDebugModes[] = { "Off", "Cascade Splits", "Shadow Map Array",
       "Raw Shadow Mask", "PCF Footprint", "Alpha-Masked Overlay",
       "Depth Compare", "Frustum Wireframe", "Blend Zone" };
   ImGui::Combo("CSM Debug", &m_CSMDebugMode, kCSMDebugModes, IM_ARRAYSIZE(kCSMDebugModes));
   ```
7. Implement per-mode visualization:
   - **Mode 1 (Cascade Splits):** Color-tint scene by cascade index. Implemented in `DeferredLighting` debug path or as a dedicated overlay.
   - **Mode 2 (Shadow Map Array):** 4-up depth-map quads in screen corners. Sample each slice of the `Texture2DArray`.
   - **Mode 3 (Raw Shadow Mask):** Display `g_ShadowMask` as grayscale.
   - **Mode 4 (PCF Footprint):** RGB: R=kernel influence (9-tap vs 1-tap diff), G=9-tap shadow, B=1-tap shadow.
   - **Mode 5 (Alpha-Masked Overlay):** Orange tint on pixels from the masked geometry path.
   - **Mode 6 (Depth Compare):** Red=shadowed, Green=lit. Direct visualization of the depth comparison.
   - **Mode 7 (Frustum Wireframe):** CPU-computed frustum corners submitted as ImGui 3D lines.
   - **Mode 8 (Blend Zone):** Magenta overlay on pixels in the cascade transition blend band.

### Phase 2: ShadowRenderer (CSM) + ShadowMaskRenderer

1. Create `src/ShadowRenderer.h` / `src/ShadowRenderer.cpp`
2. Declare shadow map texture array (2048×2048×4, D32_FLOAT)
3. Implement cascade split computation (CPU-side)
4. Implement per-cascade VP matrix computation + texel snapping
5. Reuse `GPUCulling` pipeline with shadow VP matrices → depth-only meshlet draws
6. Create **two** shadow mesh shader variants:
   - Opaque: position-only output, null PS (fast path)
   - Masked: position + UV output, alpha-test PS that samples albedo texture and discards (for grass/foliage)
7. Render opaque bucket first (null PS), then masked bucket (alpha-test PS) per cascade
8. Create `src/ShadowMaskRenderer.h` / `src/ShadowMaskRenderer.cpp`
   - Declare `R8_UNORM` shadow mask RT (screen resolution)
   - Fullscreen compute shader: read depth → reconstruct world pos → `ComputeCSMShadow()` → write R8
   - Read `g_RG_CSMShadowMap` + `g_RG_DepthTexture`, write shadow mask
9. Create `src/shaders/CommonShadow.hlsli` with `ComputeCSMShadow()` — cascade selection + 3×3 PCF
10. Create `src/shaders/ShadowMask.hlsl` — compute shader using `CommonShadow.hlsli`
11. Wire `g_RG_ShadowMask` into `DeferredRenderer`: single `g_ShadowMask.Load(uvInt).r` in `DeferredLighting_PSMain`

### Phase 3: DDGIRenderer (next phase — placeholder)

1. Integrate RTXGI-DDGI SDK
2. Create DDGI volumes + indirect query pass
3. Wire `g_RG_DDGIIndirect` into `DeferredRenderer`
*(Full details in [DDGI_Analysis.md](DDGI_Analysis.md))*

### Phase 4: Integration & Polish

1. Modify `DeferredRenderer` to conditionally use CSM + DDGI in NormalBasic mode
2. Add ImGui controls for shadow quality settings:
   - PCSS checkbox + kernel radius slider
   - Shadow map resolution toggle
3. Profile and tune cascade splits, shadow map resolution, PCSS vs fixed-kernel PCF quality/performance tradeoff
4. Test corner cases: thin geometry, alpha-masked foliage shadows, moving light direction

---

## Appendix A: File Index

| File | Purpose |
|---|---|
| [Renderer.h](../src/Renderer.h) | `IRenderer` base, `RenderingMode` enum, `Renderer` struct with all feature flags |
| [Renderer.cpp](../src/Renderer.cpp) | `ScheduleAndRunAllRenderers()` — pass scheduling logic |
| [Common.sr](../src/shaders/Common.sr) | `CommonConsts` HLSL constants including RENDERING_MODE_* |
| [DeferredRenderer.cpp](../src/DeferredRenderer.cpp) | Deferred lighting pass — needs shadow mask input |
| [DeferredLighting.hlsl](../src/shaders/DeferredLighting.hlsl) | Lighting shader — needs CSM + DDGI inputs |
| [DeferredLighting.sr](../src/shaders/DeferredLighting.sr) | Resource bindings for deferred lighting |
| [BasePassRenderer.cpp](../src/BasePassRenderer.cpp) | Opaque/Masked/Transparent renderers — reference for shadow depth-only pass |
| [GPUCulling.hlsl](../src/shaders/GPUCulling.hlsl) | GPU frustum+LOD+occlusion culling — reusable for shadows |
| [BasePassCommon.h](../src/BasePassCommon.h) | `BasePassResources` struct — reference for shadow renderer resources |
| [CommonResources.h](../src/CommonResources.h) | Global samplers, default textures, blend states |
| [RTXDIRenderer.cpp](../src/RTXDIRenderer.cpp) | ReSTIR DI + GI — to be skipped in NormalBasic |
| [SHARCRenderer.cpp](../src/SHARCRenderer.cpp) | SHARC indirect — to be skipped in NormalBasic |
| [NrdIntegration.h](../src/NrdIntegration.h) | NRD denoiser — to be skipped in NormalBasic |
| [ImGuiLayer.cpp](../src/ImGuiLayer.cpp) | UI controls — needs RenderingMode combo + CSM Debug combo |
| [ShadowRenderer.h](../src/ShadowRenderer.h) | **NEW** — CSM depth-only raster renderer per cascade |
| [ShadowMaskRenderer.h](../src/ShadowMaskRenderer.h) | **NEW** — fullscreen compute: CSM evaluation → R8 shadow mask |
| [CSMDebugRenderer.h](../src/CSMDebugRenderer.h) | **NEW** — CSM debug visualization overlay (8 modes) |
| [CommonShadow.hlsli](../src/shaders/CommonShadow.hlsli) | **NEW** — shared CSM evaluation: cascade selection + normal-offset bias (§4.12) + 3×3 PCF |
| [ShadowMask.hlsl](../src/shaders/ShadowMask.hlsl) | **NEW** — compute shader for R8 shadow mask |
| [CSMDebug.hlsli](../src/shaders/CSMDebug.hlsli) | **NEW** — 8 debug visualization modes for CSM |

## Appendix B: Key Design Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Shadow technique | CSM (4 cascades) | Well-understood, no RT hardware needed for shadows |
| Shadow map layout | Texture2DArray, 2048×2048×4 | Clean API, independent cascade resolution |
| Shadow culling | Frustum only (no HZB) | Depth-only draws are fast; HZB overhead isn't worth it |
| Shadow alpha masking | Split-pass: opaque → null PS, masked → alpha-test PS | Grass/foliage must cast proper silhouetted shadows; opaque stays fast |
| Shadow sampling | Shadow mask (R8_UNORM screen-space RT) | Separate pass: CSM → shadow mask compute → deferred lighting reads R8; modular, filterable, debuggable |
| Shadow filtering | Hardware PCF (4-tap bilinear) + 3×3 PCF (default) | Free on hardware comparison samplers; 9-tap PCF for smooth edges |
| Shadow stability | Texel snapping (quantize VP matrix) | Essential for temporal stability |
| Shadow bias | Normal-offset bias (shader) + rasterizer-level (PSO) | Normal-offset pushes sample along surface normal; bounded max offset prevents Peter Panning; standard in Unreal VSM; see §4.12 |
| Indirect GI | DDGI (next phase) | Baked or live probe-based GI; separate document |
| Transparent lighting | Simplified forward (sun + DDGI indirect, no TLV) | TLV depends on ReSTIR + SHARC which are disabled |
| TLAS | Not needed for CSM phase | CSM is pure raster; TLAS only needed later for DDGI probe rays |
| Debug views | 8-mode ImGui dropdown (Phase 1.5) | Cascade splits, shadow map viz, PCF footprint, alpha overlay, depth compare, frustum wireframe, blend zone — critical for diagnosing all shadow quality issues |
```
