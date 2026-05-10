# HobbyRenderer

A 3D rendering engine built in C++, featuring modern graphics techniques and supporting Direct3D 12 API.
<img width="`1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/241f11a4-709c-4598-a0d2-2364fb758e73" />
<img width="1600" height="900" alt="screenshot" src="https://github.com/user-attachments/assets/d8f3c499-4f13-4549-8c5e-dd43b4b3b963" />
## Features

### Core Rendering Pipeline
- **Deferred Rendering**: Multi-pass architecture with G-Buffer containing albedo, normals, ORM (occlusion/roughness/metallic), emissive, and motion vector channels
- **Reference Path Tracer**: Unbiased Monte Carlo path tracing with next event estimation, Russian roulette termination, and BRDF importance sampling
- **Physically-Based Rendering (PBR)**: Full PBR material support with metallic/roughness workflow, including transmission, thickness, and Fresnel-Schlick approximation
- **Transparency**: Forward rendering for transparent objects with transmission, IOR, spectral attenuation, and volumetric absorption
- **HDR Rendering**: High dynamic range pipeline with histogram-based automatic exposure adaptation (EV100) and physically-based tone mapping
- **Ray-Traced Shadows**: Hardware-accelerated ray tracing for directional light shadows with inline ray queries
- **ReSTIR DI (Direct Illumination)**: Advanced stochastic light sampling with initial sampling modes (uniform, Power-RIS, ReGIR-RIS), temporal and spatial resampling, and boiling filter for variance reduction
- **Bloom**: Multi-stage bloom post-processing with configurable intensity and knee parameters
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

### Performance & Profiling
- **Microprofile Integration**: Detailed CPU and GPU performance profiling with real-time visualization
- **Pipeline State Caching**: Automatic caching of graphics, meshlet, and compute pipeline states
- **GPU Profiling**: Per-renderer GPU timing, integrated timer queries, and pipeline statistics collection
- **Memory Efficiency**: Render graph resource aliasing for minimal VRAM usage with transient resource pooling

### Developer Experience
- **ImGui UI**: Real-time debugging interface with rendering mode selection, pass toggles, and performance metrics
- **Debug Visualization Modes**:
  - Depth, normals, albedo, roughness/metallic, emissive, and motion vector visualization
  - Meshlet visualization with per-meshlet coloring
  - LOD level visualization
  - Isolated bloom visualization
  - RTXDIVisualization for ReSTIR DI debugging
- **Flexible Configuration**: Runtime adjustable parameters for culling, rendering, post-processing, and ReSTIR DI tuning (noise mix, clamping, denoising)
- **Modern C++**: C++20 features with clean, maintainable architecture
- **Shader Hot Reloading**: Runtime shader recompilation and seamless reloading without engine restart

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
- **Command List Management**: Efficient command buffer pooling and reuse
- **Pipeline State Caching**: Automatic caching of graphics and compute pipelines for reduced CPU overhead

### Multi-Pass Pipeline
The rendering pipeline consists of specialized rendering passes:
- **BasePassRenderer (Opaque)**: Meshlet-based GPU-driven geometry rendering with G-Buffer output
- **BasePassRenderer (Masked)**: Alpha-tested transparency with coverage-based rendering
- **TransparentPassRenderer**: Forward rendering for refractive and transparent objects
- **DeferredRenderer**: Deferred lighting computation with ReSTIR DI integration
- **RTXDIRenderer**: ReSTIR DI initial sampling, temporal, and spatial resampling passes
- **SkyRenderer**: Atmosphere and sky rendering with Bruneton atmosphere integration
- **TLASRenderer**: Ray tracing acceleration structure updates for multi-LOD geometry
- **BloomRenderer**: Multi-stage pyramid-based bloom with prefilter and upsample passes
- **HDRRenderer**: Exposure adaptation and physically-based tone mapping
- **PathTracerRenderer**: Reference unbiased path tracing with progressive accumulation
- **ImGuiRenderer**: Debug UI and visualization rendering

### Graphics Abstraction Layer
Built on top of NVRHI (NVIDIA Rendering Hardware Interface) providing:
- Unified API for Vulkan and Direct3D 12 with seamless cross-platform rendering
- Automatic resource state management and synchronization
- Cross-API shader compilation (SPIR-V/DXIL)
- Hardware capability querying and feature negotiation
- GPU memory management and resource pooling

### Scene Management
- **glTF 2.0 Loading**: Complete scene import from standard glTF 2.0 files
- **Binary Scene Caching**: Fast loading with optional binary cache validation and versioning
- **Mesh Optimization**: Vertex quantization and LOD generation with meshoptimizer
- **Animation System**: GPU-friendly animation playback with interpolation modes
- **Lighting**: Automatic light buffer generation with ray tracing support

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
- **[RTXDI](https://github.com/NVIDIA-RTX/RTXDI)**: ReSTIR Direct Illumination framework
- **[NRD](https://github.com/NVIDIAGameWorks/RayTracingDenoiser)**: NVIDIA ray tracing denoiser (ReBlur/RELAX)
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
- **Offline Compilation**: Shaders compiled at build time for faster loading
- **Bindless Resources**: Descriptor indexing for textures and buffers
- **Ray Tracing**: Inline ray queries (RayQuery) for shadows and path tracing
- **Mesh Shaders**: GPU-driven geometry processing with automatic task/mesh shader pipeline generation
- **Compute Shaders**: For culling, histogram generation, SPD mip reduction, and post-processing

### Major Shaders

- **BasePass.hlsl**: Geometry generation with meshlet rendering and G-Buffer output
- **GPUCulling.hlsl**: Two-phase GPU culling with frustum, occlusion, and cone culling
- **HZBFromDepth.hlsl**: Hierarchical Z-buffer generation from depth
- **SPD.hlsl**: AMD Single Pass Downsampler for efficient mip-map generation
- **ReSTIR DI Shaders**: Initial sampling (uniform, Power-RIS, ReGIR-RIS), temporal resampling, and spatial resampling passes
- **DeferredLighting.hlsl**: Deferred lighting and shading with ray-traced shadows and ReSTIR DI integration
- **NRD Denoiser.hlsl**: RELAX denoiser integration for diffuse/specular noise reduction
- **PathTracer.hlsl**: Unbiased Monte Carlo path tracer with next event estimation and BRDF importance sampling
- **Sky.hlsl**: Real-time sky and atmosphere rendering with Bruneton precomputed atmosphere
- **Bloom.hlsl**: Multi-stage pyramid-based bloom with prefilter and upsample passes
- **Tonemap.hlsl**: HDR to SDR tone mapping with bloom integration and exposure adaptation
- **LuminanceHistogram.hlsl**: Per-frame luminance histogram for auto-exposure adaptation
- **TLASPatch.hlsl**: Compute shader for multi-LOD TLAS synchronization and BLAS address updates

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
