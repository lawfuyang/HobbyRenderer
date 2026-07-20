# Renderer & Build Pipeline

This document describes how the HobbyRenderer rendering pipeline and build system work,
covering `IRenderer`, Render Graph, Resource Binding, `nvrhi`, `srrhi`, `ShaderIDs`,
and their interactions.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [IRenderer & Renderer Registry](#irenderer--renderer-registry)
3. [Render Graph](#render-graph)
4. [Resource Binding](#resource-binding)
5. [nvrhi — Rendering Hardware Interface](#nvrhi--rendering-hardware-interface)
6. [srrhi — Shader Resource RHI](#srrhi--shader-resource-rhi)
7. [ShaderIDs System](#shaderids-system)
8. [Command List Management](#command-list-management)
9. [TaskScheduler & Parallel Execution](#taskscheduler--parallel-execution)
10. [Bindless Texture & Sampler System](#bindless-texture--sampler-system)
11. [Pipeline & Binding Cache](#pipeline--binding-cache)
12. [Shader Hot Reload](#shader-hot-reload)
13. [Build Pipeline](#build-pipeline)

---

## Architecture Overview

The renderer is structured as a modular, data-driven frame graph system built on top of
**nvrhi** (NVIDIA Rendering Hardware Interface) and **D3D12**. Each frame follows a
well-defined sequence:

```
Per Frame:
  RenderGraph::Reset()
  RenderGraph::BeginSetup()
  for each IRenderer:
      IRenderer::Setup(RenderGraph)         ← declares resources, registers read/write access
      RenderGraph::ScheduleRenderer()
  RenderGraph::EndSetup()
  RenderGraph::Compile()                    ← resolves lifetimes, allocates / aliases resources
  TaskScheduler::ExecuteAllScheduledTasks() ← dispatches Render() on worker threads
  RenderGraph::PostRender()
```

The key idea: renderers **declare** what resources they need during `Setup()`, the render
graph **compiles** a schedule and allocates memory, then renderers **execute** GPU work
in parallel via `Render()`.

---

## IRenderer & Renderer Registry

Every rendering pass in the engine is a class derived from `IRenderer`
([Renderer.h](src/Renderer.h)). It defines a minimal virtual interface:

```cpp
class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual void Initialize() {}
    virtual void PostSceneLoad() {}
    virtual bool Setup(RenderGraph& renderGraph) { return false; }
    virtual void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) {}
    virtual const char* GetName() const { return "Unnamed Renderer"; }
    virtual bool IsBasePassRenderer() const { return false; }
    // ... profiling state (m_CPUTime, m_GPUTime, timer queries)
    // ... m_bPassEnabled (toggled per-frame by the render graph schedule)
    // ... m_bClearOnNextRender (for clearing persistent state on technique switch)
};
```

### Lifecycle of a Renderer

| Phase | Method | Purpose |
|---|---|---|
| Startup | `Initialize()` | One-time GPU resource creation (fonts, static buffers, etc.) |
| Scene Load | `PostSceneLoad()` | React to a new scene being loaded |
| **Per Frame** | `Setup(RenderGraph&)` | Declare transient/persistent resources and their read/write access |
| **Per Frame** | `Render(cmdList, RG)` | Record GPU commands; called on a worker thread with a dedicated command list |
| Per Frame | `m_bPassEnabled` | Set by the render graph based on whether `Setup()` returned `true` |

A renderer's `Setup()` returns `false` to skip itself for the current frame (e.g. occlusion
culling disabled, no ImGui draw data, etc.).

### Renderer Registration

Renderers are registered via the `REGISTER_RENDERER` macro:

```cpp
REGISTER_RENDERER(OpaqueRenderer);       // → creates global g_OpaqueRenderer pointer
REGISTER_RENDERER(ImGuiRenderer);       // → creates global g_ImGuiRenderer pointer
```

The macro uses the `RendererRegistry` class to store factory functions. At startup, the
renderer creates instances of every registered renderer via these factories. Global
pointers (`g_OpaqueRenderer`, etc.) are set to the raw pointer of the shared instance,
allowing the main scheduling loop to reference them directly.

### Current Renderer List

The render loop schedules renderers in a fixed order that enforces implicit data
dependencies (the render graph resolves the rest):

| Renderer | Role |
|---|---|
| `ClearRenderer` | Clears depth/G-buffers/color targets |
| `TLASRenderer` | Builds/updates the top-level acceleration structure |
| `OpaqueRenderer` | Base pass: GPU culling + mesh/meshlet raster → G-Buffer |
| `MaskedPassRenderer` | Alpha-tested geometry (2nd culling phase) |
| `HZBGeneratorPhase2` | Generates hierarchical Z-buffer for occlusion culling |
| `ShadowRenderer` | CSM depth-only pass: 4-cascade D32 array (4 × 2048²) via mesh shaders (NormalBasic only) |
| `ShadowMaskRenderer` | Fullscreen compute: CSM evaluation → R8 shadow mask (NormalBasic only) |
| `CSMDebugRenderer` | CSM debug overlay with 8 visualization modes (NormalBasic only) |
| `RTXDIRenderer` | ReSTIR DI + GI sampling & resampling |
| `SHARCRenderer` | SHARC cache-based indirect lighting |
| `DeferredRenderer` | Deferred lighting: combines G-Buffer + shadow mask (NormalBasic) or RTXDI/SHARC (Normal) + IBL |
| `SkyRenderer` | Atmospheric sky or IBL environment background |
| `TransparentPassRenderer` | Forward transparent rendering |
| `TAARenderer` | Temporal anti-aliasing (FSR3) |
| `BloomRenderer` | Bloom post-processing |
| `HDRRenderer` | Auto-exposure + tone mapping |
| `ImGuiRenderer` | ImGui debug UI rendering |
| `PathTracerRenderer` | Reference path tracer (alternative rendering mode) |

---

## Render Graph

The Render Graph ([RenderGraph.h](src/RenderGraph.h), [RenderGraph.cpp](src/RenderGraph.cpp))
is an automatic resource management system inspired by modern console/PC frame graph
architectures.

### Core Concepts

**Transient Resources**: Textures and buffers that only live for part of a frame. The
render graph allocates physical GPU memory from heaps and **aliases** non-overlapping
resources into the same memory region.

**Persistent Resources**: Resources that survive across frames (e.g. accumulation buffers,
reservoir buffers, exposure history). They are not aliased and are only freed when the
owning renderer stops declaring them for several frames.

**Resource Handles**: `RGTextureHandle` and `RGBufferHandle` are opaque integer indices
(not GPU resource pointers). The render graph maps them to `nvrhi::TextureHandle` /
`nvrhi::BufferHandle` only after `Compile()`.

### Frame Lifecycle

```
Reset()                     // Clear pass state from previous frame; retain allocated heaps
BeginSetup()                // Enter declaration phase
  ┣━ DeclareTexture/DeclareBuffer()         ← transient
  ┣━ DeclarePersistentTexture/Buffer()      ← persist across frames
  ┣━ ReadTexture/WriteTexture()             ← register access
  ┣━ ReadBuffer/WriteBuffer()
  ┣━ ScheduleRenderer(IRenderer*)           ← calls renderer->Setup() + marks m_bPassEnabled
EndSetup()
Compile()                   // Compute lifetimes, allocate/alias, resolve barriers
  ┣━ Builds per-pass read/write sets
  ┣━ Computes first-last use for each resource
  ┣━ Aliases non-overlapping resources into heaps
  ┣━ Generates global sync barrier flags for passes with aliased resources
ExecuteAllScheduledTasks()  // Dispatch parallel Render() calls on worker threads
PostRender()                // Decrement force-invalidate counter, cleanup
```

### Aliasing & Heaps

The render graph manages a pool of GPU heaps. During `Compile()`, it sub-allocates
memory from existing heaps (best-fit) or creates new ones. When two resources have
non-overlapping lifetimes, they can share the same heap block — this is **aliasing**.

Resources that are aliased require a **global sync barrier** (`insertGlobalSyncBarrier()`)
at the start of the first pass that uses them, inserted by the render graph automatically
via `InsertGlobalSyncBarriers()`.

### Resource Access Modes

During `Render()`, the renderer retrieves the actual GPU resource handles:

```cpp
nvrhi::TextureHandle tex = renderGraph.GetTexture(myRGHandle, RGResourceAccessMode::Read);
nvrhi::BufferHandle  buf = renderGraph.GetBuffer(myRGBufferHandle, RGResourceAccessMode::Write);
```

### Debug UI

A built-in ImGui debug view shows:
- Texture/buffer counts (total, allocated, aliased)
- Total texture and buffer memory usage
- Lifetime visualization (Gantt chart of resource lifetimes across passes)

---

## Resource Binding

The resource binding system has two layers: **nvrhi** provides the low-level binding
model, and **srrhi** generates C++/HLSL glue code that eliminates manual descriptor
construction.

### nvrhi Binding Model

nvrhi's binding model has two symmetrical halves:

1. **Binding Layouts** — declare which binding slots exist and their resource types
   (Texture_SRV, ConstantBuffer, Sampler, etc.). Layouts are immutable and used to
   create pipelines.

2. **Binding Sets** — provide actual resource pointers for a specific layout. Sets are
   also immutable and pre-fill GPU descriptors at creation time for minimal runtime
   overhead.

```
Pipeline = Shaders + BindingLayouts[] + RenderState
Draw/Dispatch = setGraphicsState/computeState(BindingSets[])
```

nvrhi supports **Volatile Constant Buffers** — small buffers written per-frame where
the driver manages versioning, and **Push Constants** (up to 128 bytes) for fast
per-draw/dispatch data.

### The Renderer's Binding Helpers

The `Renderer` class provides convenience helpers that combine layout creation and
pipeline caching:

```cpp
// Create a BindingSetDesc from srrhi-generated resource arrays:
static nvrhi::BindingSetDesc CreateBindingSetDesc(std::span<const srrhi::ResourceEntry> resources,
                                                    uint32_t pushConstantBytes);

template<typename SrInput>
static nvrhi::BindingSetDesc CreateBindingSetDesc(const SrInput& inputs);

// Get-or-create a binding layout from a set descriptor (cached):
nvrhi::BindingLayoutHandle GetOrCreateBindingLayoutFromBindingSetDesc(
    const nvrhi::BindingSetDesc& setDesc, uint32_t registerSpace = 0);

// Get-or-create a bindless layout (cached):
nvrhi::BindingLayoutHandle GetOrCreateBindlessLayout(const nvrhi::BindlessLayoutDesc& desc);
```

### RenderPassParams

The `RenderPassParams` struct bundles everything needed to issue a compute or graphics
pass:

```cpp
struct RenderPassParams
{
    nvrhi::CommandListHandle commandList;
    uint32_t shaderID;                          // ShaderID:: constant
    nvrhi::BindingSetDesc bindingSetDesc;        // primary binding set
    uint32_t registerSpace;
    std::vector<BindingSetDescAndRegisterSpace> additionalBindingSets;  // up to 8 total
    const void* pushConstants;
    size_t pushConstantsSize;
    bool bIncludeBindlessResources;              // automatically includes bindless texture/sampler tables
    ComputeDispatchParams dispatchParams;         // for compute: x, y, z or indirect buffer
    nvrhi::FramebufferHandle framebuffer;        // for graphics: render targets
    nvrhi::DepthStencilState* depthStencilState;
    nvrhi::BlendState::RenderTarget* blendState;
};

// Dispatch helpers:
void AddComputePass(const RenderPassParams& params);
void AddFullScreenPass(const RenderPassParams& params);
```

These helpers automatically:
- Resolve the shader from `shaderID`
- Create/lookup the binding layout
- Add the global bindless texture and sampler descriptor tables (when `bIncludeBindlessResources` is true)
- Create/lookup the pipeline (graphics, meshlet, or compute) via the pipeline cache
- Record the draw or dispatch call on the command list

---

## nvrhi — Rendering Hardware Interface

nvrhi (NVIDIA Rendering Hardware Interface) is a GPU abstraction library that sits
between the renderer and the native graphics API ([external/nvrhi](external/nvrhi)).

### Key Features Used by HobbyRenderer

| Feature | Usage |
|---|---|
| **D3D12 Backend** | The renderer uses Direct3D 12 exclusively |
| **Automatic Resource State Tracking** | nvrhi tracks texture/buffer states and inserts barriers |
| **Binding Layouts & Sets** | All shader resource binding goes through nvrhi's binding model |
| **Bindless Descriptor Tables** | Global texture array and sampler heap for bindless rendering |
| **Pipeline State Objects** | Graphics, Meshlet, and Compute PSOs with automatic caching |
| **Command Lists** | Deferred command list recording with multi-threaded submission |
| **Timer Queries** | GPU timing for each render pass |
| **Validation Layer** | Optional debug validation of API calls and resource states |

### Graphics Abstraction Layer (GraphicRHI)

The `GraphicRHI` class ([GraphicRHI.h](src/GraphicRHI.h)) wraps nvrhi device creation
and swapchain management:

```cpp
class GraphicRHI
{
    nvrhi::DeviceHandle m_NvrhiDevice;
    nvrhi::TextureHandle m_NvrhiSwapchainTextures[2];
    // Creates nvrhi device (D3D12), swapchain, handles window resize
    virtual void Initialize(SDL_Window* window) = 0;
    virtual bool AcquireNextSwapchainImage(uint32_t* outImageIndex) = 0;
    virtual bool PresentSwapchain(uint32_t imageIndex) = 0;
};
```

The concrete implementation is `D3D12GraphicRHI` (created via `CreateD3D12GraphicRHI()`).

---

## srrhi — Shader Resource RHI

srrhi (Shader Resource Render Hardware Interface) is a code-generation tool that reads
`.sr` source files and generates synchronized C++ headers and HLSL include files
([external/srrhi](external/srrhi), [external/srrhi/README.md](external/srrhi/README.md)).

### What `.sr` Files Describe

`.sr` files use an HLSL-like syntax to declare:

- **`cbuffer` blocks** — constant buffer layouts with HLSL types
- **`struct` definitions** — reusable compound types
- **`srinput` scopes** — named binding sets grouping cbuffers, SRVs, UAVs, and samplers
- **`#include`** — composition of shared definitions
- **`extern`** — references to C++-side types

### Generated Outputs

For each `.sr` file in `src/shaders/`, srrhi produces:

| Output | Location | Content |
|---|---|---|
| C++ Header | `src/shaders/srrhi/cpp/<name>.h` | C++ struct with typed `Set*()` methods and a `ResourceEntry` array |
| HLSL Include | `src/shaders/srrhi/hlsl/<name>.hlsli` | HLSL namespace with `Get*()` accessor functions |

### Example

**Input** (`BasePass.sr`):
```hlsl
cbuffer SceneConstants {
    float4x4 viewProj;
    float3   cameraPosWS;
    float    exposure;
};

srinput BasePassInputs {
    SceneConstants m_Scene;              // → b0
    Texture2D<float4> m_Albedo;          // → t0
    RWTexture2D<float4> m_Output;        // → u0
    SamplerState m_Linear;               // → s0
};
```

**C++ Usage**:
```cpp
#include "shaders/srrhi/cpp/BasePass.h"

srrhi::BasePassInputs inputs;
inputs.SetScene(viewProj, cameraPosWS, exposure);   // cbuffer fields
inputs.SetAlbedo(myAlbedoTexture);                    // SRV
inputs.SetOutput(myOutputTexture);                    // UAV
inputs.SetLinear(mySampler);                          // Sampler

auto bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
```

**HLSL Usage**:
```hlsl
#include "BasePass.hlsli"

// Access cbuffer:
float4x4 vp = srrhi::BasePassInputs::GetScene().viewProj;
// Access texture:
float4 albedo = srrhi::BasePassInputs::GetAlbedo().Sample(srrhi::BasePassInputs::GetLinear(), uv);
```

### Supported Resource Types

| `.sr` declaration | `ResourceType` | Reg |
|---|---|---|
| `Texture2D<float4>` | `Texture_SRV` | `t#` |
| `RWTexture2D<float4>` | `Texture_UAV` | `u#` |
| `StructuredBuffer<T>` | `StructuredBuffer_SRV` | `t#` |
| `RWStructuredBuffer<T>` | `StructuredBuffer_UAV` | `u#` |
| `ByteAddressBuffer` | `RawBuffer_SRV` | `t#` |
| `RWByteAddressBuffer` | `RawBuffer_UAV` | `u#` |
| `RaytracingAccelerationStructure` | `RayTracingAccelStruct` | `t#` |
| `cbuffer` ref | `ConstantBuffer` | `b#` |
| `[push_constant] cbuffer` ref | `PushConstants` | `b#` |
| `SamplerState` | `Sampler` | `s#` |

---

## ShaderIDs System

The `ShaderIDsGenerator` tool ([ShaderIDsGenerator](ShaderIDsGenerator)) reads shader
configuration files (`.cfg`) and generates a `ShaderIDs.h` header with compile-time
constants and metadata for every shader permutation.

### Input: Shader Config Files

**`src/shaders/shaders.cfg`** lists all HLSL source files, shader profiles, entry points,
and preprocessor permutations:

```
BasePass.hlsl -T vs -E VSMain -m 6_8
BasePass.hlsl -T ps -E GBuffer_PSMain -m 6_8
BasePass.hlsl -T ps -E GBuffer_PSMain_AlphaTest -m 6_8 -D ALPHA_TEST=1 -s _AlphaTest
SPD.hlsl -T cs -E SPD_CSMain -m 6_8 -D SPD_NUM_CHANNELS={1,3}
```

The `{1,3}` brace syntax generates multiple permutation entries from a single line.
The `-s` flag specifies a suffix for the output filename.

**`src/shaders/NRDShaders.cfg`** lists NRD (noise reduction/denoising) shaders.

### Output: `src/shaders/ShaderIDs.h`

The generated header defines:

```cpp
namespace ShaderID
{
    struct ShaderEntry {
        const char* key;        // unique cache key
        const char* outputFile; // e.g. "shaders/dxil/BasePass_VSMain.dxil"
        const char* entryPoint; // e.g. "VSMain"
        const char* definesStr; // e.g. "ALPHA_TEST=1"
        nvrhi::ShaderType type;
    };

    // Constants (auto-assigned, alphabetically sorted):
    inline constexpr uint32_t BASEPASS_VSMAIN = 5u;
    inline constexpr uint32_t BASEPASS_MSMAIN = 4u;
    // ... ~213 total entries ...

    inline constexpr uint32_t COUNT = 213u;

    // Metadata array indexed by constant:
    inline constexpr ShaderEntry ENTRIES[COUNT] = { ... };
}
```

### Shader Loading at Runtime

At startup, `Renderer::LoadShaders()` reads the compiled DXIL binaries and populates
`m_ShaderHandles[ShaderID::COUNT]`. Shaders with the same output file (permutations via
preprocessor defines) are loaded once and shared. The function detects NVSP blob format
and creates individual `nvrhi::ShaderHandle` entries for each entry point.

Retrieval:
```cpp
nvrhi::ShaderHandle shader = g_Renderer.GetShaderHandle(ShaderID::BASEPASS_VSMAIN);
```

---

## Command List Management

The renderer uses a **command list pool** to avoid allocations per frame:

```cpp
std::vector<nvrhi::CommandListHandle> m_CommandListFreeList;    // available for reuse
std::vector<nvrhi::CommandListHandle> m_PendingCommandLists;    // recorded, not yet submitted
std::vector<nvrhi::CommandListHandle> m_InFlightCommandLists;   // submitted, waiting on GPU
```

**Lifecycle**:
1. `AcquireCommandList()` pops from `m_CommandListFreeList` or creates a new one
2. Each renderer records GPU commands into its own command list during `Render()`
3. After all `Render()` calls complete, `ExecutePendingCommandLists()`:
   - Calls `waitForIdle()` to sync the GPU
   - Moves `m_InFlightCommandLists` → `m_CommandListFreeList` (ready for reuse)
   - Submits `m_PendingCommandLists` to the GPU, moves them → `m_InFlightCommandLists`

Each command list is wrapped in a `ScopedCommandList` during recording, which handles
`open()`/`close()` and optional PIX/GPU marker insertion.

---

## TaskScheduler & Parallel Execution

The `TaskScheduler` ([TaskScheduler.h](src/TaskScheduler.h)) is a work-stealing thread
pool (12 threads by default) that enables parallel command list recording.

### How Renderers Execute in Parallel

During `RenderGraph::ScheduleRenderer()`, each renderer's `Render()` call is wrapped
in a task and deferred:

```cpp
g_Renderer.m_TaskScheduler->ScheduleTask([pRenderer, cmd, ...]() {
    ScopedCommandList scopedCmd{ cmd, pRenderer->GetName() };
    // ... GPU timing queries, global sync barriers ...
    pRenderer->Render(scopedCmd, g_Renderer.m_RenderGraph);
    // ... CPU/GPU timing capture ...
}, /* bImmediateExecute = */ false);
```

Tasks are **deferred** (not executed immediately). After `RenderGraph::Compile()`, the
main thread calls `TaskScheduler::ExecuteAllScheduledTasks()`, which:
1. Wakes all worker threads
2. The main thread also participates by stealing tasks
3. Blocks until all tasks are complete

This means multiple renderers can record GPU commands concurrently on separate threads,
each with its own command list. The render graph's compile phase ensures that the
declared read/write dependencies are respected — renderers that don't share resources
can run fully in parallel.

The `ParallelFor()` helper distributes work across threads for data-parallel operations
like GPU culling dispatch.

---

## Bindless Texture & Sampler System

The renderer maintains two global bindless descriptor tables accessible by all shaders:

### Texture Descriptor Table

```cpp
// Initial capacity: 1024 descriptors
// Type: MutableSrvUavCbv (supports Texture_SRV, Texture_UAV, ConstantBuffer, etc.)
// Used for: all scene textures referenced by index
```

Textures are registered via `RegisterTexture(handle)` → returns an index, or
`RegisterTextureAtIndex(index, handle)` for fixed slots. The first slots are reserved
for default textures (black, white, normal, etc.) defined by `CommonConsts::DEFAULT_TEXTURE_COUNT`.

### Sampler Descriptor Table

```cpp
// Initial capacity: 128 descriptors
// Type: MutableSampler (samplers only)
// Used for: bindless sampler selection
```

### Shader-Side Access

Shaders access bindless resources through helper functions in [Bindless.hlsli](src/shaders/Bindless.hlsli):

```hlsl
// Pixel shader (with implicit derivatives):
float4 color = SampleBindlessTexture(textureIndex, samplerIndex, uv);

// Compute/ray tracing (with explicit LOD):
float4 color = SampleBindlessTextureLevel(textureIndex, samplerIndex, uv, lod);

// With explicit gradients:
float4 color = SampleBindlessTextureGrad(textureIndex, samplerIndex, uv, ddx, ddy);
```

Both `RenderPassParams` automatically include the bindless tables when
`bIncludeBindlessResources` is `true`.

---

## Pipeline & Binding Cache

To minimize CPU overhead, the renderer caches all PSOs and binding layouts:

```cpp
std::mutex m_CacheMutex;  // thread-safe access
std::unordered_map<size_t, nvrhi::BindingLayoutHandle>    m_BindingLayoutCache;
std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle>  m_GraphicsPipelineCache;
std::unordered_map<size_t, nvrhi::MeshletPipelineHandle>   m_MeshletPipelineCache;
std::unordered_map<size_t, nvrhi::ComputePipelineHandle>   m_ComputePipelineCache;
```

Key methods:
```cpp
nvrhi::BindingLayoutHandle GetOrCreateBindingLayoutFromBindingSetDesc(desc, registerSpace);
nvrhi::BindingLayoutHandle GetOrCreateBindlessLayout(desc);
nvrhi::GraphicsPipelineHandle GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
nvrhi::MeshletPipelineHandle GetOrCreateMeshletPipeline(pipelineDesc, fbInfo);
nvrhi::ComputePipelineHandle GetOrCreateComputePipeline(shader, bindingLayouts);
```

Each method hashes the descriptor and looks up the cache before creating a new object.

---

## Shader Hot Reload

The renderer supports runtime shader reloading without restarting the application
(triggered via ImGui UI or a keyboard shortcut):

```cpp
void Renderer::ReloadShaders()
{
    m_RHI->m_NvrhiDevice->waitForIdle();       // flush GPU
    m_GraphicsPipelineCache.clear();            // drop all cached PSOs
    m_MeshletPipelineCache.clear();
    m_ComputePipelineCache.clear();
    m_BindingLayoutCache.clear();               // drop all cached binding layouts
    UnloadShaders();                            // release old shader handles
    LoadShaders();                              // re-read DXIL binaries from disk
}
```

This means you can edit HLSL files, re-run `build_shaders`, and press the reload
button — the next frame will use the new shaders with all cached state rebuilt.

---

## Build Pipeline

The build system is CMake-based with several code-generation steps that must execute
in the correct order.

### Dependency Graph

```
build_srrhi ──────┐
                  ├──→ build_shaders ──→ ${PROJECT_NAME} (main exe)
build_shaderids ──┘
```

Details on each step below.

### Step 1: `build_srrhi` — SRRHI Code Generation

- **Trigger**: `.sr` files added or modified under `src/shaders/`
- **Tool**: `bin/srrhi.exe` (built from `external/srrhi`)
- **Command**: `srrhi -i src/shaders -o src/shaders/srrhi`
- **Outputs**:
  - C++ headers → `src/shaders/srrhi/cpp/*.h`
  - HLSL includes → `src/shaders/srrhi/hlsl/*.hlsli`
- **Rule**: Never manually edit any file under `src/shaders/srrhi/`
- **Includes**: C++ code `#include`s from `src/shaders/srrhi/cpp/`, HLSL `#include`s from `src/shaders/srrhi/hlsl/`
- **Order**: Must run before shader compilation (the generated `.hlsli` files are included by HLSL shaders)

### Step 2: `build_shaderids` — ShaderIDs.h Generation

- **Trigger**: `.cfg` files modified under `src/shaders/`
- **Tool**: `ShaderIDsGenerator.exe` (built from `ShaderIDsGenerator/`)
- **Command**: `ShaderIDsGenerator -i src/shaders/shaders.cfg -i src/shaders/NRDShaders.cfg -o src/shaders/`
- **Output**: `src/shaders/ShaderIDs.h` (generated file — do not edit)
- **Order**: Must run before C++ compilation (the main project `#include`s `ShaderIDs.h`)

### Step 3: `build_shaders` — HLSL → DXIL Compilation

- **Trigger**: `.hlsl` or `.hlsli` files modified under `src/shaders/`
- **Tool**: NVIDIA ShaderMake (`external/ShaderMake`) invoked via CMake
- **Compiler**: DXC (DirectX Shader Compiler) — produces DXIL for D3D12
- **Configs**:
  - `src/shaders/shaders.cfg` — main shaders
  - `external/nrd/Shaders/shaders.cfg` — NRD denoising shaders
- **Output**: `bin/shaders/dxil/*.dxil` (binary blob format with NVSP header)
- **Flags**: `--shaderModel 6_8`, `--hlsl2021`, `-D DXIL`, `-D WITH_NRD=1`, `-X -enable-16bit-types`
- **Include paths**: `src/shaders`, `external/rtxdi/Include`, `external/nvrhi/include`, `src/shaders/rtxdi/SharedShaderInclude`, `external/SHARC/include`
- **Depends on**: `build_srrhi` (automatic via CMake dependency)
- **Dependency for**: Main executable (automatic via CMake dependency)

### Step 4: C++ Compilation — Main Executable

- **Tool**: MSVC (Visual Studio 2019+) with C++20
- **Depends on**: `build_shaders`, `build_shaderids`
- The main executable links against nvrhi, SDL3, cgltf, meshoptimizer, RTXDI, NRD, SHARC, FidelityFX FSR3, and other libraries

### ShaderMake Config Format

Each line in a `.cfg` file follows this format:

```
<path/to/shader.hlsl> -T <profile> -E <entryPoint> [-m <shaderModel>] [-D <DEFINE>={values}] [-s <suffix>]
```

- `-T`: Shader profile (`vs`, `ps`, `cs`, `ms`, `as`)
- `-E`: Entry point function name
- `-m`: Shader model (default: 6.8)
- `-D`: Preprocessor defines; `{A,B}` brace syntax creates multiple permutations
- `-s`: Output filename suffix (for disambiguating permutations)
