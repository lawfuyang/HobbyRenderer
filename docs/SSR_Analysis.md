SSR (Screen Space Reflections) is one of those techniques that fits naturally into a deferred renderer because you already have almost everything it needs:

* depth buffer
* normals
* material properties (roughness/metalness)
* scene color (lit image)

The core idea is surprisingly simple:

> **Pretend every reflective pixel shoots a reflection ray, but instead of intersecting actual scene geometry, intersect the depth buffer.**

This makes SSR much cheaper than ray tracing, but it also explains all of its artifacts.

---

# High-level idea

Imagine you've already rendered this:

```
Geometry Pass
--------------------
GBuffer
    Normal
    Albedo
    Roughness
    Metallic
    Depth

↓

Deferred Lighting

↓

HDR Scene Color
```

Now add SSR after lighting:

```
Geometry Pass
↓

Deferred Lighting

↓

SSR
    Uses:
        HDR Scene Color
        Depth
        Normals
        Roughness

↓

Composite

↓

Bloom
↓

Tonemap
```

The SSR pass computes only the reflected color.

Later you blend

```
FinalColor =
    SceneLighting +
    SSRReflection
```

(or mix with probes/environment maps.)

---

# What information do you need?

Per pixel:

```
World Position
World Normal
View Direction
Surface Roughness
HDR Scene Color
Depth Buffer
```

Most of this already exists in your renderer.

Usually you reconstruct world position from depth.

---

# Step 1 — Reconstruct position

For current pixel

```
depth = DepthTexture.Load()

viewPos = ReconstructViewPosition(depth, uv)

worldPos = mul(viewPos, InvView)
```

---

# Step 2 — Compute reflection ray

Reflection is just

```
R = reflect(-ViewDirection, Normal)
```

Example

```
camera

     \
      \
       \
        P
       /
      /
     reflected ray
```

---

# Step 3 — March the ray

This is the heart of SSR.

Instead of tracing against triangles...

You repeatedly advance

```
P += R * stepSize
```

At every step

Project back onto the screen.

```
screen = Project(P)
```

Now sample the depth buffer there.

---

Suppose

Ray says

```
Ray depth = 12.3m
```

Depth buffer says

```
Geometry depth = 12.0m
```

If they're close enough

```
abs(rayDepth - depthBuffer) < thickness
```

you probably hit something.

---

# Visualization

```
Camera

    \
     \
      \
       *
      /
     /
    /
Reflect ray

step
↓

project

↓

sample depth

↓

keep walking

↓

hit
```

---

# Why projection?

Because the only geometry available is

```
Depth Texture
```

not the mesh.

Every ray step becomes

```
3D point

↓

project

↓

screen pixel

↓

depth lookup
```

---

# Pseudocode

```cpp
float3 position = ReconstructPosition(uv);

float3 normal = LoadNormal(uv);

float3 V = normalize(cameraPos - position);

float3 R = reflect(-V, normal);

float3 ray = position;

for(int i = 0; i < MAX_STEPS; i++)
{
    ray += R * STEP_SIZE;

    float2 rayUV = Project(ray);

    if(rayUV outside screen)
        break;

    float sceneDepth = Depth(rayUV);

    float rayDepth = ProjectedDepth(ray);

    if(abs(rayDepth - sceneDepth) < THICKNESS)
    {
        reflection = SceneColor(rayUV);
        break;
    }
}
```

That is literally the basic SSR algorithm.

---

# Binary search refinement

The marching step is coarse.

You often overshoot.

Instead of

```
hit
```

you get

```
step

step

step

inside wall
```

After detecting a hit,

run a binary search between

```
previousPoint

currentPoint
```

for ~5 iterations.

This produces much sharper reflections.

---

# Thickness

Imagine the ray barely misses an object because the depth buffer is only one surface.

```
|
| wall
|

ray
 \
  \
```

Thickness pretends objects have some volume.

```
if(depthDifference < thickness)
```

Typical values

```
0.05

0.1

0.2

0.5
```

depending on scene scale.

---

# Roughness

Don't compute SSR for everything.

```
roughness > 0.8

→ skip
```

Only reflective materials.

```
metal

wet floor

polished wood

glass
```

---

# Fading

SSR has unavoidable failures.

Fade reflections by

* distance
* screen edge
* grazing angle
* roughness

Example

```
reflection *= edgeFade;

reflection *= distanceFade;

reflection *= Fresnel;

reflection *= (1 - roughness);
```

---

# Why artifacts happen

SSR only knows what's visible on screen.

Imagine

```
Camera

[ Wall ]

      Object
```

The object is hidden.

Depth buffer never saw it.

Reflection ray cannot hit it.

Result

Missing reflections.

Likewise

```
Mirror

reflects something

that is off-screen
```

Impossible.

No data.

This is why SSR is usually combined with

* reflection probes
* skybox
* hardware ray tracing

SSR fills in the visible details.

The fallback fills in everything else. ([Flax Engine Documentation][1])

---

# Common improvements

## Hierarchical Z (Hi-Z)

Instead of stepping

```
128
```

times,

build a mip pyramid of depth.

```
Depth

↓

Mip 1

↓

Mip 2

↓

Mip 3
```

Large empty space gets skipped quickly.

Modern engines almost always use Hi-Z SSR.

---

## Half-resolution SSR

Compute reflections at

```
960×540
```

instead of

```
1920×1080
```

Upsample afterward.

Huge performance gain.

---

## Temporal accumulation

Reuse previous frame's reflections.

```
Current

+

History

↓

Blend
```

Reduces noise dramatically, especially with jittered rays. ([npm][2])

---

## Stochastic SSR

Instead of one deterministic ray

sample slightly different directions

based on roughness.

Average over time (TAA).

Produces glossy reflections.

---

# Typical D3D12 render graph

```
Geometry Pass

↓

GBuffer

↓

Deferred Lighting

↓

HDR Scene Color

↓

SSR Pass
    Input:
        HDR Color
        Depth
        Normals
        Roughness

↓

Reflection Texture

↓

Blur (optional)

↓

Temporal Resolve

↓

Composite

↓

Bloom

↓

Tonemap
```

---

# Compute shader or pixel shader?

Both work.

Many modern engines implement SSR as a **compute shader** because it offers:

* flexible thread groups
* better cache behavior
* easier temporal integration
* straightforward half-resolution rendering

A full-screen pixel shader is perfectly fine for a first implementation.

---

# Good open-source implementations

These are worth studying because they implement the "real" version rather than the minimal algorithm:

* [Google Filament](https://github.com/google/filament) — Modern PBR renderer with SSR, Hi-Z, temporal techniques, and excellent rendering architecture.
* [The Forge](https://github.com/ConfettiFX/The-Forge) — Cross-platform rendering framework with advanced rendering samples, including screen-space techniques.
* [AMD FidelityFX SSSR (Stochastic Screen Space Reflections)](https://github.com/GPUOpen-Effects/FidelityFX-SSSR) — Probably the best production-quality reference. Implements hierarchical depth traversal, stochastic rays, temporal filtering, and denoising.
* [NVIDIA Falcor](https://github.com/NVIDIAGameWorks/Falcor) — Research renderer containing multiple reflection implementations (SSR and ray tracing) that are easy to compare.
* [BGFX Examples](https://github.com/bkaradzic/bgfx) — Includes several screen-space rendering examples and is a good reference for renderer organization.
* [WebGPU Sponza Demo](https://github.com/gnikoloff/webgpu-sponza-demo) — A clean modern renderer featuring both linear and Hi-Z SSR implementations, TAA, and deferred rendering. Even though it's WebGPU, the algorithms translate directly to D3D12. ([Reddit][3])

---

# Recommended implementation roadmap

If you're adding SSR to an existing deferred D3D12 renderer, I'd build it incrementally:

1. **Basic linear ray marching**

   * Reconstruct view-space position
   * Compute reflection vector
   * March against the depth buffer
   * Sample HDR scene color on hit

2. **Binary search refinement**

   * Sharper, more stable intersections

3. **Thickness and fade heuristics**

   * Reduce holes and obvious artifacts

4. **Roughness-aware reflections**

   * Skip or blur reflections on rough surfaces

5. **Half-resolution SSR**

   * Significant performance improvement

6. **Hierarchical Z traversal**

   * Faster ray marching with longer rays

7. **Temporal reprojection (using your TAA history)**

   * Much less noise and shimmer

8. **Fallback blending**

   * Mix SSR with reflection probes or an environment map when rays miss

This progression mirrors how many production engines evolved their SSR implementations: start with a straightforward screen-space ray marcher, then improve quality and performance with Hi-Z acceleration, temporal accumulation, and fallback reflections.

[1]: https://docs.flaxengine.com/manual/graphics/post-effects/screen-space-reflections.html "Screen Space Reflections | Flax Documentation"
[2]: https://www.npmjs.com/package/screen-space-reflections "screen-space-reflections - npm"
[3]: https://www.reddit.com/r/GraphicsProgramming/comments/1hhvscp "WebGPU Sponza Demo"
