# nvrhi D3D12 — DXR 1.2 gap analysis: Clustered Geometry, Partitioned TLAS, indirect AS builds

**Date:** 2026-11-25
**Subject:** `external/nvrhi` (fork `lawfuyang/nvrhi`, submodule HEAD `403825e`)
**Reference implementations:**
`D:\Workspace\DirectX-Graphics-Samples\Samples\Desktop\D3D12Raytracing\src\D3D12RaytracingClusteredGeometry`
and `...\D3D12RaytracingPartitionedTLAS` (both build against `Microsoft.Direct3D.D3D12` **1.722.0-preview**).
**Spec:** [DXR Functional Spec Part 2](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing2.html), v0.40 (2026-11-23).

---

## 1. Executive summary

Your suspicion is **half correct**. nvrhi's D3D12 backend already implements *clustered geometry*
(CLAS / cluster templates / Cluster BLAS) twice — once through NVAPI and once through the native
DXR 1.2 API — and already wraps `ExecuteIndirectRTASOperations`. What is **entirely missing** is
**Partitioned TLAS**, and what is only **partially exposed** is the generic *indirect
acceleration-structure operation* surface (`D3D12_RTAS_OPERATION_DESC`).

| DXR 1.2 feature | nvrhi state | Notes |
|---|---|---|
| Clustered geometry (CLAS build / templates / instantiate / move / BLAS-from-CLAS) | **Implemented** (DXR + NVAPI + Vulkan) | Missing COMPRESSED1, several vertex formats, template `VertexInstantiationFormat` / bounding box, DXR-path per-argument index formats |
| Partitioned TLAS | **Absent** | No types in `nvrhi.h`, no sizing, no build/update path, no validation, no HLSL structs |
| Indirect acceleration-structure builds | **Partial** | Only *cluster* operations are wrapped, one `D3D12_RTAS_OPERATION_DESC` at a time; no address-indirection flags, no `ToolsInfo`, no `PARTITIONED_TLAS` op, no multi-descriptor execute |

Additionally, **this repository cannot use any of it today**: the build stages **Agility SDK 1.620.1**
(`CMakeLists.txt:142-150`) whose `d3d12.h` contains none of the DXR 1.2 cluster/PTLAS types, and both
`NVRHI_D3D12_WITH_DXRCLUSTER` and `NVRHI_WITH_NVAPI` are off (`CMakeLists.txt:171-176`).

---

## 2. What nvrhi has today (the plumbing PTLAS can reuse)

| Layer | Location | State |
|---|---|---|
| Public cluster API (`rt::cluster::OperationType/Params/Desc`, `nv::`/`dxr::` GPU arg layouts) | `include/nvrhi/nvrhi.h:1848-1985`, `include/nvrhi/nvrhiHLSL.h:44-330` | Complete for the 5 cluster op types |
| Feature query | `Feature::RayTracingClusters` + `RayTracingClustersFeatureInfo{ ClusterApi api }` (`nvrhi.h:3267-3277`), implemented at `d3d12-device.cpp:856-865` | Reports `None/Nv/Dxr` |
| Capability probe | `d3d12-device.cpp:450-500`: `ID3D12DeviceRaytracing2` + `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2)` → `ClustersAndPTLASSupported` → `m_ClusterApi = Dxr` | **Already checks the PTLAS bit** but only uses it for clusters |
| Experimental feature opt-in | `d3d12::enableExperimentalFeatures()` — `D3D12ExperimentalShaderModels` + `D3D12RaytracingPreview` (`d3d12-device.cpp:119-137`) | Required by the preview SDK |
| Device extension interface | `Context::deviceRaytracing2` (`d3d12-backend.h:181`) | `GetRTASOperationPrebuildInfo` lives here — already held |
| Command-list extension interface | `InternalCommandList::commandListRaytracing2` (`d3d12-backend.h:1046`, QI'd in `d3d12-commandlist.cpp:123-125`) | `ExecuteIndirectRTASOperations` lives here — already held |
| Cluster op translation (DXR) | `d3d12-raytracing.cpp:1298-1491` (`dxr_clusters::translateClusterOperation`) | Fills `D3D12_RTAS_OPERATION_INPUTS` + `D3D12_RTAS_BATCHED_OPERATION_DATA` |
| Cluster op translation (NVAPI) | `d3d12-raytracing.cpp:1086-1222` | Same shape, NVAPI structs |
| Sizing | `Device::getClusterOperationSizeInfo` (`d3d12-raytracing.cpp:1496-1536`) | `GetRTASOperationPrebuildInfo` on DXR, `NvAPI_...GetRequirementsInfo` on NVAPI |
| Execution | `CommandList::executeMultiIndirectClusterOperation` (`d3d12-raytracing.cpp:2817-2888`) with `prepareClusterOperation` (2731-2815) | Allocates scratch from `m_DxrScratchManager`, transitions inputs to `ShaderResource`, UAV-writes outputs |
| Pipeline opt-in | `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_CLUSTERED_GEOMETRY` from `PipelineDesc::allowClusters` (`d3d12-raytracing.cpp:1750-1765`, `nvrhi.h:2988`) | Good |
| Accel-struct object | `IAccelStruct` + `AccelStruct` (`nvrhi.h:1834-1842`, `d3d12-backend.h:866-896`) | Buffer-backed, SRV-able (`createSRV` sets `RAYTRACING_ACCELERATION_STRUCTURE` view), `getDeviceAddress()` |
| Buffer creation | `BufferDesc::isAccelStructStorage` → `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE` (`d3d12-buffer.cpp:96-98`) | Exactly what a PTLAS allocation needs |

**Conclusion:** the hard parts (experimental enablement, `ID3D12DeviceRaytracing2`,
`ID3D12CommandListRaytracing2`, scratch suballocation, rebarrier/state plumbing) are done. PTLAS is
an *addition*, not a re-architecture.

---

## 3. Feature matrix against the spec

### 3.1 Clustered geometry — implemented, with concrete holes

| Spec item (1.722) | nvrhi | Gap |
|---|---|---|
| `D3D12_RTAS_OPERATION_TYPE_MOVE_CLUSTER_OBJECTS` | `OperationType::Move` + `OperationMoveType::{BottomLevel,ClusterLevel,Template}` | ok |
| `BUILD_CLAS_FROM_TRIANGLES` | `OperationType::ClasBuild` | ok |
| `BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES` | `OperationType::ClasBuildTemplates` | see C3/C4 |
| `INSTANTIATE_CLUSTER_TEMPLATES` | `OperationType::ClasInstantiateTemplates` | see C3 |
| `BUILD_BLAS_FROM_CLAS` | `OperationType::BlasBuild` | ok |
| implicit / explicit / GET_SIZES modes | `OperationMode` | ok |
| `D3D12_RTAS_CLUSTER_LIMITS` | `OperationClasBuildParams` | ok |
| `D3D12_VERTEX_FORMAT` (14 values) | `Format` + `translateClusterVertexFormat` (DXR) | **C1** |
| `D3D12_VERTEX_FORMAT_COMPRESSED1` + `MaxCompressedClusterPositionsSize` | — | **C2** |
| `VertexHintFormat` vs `VertexInstantiationFormat` | single `OperationClasBuildParams::vertexFormat` | **C3** |
| `TemplateInstancePrefersCOMPRESSED1` | not queried (`RayTracingClustersFeatureInfo` has only `api`) | **C4** |
| `InstantiationBoundingBoxLimit` | not modelled (CPU desc has no field) | **C5** |
| `GeometryIndexAndFlagsIndexFormat`, `OpacityMicromapIndexFormat` | DXR path hardcodes `D3D12_INDEX_FORMAT_NONE` | **C6** |
| `D3D12_RTAS_OPERATION_FLAG_ALLOW_DATA_ACCESS` | injected implicitly when `RayTracingPositionFetch` is supported | **C7** |
| `D3D12_RTAS_OPERATION_ADDRESS_RESOLUTION_FLAG_INDIRECTED_*` | always `FLAG_NONE` | **C8** |
| `D3D12_RTAS_BATCHED_OPERATION_DATA::ToolsInfo` | never set | **C8** |
| Multiple `pDescs` per `ExecuteIndirectRTASOperations` | always `NumOperationDescs == 1` | **C9** |

#### C1 — `translateClusterVertexFormat` cannot represent most of `D3D12_VERTEX_FORMAT`
`d3d12-raytracing.cpp:1367-1384` maps only `RG32_FLOAT, RGB32_FLOAT, RG16_FLOAT, RG16_UNORM,
RG16_SNORM, RG8_UNORM, RG8_SNORM, R10G10B10A2_UNORM`; everything else falls into `default: assert(false)`.
`OperationClasBuildParams::vertexFormat` is a plain `nvrhi::Format` and documents the same restricted
list (`nvrhi.h:1902-1906`), and `nvrhi::Format` has **no** packed `FLOAT16_3 / UNORM16_3 / SNORM16_3 /
UNORM8_3 / SNORM8_3` (`nvrhi.h:162-212` only adds `RGB32_*` as 3-component). So the 6-byte and 3-byte
`D3D12_VERTEX_FORMAT` values are unreachable from *both* backends. The NVAPI path
(`translateCLASBuildOperationVertexFormat`, `:1145-1152`) is worse than the DXR path only in failure
mode: `getDxgiFormatMapping` yields `DXGI_FORMAT_UNKNOWN`, which is `assert`ed in debug and silently
forwarded in release.

Fix: add a dedicated `rt::cluster::VertexFormat` enum mirroring `D3D12_VERTEX_FORMAT` one-to-one, and
map it per backend: DXR native, NVAPI/Vulkan to the DXGI equivalent where one exists, rejecting (not
silently downgrading) formats the backend cannot express.

#### C2 — COMPRESSED1 is not representable at all
There is no `Format` value and no field for `maxCompressedClusterPositionsSize` /
`D3D12_VERTEX_FORMAT_COMPRESSED1_TEMPLATE_HEADER` (`compressed1Header` exists only in the `dxr::`
GPU-arg struct, `nvrhiHLSL.h:298-299`, and is never fed from the CPU side).
Required additions: the enum value, the `union { MinPositionTruncateBitCount; MaxCompressedClusterPositionsSize; }`
in the inputs desc, the 12-byte per-cluster header in the GPU args (app-written), and the
4-byte template header in `dxr::IndirectTriangleTemplateArgs` driven by the desc.

#### C3 — one vertex format for both template hint and instantiation
The 1.722 desc splits into `D3D12_RTAS_CLUSTER_TEMPLATE_TRIANGLES_INPUTS_DESC`
(`VertexHintFormat` + `VertexInstantiationFormat`) and
`D3D12_RTAS_INSTANTIATE_CLUSTER_TEMPLATE_INPUTS_DESC` (`VertexSourceFormat`). nvrhi's
`translateClusterOperation` (`d3d12-raytracing.cpp:1418-1470`) writes the same
`params.clas.vertexFormat` into `VertexHintFormat` and `VertexInstantiationFormat` (`:1459`), and into
`VertexSourceFormat` for instantiation. Fine as a default, but the API cannot express the
hint/instantiation split that `TemplateInstancePrefersCOMPRESSED1` exists to exploit.

#### C4 — no `TemplateInstancePrefersCOMPRESSED1`
`D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2` has two BOOLs in 1.722
(`ClustersAndPTLASSupported`, `TemplateInstancePrefersCOMPRESSED1` — preview `d3d12.h:43330-43334`).
nvrhi reads the first and drops the second. `RayTracingClustersFeatureInfo` needs it (see §6).

#### C5 — `InstantiationBoundingBoxLimit` is not modelled
Both the CPU desc and `OperationDesc` lack it; the `dxr::` GPU arg struct has
`instantiationBoundingBoxLimit` (`nvrhiHLSL.h:297`) but no caller-facing way to request it. It is a
GPU-side pointer in the args, so the *device* address is app-written; what nvrhi lacks is only
documentation/pass-through (no translation needed). Low priority.

#### C6 — DXR path silently disables per-triangle geometry-index / OMM index buffers
`translateClusterOperation` sets `GeometryIndexAndFlagsIndexFormat = D3D12_INDEX_FORMAT_NONE` and
`OpacityMicromapIndexFormat = D3D12_INDEX_FORMAT_NONE` unconditionally for `ClasBuild` and
`ClasBuildTemplates` (`d3d12-raytracing.cpp:1447`). On the NVAPI/Vulkan layout those formats
live in each GPU argument; on DXR they live in the inputs desc, i.e. **they cannot vary per argument**.
So either:
* add them to `OperationClasBuildParams` and accept "one format per operation" on DXR (assert if the
  app tries to mix), or
* document the DXR limitation and leave the buffers unused.

Right now the failure mode is silent (sequential indexing, wrong geometry indices), which is the worst
option. At minimum, the `dxr::` path should `assert` when `geometryIndexAndFlagsIndexBuffer` /
`opacityMicromapIndexBuffer` are non-null.

#### C7 — `ALLOW_DATA_ACCESS` is implicit
`dxr_clusters::translateClusterOperation(params, allowDataAccess, out)` ORs
`D3D12_RTAS_OPERATION_FLAG_ALLOW_DATA_ACCESS` whenever `m_RayTracingPositionFetchSupported`
(`:1418,1426`). That is a reasonable default (the flag is what makes `TriangleObjectPositions()` legal),
but the flag also costs memory on some implementations. Expose `OperationFlags::AllowDataAccess`
(default on when position fetch is available) instead of hiding it.

#### C8 / C9 — the indirect surface is narrower than `ExecuteIndirectRTASOperations`
`prepareClusterOperation` (`:2731-2815`) always writes `AddressResolutionFlags = NONE`, never sets
`ToolsInfo`, and `executeMultiIndirectClusterOperation` always passes `NumOperationDescs = 1`.
Consequences:
* GPU-generated *addresses* (result/scratch/args/arg-count written by a compute shader) are unsupported.
* PIX can't get the flattened tools block.
* Batches of different operation types must be issued as separate calls (legal, but the API allows
  them together and drivers are allowed to overlap adjacent compatible types).
Also note `prepareClusterOperation` returns early when `desc.params.maxArgCount == 0`
(`:2733-2735`) and validation rejects a null arg-count buffer with `maxArgCount == 0`
(`validation-commandlist.cpp:1902-1906`) — so a *fully* GPU-driven count with `MaxArgCount = 0` is
impossible even though the hardware allows `IndirectArgumentArraySize` to drive the count under a
non-zero `MaxArgCount`. That is by design (the count is clamped to `MaxArgCount`), but the API should
document it.

### 3.2 Partitioned TLAS — nothing exists

Required D3D12 surface (all present in preview 1.722, `d3d12.h:47139-47550`):

| D3D12 object | Purpose | In nvrhi |
|---|---|---|
| `D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS` (`0x8`) | selects the PTLAS arm of `D3D12_RTAS_OPERATION_INPUTS` | **no** |
| `D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC { Flags, InstanceCount, MaxInstancePerPartitionCount, PartitionCount, MaxInstanceInGlobalPartitionCount }` | capacity, sizing input, and build input | **no** |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION_DATA { Source, Dest, Scratch, IndirectPartitionedTlasOpCount, IndirectPartitionedTlasOps }` | in-place / growth build | **no** |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION { Type, ArgCount, ArgData(GPU VA + stride) }` | GPU-side sub-operation list | **no** |
| `..._OPERATION_TYPE_{WRITE_INSTANCE, UPDATE_INSTANCE, TRANSLATE_PARTITION}` | sparse update verbs | **no** |
| `..._WRITE_INSTANCE_ARGS` (104 B), `..._UPDATE_INSTANCE_ARGS` (16 B), `..._TRANSLATE_PARTITION_ARGS` (16 B) | per-instance GPU records | **no** (only `rt::IndirectInstanceDesc`, which is the *TLAS* 64 B layout — not interchangeable) |
| `D3D12_RTAS_PARTITIONED_TLAS_INSTANCE_FLAGS` (adds `ENABLE_EXPLICIT_AABB = 0x40`) | instance flags | `rt::InstanceFlags` stops at `DisableOMMs = 0x20` |
| `D3D12_RTAS_PARTITIONED_TLAS_PARTITION_INDEX_GLOBAL_PARTITION = 0xffffffff` | global partition | **no** |
| `D3D12_RTAS_PARTITIONED_TLAS_FLAG_{FAST_TRACE,FAST_BUILD,ENABLE_PARTITION_TRANSLATION}` | build hints / translation opt-in | **no** |
| `ID3D12DeviceRaytracing2::GetRTASOperationPrebuildInfo` with the PTLAS inputs | sizing | interface already held; no caller |
| `ID3D12CommandListRaytracing2::ExecuteIndirectRTASOperations` with `pPartitionedTlasOperationData` | build/update | interface already held; no caller |

Semantic traps the implementation must handle:

1. **Not a `IAccelStruct` build path.** A PTLAS is invalid input to
   `BuildRaytracingAccelerationStructure`, `CopyRaytracingAccelerationStructure` (except
   serialize/deserialize/tools modes) and `EmitRaytracingAccelerationStructurePostbuildInfo`.
   `Device::createAccelStruct` currently sizes *every* AS with
   `GetRaytracingAccelerationStructurePrebuildInfo` (`d3d12-raytracing.cpp:882-920`) — that returns
   zero for a PTLAS. Sizing must branch to `GetRTASOperationPrebuildInfo`.
2. **Instances live in the structure, not in a CPU array.** There is no `buildTopLevelAccelStruct`
   equivalent: the initial content is a GPU-written `WRITE_INSTANCE` batch, and updates must be
   in-place (source == dest) to retain untouched partitions.
3. **Liveness.** nvrhi's TLAS tracks referenced BLAS handles (`AccelStruct::bottomLevelASes`) for
   lifetime. PTLAS instance records are written *by the GPU*, so nvrhi cannot see the BLAS set. The
   API must state that the application owns the lifetime of every BLAS referenced by a PTLAS, or
   accept an optional list for tracking.
4. **Resource states.** AS data: `AccelStructRead`/`AccelStructWrite`. Op list, op count, arg arrays:
   `NON_PIXEL_SHADER_RESOURCE` (the spec explicitly says *not* `INDIRECT_ARGUMENT`) — nvrhi's
   `ResourceStates::AccelStructBuildInput` already maps to that (`d3d12-constants.cpp:268`).
   Address/size output arrays: `UnorderedAccess`. A UAV barrier is required before tracing
   (`copyRaytracingAccelerationStructure`/cluster code already do this).
5. **`InstanceIndex` is stable and app-assigned**; capacity is fixed per build and only changes when
   the inputs desc changes (expensive "resize", spec *PTLAS resizing*). Do not recompute the allocation
   from a small per-frame write count.

### 3.3 Indirect acceleration-structure builds

The 1.722 API has exactly six operation types; there is **no** indirect BLAS-from-triangles yet (the
spec lists it as a *potential future feature*). "Indirect AS builds" therefore means: **all RTAS
builds become GPU-driven through one entry point**, with batched arguments (and, in later spec
revisions, one level of address indirection).

nvrhi exposes this *only* for the five cluster verbs, through a cluster-shaped API, one descriptor at
a time, with `AddressResolutionFlags = NONE`. To be complete it needs:

* an operation descriptor that can carry either `pBatchedOperationData` or
  `pPartitionedTlasOperationData`;
* `AddressResolutionFlags` pass-through (6 flags) — required for fully GPU-driven culling/LOD
  pipelines, which is the main reason the API exists;
* an array of descriptors per execute call;
* `ToolsInfo` (optional, PIX/debug);
* result-size width documentation: in 1.722 `ResultSizeArray` is always `UINT64` (nvrhi's
  `dxr::OperationSize = uint64_t` is correct for this preview; spec v0.39 introduced an opt-in
  `RESULT_SIZE_64BIT` flag that does **not** exist in 1.722 — do not add it).

---

## 4. Gap catalogue (actionable)

| # | Gap | Severity | Where |
|---|---|---|---|
| G1 | PTLAS: no types, no creation, no sizing, no build/update, no HLSL args | **blocker** for PTLAS | `nvrhi.h`, `nvrhiHLSL.h`, `d3d12-raytracing.cpp`, `d3d12-backend.h` |
| G2 | `createAccelStruct` cannot size/create a partitioned AS | **blocker** | `d3d12-raytracing.cpp:882` |
| G3 | No `Feature`/info for PTLAS; `RayTracingClustersFeatureInfo` drops `TemplateInstancePrefersCOMPRESSED1` | high | `nvrhi.h:3267`, `d3d12-device.cpp:856` |
| G4 | Generic RTAS op execution (multi-desc, address indirection, `ToolsInfo`) | medium | `d3d12-raytracing.cpp:2817` |
| G5 | COMPRESSED1 + 6-byte vertex formats | medium (perf/memory) | `nvrhi.h:1902-1906`, `:1367` |
| G6 | Template hint vs instantiation format | medium | `:1459` |
| G7 | DXR per-argument index formats silently ignored | high (correctness on DXR) | `:1447,1461` |
| G8 | `ALLOW_DATA_ACCESS` not exposed | low | `:1426` |
| G9 | Validation layer knows nothing about PTLAS or the generic surface | medium | `validation-commandlist.cpp`, `validation-device.cpp` |
| G10 | d3d11 + Vulkan need stubs / explicit `NotSupported` | medium | `d3d11-*`, `vulkan-*` |
| G11 | Repo build uses Agility 1.620.1 and leaves both cluster options off | **blocker** to run anything | `CMakeLists.txt:142-176` |
| G12 | No tests or docs for PTLAS | medium | `tests/d3d12/*`, `README.md` |

---

## 5. Proposed nvrhi API

### 5.1 Object model decision

Reuse `rt::IAccelStruct` rather than introduce a new handle type. Reasons: descriptors and bindings
already work (`AccelStruct::createSRV`, `getDeviceAddress`, `setAccelStructState`), and a PTLAS is
bound to shaders exactly like a TLAS. Discriminate with the desc:

```cpp
namespace nvrhi::rt
{
    struct PartitionedAccelStructParams
    {
        uint32_t instanceCount = 0;                  // capacity, stable InstanceIndex range
        uint32_t partitionCount = 0;
        uint32_t maxInstancesPerPartition = 0;       // excludes the global partition
        uint32_t maxInstancesInGlobalPartition = 0;
        bool preferFastTrace = true;                 // mutually exclusive with preferFastBuild
        bool preferFastBuild = false;
        bool enablePartitionTranslation = false;     // costs memory; enables TRANSLATE_PARTITION
    };

    struct AccelStructDesc
    {
        ...
        bool isPartitioned = false;                  // new
        PartitionedAccelStructParams partitioned{};  // new; only when isPartitioned
    };
}
```

`AccelStruct` gains `bool isPartitioned` (copied from the desc). `buildTopLevelAccelStruct*`,
`copyRaytracingAccelerationStructure`, `compactBottomLevelAccelStructs` and the OMM/blas paths must
assert `!isPartitioned`; `createAccelStruct` branches its sizing:

```cpp
if (desc.isPartitioned && m_Context.deviceRaytracing2 && m_ClusterApi == ClusterApi::Dxr)
    // fill D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC, call GetRTASOperationPrebuildInfo
else
    // existing GetRaytracingAccelerationStructurePrebuildInfo path
```

The allocation uses the existing `BufferDesc{ .canHaveUAVs = true, .isAccelStructStorage = true,
.initialState = AccelStructWrite }` recipe, which already yields
`D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE`.

### 5.2 Execution API

Minimum that solves the problem (one new command-list method, one new device method):

```cpp
namespace nvrhi::rt
{
    struct PartitionedAccelStructUpdateDesc
    {
        bool performUpdate = false;                 // false => SourceAccelerationStructureData = 0

        // GPU-resident operation list; must stay alive until the list completes.
        IBuffer* operationCountBuffer = nullptr;    // UINT32 count of D3D12_RTAS_PARTITIONED_TLAS_OPERATION
        uint64_t operationCountOffset = 0;
        IBuffer* operationBuffer = nullptr;         // array of D3D12_RTAS_PARTITIONED_TLAS_OPERATION
        uint64_t operationOffset = 0;
        uint32_t maxOperationCount = 0;             // validation only
    };
}

// device
virtual bool queryPartitionedAccelStructPrebuildInfo(const rt::AccelStructDesc& desc,
                                                     rt::AccelStructPrebuildInfo& outInfo) = 0;

// command list
virtual void executePartitionedAccelStructOps(rt::IAccelStruct* as,
                                              const rt::PartitionedAccelStructUpdateDesc& desc) = 0;
```

Keep capacity immutable per `IAccelStruct`: a resize is "create a new PTLAS and do a full WRITE_INSTANCE
build", which is all the spec's *PTLAS resizing* case needs and avoids a whole second set of allocation
rules. The scratch requirement depends only on the (fixed) inputs desc, so it can be suballocated from
`m_DxrScratchManager` exactly like the cluster path, with the size from
`queryPartitionedAccelStructPrebuildInfo().scratchBytes`.

Implementation sketch (`d3d12-raytracing.cpp`):

```cpp
D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC inputs = {};
inputs.Flags = fastTrace ? FAST_TRACE : (fastBuild ? FAST_BUILD : NONE);
if (desc.partitioned.enablePartitionTranslation)
    inputs.Flags |= ENABLE_PARTITION_TRANSLATION;
inputs.InstanceCount = ...; inputs.MaxInstancePerPartitionCount = ...;
inputs.PartitionCount = ...; inputs.MaxInstanceInGlobalPartitionCount = ...;

D3D12_RTAS_OPERATION_INPUTS opInputs = {};
opInputs.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS;
opInputs.pPartitionedTLASInputsDesc = &inputs;

D3D12_RTAS_PARTITIONED_TLAS_OPERATION_DATA data = {};
data.SourceAccelerationStructureData = update ? as->dataBuffer->gpuVA : 0;
data.DestAccelerationStructureData     = as->dataBuffer->gpuVA;
data.ScratchAccelerationStructureData  = <m_DxrScratchManager suballocation, 256B aligned>;
data.IndirectPartitionedTlasOpCount    = opCountBuffer->gpuVA + offset;
data.IndirectPartitionedTlasOps        = opBuffer->gpuVA + offset;

D3D12_RTAS_OPERATION_DESC opDesc = {};
opDesc.Inputs = opInputs;
opDesc.pPartitionedTlasOperationData = &data;
commandListRaytracing2->ExecuteIndirectRTASOperations(1, &opDesc, FLAG_NONE);
// then: UAV barrier on as->dataBuffer
```

Automatic barriers: op buffers → `ResourceStates::ShaderResource`; AS → `AccelStructWrite`
(`AccelStructRead|Write` for in-place update); referenced buffers registered in
`m_Instance->referencedResources`.

### 5.3 Optional phase 2 — generalize the indirect layer

To cover "indirect acceleration-structure builds" properly and to stop the cluster wrapper from being
the only entry point, add a thin, non-abstracting mirror of the D3D12 API:

```cpp
namespace nvrhi::rt::indirect
{
    enum class OperationType : uint8_t
    { MoveClusterObjects, BuildBlasFromClas, BuildClasFromTriangles,
      BuildClusterTemplatesFromTriangles, InstantiateClusterTemplates, PartitionedTlas };

    enum class AddressResolutionFlags : uint8_t
    { None=0, BatchResult=1, BatchScratch=2, DestinationAddressArray=4,
      ResultSizeArray=8, IndirectArgumentArray=0x10, IndirectArgumentCount=0x20 };

    struct BatchedOperationData
    {
        AddressResolutionFlags addressResolutionFlags = AddressResolutionFlags::None;
        GpuVirtualAddress batchResultData = 0;       // or IBuffer + offset
        GpuVirtualAddress batchScratchData = 0;      // 0 => nvrhi suballocates
        GpuVirtualAddressAndStride resultAddressArray{};
        GpuVirtualAddressAndStride resultSizeArray{};
        GpuVirtualAddressAndStride indirectArgumentArray{};
        GpuVirtualAddress indirectArgumentArraySize = 0;
        GpuVirtualAddress toolsInfo = 0;
    };

    struct OperationDesc
    {
        OperationType type;
        // CPU inputs union: cluster params (existing) or partitioned params
        rt::cluster::OperationParams cluster;
        rt::PartitionedAccelStructParams partitioned;
        BatchedOperationData data;                   // or PartitionedTlasOperationData
    };
}
```

with `ICommandList::executeIndirectRtAsOperations(const rt::indirect::OperationDesc* descs, size_t count)`
and `IDevice::queryRtAsOperationPrebuildInfo(...)`. `rt::cluster::OperationDesc` then becomes a
convenience adapter, and `executeMultiIndirectClusterOperation` stays as a source-compatible wrapper.
This is where address-indirection (G4) lands once, for all operation types.

**Recommendation:** ship §5.1 + §5.2 first (unblocks PTLAS). Add §5.3 only when a caller actually
needs GPU-resolved addresses or mixed batches — AGENTS.md simplicity rules apply.

### 5.4 HLSL side

`nvrhiHLSL.h` needs GPU-record layouts so the app's compute passes can write them:

```cpp
namespace nvrhi::rt::partitioned
{
    static const uint32_t kGlobalPartition = 0xffffffff;

    struct WriteInstanceArgs   // 104 B, matches D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS
    {
#ifdef __cplusplus
        float transform[3][4];
#else
        float4 transform[3];
#endif
        uint32_t instanceID;
        uint32_t instanceMask;
        uint32_t instanceContributionToHitGroupIndex;
        uint32_t instanceFlags;                 // rt::InstanceFlags | ExplicitAABB
        GpuVirtualAddress accelerationStructure;
        uint32_t instanceIndex;
        uint32_t partitionIndex;
        RaytracingAabb explicitAabb;            // MinX..MaxZ
    };
    struct UpdateInstanceArgs  { uint32_t instanceIndex, hitGroupContribution; GpuVirtualAddress as; };
    struct TranslatePartitionArgs { uint32_t partitionIndex; float translation[3]; };
    struct Operation { uint32_t type; uint32_t argCount; GpuVirtualAddressAndStride argData; };
}
```

Add `rt::InstanceFlags::ExplicitAABB = 64` (bit-compatible with
`D3D12_RTAS_PARTITIONED_TLAS_INSTANCE_FLAG_ENABLE_EXPLICIT_AABB`).

Note: `rt::IndirectInstanceDesc` (existing 64 B TLAS record) must **not** be reused — the PTLAS record
carries `InstanceIndex`/`PartitionIndex`/`ExplicitAABB` and 32-bit flag fields.

---

## 6. Feature reporting

`RayTracingClustersFeatureInfo` (`nvrhi.h:3274-3277`) currently carries only `api`. Add:

```cpp
struct RayTracingClustersFeatureInfo
{
    ClusterApi api = ClusterApi::None;
    bool partitionedTlasSupported = false;    // ClustersAndPTLASSupported
    bool templateInstancePrefersCompressed1 = false;
};
```

* DXR: both from `D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2` (already queried at
  `d3d12-device.cpp:461-474`; cache the two BOOLs on `Device`).
* NVAPI: `partitionedTlasSupported = false`, preference `false`.
* Vulkan: `false`/`false`.

Adding a field changes `sizeof`, so every `queryFeatureSupport` caller that passes a size literal must
rebuild — acceptable in this repo (everything is built from source), but call it out in the README.

Also verify the probe: the samples use
`ID3D12Device16::CheckFeatureSupport1(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2, &d, sizeof(d), D3D12_PREVIEW_SDK_VERSION)`
(`PTLAS/DeviceResources.cpp:101-105`, `ClusteredGeometry.cpp:360-390`), while nvrhi calls
`ID3D12Device::CheckFeatureSupport(..., sizeof(raytracing2))`. If the latter ever returns
`E_INVALIDARG` on a future preview, port the versioned call (add `ID3D12Device16` to `Context`).

---

## 7. Implementation checklist

Ordered so each step compiles and is testable.

### Phase A — enablement (no nvrhi source change)
1. `CMakeLists.txt`: bump `AGILITY_SDK_VERSION` to `1.722.0-preview` (or later **only if** nvrhi's
   version guard in `include/nvrhi/d3d12.h:31-39` is updated), stage the matching
   `D3D12Core.dll`/`d3d12SDKLayers.dll`.
2. Set `NVRHI_D3D12_WITH_DXRCLUSTER=ON` and `NVRHI_D3D12_DXRCLUSTER_INCLUDE_DIR=<agility>/build/native/include`
   (`external/nvrhi/CMakeLists.txt:405-412` requires it).
3. Decide on `NVRHI_WITH_NVAPI` (falls back for non-DXR devices) and check that
   `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP` still compiles against the preview header.
4. Application: call `nvrhi::d3d12::enableExperimentalFeatures(...)` before device creation
   (Windows Developer Mode + driver ≥ 621.01 required); set `DeviceDesc::enableDxrClusters = true`.
5. Verify: `Feature::RayTracingClusters` returns `Dxr`, and a trivial CLAS build round-trips.

### Phase B — PTLAS in nvrhi
6. `include/nvrhi/nvrhi.h`: `PartitionedAccelStructParams`, `AccelStructDesc::isPartitioned`,
   `PartitionedAccelStructUpdateDesc`, `ICommandList::executePartitionedAccelStructOps`,
   `IDevice::queryPartitionedAccelStructPrebuildInfo`, `InstanceFlags::ExplicitAABB`,
   extended `RayTracingClustersFeatureInfo`.
7. `include/nvrhi/nvrhiHLSL.h`: `rt::partitioned` record structs + `kGlobalPartition`.
8. `src/d3d12/d3d12-backend.h`: `AccelStruct::isPartitioned`, command-list decl,
   `PartitionedAccelStructAddresses` helper if it mirrors `ClusterOperationAddresses`.
9. `src/d3d12/d3d12-raytracing.cpp`:
   * `fillPartitionedTlasInputs(...)` + `queryPartitionedAccelStructPrebuildInfo`;
   * branch in `Device::createAccelStruct` (sizing, initial state, `isPartitioned` copy);
   * `CommandList::executePartitionedAccelStructOps` (scratch suballocation, barriers, execute, UAV);
   * assert `!isPartitioned` in the TLAS/copy/compact paths.
10. `src/d3d12/d3d12-device.cpp`: cache `partitionedTlasSupported` /
    `templateInstancePrefersCompressed1`; return them from `queryFeatureSupport`.
11. `src/validation/*`: `Feature` gate (PTLAS requires `api == Dxr`), `requireType(Compute)`,
    null/offset/stride checks, `maxOperationCount`, `performUpdate` + capacity rules.
12. `src/d3d11/*`, `src/vulkan/*`: stubs returning `utils::NotSupported()`; Vulkan has no PTLAS
    equivalent (no `VK_*partitioned*` extension), NVAPI has none either.
13. Tests: `tests/d3d12/device-tests.cpp` (feature probe, prebuild > 0), a `core-tests` PTLAS build
    with one WRITE_INSTANCE batch, trace a ray, then update one partition in place.
14. `README.md`: document the preview SDK, the two capability bits, the liveness rule and the
    "not a TLAS" restrictions.

### Phase C — cluster-completeness (independent of PTLAS)
15. `rt::cluster::VertexFormat` enum + full mapping (C1), COMPRESSED1 (C2),
    hint/instantiation split (C3), `AllowDataAccess` flag (C8→C7).
16. DXR-path index formats: add to `OperationClasBuildParams` and assert when unsupported (G7).
17. Optional: generic indirect layer with address-indirection flags and `ToolsInfo` (§5.3).

---

## 8. Verification plan

| Check | Method |
|---|---|
| Capability plumbing | `nvrhi` device test: `RayTracingClustersFeatureInfo{ api = Dxr, partitionedTlasSupported = true }` on the target GPU |
| PTLAS sizing | Compare `queryPartitionedAccelStructPrebuildInfo` against the sample's numbers for the same capacity desc |
| Sparse update correctness | Reproduce the sample's "one domino, quiet scene" scenario: write count settles to 0, image identical to regular TLAS |
| In-place update | Render with source == dest, then compare against a fresh full rebuild (byte-identical trace result is too strong; compare hits within tolerance) |
| Partition translation | Translate a partition, verify traces move by the translation while instance transforms are unchanged |
| Global partition | Assign an instance to `0xffffffff`, verify it traces and that capacity limits are enforced |
| Cluster completeness | Build a CLAS with COMPRESSED1 and with 16-bit 3-component vertices; compare with the clustered-geometry sample |
| Barriers/lifetime | PIX capture: op buffers in `NON_PIXEL_SHADER_RESOURCE`, UAV barrier before `DispatchRays`, no `INDIRECT_ARGUMENT` state |

---

## 9. Risks / open questions

1. **SDK pin.** `include/nvrhi/d3d12.h:34-39` hard-`#error`s unless `D3D12_PREVIEW_SDK_VERSION == 722`
   and warns about NVIDIA driver 621.01. Upgrading the Agility SDK for other reasons will break the
   build; the guard (and the 64-bit `ResultSizeArray` assumption, `dxr::OperationSize`) must be revisited
   together.
2. **`CheckFeatureSupport` vs `CheckFeatureSupport1`.** nvrhi uses the unversioned call for a
   preview-only feature enum. Verify on the target driver; keep `ID3D12Device16` as a fallback.
3. **Resource-state intent.** The spec forbids `INDIRECT_ARGUMENT` for PTLAS op inputs. nvrhi's
   `AccelStructBuildInput` already maps correctly, but using `ShaderResource` explicitly (as the
   cluster path does) is safer and self-documenting.
4. **Liveness of GPU-referenced BLASes.** This is the one place where PTLAS breaks an existing nvrhi
   invariant. Decide: document-only, or optional `span<IAccelStruct*>` on the write path.
5. **Virtual resources.** `AccelStructDesc::isVirtual` + `bindAccelStructMemory` work for a PTLAS
   allocation, but rebinding changes the GPUVA embedded in nothing (PTLAS stores BLAS addresses, not its
   own), so the existing `bindAccelStructMemory` behaviour is fine — confirm with a test.
6. **Multi-op ordering.** `ExecuteIndirectRTASOperations` may execute descriptors in any order. A
   pipeline that builds CLAS and Cluster BLAS in one call cannot rely on ordering; nvrhi must not
   imply it in the API.
7. **`maxArgCount == 0`.** Fully GPU-driven counts remain impossible (clamped to `MaxArgCount`).
   Document; do not "fix" by synthesising a large count.

---

## Appendix A — sample API → nvrhi mapping

| Sample call (PTLAS) | nvrhi equivalent needed |
|---|---|
| `ID3D12Device16::CheckFeatureSupport1(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2, ...)` | `queryFeatureSupport(Feature::RayTracingClusters, &info, sizeof(info))` |
| `ID3D12DeviceRaytracing2::GetRTASOperationPrebuildInfo({PARTITIONED_TLAS, &inputs})` | `IDevice::queryPartitionedAccelStructPrebuildInfo(desc, out)` |
| `CreateBuffer(ResultDataMaxSize, UAV \| RAYTRACING_ACCELERATION_STRUCTURE, RAYTRACING_ACCELERATION_STRUCTURE)` | `createAccelStruct(AccelStructDesc{ .isPartitioned = true, ... })` |
| `ExecuteIndirectRTASOperations(1, {Inputs=PTPLAS, pPartitionedTlasOperationData}, NONE)` (initial, `Source = 0`) | `executePartitionedAccelStructOps(as, { .performUpdate = false, ... })` |
| same with `Source == Dest` (sparse update) | `executePartitionedAccelStructOps(as, { .performUpdate = true, ... })` |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION` array written by compute | `rt::partitioned::Operation` + app buffer |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS` | `rt::partitioned::WriteInstanceArgs` |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION_UPDATE_INSTANCE_ARGS` | `rt::partitioned::UpdateInstanceArgs` |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TRANSLATE_PARTITION_ARGS` | `rt::partitioned::TranslatePartitionArgs` |
| `CD3DX12_RESOURCE_BARRIER::UAV(ptlas)` after build | automatic barrier (`AccelStructWrite` → `AccelStructRead`) |

| Sample call (clusters) | nvrhi equivalent |
|---|---|
| `ExecuteIndirectRTASOperations` with `ClusterTrianglesDesc` | `executeMultiIndirectClusterOperation` (`OperationType::ClasBuild`) |
| `ClusterTemplateTrianglesDesc` | `ClasBuildTemplates` (missing hint/instantiation split, C3) |
| `InstantiateClusterTemplateDesc` | `ClasInstantiateTemplates` |
| `ClasDesc` | `BlasBuild` |
| `ClusterMovesDesc` | `Move` |
| `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_CLUSTERED_GEOMETRY` | `PipelineDesc::allowClusters` |
| `D3D12_RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY` / pre-SM6.9 pipeline flag | not provided by nvrhi — app-side shader/pipeline define (check `NVRHI_RAYQUERY_OMM_PIPELINE_FLAG` as precedent) |

## Appendix B — layout constants to pin with `static_assert`

| Struct | Size | Alignment / stride rule |
|---|---|---|
| `D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS` | 80 | 8 |
| `D3D12_RTAS_OPERATION_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES_ARGS` | 96 | 8 |
| `D3D12_RTAS_OPERATION_INSTANTIATE_CLUSTER_TEMPLATES_ARGS` | 32 | 8 |
| `D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS` | 16 | 8 |
| `D3D12_RTAS_OPERATION_MOVE_CLUSTER_OBJECTS_ARGS` | 8 | 8 |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS` | 104 | natural (4) |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION_UPDATE_INSTANCE_ARGS` | 16 | 8 |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TRANSLATE_PARTITION_ARGS` | 16 | 4 |
| `D3D12_RTAS_PARTITIONED_TLAS_OPERATION` | 24 | 8 |
| `D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC` | 20 | 4 |

Existing nvrhi `static_assert` blocks (`d3d12-raytracing.cpp:1029-1084` for NVAPI and `:1225-1295` for
DXR) are the model for the PTLAS equivalents.
