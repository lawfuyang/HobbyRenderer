# HobbyRenderer

A 3D rendering engine built in C++, featuring modern graphics techniques and supporting Direct3D 12 API.
<img width="`1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/241f11a4-709c-4598-a0d2-2364fb758e73" />
<img width="1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/d8f3c499-4f13-4549-8c5e-dd43b4b3b963" />
## Features

### Core Rendering Pipeline
- **Deferred Rendering**: Multi-pass architecture with G-Buffer containing albedo, geometry normals, world-space normals, ORM (occlusion/roughness/metallic), emissive, and motion vector channels
- **Reference Path Tracer**: Unbiased Monte Carlo path tracing with next event estimation, Russian roulette termination, and BRDF importance sampling
- **Physically-Based Rendering (PBR)**: Full PBR material support with metallic/roughness workflow, including transmission, thickness, and Fresnel-Schlick approximation
- **Transparency**: Forward rendering for transparent objects with transmission, IOR, spectral attenuation, and volumetric absorption
- **HDR Rendering**: High dynamic range pipeline with histogram-based automatic exposure adaptation (EV100) and physically-based tone mapping
- **Ray-Traced Shadows**: Hardware-accelerated ray tracing for directional light shadows with inline ray queries
- **ReSTIR DI (Direct Illumination)**: Advanced stochastic light sampling with initial sampling modes (uniform, Power-RIS, ReGIR-RIS), temporal and spatial resampling, and boiling filter for variance reduction
- **ReSTIR GI (Global Illumination)**: Indirect lighting via RTXDI's ReSTIR GI framework with temporal & spatial resampling, final visibility rays, MIS, and additive BRDF blending
- **ReGIR (Reservoir-based Grid Importance Resampling)**: Onion-mode spatial grid for efficient light distribution (5 detail layers, 10 coverage layers, 512 lights per cell) with configurable cell size and presampling
- **NRD RELAX Denoising**: NVIDIA Real-time Denoiser integration with diffuse + specular RELAX denoising, anti-firefly filtering, and NRD-pack normal roughness pre-pass
- **SHARC (Spatial Hash Radiance Cache)**: Screen-space indirect lighting via hash-based radiance cache with sparse update, temporal resolve/eviction, and screen-space query passes
- **Indirect Lighting Pipeline**: Selectable indirect lighting technique — None, ReSTIR GI, or SHARC — all composited into the deferred shading pass
- **FSR3 Temporal Anti-Aliasing (TAA)**: AMD FidelityFX SDK integration providing high-quality TAA with HDR support, sharpness control, jitter cancelation, debug view, and exposure-aware pre-exposure
- **Bloom**: Multi-stage pyramid-based bloom with prefilter, configurable intensity, knee, and upsample radius
- **Atmosphere Rendering**: Physically-based sky and sun atmosphere lighting across all rendering modes

### Rendering Techniques
- **GPU-Driven Rendering**: Meshlet-based geometry processing using mesh shaders with indirect dispatch
- **Meshlet Rendering**: Efficient GPU-driven rendering with automatic meshlet generation and caching (64 vertices, 96 triangles per meshlet)
- **Ray Tracing Acceleration**:
  - **Multi-LOD BLAS/TLAS**: Bottom and top-level acceleration structures with per-LOD geometry support
  - **TLASPatch Synchronization**: Compute shader for updating BLAS addresses across LOD levels
  - **LOD-Aware Ray Tracing**: Automatic or manual LOD selection for ray tracing operations
- **Bindless Textures & Samplers**: Descriptor indexing for unlimited texture and sampler access without binding changes
- **Hierarchical Z-Buffer (HZB)**: Multi-level depth buffer for efficient occlusion culling using AMD Single Pass Downsampler (SPD) with min reduction
- **Advanced GPU Culling**: 
  - **Phase 1**: Frustum culling combined with occlusion culling using HZB
  - **Phase 2**: Occlusion culling on occluded primitives with meshlet job generation
  - **Cone Culling**: Conservative back-face and silhouette culling for opaque geometry
  - **Hierarchical LOD (Level of Detail)**: Up to 8 distance-based LOD levels with progressive mesh simplification using meshoptimizer
- **Multi-threaded Rendering**: Parallel command list recording and asynchronous task scheduling
- **Image-Based Lighting (IBL)**: Environment lighting with irradiance and radiance cubemaps, including BRDF lookup table and Bruneton atmosphere textures
- **AMD FidelityFX SPD**: Single Pass Downsampler for efficient HZB generation (min reduction) and texture mip-map chain generation (average reduction)
- **FSR3 TAA**: AMD FidelityFX Super Resolution 3 temporal anti-aliasing with native resolution support, HDR aware, motion vector jitter cancelation, and runtime sharpness control

### Performance & Profiling
- **Microprofile Integration**: Detailed CPU and GPU performance profiling with real-time visualization and HTML dump export
- **Pipeline State Caching**: Automatic caching of graphics, meshlet, and compute pipeline states
- **GPU Profiling**: Per-renderer GPU timing via integrated timer queries, pipeline statistics collection (IA, VS, GS, PS, HS, DS, CS, AS, MS invocations and primitive counts), and per-pass CPU/GPU timing tables
- **Memory Efficiency**: Render graph resource aliasing for minimal VRAM usage with transient resource pooling, detailed memory statistics, and D3D12 VRAM usage monitoring
- **Render Graph Visualizer**: Real-time resource lifetime visualization, heap allocation tracking, pass dependency graphing, and per-resource read/write access inspection

### Developer Experience
- **ImGui UI**: Real-time debugging interface with rendering mode selection, indirect lighting technique selection, pass toggles, per-pass CPU/GPU timing table, and comprehensive performance metrics
- **Debug Visualization Modes**:
  - Instance visualization (per-instance coloring)
  - Meshlet visualization (per-meshlet coloring)
  - World normals, albedo, roughness, metallic, emissive visualization
  - LOD level visualization
  - Motion vector visualization
  - ReGIR cell visualization
  - Isolated bloom debug view
  - TAA debug view (FSR3 native debug overlay)
  - SHARC cache debug overlay (heatmap)
- **Flexible Configuration**: Runtime adjustable parameters for culling (frustum, occlusion, cone), rendering mode, meshlet toggling, LOD forcing, post-processing (auto exposure, adaptation speed, bloom, tone mapping), and lighting (sky, RT shadows)
- **ReSTIR DI/GI Tuning**: Advanced/standard settings panels with per-pass parameter control — initial sampling (BRDF, light, environment counts), temporal/spatial resampling thresholds, boiling filter strength, bias correction modes, and GI final visibility/MIS
- **ReGIR Configuration**: Runtime cell size, grid build samples, sampling jitter, presampling and fallback mode selection
- **NRD Denoising Toggle**: RELAX diffuse+specular denoising with anti-firefly filtering
- **Scene Statistics**: Real-time display of instance counts (opaque, masked, transparent), VRAM usage, pipeline statistics, and render graph memory breakdown
- **Modern C++**: C++20 features with clean, maintainable architecture and automatic dependency management via CMake
- **Shader Hot Reloading**: Runtime shader recompilation and seamless reloading without engine restart
- **D3D12 Debug & Validation**: Configurable debug layer, GPU-based validation, and stable power state for consistent profiling results
- **Camera State Persistence**: Automatic camera state save/restore across scene loads via `CameraStateManager`
- **Command Line Configuration**: Scene path and validation flags configurable via command line arguments
- **Screenshot Capture**: One-click backbuffer screenshot saving at runtime

## Architecture

The engine is built around several key architectural components:

### Render Graph System
- **Automatic Resource Management**: Transient resources (textures, buffers) are automatically allocated and freed
- **Resource Aliasing**: Memory-efficient resource reuse across rendering passes
- **Data-Flow Tracking**: Implicit dependency resolution between rendering passes
- **Efficient Scheduling**: Automatic pass ordering based on resource dependencies

### Rendering Pipeline Architecture
- **Modular Renderer Design**: Each rendering pass is implemented as a separate `IRenderer` interface
- **Registered Renderer System**: Dynamic renderer registration for flexible pass composition
- **Per-Pass Toggle**: Each rendering pass can be individually enabled/disabled at runtime via the ImGui UI for targeted debugging
- **Command List Management**: Efficient command buffer pooling and reuse with parallel renderer execution support
- **Pipeline State Caching**: Automatic caching of graphics and compute pipelines for reduced CPU overhead

### Graphics Abstraction Layer
Built on top of NVRHI (NVIDIA Rendering Hardware Interface) providing:
- Unified API for Vulkan and Direct3D 12 with seamless cross-platform rendering
- Automatic resource state management and synchronization
- Cross-API shader compilation (SPIR-V/DXIL)
- Hardware capability querying and feature negotiation
- GPU memory management and resource pooling

### Scene Management
- **glTF 2.0 Loading**: Complete scene import from standard glTF 2.0 files via cgltf
- **Binary Scene Caching**: Fast loading with binary cache validation, versioning, and cache invalidation
- **Mesh Optimization**: Vertex quantization, mesh optimization, and automatic LOD generation (up to 8 levels) with meshoptimizer
- **Animation System**: GPU-friendly animation playback with multiple interpolation modes (linear, step, cubic spline, SLERP, Catmull-Rom); automatic identification of animated and dynamic nodes with topologically-sorted update order; material animation support (emissive intensity)
- **Lighting**: Automatic light buffer generation with ray tracing support; directional/infinite lights with sun direction and angular size; point/local lights with per-light RIS tile PDF sampling; environment lights with equirectangular PDF-based sampling; always a fallback default directional light
- **Camera Persistence**: `CameraStateManager` automatically saves camera position/orientation and restores it after scene transitions

## Dependencies

The project automatically downloads and builds the following dependencies:
- **[SDL3](https://www.libsdl.org/)**: Cross-platform windowing and input
- **[NVRHI](https://github.com/NVIDIA-RTX/NVRHI)**: Graphics API abstraction layer
- **[ImGui](https://github.com/ocornut/imgui)**: Immediate mode GUI
- **[cgltf](https://github.com/jkuhlmann/cgltf)**: glTF 2.0 parsing
- **[DirectX Shader Compiler (DXC)](https://github.com/microsoft/DirectXShaderCompiler)**: HLSL compilation
- **[ShaderMake](https://github.com/NVIDIA-RTX/ShaderMake)**: Offline shader compilation tool
- **[meshoptimizer](https://github.com/zeux/meshoptimizer)**: Mesh optimization and quantization
- **[microprofile](https://github.com/jonasmr/microprofile)**: Performance profiling
- **[stb_image](https://github.com/nothings/stb)**: Image loading
- **[RTXDI](https://github.com/NVIDIA-RTX/RTXDI)**: ReSTIR Direct Illumination framework and light sampling utilities
- **[NRD](https://github.com/NVIDIAGameWorks/RayTracingDenoiser)**: NVIDIA Real-time Denoiser (RELAX for diffuse + specular)
- **[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)**: FSR3 temporal anti-aliasing and Single Pass Downsampler (SPD)
- **[SHARC](https://github.com/NVIDIAGameWorks/SHARC)**: Spatial Hash Radiance Cache for real-time indirect lighting
- **[Agility SDK](https://github.com/microsoft/DirectX-Headers)**: Direct3D 12 core headers and runtime support
- **[MathLib](https://github.com/NVIDIA-RTX/MathLib)**: Vector/matrix math utilities

## Rendering Modes

The engine supports multiple rendering modes, selectable via the ImGui UI or at runtime:

- **Normal Mode**: Standard deferred rendering with real-time performance focus
- **IBL Mode**: Image-based lighting dominant, useful for controlled lighting conditions
- **Reference Path Tracer**: Unbiased Monte Carlo path tracing for reference-quality rendering with progressive refinement

## Shader System

Shaders are written in HLSL and compiled offline to both SPIR-V (Vulkan) and DXIL (Direct3D 12) using ShaderMake. The shader configuration is defined in `src/shaders/shaders.cfg`.

Key shader features:
- **Shader Model 6.8**: Latest HLSL features including mesh shaders and ray tracing
- **Cross-compilation**: Automatic compilation to both SPIR-V and DXIL formats
- **Offline Compilation**: Shaders compiled at build time for faster loading via ShaderMake
- **Bindless Resources**: Descriptor indexing for textures and buffers
- **Ray Tracing**: Inline ray queries (RayQuery) for shadows, ReSTIR DI/GI, SHARC, and path tracing
- **Mesh Shaders**: GPU-driven geometry processing with automatic task/mesh shader pipeline generation
- **Compute Shaders**: For culling, histogram generation, SPD mip reduction, ReSTIR passes, SHARC passes, and post-processing
- **SRRHI (Shader Resource Render Hardware Interface)**: Custom `.sr` shader resource interface system that generates C++ and HLSL binding code automatically, eliminating manual descriptor table construction

### Supported Features
- **Meshes**: Triangle meshes with automatic vertex quantization, mesh optimization, and up to 8-level hierarchical LOD generation
- **Materials**: 
  - Core PBR with albedo, normal, ORM (occlusion/roughness/metallic), and emissive textures
  - KHR Extensions: Index of Refraction (IOR), transmission factor, volumetric properties
  - Volume Properties: Thickness factor, attenuation distance/color, absorption (Sigma_A) and scattering (Sigma_S) coefficients, thin-surface flag
  - Material Animations: Emissive intensity animation via JSON
- **Animations**:
  - Skeletal and transform animations with multiple interpolation modes
  - Interpolation Types: Linear, step (discrete), cubic spline, spherical linear interpolation (SLERP) for quaternions, and Catmull-Rom spline
  - Dynamic Node Tracking: Automatic identification of animated nodes
  - Material Animation: Emissive intensity animation support
- **Lights**:
  - Directional/infinite lights with ray-traced shadow support and atmosphere-aware intensity
  - Point/local lights with per-light PDF sampling
  - Environment lights with PDF-based sampling in ReSTIR DI
  - Configurable per-light sampling (128 RIS tiles × 1024 samples)
- **Cameras**: Perspective cameras with configurable field-of-view, near/far planes, manual/auto exposure with EV100 support, and exposure compensation
- **Texture Formats**: Supports DDS cubemaps for IBL (irradiance and radiance maps), BRDF LUT, and Bruneton atmosphere precomputed textures
