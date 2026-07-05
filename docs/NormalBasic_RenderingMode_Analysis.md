# NormalBasic Rendering Mode — Analysis & Implementation Plan

> **Date:** 2026-07-03  
> **Status:** Analysis Phase — No Code Modified Yet  
> **Goal:** Replace ReSTIR DI with a classic rasterizer pipeline (CSM + DDGI) in a new `NormalBasic` mode

---

## Table of Contents

1. [Codebase Architecture Overview](#1-codebase-architecture-overview)
2. [RenderingMode Enum & CommonConsts Changes](#2-renderingmode-enum--commonconsts-changes)
3. [Render Pass Scheduling — Current vs NormalBasic](#3-render-pass-scheduling--current-vs-normalbasic)
4. [Cascaded Shadow Maps (CSM) — Deep Dive](#4-cascaded-shadow-maps-csm--deep-dive)
   - 4.11 [Alpha-Masked Geometry in Shadow Pass (Grass & Foliage)](#411-alpha-masked-geometry-in-shadow-pass-grass--foliage)
   - 4.12 [Shadow Mask (Screen-Space R8 Texture)](#412-shadow-mask-screen-space-r8-texture)
5. [RTXGI-DDGI — Deep Dive](#5-rtxgi-ddgi--deep-dive)
6. [Disabled Features & Impact Analysis](#6-disabled-features--impact-analysis)
7. [Implementation Roadmap](#7-implementation-roadmap)

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

Found in [Renderer.cpp](src/Renderer.cpp) ~lines 817-848:

```
ClearRenderer → TLASRenderer → OpaqueRenderer → MaskedPassRenderer
→ HZBGeneratorPhase2 → RTXDIRenderer → SHARCRenderer
→ DeferredRenderer → SkyRenderer → TransparentPassRenderer
→ TAARenderer → BloomRenderer → HDRRenderer → ImGuiRenderer
```

### 1.3 Key Feature Flags (Renderer struct)

From [Renderer.h](src/Renderer.h) lines 221-310:

| Flag | Type | Default | Purpose |
|---|---|---|---|
| `m_Mode` | `RenderingMode` | `Normal` | Current rendering mode |
| `m_UseMeshletRendering` | `bool` | `true` | GPU-driven meshlet pipeline |
| `m_EnableRTShadows` | `bool` | `true` | RT inline ray query shadows |
| `m_EnableReSTIRDI` | `bool` | `true` | ReSTIR direct illumination |
| `m_EnableReSTIRDenoising` | `bool` | `true` | NRD REBLUR denoising |
| `m_IndirectLightingTechnique` | `uint32_t` | `2` (SHARC) | 0=None, 1=ReSTIR GI, 2=SHARC |
| `m_EnableOcclusionCulling` | `bool` | `true` | 2-phase HZB occlusion |
| `m_EnableSky` | `bool` | `true` | Sky/atmosphere rendering |
| `m_EnableDDGI` | `bool` | `true` | DDGI probe RT + blending; when disabled, only indirect query from baked probes runs (no RT) |

### 1.4 Dependencies of Features to Disable

| Feature | Where Used | Dependency Chain |
|---|---|---|
| **ReSTIR DI** | `RTXDIRenderer`, `CompositingPass` | ReSTIR DI → NRD → Denoised output → DeferredRenderer |
| **ReSTIR GI** | `RTXDIRenderer` (GI pass) | ReSTIR GI → NRD → Denoised output |
| **SHARC** | `SHARCRenderer` (Update/Resolve/Query) | SHARC → g_RG_SHARCIndirect → DeferredRenderer |
| **OMM** | `RTXDIRenderer` (BLAS with OMM), nvrhi OMM paths | OMM → BLAS build (RT only; rasterizer doesn't need OMM) |
| **NRD** | `NrdIntegration` → `RTXDIRenderer` | NRD denoising for ReSTIR DI & GI outputs |
| **TLAS** | `TLASRenderer`, `RTXDIRenderer`, `PathTracerRenderer` | Only used for RT shadows, ReSTIR DI/GI, SHARC |

**Key Insight:** All disabled features are RT-dependent. None affect the rasterized G-Buffer pass, HZB, deferred lighting, sky, transparency, TAA, or post-processing passes. This makes the split clean.

---

## 2. RenderingMode Enum & CommonConsts Changes

### 2.1 Current State

**Host-side** ([Renderer.h](src/Renderer.h) line 82):
```cpp
enum class RenderingMode : uint32_t
{
    Normal = srrhi::CommonConsts::RENDERING_MODE_NORMAL,          // = 0
    IBL = srrhi::CommonConsts::RENDERING_MODE_IBL,                // = 1
    ReferencePathTracer = srrhi::CommonConsts::RENDERING_MODE_PATH_TRACER  // = 2
};
```

**Shader-side** ([Common.sr](src/shaders/Common.sr) line 85):
```hlsl
static const int RENDERING_MODE_NORMAL = 0;
static const int RENDERING_MODE_IBL = 1;
static const int RENDERING_MODE_PATH_TRACER = 2;
```

### 2.2 Required Changes

1. **Rename `RENDERING_MODE_NORMAL` → `RENDERING_MODE_NORMAL_ADVANCED`** (value stays 0)
2. **Add `RENDERING_MODE_NORMAL_BASIC = 3`** (new value)
3. Update both [Renderer.h](src/Renderer.h) and [Common.sr](src/shaders/Common.sr)
4. Update ImGui combo in [ImGuiLayer.cpp](src/ImGuiLayer.cpp):
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
    // NEW
    m_RenderGraph.ScheduleRenderer(g_ShadowRenderer);       // CSM depth-only pass
    m_RenderGraph.ScheduleRenderer(g_DDGIRenderer);          // DDGI: probe RT+blend (if m_EnableDDGI) + indirect query (always)
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

| Pass | NormalAdvanced | NormalBasic | Notes |
|---|---|---|---|
| ClearRenderer | ✅ | ✅ | |
| TLASRenderer | ✅ | ❌ | RT-only; skipped |
| OpaqueRenderer | ✅ | ✅ | |
| MaskedPassRenderer | ✅ | ✅ | |
| HZBGeneratorPhase2 | ✅ | ✅ | |
| **ShadowRenderer** | ❌ | ✅ | **NEW** — CSM: renders opaque (null PS) + masked (alpha-test PS) buckets |
| **ShadowMaskRenderer** | ❌ | ✅ | **NEW** — fullscreen compute: evaluates CSM → writes R8 shadow mask |
| **DDGIRenderer** | ❌ | ✅ | **NEW** — probe RT + blend (if `m_EnableDDGI`) + indirect query (always); see §5.13 Bake Mode |
| RTXDIRenderer | ✅ | ❌ | Disabled |
| SHARCRenderer | ✅ (if enabled) | ❌ | Disabled |
| DeferredRenderer | ✅ | ✅ | Modified — uses shadow map & DDGI indirect |
| SkyRenderer | ✅ | ✅ | |
| TransparentPassRenderer | ✅ (with TLV — ReSTIR DI + SHARC GI) | ✅ (simplified — no TLV; see §6.4) | TLV requires ReSTIR DI + SHARC, both disabled in NormalBasic |
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

- **GPU Culling**: [GPUCulling.hlsl](src/shaders/GPUCulling.hlsl) already performs frustum culling, LOD selection, visibility counting, and indirect argument generation. This can be reused with a shadow-specific view-proj matrix.
- **Meshlet Rendering**: [BasePassRenderer.cpp](src/BasePassRenderer.cpp) lines 420-443 show the meshlet dispatch pipeline. A shadow variant would use a **null PS** (no pixel shader at all) — only the depth target is written via the rasterizer's depth output.
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

**Use per-cascade light-frustum culling only.** The existing GPU culling infrastructure ([GPUCulling.hlsl](src/shaders/GPUCulling.hlsl)) already performs frustum culling — simply pass the shadow VP matrix as `m_ViewProj` in the `CullingConstants`. No occlusion culling or HZB is needed for shadow passes; depth-only draws with a null PS are extremely fast and the additional overhead of HZB generation per cascade is not justified.

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

#### 4.8.1 Hardware PCF (4-tap) — ✅ Enabled by default

D3D12 hardware comparison samplers perform **4-tap bilinear PCF automatically** in a single `SampleCmpLevelZero` call:

```hlsl
SamplerComparisonState g_ShadowSampler : register(s1);
// Create with D3D12_COMPARISON_FILTER_COMPARISON_LINEAR

float shadow = g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, uv, depth);
```

This is the fastest option — zero extra ALU cost, fully hardware-accelerated.

#### 4.8.2 3×3 PCF (9-tap) — ✅ Enabled by default

Wider kernel for softer, more stable shadows:

```hlsl
float shadow = 0;
for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++)
        shadow += g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, uv + offset, depth);
shadow /= 9.0;
```

Each tap is a hardware-accelerated comparison sample (4-tap bilinear underneath). The 9 outer taps produce a smooth falloff equivalent to a 6×6 sample footprint at minimal ALU cost. This is the default behavior for NormalBasic mode.

#### 4.8.3 PCSS (Percentage-Closer Soft Shadows) — ❌ Disabled by default

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

#### Problem

The depth-only shadow pass described in §4.2 uses a **null PS** (no pixel shader) — the hardware rasterizer writes depth automatically. This works perfectly for opaque geometry, but **alpha-masked geometry** (grass, foliage, fences, tree leaves, etc.) renders as a **solid opaque quad** in the shadow map. The result: grass and foliage cast full-rectangle shadows instead of respecting their alpha-masked silhouette.

In the existing NormalAdvanced pipeline this is not an issue because RT shadows (inline ray queries) use the `AnyHit` shader which performs alpha testing via `AlphaTest()` in [RaytracingCommon.hlsli](src/shaders/RaytracingCommon.hlsli). For NormalBasic's rasterized CSM, we must handle this differently.

#### Existing Infrastructure

The codebase already has all the pieces needed:

1. **Scene already separates instances by alpha mode** — `m_OpaqueBucket` and `m_MaskedBucket` in `Scene` struct. The `MaskedPassRenderer` renders instances from the masked bucket with `ALPHA_MODE_MASK`.

2. **Alpha-test pixel shader already exists** — `GBuffer_PSMain_AlphaTest` in [BasePass.hlsl](src/shaders/BasePass.hlsl) (lines 284-286) samples the albedo texture, computes alpha, and calls `discard` when `alpha < mat.m_AlphaCutoff`:

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

#### Solution: Two-Pass Shadow Rendering

Split the shadow depth pass into two draw batches per cascade:

```
For each cascade:
  1. Draw OPAQUE instances   → null PS (depth-only, no material lookups)
  2. Draw MASKED instances   → alpha-test PS (samples albedo, discards below cutoff)
```

This way opaque geometry stays fast (null PS, zero texture bandwidth), and only masked geometry pays the cost of texture sampling + alpha test.

#### Shader Variant Strategy

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

#### Performance Considerations

| Aspect | Impact |
|---|---|
| **Opaque instances** | Zero change — still null PS, no texture reads |
| **Masked instances** | Additional cost: albedo texture reads + alpha compare + `discard` |
| **Texture bandwidth** | Only paid for masked geometry in the shadow pass; these are typically a small fraction of total geometry |
| **Quad utilization** | `discard` in a pixel shader reduces quad utilization on GPUs (4-pixel quads); masked geometry already has poor utilization in the G-Buffer pass, so this is not a new regression |
| **Culling** | Masked instances still culled against the light frustum, so only visible masked geometry triggers the alpha-test shader |

#### Design Decision

**Use the split-pass approach (opaque → null PS, masked → alpha-test PS).** This is the standard approach used by Unreal Engine, Unity, and virtually all production renderers with CSM. The alternative (no alpha testing in shadows → solid blocky shadows for foliage) produces unacceptable visual quality and is not a viable option for any scene with vegetation.

---

### 4.12 Shadow Mask (Screen-Space R8 Texture)

For the NormalBasic implementation with **1 directional light**, shadows are computed via a **screen-space shadow mask** — a separate fullscreen pass that evaluates CSM and writes per-pixel visibility to an `R8_UNORM` render target. The deferred lighting shader then reads this precomputed value with a single texture load.

#### Pipeline

```
Frame:
  1. ShadowRenderer → writes CSM depth array (4 × 2048², D32_FLOAT)
  2. ShadowMaskRenderer → fullscreen compute/pixel pass:
       for each pixel:
         - read depth from GBuffer, reconstruct world pos
         - select cascade by view-space depth
         - SampleCmpLevelZero() with PCF kernel (3×3 = 9 taps)
         - write shadow factor (0..1) to R8_UNORM RT
  3. DeferredRenderer → fullscreen PS:
       shadow = g_ShadowMask.Load(uvInt).r;  // single R8 load, no CSM sampling
       color = lighting * shadow;
```

#### ShadowMaskRenderer Design

```cpp
class ShadowMaskRenderer : public IRenderer
{
    RGTextureHandle m_ShadowMask;  // R8_UNORM, screen resolution

    bool Setup(RenderGraph& renderGraph) override
    {
        renderGraph.ReadTexture(g_RG_CSMShadowMap);   // CSM depth array
        renderGraph.ReadTexture(g_RG_DepthTexture);   // reconstruct world pos
        renderGraph.WriteTexture(m_ShadowMask);       // output shadow factor
        return true;
    }
};
```

#### Shadow Mask Compute Shader

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
    
    // Reconstruct world position
    float2 uv = (float2(uvInt) + 0.5f) / g_Constants.m_OutputSize;
    float4 clipPos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
    float4 worldPos4 = mul(clipPos, g_Constants.m_ClipToWorld);
    float3 worldPos = worldPos4.xyz / worldPos4.w;
    
    // Cascade selection + shadow evaluation
    float shadow = ComputeCSMShadow(worldPos, worldPosViewZ,
        g_CSMShadowMap, g_ShadowSampler,
        g_Constants.m_ShadowViewProj, g_Constants.m_CascadeSplits);
    
    g_RWShadowMask[uvInt] = shadow;
}
```

#### Advantages

| Aspect | Benefit |
|---|---|
| **Modularity** | Shadow evaluation is a separate, independently debuggable pass |
| **Deferred lighting simplicity** | Lighting shader reads 1× R8 instead of 9× depth comparison samples + cascade logic |
| **Filtering flexibility** | Shadow mask can be post-processed: PCSS with variable kernel sizes, temporal accumulation — without touching the lighting shader |
| **Cache behavior** | CSM depth reads are localized to the shadow mask pass; deferred lighting pass only reads GBuffer + R8 mask |
| **Multi-light ready** | When multiple shadow-casting lights are added, each contributes to the same mask or separate masks; lighting pass reads precomputed results |
| **Half-res option** | Shadow mask can be computed at half resolution and bilinearly upsampled for performance |
| **Debugging** | Trivially visualize the shadow mask RT (e.g., in ImGui) |

#### Resource Specification

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

#### Implementation Note

The CSM shadow evaluation logic lives in a shared HLSL header [CommonShadow.hlsli](src/shaders/CommonShadow.hlsli) (new file), used by both `ShadowMask_CS` and any future passes that need CSM access:

```hlsl
// In CommonShadow.hlsli (new file)
float ComputeCSMShadow(
    float3 worldPos,
    float viewDepth,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler,
    float4x4 shadowViewProj[4],
    float4 cascadeSplits)
{
    uint cascadeIndex = SelectCascade(viewDepth, cascadeSplits);
    float4 lightSpacePos = mul(float4(worldPos, 1.0f), shadowViewProj[cascadeIndex]);
    float3 shadowUV = lightSpacePos.xyz / lightSpacePos.w;
    shadowUV.xy = shadowUV.xy * 0.5f + 0.5f;
    shadowUV.y = 1.0f - shadowUV.y;

    // Out of bounds → fully lit
    if (any(shadowUV.xy < 0.0f) || any(shadowUV.xy > 1.0f))
        return 1.0f;

    // 3×3 PCF
    float shadow = 0.0f;
    float2 texelSize = 1.0f / 2048.0f;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            shadow += shadowMap.SampleCmpLevelZero(
                shadowSampler,
                float3(shadowUV.xy + float2(x, y) * texelSize, cascadeIndex),
                shadowUV.z - SHADOW_BIAS);
    return shadow / 9.0f;
}
```

---

## 5. RTXGI-DDGI — Deep Dive

### 5.1 How DDGI Works (From Reference Study)

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

### 5.2 Reference Sample Analysis

The test harness ([DDGI_D3D12.cpp](REFERENCES/RTXGI-DDGI/samples/test-harness/src/graphics/DDGI_D3D12.cpp)) shows:
- **Single large volume** covering the entire map (Cornell Box / Sponza)
- **Unmanaged resource mode** — application creates textures, SDK creates PSOs
- **Bindless resource access** — resources accessed via descriptor heap indices
- **Probe ray tracing** uses inline ray tracing (`RayQuery`) in a compute shader, not a ray generation shader. This avoids DXR hit groups and state objects entirely — all DDGI RT is done from a pure CS with a single `Dispatch()` call.
- **Indirect lighting query** in a fullscreen compute shader ([IndirectCS.hlsl](REFERENCES/RTXGI-DDGI/samples/test-harness/shaders/IndirectCS.hlsl))

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

### 5.3 Texture Formats

DDGI uses these texture arrays:

| Resource | Format | Dimensions | Notes |
|---|---|---|---|
| Ray Data | RGBA32_FLOAT or RG32_FLOAT | `(probesPerPlane, numRays, numPlanes)` | Temporary, per-frame |
| Irradiance | R10G10B10A2_UNORM or RGBA16_FLOAT | `(probeTexels * probesX, probeTexels * probesZ, probesY)` | Persistent, gamma-encoded |
| Distance | RG16_FLOAT | `(probeTexels * probesX, probeTexels * probesZ, probesY)` | Persistent |
| Probe Data | RGBA16_FLOAT | `(probesX, probesZ, probesY)` | Offsets + classification |
| Variability | R16_FLOAT | Like irradiance (no border) | Per-texel coefficient of variation |
| Variability Avg | RG16_FLOAT | Reduction pyramid | Average across volume |

### 5.4 Volume Placement Strategy

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

### 5.5 Probe Update Strategies

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

### 5.6 Volume Culling

**Q: Frustum culling? Occlusion culling for volumes?**

**A:** 
- **Frustum culling:** `DDGIVolume::GetAxisAlignedBoundingBox()` → test against camera frustum. If volume is outside frustum, skip probe updates for that frame.
- **Occlusion culling:** More complex. Can use the HZB from the main camera pass. If all probes in a volume are behind occluders (as determined by HZB), skip the volume. **Not recommended for initial implementation** — frustum culling is sufficient.

### 5.7 Variability & Convergence

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

### 5.8 Indirect Diffuse vs Specular

**Q: Is DDGI only for indirect diffuse? What about indirect specular?**

**A:** DDGI is **diffuse-only**. It stores irradiance (hemispherical integral of incoming radiance weighted by cos θ). For specular indirect:
- Screen-space reflections (SSR) — cheap, works in NormalBasic
- Ray-traced reflections — requires RT support (not in NormalBasic scope)
- **Recommendation:** DDGI for indirect diffuse + SSR (if desired) for indirect specular

### 5.10 Non-Volume Areas (Far Distance)

**Q: What about indirect lighting for areas outside volumes?**

**A:** The SDK provides `DDGIGetVolumeBlendWeight()` which returns 1.0 inside the volume and decays to 0 outside based on distance. Use this to blend with:
- A constant ambient color (simple)
- A sky-derived ambient term (better — sample sky irradiance)
- An IBL probe (best but needs environment map)

For NormalBasic: blend with sky ambient outside DDGI volume. This is what the test harness does.

### 5.11 DDGI Integration Architecture for HobbyRenderer

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
- **TLAS not needed** in bake mode (see §6.1)

### 5.12 Ray Tracing Requirement for DDGI

**Critical note:** DDGI requires GPU ray tracing for probe ray tracing. The probe rays need to trace against the scene's TLAS/BLAS. This means:

- **TLAS is still needed** — but only for DDGI probes, not for shadows
- **BLAS must be built** — they already are (Scene.cpp builds them for meshlet LOD)
- **DXR 1.1 support is required** — DDGI probe rays use inline `RayQuery` in a **compute shader** (no raygen, no any-hit, no hit groups). This keeps everything as a pure `Dispatch()` call with a single compute PSO.

This is a key trade-off: NormalBasic removes ReSTIR DI/GI/SHARC but adds DDGI which still requires RT support. However, DDGI's RT cost is much lower:
- Fewer rays (probeCount × raysPerProbe vs per-pixel rays for ReSTIR)
- Inline RT is simpler and lighter than the DXR hit-group pipeline — no shader tables, no state objects for hit groups
- Can be amortized over multiple frames

### 5.13 DDGI Bake Mode — Use HWRT to Converge, Then Disable RT

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
- **TLAS is freed in bake mode:** When `m_EnableDDGI = false`, `TLASRenderer` can be skipped entirely (see §6.1). No BLAS rebuild, no TLAS update — pure raster pipeline.
- **Convergence detection:** The SDK provides `CalculateDDGIVolumeVariability()` and `ReadbackDDGIVolumeVariability()` (see §5.10). When variability drops below threshold across all volumes, probes are considered "converged enough." This can be used for automatic bake stop.

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
```

---

## 6. Disabled Features & Impact Analysis

### 6.1 Feature Dependency Map

```
NormalAdvanced (current Normal):
  TLAS ────────┬──→ RTXDIRenderer (ReSTIR DI + GI)
               │       └── NRD (REBLUR denoising)
               │       └── OMM (opacity micromaps)
               ├──→ SHARCRenderer (spatial hash radiance cache)
               │       └── requires TLAS for ray queries
               └──→ DeferredRenderer (RT shadows via inline ray queries)

NormalBasic (m_EnableDDGI = true):
  TLAS ────────┬──→ ShadowRenderer (depth-only raster, NO TLAS needed)
               └──→ DDGIRenderer (probe rays NEED TLAS)
  ───→ NO ReSTIR DI, NO ReSTIR GI, NO SHARC, NO NRD, NO OMM, NO TLV
  ───→ TransparentPassRenderer (simplified forward — sun + DDGI indirect only; no TLV)

NormalBasic (m_EnableDDGI = false) — baked GI mode:
  ───→ ShadowRenderer (depth-only raster)
  ───→ DDGIRenderer (indirect query only, NO TLAS)
  ───→ TransparentPassRenderer (simplified forward — sun + DDGI indirect only; no TLV)
  ───→ NO TLAS, NO ReSTIR DI, NO ReSTIR GI, NO SHARC, NO NRD, NO OMM, NO TLV
  ───→ Pure raster pipeline. Zero RT cost.
```

### 6.2 What Actually Gets Skipped

| Component | What Happens | Impact |
|---|---|---|
| `TLASRenderer` | **Conditional** — needed only when `m_EnableDDGI=true` (probe ray tracing). Skipped entirely in bake mode (`m_EnableDDGI=false`) — pure raster pipeline, zero RT cost |
| `RTXDIRenderer` | Entire renderer skipped (`Setup()` returns false when `m_EnableReSTIRDI=false`) | Saves: RIS buffer alloc, presampling, temporal resampling, spatial resampling, compositing, NRD denoising passes |
| `SHARCRenderer` | Entire renderer skipped | Saves: Update, Resolve, Query passes |
| `NrdIntegration` | Not instantiated | Saves: REBLUR denoiser, permanent/transient pools, PackNormalRoughness pass |
| OMM | Not built for BLAS (or built with `AllowOMM=false`) | Saves: OMM build time, OMM memory. Already handled by `RTXDIRenderer` skipping build flags |
| RT Shadows (`m_EnableRTShadows`) | Set to `false` in NormalBasic, use CSM instead | DeferredRenderer uses shadow map instead of inline ray queries |
| TLV (Translucency Lighting Volume) | Entirely skipped — TLV injection reads ReSTIR DI RIS buffers + SHARC resolved buffers, both unavailable in NormalBasic | Transparent objects use simplified forward lighting (sun + DDGI indirect only); see §6.4 |

### 6.3 DeferredRenderer Modifications

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

### 6.4 Transparent Lighting in NormalBasic — No TLV

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

> **Note:** The existing brute-force `AccumulateDirectLighting` loop in [BasePass.hlsl](src/shaders/BasePass.hlsl) iterates over `g_PerFrame.m_LightCount` scene lights. In NormalBasic, this loop could still run but would evaluate at most the sun (1 light), making the O(N) cost trivial. Alternatively, the transparent PS could branch on `RENDERING_MODE_NORMAL_BASIC` to use the simplified sun-only path above, bypassing the polymorphic light loop entirely.

---

## 7. Implementation Roadmap

### Phase 1: Enum & Skeleton

1. Add `RENDERING_MODE_NORMAL_BASIC = 3` to [Common.sr](src/shaders/Common.sr)
2. Rename `RENDERING_MODE_NORMAL` → `RENDERING_MODE_NORMAL_ADVANCED` in Common.sr
3. Update `RenderingMode` enum in [Renderer.h](src/Renderer.h)
4. Add `NormalBasic` branch in `ScheduleAndRunAllRenderers()` in [Renderer.cpp](src/Renderer.cpp)
5. Update ImGui combo in [ImGuiLayer.cpp](src/ImGuiLayer.cpp)
6. Add feature flag `m_EnableReSTIRDI = false`, `m_EnableRTShadows = false`, `m_IndirectLightingTechnique = 0` when switching to NormalBasic

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

### Phase 3: DDGIRenderer

1. Integrate RTXGI-DDGI SDK (`rtxgi-sdk/include`, `rtxgi-sdk/shaders`, `rtxgi-sdk/src`)
2. Create `src/DDGIRenderer.h` / `src/DDGIRenderer.cpp`
3. Compile DDGI SDK shaders with `RTXGI_DDGI_RESOURCE_MANAGEMENT=0` (unmanaged), bindless mode
4. Create 3 camera-following ISVs (near / medium / far) with staggered update intervals
5. Implement probe ray tracing (compute shader with inline `RayQuery` using existing TLAS)
6. Call SDK: `UpdateDDGIVolumeProbes()`, `RelocateDDGIVolumeProbes()`, `ClassifyDDGIVolumeProbes()`
7. Fullscreen CS pass calling `DDGIGetVolumeIrradiance()` → writes `g_RG_DDGIIndirect`
8. Wire `g_RG_DDGIIndirect` into `DeferredRenderer`
9. Add `m_EnableDDGI` flag (default `false`) in `Renderer.h` — controls probe RT + blending
10. Implement bake mode: when `m_EnableDDGI = false`, skip probe RT/blending but still run indirect query
11. Add ImGui checkbox for `m_EnableDDGI` + convergence indicators in `ImGuiLayer.cpp` (see §5.13)
12. Implement convergence detection: `CalculateDDGIVolumeVariability()` → auto-stop when below threshold

### Phase 4: Integration & Polish

1. Modify `DeferredRenderer` to conditionally use CSM + DDGI in NormalBasic mode
2. Add ImGui controls for shadow/DDGI quality settings:
   - PCSS checkbox + kernel radius slider
   - DDGI enable checkbox + convergence threshold + auto-stop
   - DDGI probe density presets (Low/Medium/High)
3. Profile and tune cascade splits, shadow map resolution, 3-ISV probe density, row slice divisors, PCSS vs fixed-kernel PCF quality/performance tradeoff
4. Test DDGI bake workflow: enable → wait for convergence → disable → verify zero RT cost + correct indirect
5. Test corner cases: moving camera with baked probes, thin geometry, outdoor scenes

---

## Appendix A: File Index

| File | Purpose |
|---|---|
| [Renderer.h](src/Renderer.h) | `IRenderer` base, `RenderingMode` enum, `Renderer` struct with all feature flags |
| [Renderer.cpp](src/Renderer.cpp) | `ScheduleAndRunAllRenderers()` — pass scheduling logic |
| [Common.sr](src/shaders/Common.sr) | `CommonConsts` HLSL constants including RENDERING_MODE_* |
| [DeferredRenderer.cpp](src/DeferredRenderer.cpp) | Deferred lighting pass, conditionally reads RTXDI/SHARC outputs |
| [DeferredLighting.hlsl](src/shaders/DeferredLighting.hlsl) | Lighting shader — needs CSM + DDGI inputs |
| [DeferredLighting.sr](src/shaders/DeferredLighting.sr) | Resource bindings for deferred lighting |
| [BasePassRenderer.cpp](src/BasePassRenderer.cpp) | Opaque/Masked/Transparent renderers — reference for shadow depth-only pass |
| [GPUCulling.hlsl](src/shaders/GPUCulling.hlsl) | GPU frustum+LOD+occlusion culling — reusable for shadows |
| [BasePassCommon.h](src/BasePassCommon.h) | `BasePassResources` struct — reference for shadow renderer resources |
| [CommonResources.h](src/CommonResources.h) | Global samplers, default textures, blend states |
| [RTXDIRenderer.cpp](src/RTXDIRenderer.cpp) | ReSTIR DI + GI — to be skipped in NormalBasic |
| [SHARCRenderer.cpp](src/SHARCRenderer.cpp) | SHARC indirect — to be skipped in NormalBasic |
| [NrdIntegration.h](src/NrdIntegration.h) | NRD denoiser — to be skipped in NormalBasic |
| [ImGuiLayer.cpp](src/ImGuiLayer.cpp) | UI controls — needs RenderingMode combo update |
| [DDGIVolume.h](REFERENCES/RTXGI-DDGI/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h) | DDGI volume API |
| [Integration.md](REFERENCES/RTXGI-DDGI/docs/Integration.md) | DDGI integration guide |
| [DDGIVolume.md](REFERENCES/RTXGI-DDGI/docs/DDGIVolume.md) | DDGI volume reference |
| [DDGI_D3D12.cpp](REFERENCES/RTXGI-DDGI/samples/test-harness/src/graphics/DDGI_D3D12.cpp) | DDGI test harness reference implementation |

## Appendix B: Key Design Decisions Summary

| Decision | Choice | Rationale |
|---|---|---|
| Shadow technique | CSM (4 cascades) | Well-understood, no RT hardware needed for shadows |
| Shadow map layout | Texture2DArray, 2048×2048×4 | Clean API, independent cascade resolution |
| Shadow culling | Frustum only (no HZB) | Depth-only draws are fast; HZB overhead isn't worth it |
| Shadow alpha masking | Split-pass: opaque → null PS, masked → alpha-test PS | Grass/foliage must cast proper silhouetted shadows; opaque stays fast |
| Shadow sampling | Shadow mask (R8_UNORM screen-space RT) | Separate pass: CSM → shadow mask compute → deferred lighting reads R8; modular, filterable, debuggable |
| Shadow filtering | Hardware PCF (4-tap bilinear) + 3×3 PCF (default) | Free on hardware comparison samplers; 9-tap PCF for smooth edges |
| Shadow stability | Texel snapping (quantize VP matrix) | Essential for temporal stability |
| DDGI volume count | 3 ISVs (near/medium/far, camera-following) | High detail near, ambient far; consistent ~930K rays/frame |
| DDGI near probe density | 20×12×20, 1.5m spacing | High-quality indirect for surfaces close to camera |
| DDGI medium probe density | 20×10×20, 4.0m spacing | Balanced mid-range coverage |
| DDGI far probe density | 20×10×20, 12.0m spacing | Low-frequency ambient for distant geometry |
| DDGI update strategy | Row slicing: near÷2, med÷2, far÷4 | All volumes updated every frame, consistent ray count, no bursts |
| DDGI volume culling | Frustum culling only | Simple, sufficient; skip volumes fully outside frustum |
| DDGI + far distance | Blend with sky ambient | No complex falloff needed |
| Indirect specular | Not in scope for NormalBasic | SSR can be added later if needed |
| DDGI operation mode | `m_EnableDDGI` flag: bake (default off) vs live (on) | Primary use case is baking: converge probes with HWRT, then disable RT; indirect query always runs from persistent textures |
| TLAS | Only needed when `m_EnableDDGI = true` | DDGI probe rays need TLAS; when disabled (bake mode), TLASRenderer is skipped entirely |
| DDGI bake convergence | SDK variability readback + auto-stop | Probes converge in 2-5 seconds; variability < 0.02 indicates "done" |
