# HobbyRenderer

A 3D rendering engine built in C++, featuring modern graphics techniques and supporting both Vulkan and Direct3D 12 APIs.

## Features

### Core Rendering Pipeline
- **Deferred Rendering**: Multi-pass architecture with G-Buffer containing albedo, normals, ORM (occlusion/roughness/metallic), emissive, and motion vector channels
- **Reference Path Tracer**: Unbiased Monte Carlo path tracing with next event estimation, Russian roulette termination, and BRDF importance sampling
- **Physically-Based Rendering (PBR)**: Full PBR material support with metallic/roughness workflow, including transmission, thickness, and Fresnel-Schlick approximation
- **Transparency**: Forward rendering for transparent objects with transmission, IOR, and spectral attenuation
- **HDR Rendering**: High dynamic range pipeline with histogram-based automatic exposure adaptation (EV100) and physically-based tone mapping
- **Ray-Traced Shadows**: Hardware-accelerated ray tracing for directional light shadows with inline ray queries
- **Bloom**: Multi-stage bloom post-processing with configurable intensity and knee parameters
- **Atmosphere Rendering**: Physically-based sky and sun atmosphere lighting across all rendering modes

### Rendering Techniques
- **GPU-Driven Rendering**: Meshlet-based geometry processing using mesh shaders with indirect dispatch
- **Meshlet Rendering**: Efficient GPU-driven rendering with automatic meshlet generation and caching
- **Bindless Textures**: Descriptor indexing for unlimited texture access without binding changes
- **Hierarchical Z-Buffer (HZB)**: Multi-level depth buffer for efficient occlusion culling using AMD Single Pass Downsampler (SPD) with min/max/average reductions
- **Advanced GPU Culling**: 
  - **Phase 1**: Frustum culling combined with occlusion culling using HZB
  - **Phase 2**: Occlusion culling on occluded primitives with meshlet job generation
  - **Cone Culling**: Back-face and silhouette culling for opaque geometry
  - **Hierarchical LOD (Level of Detail)**: Automatic LOD generation with progressive mesh simplification using meshoptimizer
- **Multi-threaded Rendering**: Parallel command list recording and asynchronous task scheduling
- **Image-Based Lighting (IBL)**: Environment lighting with irradiance and radiance cubemaps, including BRDF lookup table

### Performance & Profiling
- **Microprofile Integration**: Detailed CPU and GPU performance profiling with real-time visualization
- **Pipeline State Caching**: Automatic caching of graphics and compute pipeline states
- **GPU Profiling**: Integrated timer queries and GPU metrics collection
- **Memory Efficiency**: Render graph resource aliasing for minimal VRAM usage

### Developer Experience
- **ImGui UI**: Real-time debugging interface with rendering mode selection, pass toggles, and performance metrics
- **Debug Visualization**: Multiple visualization modes including G-Buffer inspection, lighting components, occlusion visualization
- **Flexible Configuration**: Runtime adjustable parameters for culling, rendering, and post-processing
- **Cross-Platform Validation**: Graphics API validation layers (NVIDIA NVRHI) for robust error detection
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
- **BasePassRenderer (Opaque)**: Geometry rendering with meshlet-based GPU-driven rendering
- **BasePassRenderer (Masked)**: Alpha-tested geometry
- **TransparentPassRenderer**: Forward rendering for transparent objects
- **DeferredRenderer**: Deferred lighting computation
- **SkyRenderer**: Atmosphere and sky rendering
- **TLASRenderer**: Ray tracing acceleration structure building
- **BloomRenderer**: Multi-stage bloom post-processing
- **HDRRenderer**: Exposure adaptation and tone mapping
- **PathTracerRenderer**: Reference unbiased path tracing
- **ImGuiRenderer**: Debug UI rendering

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

## Usage

### Command Line Options
- `--scene <path>`: Load a scene file
- `--vulkan`: Select Vulkan graphics API (default: D3D12)
- `--rhidebug`: Enable graphics API validation layers
- `--rhidebug-gpu`: Enable GPU-assisted validation (requires --rhidebug)
- `--skip-textures`: Skip loading textures from scene
- `--skip-cache`: Skip loading/saving scene cache
- `--irradiance <path>`: Path to irradiance cubemap texture (DDS)
- `--radiance <path>`: Path to radiance cubemap texture (DDS)
- `--envmap <path>`: Path to environment map (auto-infers irradiance/radiance DDS files)
- `--brdflut <path>`: Path to BRDF LUT texture (DDS)
- `--execute-per-pass`: Execute command lists per rendering pass
- `--execute-per-pass-and-wait`: Wait for GPU idle after each pass execution
- `--disable-rendergraph-aliasing`: Disable render graph resource aliasing
- `--help, -h`: Show help message

### Example
```bash
AgenticRenderer.exe --scene scenes/sponza.gltf --vulkan --rhidebug
```

### Controls
- **Mouse**: Camera rotation and movement
- **WASD**: Camera movement

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
- **DeferredLighting.hlsl**: Deferred lighting and shading with ray-traced shadows
- **PathTracer.hlsl**: Unbiased Monte Carlo path tracer with next event estimation and BRDF importance sampling
- **Sky.hlsl**: Real-time sky and atmosphere rendering
- **Bloom.hlsl**: Multi-stage bloom filtering
- **Tonemap.hlsl**: HDR to SDR tone mapping with bloom integration
- **LuminanceHistogram.hlsl**: Per-frame luminance histogram for exposure adaptation

## Scene Format Support

This application supports loading scene files in the following formats:

- **glTF (.gltf)**: Standard glTF 2.0 files (text-based only, GLB binary format not supported)
- **Scene JSON (.scene.json)**: Custom JSON scene format

### Sample Scenes

You can download sample scenes from the following NVIDIA repositories:
- **[RTXPT-Assets](https://github.com/NVIDIA-RTX/RTXPT-Assets)**: Path tracing sample assets
- **[RTXDI-Assets](https://github.com/NVIDIA-RTX/RTXDI-Assets)**: RTX Direct Illumination sample assets

### Supported Features
- **Meshes**: Triangle meshes with automatic vertex quantization, mesh optimization, and hierarchical LOD generation
- **Materials**: PBR materials with albedo, normal, ORM (occlusion/roughness/metallic), and emissive textures; includes transmission, IOR, and thickness properties
- **Animations**: Skeletal animations with step, linear, and cubic spline interpolation modes
- **Lights**: Directional lights with ray-traced shadow support and atmosphere-aware intensity
- **Cameras**: Perspective cameras with configurable field-of-view, near/far planes, and EV100 exposure values
- **Texture Formats**: Supports DDS cubemaps for IBL (irradiance and radiance maps) and BRDF LUT
