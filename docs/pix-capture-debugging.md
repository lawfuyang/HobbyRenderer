# Debugging with PIX captures

How to take, read, process and debug `.wpix` captures of this renderer with
`pixtool.exe`, and what the capture can and cannot tell you. Written from the tool's own
`--help` output plus measurements on this machine (PIX 2610.28, driver 616.92, RTX 3080).

Operational note: the opacity-micromap feature is off by default (`--enable-omm` turns it on, and
`--pix-capture-load` captures the bake and BLAS builds where its data is created).

---

## 1. Taking a capture

PIX captures are taken **by the application**, through WinPixEventRuntime + WinPixGpuCapturer
(see `src/PixCapture.h`). No PIX UI or attach is involved.

| Switch | What it brackets | File |
|---|---|---|
| `--pix` | nothing by itself; the ImGui panel's **PIX Capture** button captures the next frame | `bin/captures/<scene>-frame<n>.wpix` |
| `--pix-capture-frame <n>` | one frame, from swapchain acquire to present, then exits | same |
| `--pix-capture-load` | the **scene load**: bake → OMM array builds → BLAS/TLAS builds, then exits | `bin/captures/<scene>-load.wpix` |
| `--pix-capture-dir <dir>` | capture directory (default `bin/captures/<scene stem>`) | — |

Rules that cost time when ignored:

- A **frame** capture starts at a frame boundary. Anything that happens during load — the
  micromap array builds, the BLAS builds, texture uploads — is *not* in it. Use
  `--pix-capture-load` when the question is about those.
- PIX and RenderDoc hook the same queue, so their brackets are mutually exclusive
  (`Renderer.cpp` asserts this).
- A capture stalls the frame it records (the capturer copies the GPU state). Never mix
  captures or `--screenshot-frame` into frame-time or hitch measurements.

`WinPixGpuCapturer.dll` must sit next to `HobbyRenderer.exe`; CMake stages it from the
newest `C:\Program Files\Microsoft PIX\<version>` install, and `PixCapture::Load` falls back
to scanning that directory.

---

## 2. The tool

```
"C:\Program Files\Microsoft PIX\2610.28\pixtool.exe"
```

Every command line is a **sequence of commands**, executed left to right, so state carries
over (`open-capture` then one or more operations). Global options go *before* the commands:

| Global option | Meaning |
|---|---|
| `--output=quiet\|trace\|engine\|verbose` | console verbosity, default `trace`. `verbose` prints the replay/analysis steps (use it when something fails) |
| `--log=<level>` / `--log-file=<path>` | log file verbosity (`off` disables), default `verbose` in `%TEMP%\pixtool.log` |
| `--help [<command>]` | usage for everything, or for one command. This is the authority; the list below is a summary |

Exit codes: `0` success, `7` "engine request failed" (the analysis failed, message on stderr),
`99999` unknown failure — usually one of the known issues in §6.

---

## 3. Command reference

Capture production:

| Command | Notes |
|---|---|
| `launch <exe> [--command-line=] [--working-directory=] [--setenv=VAR=VAL] [--timing] [--captureFromStart] [--force11on12] [--remote=]` | runs an app under PIX. Not needed here: this renderer captures itself |
| `launch-app <package> <app>` | UWP equivalent |
| `set-gpu-capture-parameters [--frames=<n>] [--capture-key=<F1-F12>] [--remote=]` | defaults for the commands below |
| `take-capture [--open] [--frames=<n>]` | capture the launched app |
| `programmatic-capture [--open] [--until-exit]` | waits for the app's own `PIXBeginCapture2`; `--until-exit` + `save-all-captures` collects several |
| `save-capture <file>` / `save-all-captures <dir>` | writes captures taken above |

Reading and processing:

| Command | Notes |
|---|---|
| `open-capture <file> [--remote=] [--disable-gpu-plugins] [--use-replay-time-executeindirect-buffers] [--enable-recreate-at-gpuva] [--disable-application-specific-driver-state]` | opens a `.wpix` for the commands that follow |
| `list-counters` | prints every hardware counter name this GPU+driver exposes (618 here) |
| `save-event-list <csv> [--counters=<pattern>]... [--counter-groups=<pattern>]... [--queue-name=]` | the workhorse: one CSV row per API call/marker. `Queue ID, Parent, Name, Global ID` are always present; one extra column per requested counter |
| `save-resource <file> [--rtv=<n>] [--depth] [--global-id=<id> \| --marker=<name>]` | saves a render target (`.dds`) or a depth buffer (`.png`) from one event |
| `save-high-frequency-counters <csv> [--counters=<pattern>]... [--merge]` | per-timestamp counter sampling (a timeline, not per event) |
| `save-screenshot <png>` | the capture's "gold image" (broken on this machine, see §6) |
| `perform-single-playback [--do-not-expand-executeindirect] [--loop] [--time-cpu] [--measure-cycles]` | replays the frame once. Silent and exit code 0 on success |
| `run-debug-layer` | forces the D3D12 debug layer during replay. **Resets the device on this renderer** (§6) |
| `recapture-region <out.wpix> [--start=<global id>] [--end=<global id>]` | re-captures a slice of the capture |
| `recapture-single-playback [--include-recreation] [--expand-execute]` | re-captures a playback of the open file |
| `begin-recapture` / `end-recapture` | bracket a capture of pixtool itself |
| `take-new-timing-capture <file> [--duration=<n>]` | timing capture of the attached app |
| `attach <pid>` | attach to a running process |
| `export-to-cpp <dir> [--force] [--use-winpixeventruntime]` | exports the capture as a standalone C++ replay project (the only CLI route to shaders/constants) |
| `upgrade-gpu-capture <src> [--dest=]` | converts to the newest capture format |

Counter pattern syntax: `*` matches any run of characters, one pattern per `--counters=`
switch (`--counters=gpu__*` = the 17 `gpu__` counters). `--counter-groups=` takes group
patterns such as `D3D:*`.

---

## 4. Recipes

All paths are relative to the repository root and assume
`PXT='C:\Program Files\Microsoft PIX\2610.28\pixtool.exe'`.

### 4.1 Event list of a frame

```powershell
& $PXT --output=verbose open-capture bin\captures\bistrox-frame90.wpix `
      save-event-list bin\ev.csv
```

The CSV is hierarchical through `Parent` (a `Global ID`, or `-1` for roots). Marker rows
(*our* `PIXBeginEvent` scopes) have an **empty** `Global ID`, so select by name:

```powershell
$c = Import-Csv bin\ev.csv
$c | Where-Object { $_.Name -match 'Renderer$|Pass$' } |
     ForEach-Object { "{0,6} p={1,6} {2}" -f $_.'Global ID', $_.Parent, $_.Name }
```

### 4.2 Event list with hardware counters

```powershell
& $PXT --output=quiet open-capture bin\captures\bistrox-frame90.wpix `
      save-event-list bin\ev_c.csv `
      --counters=gpu__* --counters=smsp__inst_executed.sum `
      --counters=l1tex__t_sectors.sum --counters=dram__bytes.sum
```

Do **not** use `--counters=*` (§6). Rows for markers and API calls carry empty counter
cells; filter on `gpu__time_duration.sum -ne ''`. Aggregate per pass name:

```powershell
$g = @{}
foreach ($r in (Import-Csv bin\ev_c.csv | Where-Object { $_.'gpu__time_duration.sum' -ne '' })) {
    $n = ($r.Name -replace "`0",'').Trim(); if (-not $n) { continue }
    if (-not $g[$n]) { $g[$n] = @(0.0, 0.0) }
    $g[$n][0] += [double]$r.'smsp__inst_executed.sum'
    $g[$n][1] += [double]$r.'l1tex__t_sectors.sum'
}
$g.GetEnumerator() | Sort-Object { $_.Value[0] } -Descending |
    Select-Object -First 15 | ForEach-Object { "{0,-46} inst={1,14:N0} sect={2,13:N0}" -f $_.Key, $_.Value[0], $_.Value[1] }
```

`smsp__inst_executed.sum` and the `l1tex` counters reproduce to 0.1 % between two runs of
the same binary, so they are the right tool for A/B comparisons; `gpu__time_duration.sum`
under counter collection is inflated by replay instrumentation and only useful for ranking.

### 4.3 A per-pass image

```powershell
# textures: .dds (PNG only works with --depth)
& $PXT --output=quiet open-capture bin\captures\bistrox-frame90.wpix `
      save-resource bin\deferred.dds --marker=Deferred --rtv=0
# depth of the base pass, as a usable image
& $PXT --output=quiet open-capture bin\captures\bistrox-frame90.wpix `
      save-resource bin\depth.png --marker=MaskedPass --depth
```

`--global-id=<id>` selects an exact event; `--marker=<name>` picks the last child of that
marker that has the resource bound. Our markers are listed in §5.

### 4.4 Capturing and inspecting the load (OMM/BLAS builds)

```powershell
.\bin\HobbyRenderer.exe --scene "D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf" --pix-capture-load --screenshot-frame 15
& $PXT --output=quiet open-capture bin\captures\bistrox-load.wpix save-event-list bin\ev_load.csv
```

9827 events; the micromap array builds are the 121
`BuildRaytracingAccelerationStructure` events under the `Build Scene OMMs` marker (D3D12
builds OMM arrays through the same entry point as a BLAS, so the name does not distinguish
them — the PIX acceleration-structure viewer does, and it shows the `OMM_Data`,
`OMM_Descs` and `OMM_Indices` inputs). The 1299 events under `Build Scene Accel Structs`
are the per-LOD BLASes and the TLAS, whose geometries link those arrays.

### 4.5 Playback

```powershell
& $PXT --output=verbose open-capture bin\captures\bistrox-frame90.wpix perform-single-playback
```

Success is silent (`exit 0`, only the analysis engine lines). Add `run-debug-layer` **only**
if you want the D3D12 layer's opinion and accept the device reset (§6); for validation of
the *running* app use the renderer's own `--rhidebug` instead, which covers the same ground
without PIX in the middle.

### 4.6 Sanity checks worth running on any capture

1. `save-event-list` — does the frame contain the passes you expect, in the expected order?
2. `perform-single-playback` — exit 0? (A reset here means the capture is not replayable.)
3. Event count and root-marker count stable across runs? Marker names are the renderer's own
   scopes, so a missing one means the pass did not run.

---

## 5. Our marker scopes

Recorded with `ScopedCommandList` / `ScopedGpuTimerGroup`, so they appear as event names:
`Opaque Renderer` (`Base Pass Render (Phase 1) - Opaque/Masked/Transparent`), `MaskedPass`,
`TransparentPass`, `HZBGeneratorPhase2`, `TLAS Update`, `Build Scene OMMs`,
`Build Scene Accel Structs`, `SHARCRenderer` (`SHARCUpdate_SHARCUpdate_CSMain`,
`SharcResolve_SHARCResolve_CSMain`), `RTXDIRenderer` (`rtxdi/LightingPasses/...`),
`NRD Denoise` (`REBLUR_DiffuseSpecular - *`), `Deferred` (`DeferredLighting_..._PSMain`),
`FSR3 TAA`, `HDRPostProcess`, `Luminance Histogram`, `Tonemapping`, `GPUCulling_*`,
`Build Indirect Arguments`, `DispatchMesh` / `Dispatch` / `ExecuteIndirect` for the raw
draws and dispatches.

nvrhi also emits `[rlaw]`-tagged markers (copies, barriers, state transitions) with the
WinPixEventRuntime build, so the event list is noisy at the bottom of the tree.

---

## 6. Known issues on this machine

These are tool/driver bugs, not renderer bugs; all were reproduced with a clean capture.

| Symptom | Cause / workaround |
|---|---|
| `save-screenshot` fails in <1 s with `0x80004003` / `PIXTOOL99999` | pixtool's gold-image path is broken here. Use `save-resource --rtv=0` on the final composition event, or the renderer's own `--screenshot-frame` |
| `save-resource …png` fails with `0x80070032` (`ERROR_NOT_SUPPORTED`) | PNG is only for `--depth`; textures must be written as `.dds` (they keep their DXGI format, e.g. 26 = `R10G10B10A2_UNORM`) |
| `run-debug-layer` + `perform-single-playback` → `Replay: device has been removed` (`0x887A0007`, device reset) during `GET_TITLE_WARNINGS` | the debug layer's replay resets the device on this renderer. Drop `run-debug-layer`; validate with the app's `--rhidebug` |
| `--counters=*` → `E_PIX_PERFORMANCE_ANALYSIS_FAILED`, `Expected 71 number of results per event … 50 returned` | the NVIDIA plugin mis-handles the full 618-counter request. Ask for a pattern (`--counters=gpu__*`) or explicit names |
| `GPU plugin initialization failed: 0x80004005 … performance counters` | the NVIDIA performance-counter permission is off: **NVIDIA App → System → Advanced → Developer → Manage GPU Performance Counters → allow all users** (needs admin). `list-counters` working is the proof it is on |
| `WinPixGpuCapturer.dll error: Invalid/Uninitialized descriptor used in command list event` ×2730 on streamed scenes | the capturer does not track sampler-feedback-map descriptors created by `CreateSamplerFeedbackUnorderedAccessView` inside a bindless heap. Harmless; 0 for scenes without streaming |
| `Global ID` empty for many rows | PIX only numbers some events. Use name/`Parent`/`--marker` instead of assuming every row has an id |
| Names contain a trailing `\0` when read with `Import-Csv` | PIX writes fixed-width strings. `-replace "`0",''` before comparing |

---

## 7. What the CLI cannot do

Everything not listed in §3 lives in the PIX GUI only:

- shader source, DXIL, disassembly, and the pipeline state of an event;
- constant/root-constant values (the "Parameters" tab);
- the acceleration-structure viewer (BLAS/TLAS contents, OMM arrays and their data);
- arbitrary UAV/buffer dumps — `save-resource` only writes RTVs and depth;
- editing/annotating the capture.

`export-to-cpp <dir>` is the CLI escape hatch: it writes the whole capture (resources,
command lists, shaders) as a replayable C++ project, which is how a headless pipeline can
get at data the other commands do not expose. It is large and slow — use it as a last resort.

---

## 8. Quick reference

```powershell
$PXT = 'C:\Program Files\Microsoft PIX\2610.28\pixtool.exe'

# event list, counters, per-pass image, playback, load capture
& $PXT --output=quiet open-capture bin\captures\bistrox-frame90.wpix save-event-list bin\ev.csv
& $PXT --output=quiet open-capture bin\captures\bistrox-frame90.wpix save-event-list bin\evc.csv --counters=gpu__*
& $PXT --output=quiet open-capture bin\captures\bistrox-frame90.wpix save-resource bin\rt.dds --marker=Deferred --rtv=0
& $PXT --output=verbose open-capture bin\captures\bistrox-frame90.wpix perform-single-playback
& $PXT open-capture bin\captures\bistrox-load.wpix save-event-list bin\ev_load.csv
& $PXT open-capture bin\captures\bistrox-frame90.wpix list-counters
```

---

## 9. The PIX API (`PixApi.h`)

`pixtool` covers event lists, counters, render targets and playback. Everything else the capture
holds — resource contents, pipeline state, shader code, resource names — is reachable through the
**PIX API**: `include\PixApi.h` + `lib\PixApi.lib` + `pixapi.dll` inside the PIX install, with a
C# projection (`PixApiCsExt.dll`) and a Python binding on top of it.

Reference samples live outside this repository (clone `https://github.com/microsoft/PIX-Samples`,
here at `D:\Workspace\pix-samples`); the sample `README.md` files are the authority for each
workflow. The install also ships the full reference as
`<PixInstall>\docs\html\index.html`.

### 9.1 Finding the install without hardcoding it

Never bake `C:\Program Files\Microsoft PIX\2610.28` into a script. Two independent routes:

1. **Just look next to the tool you already have.** `pixtool.exe` sits in the same folder as
   `include\`, `lib\` and `pixapi.dll`, so `(Get-Command pixtool.exe).Source | Split-Path` *is* the
   install directory. That is the cheapest discovery and it cannot drift.
2. **Scan the convention paths and pick the newest version numerically.** PIX names its folders
   `YYMM.DD[.NNN][-flavor]` (`2606.17`, `2602.24.004-main`), so parse the numeric fields — a plain
   string sort puts `2606.9` after `2606.17`, which is wrong:

```powershell
function Get-PixInstallDir {
    param([string]$Override)
    if ($Override) { return $Override }                      # -PixInstallDir / PIX_DIR wins
    if ($env:PIX_DIR) { return $env:PIX_DIR }
    $roots = @("$env:ProgramW6432\Microsoft PIX", "$env:ProgramFiles\Microsoft PIX",
               "$env:ProgramW6432\Microsoft PIX Preview", "$env:ProgramFiles\Microsoft PIX Preview")
    $candidates = foreach ($root in $roots) {
        if (Test-Path $root) {
            Get-ChildItem $root -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'include\PixApi.h') } |
                ForEach-Object {
                    $ver = ($_.Name -split '-')[0]
                    $key = ($ver -split '\.' | ForEach-Object { '{0:D6}' -f [int]$_ }) -join '.'
                    [pscustomobject]@{ Path = $_.FullName; Key = $key }
                }
        }
    }
    ($candidates | Sort-Object Key -Descending | Select-Object -First 1).Path
}
```

Expected layout under the result: `include\PixApi.h` (+ the sibling `PixApi*.h` headers),
`lib\PixApi.lib`, `pixapi.dll`, `PixApiCsExt.dll`, and `docs\html\index.html`.

### 9.2 Call sequence

The API is COM-shaped but has a plain C entry point, so a consumer is short:

```cpp
#include <PixApi.h>            // pulls in the interface headers you use
// link lib\PixApi.lib, keep pixapi.dll next to the exe or on PATH

IPixFactory* factory = nullptr;
HRESULT hr = PixCreateFactory(__uuidof(IPixFactory), reinterpret_cast<void**>(&factory));

IPixGpuCaptureDocument* document = nullptr;
hr = factory->OpenGpuCaptureDocument(L"bin\\captures\\bistrox-load.wpix",
                                     /*notifications*/ nullptr, /*logger*/ nullptr,
                                     __uuidof(IPixGpuCaptureDocument), reinterpret_cast<void**>(&document));

// document-level queries
IPixGpuCaptureQueueList* queues = nullptr;   document->GetQueues(__uuidof(IPixGpuCaptureQueueList), ...);
IPixD3D12Resources*      resources = nullptr; document->GetD3D12Resources(__uuidof(IPixD3D12Resources), ...);
PIX_EVENT_INFO event;                          document->GetEvent(...);
IPixGpuCaptureAnalysis*  analysis = nullptr;   document->GetAnalysis(__uuidof(IPixGpuCaptureAnalysis), ...);
```

Document methods worth knowing (`PixApiGpuCaptureInterfaces.h`):

| Method | Gives you |
|---|---|
| `GetFileInfo`, `GetApplicationDescription` | capture/app metadata |
| `GetQueues` | the queues and the event tree (equivalent of `save-event-list`) |
| `GetEvent` | one `PIX_EVENT_INFO` by id |
| `GetD3D12Resources`, `GetD3D12ApiObjects` | every resource/object in the capture, with names and descs |
| `GetAccessedResources`, `GetResourceViews` | what an event touched, including **bindless and dynamically indexed** resources (this is how a bindless renderer is inspected properly) |
| `GetAnalysis` → start analysis | required before counters, playback and any data read |
| `GetGpuCounters`, `GetOccupancy`, `GetHighFrequencyCounters` | the same data `pixtool save-event-list --counters` / `save-high-frequency-counters` produce, with types instead of CSV |
| `GetBufferBytesFromResource`, `GetBufferBytesFromResourceView` | **raw buffer bytes at an event** (an `IPixBlob`) — the only headless way to read actual data out of a capture |
| `GetTextureBytesFromResource` | same for textures |
| `GetShaderProfiling`, `GetDrPix` | shader profiling and the automated performance experiments |
| `GetCapturedScreenshot` | the gold image (`pixtool save-screenshot` is broken here, this is the API path to it) |

Data reads are gated: analysis must be running, the resource must belong to that analysis
instance, and the event must have a parent command list. Windows **Developer Mode** must be on, or
the call fails with `E_PIX_DEVELOPER_MODE_NOT_ENABLED` (-1967390720).

### 9.3 Which sample shows what

| Sample (`api\retail\…`) | Shows |
|---|---|
| `GpuCounters` | create factory → open capture → analysis → enumerate counters/groups → occupancy → high-frequency counters → per-event GPU timing |
| `ResourceViews` | `IPixD3D12Resources` / `IPixD3D12Resource` / `IPixResourceViews`: names, descs and the views bound at an event |
| `PipelineInspection` | pipeline states and the global root signature of an event |
| `DrPix` | running Dr. PIX experiments on a capture |
| `CaptureConverter`, `TimingCapture` | converting captures and timing captures |
| `api\preview\…` | same idea against the preview-only surface (`D3DStateInspection`, `DXDumpFileParser`, `GpuCapture`) |

### 9.4 What actually works on this machine

- Only one install exists: `C:\Program Files\Microsoft PIX\2610.28` (retail). `include`, `lib\PixApi.lib`,
  `pixapi.dll`, `PixApiCsExt.dll`, `docs\pixapi.zip` are all present; .NET SDK 10.0.403 is installed.
- **The samples and the retail API do not line up.** Building `api\retail\ResourceViews\csharp` against
  2610.28 fails on `PixApiExtensions.OpenGpuCaptureDocument<T>` (missing `notifications` overload),
  `PIX_API_OBJECT_ID` → `ulong`, and `PIX_RESOURCE_DESC` → `D3D12_RESOURCE_DESC2*`. The samples' own
  bootstrap (`api\_pix_bootstrap.py`) says it plainly: it scans **only** `Microsoft PIX Preview` because
  "PIX API is not shipped in a retail build yet". So `D:\Workspace\pix-samples` needs a PIX **Preview**
  install (dated after 2026/06/15) to build and run; pointing it at the retail install is not supported.
- The C++ path compiles and loads against retail: `tools/PixProbe` (see 9.5) resolves the install, loads
  `pixapi.dll` by hand and calls `PixCreateFactory`. On retail 2610.28 `OpenGpuCaptureDocument` then fails
  with `0x8ABC000B` (PIX error facility base `0x8ABC0000` = `E_PIX_DEVELOPER_MODE_NOT_ENABLED`, so this is a
  PIX-side refusal, not a file problem — `pixtool` opens the same capture). 2610.28 is the newest public
  build and no Preview install exists to compare against, so **the PIX API is not usable for reading captures
  on this machine today**; `pixtool` is the working headless path, and `PixProbe` is the ready base for the
  day the retail API starts accepting documents.
- Loading `pixapi.dll` needs its own folder on the DLL search path (it has ~15 sibling dependencies):
  `SetDefaultDllDirectories(...) ; AddDllDirectory(pixDir) ; LoadLibraryExW(...)`.
- The Python samples need `pythonnet >= 3.0` (`import clr`); Python 3.14 is installed here but pythonnet is
  not. Building the C++ samples needs the `Microsoft.Direct3D.D3D12` package (1.721.1-preview) — this
  repository supplies the same headers from `external/AgilitySDK` instead.

### 9.5 `tools/PixProbe` — the API consumer in this repo

`tools/PixProbe/main.cpp`, built by CMake as the `PixProbe` target whenever the discovered PIX install has
`include\PixApi.h` (headers only — no import library, `PixCreateFactory` is resolved with
`GetProcAddress`). It is the template to extend rather than starting from the samples:

```powershell
.\bin\PixProbe.exe bin\captures\bistrox-load.wpix --filter=Renderer --limit=8
.\bin\PixProbe.exe bin\captures\bistrox-load.wpix --screenshot=bin\gold.bmp
```

Discovery order inside the tool: `--pix-dir=<dir>` → `PIX_DIR` → newest
`%ProgramW6432%\|%ProgramFiles%\Microsoft PIX[\ Preview|\ Experimental]\<version>` that contains
`include\PixApi.h`, compared by parsed numeric version (the lexical-sort trap the samples warn about).
It prints file info, the queue/event tree (with `--filter`/`--limit`), and writes the capture's gold image
as BMP through `GetCapturedScreenshot` — the data `pixtool save-screenshot` cannot produce on this machine.

Extending it: `GetD3D12Resources` + `IPixCollection::Get(index, __uuidof(IPixD3D12Resource), ...)` enumerates
resources (names and descs — this is how the OMM `OMM_Data`/`OMM_Descs`/`OMM_Indices` buffers become
visible), and after `GetAnalysis` → `StartAnalysis`, `GetBufferBytesFromResource(resource, &event, ...,
__uuidof(IPixBlob), ...)` returns the raw bytes of a buffer at an event — the only headless way to read the
mask data itself. That path needs Developer Mode and a Preview install, per 9.4.

---

## 10. Worked example: verifying opacity micromaps

Two captures of the same frame, one with OMM and one with `--disable-omm`, are enough — the
capture is deterministic in the counters even though the frame is sampled stochastically.

```powershell
$scene = "D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf"
.\bin\HobbyRenderer.exe --scene $scene --pix-capture-frame 90                       # capture A
.\bin\HobbyRenderer.exe --scene $scene --pix-capture-frame 90 --disable-omm          # capture B
```

Then extract per-pass work with `pixtool` (or `GetGpuCounters` / `GetEvent` through the API) and
compare, per pass, in the two event lists:

```powershell
function Agg($p) {
    $g = @{}
    foreach ($r in (Import-Csv $p | Where-Object { $_.'gpu__time_duration.sum' -ne '' })) {
        $n = ($r.Name -replace "`0",'').Trim(); if (-not $n) { continue }
        if (-not $g[$n]) { $g[$n] = @(0.0, 0.0) }
        $g[$n][0] += [double]$r.'smsp__inst_executed.sum'
        $g[$n][1] += [double]$r.'l1tex__t_sectors.sum'
    }
    $g
}
$on = Agg bin\ev_omm.csv; $off = Agg bin\ev_noomm.csv
foreach ($k in $on.Keys) { if ($off[$k]) { "{0,-46} inst x{1:N3}  sect x{2:N3}" -f $k, ($on[$k][0]/$off[$k][0]), ($on[$k][1]/$off[$k][1]) } }
```

Rules that make the comparison meaningful:

1. Both captures must have the **same event count** (same view, same streaming state); otherwise the
   frames are not comparable at all.
2. Compare `smsp__inst_executed.sum` and the `l1tex` counters. They reproduce to ~0.1 % between two
   identical runs, while `gpu__time_duration.sum` under counter collection is inflated by replay.
3. Raster and denoise passes must read ×1.000 — that is the control proving the only difference is
   ray-query traversal.

The `--pix-capture-load` capture answers the *structure* question (which OMM arrays exist, which
BLAS link them, how big they are); the frame pair answers the *effect* question (how much work the
masks add or remove). `GetBufferBytesFromResource` on the load capture is the only way to read the
mask bytes themselves, and `GetTextureBytesFromResource` / `GetCapturedScreenshot` cover the image
side that `save-resource` and `save-screenshot` serve (or fail to serve) on this machine.

### 10.1 Measured on Bistro frame 90 (current build)

Both captures 1441 events; raster and denoise passes ×1.000 in every column, so the two frames are
the same view of the same work.

| pass | inst (OMM / no OMM) | l1tex sectors | dram bytes |
|---|---|---|---|
| `rtxdi/…/ShadeSamples_main` | ×1.545 | ×0.901 | ×0.940 |
| `rtxdi/…/SharcIndirectQuery` | ×1.569 | ×0.947 | ×0.951 |
| `SHARCUpdate_SHARCUpdate_CSMain` | ×1.395 | ×0.908 | ×0.982 |
| `SHARCRenderer` | ×1.329 | ×0.918 | ×0.954 |
| `rtxdi/…/SpatialResampling_main` | ×0.994 | ×0.997 | ×1.004 |
| `MaskedPass`, `Opaque Renderer`, `REBLUR_*`, `FSR3 TAA` | ×1.000 | ×1.000 | ×1.000 |

Reading: the masks are **active** (only the ray-query passes move) and they **cut texture traffic**
(5–10 % fewer L1 sectors, 2–6 % less DRAM) but **add shader work** (33–57 % more instructions),
because every non-opaque instance is built with `InstanceFlags::ForceNonOpaque`, which turns
OMM-resolved *opaque* hits back into shader candidates that alpha-test again. Dropping that flag
measured ×0.758–0.817 of the same passes earlier, at the cost of a slightly coarser silhouette.

