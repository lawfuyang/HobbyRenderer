If you're starting from a typical D3D12 deferred renderer (G-buffer → lighting → post-processing), OIT usually ends up being an **additional transparent rendering pipeline** that sits on top of your opaque renderer. You generally do **not** integrate transparency into the G-buffer itself.

The typical frame becomes:

```text
Depth Prepass (optional)

Opaque Geometry
    ↓
GBuffer Pass
    ↓
Deferred Lighting
    ↓
Lit Opaque HDR Image
    ↓
-----------------------------
Transparent OIT Pass
-----------------------------
    ↓
Composite Transparent Result
    ↓
Post Processing
Tone Mapping
UI
```

The key idea is:

> **Opaque rendering determines the visible solid surfaces. Transparent rendering computes all translucent fragments without relying on draw order, then composites them over the lit opaque image.**

---

# Why transparency is difficult

Normal alpha blending is

```
dst = src * alpha + dst * (1-alpha)
```

which is **not commutative**.

Meaning

```
A then B != B then A
```

So the GPU normally requires:

```
back-to-front sorting
```

Unfortunately this breaks for

* intersecting meshes
* particle systems
* foliage
* hair
* glass inside glass
* large CAD scenes

because there is no global ordering that works.

Order Independent Transparency (OIT) removes that requirement.

---

# Major families of OIT

There are roughly four approaches.

## 1. Weighted Blended OIT ⭐

The easiest.

Instead of storing every fragment, every transparent fragment contributes to two accumulation buffers.

Advantages

* simple
* fast
* works well for games
* one geometry pass
* no sorting

Disadvantages

* approximate
* thick glass looks wrong
* overlapping high-alpha surfaces are inaccurate

This is what I'd recommend implementing first.

---

# What most modern engines do

Approximate:

```
Deferred renderer

↓

Weighted Blended OIT
```

---

# How it fits into a deferred renderer

Your renderer currently probably looks like

```
Geometry

↓

GBuffer

↓

Lighting

↓

HDR color buffer
```

Transparent objects cannot really participate in the GBuffer because

* one pixel may contain many transparent layers
* GBuffer only stores one surface

Instead

```
Opaque

↓

Deferred Lighting

↓

HDR color
```

Now render transparent meshes separately.

The transparent shaders compute lighting directly (forward shading).

So your renderer becomes

```
Opaque
↓

Deferred Lighting

↓

HDR buffer

↓

Transparent Forward OIT

↓

Composite

↓

Post
```

Notice transparent objects are almost always **forward rendered**, even in deferred engines.

---

# Weighted Blended OIT

This is surprisingly elegant.

Instead of

```
destination color
```

we create two render targets.

```
AccumColor

Revealage
```

Every transparent fragment writes

```
Accum += color * alpha * weight

Reveal *= (1-alpha)
```

using blending.

At the end

```
FinalTransparentColor =
Accum / max(weightSum, epsilon)
```

Then

```
Final =
Opaque +
Transparent * Reveal
```

No sorting.

---

High level:

```
Clear accumulation

for transparent object

    draw

Composite
```

---

Pseudo:

```cpp
RenderOpaque();

DeferredLighting();

ClearAccumBuffers();

RenderTransparentWeighted();

CompositeTransparency();
```

Transparent shader

```cpp
float alpha = material.alpha;

float weight = ComputeWeight(depth, alpha);

AccumColor += color * alpha * weight;

AccumWeight += alpha * weight;

Revealage *= (1 - alpha);
```

Composite

```cpp
transparent =
AccumColor / max(AccumWeight, 1e-5);

final =
transparent +
opaque * Revealage;
```

The weighting function is important for quality and is usually depth-dependent. ([NVIDIA Docs][1])

---

# Frame graph example

```
Depth

↓

GBuffer

↓

Deferred Lighting

↓

HDR
         \
          \
           Transparent Forward
             |
             |
      Accum / Reveal
             |
      Composite
             |
      HDR
             |
     Bloom
             |
 Tone Map
```

---

# What shaders change?

Opaque

```
No change
```

Transparent

Instead of deferred

```
Forward lighting

↓

OIT output
```

Usually you'll sample:

* shadow maps
* environment map
* clustered lights
* reflection probes

just like a normal forward renderer.

---

# Excellent open-source implementations

These are some of the best references available:

### NVIDIA Vulkan OIT sample

Implements multiple OIT techniques (including Weighted Blended) with clear shaders. Although it's Vulkan, the algorithms map directly to D3D12 because they're based on common GPU features (UAVs/storage buffers, atomics, fullscreen composition). ([Reddit][3])

### Diligent Engine Tutorial 29

Excellent because it implements and compares:

* regular alpha blending
* Weighted Blended OIT
* Layered OIT

It's modern C++ and backend-agnostic (supports D3D11/D3D12/Vulkan/OpenGL), making it especially useful if you're working in D3D12. [Diligent Engine Tutorial 29 – OIT](https://diligentgraphics.github.io/docs/d0/d40/DiligentSamples_Tutorials_Tutorial29_OIT_readme.html)

### NVIDIA Weighted Blended OIT sample

Although it's an OpenGL sample, it follows the original McGuire & Bavoil algorithm closely and explains the accumulation/revealage buffers and weighting strategy. [NVIDIA Weighted Blended OIT Sample](https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/opengl_samples/weightedblendedoitsample.htm)

---

Given that you already have a deferred renderer with a G-buffer and deferred lighting, I'd recommend this progression:

1. Keep your opaque pipeline exactly as it is.
2. Add a forward transparent pass that samples the same lighting data used by your deferred renderer.
3. Implement **Weighted Blended OIT** to establish the integration points (render targets, blending, composition).

That path minimizes disruption to your existing renderer while giving you a working OIT implementation early, and the architectural changes (separate forward transparent pass and final composition) carry over cleanly to more advanced OIT techniques.

[1]: https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/opengl_samples/weightedblendedoitsample.htm "Weighted Blended OIT Sample"
[2]: https://github.khronos.org/Vulkan-Site/samples/latest/samples/api/oit_linked_lists/README.html "Order-independent transparency with per-pixel ordered linked lists :: Vulkan Documentation Project"
[3]: https://www.reddit.com/r/vulkan/comments/1hbjvu2 "Order-independent transparency in Vulkan - anyone done it?"
