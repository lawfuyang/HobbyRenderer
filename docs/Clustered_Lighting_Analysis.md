Since you already have a deferred renderer (G-buffer → lighting pass), clustered lighting is actually a fairly incremental upgrade. You're replacing:

> "For every pixel, iterate over every light"

with

> "For every pixel, iterate only over the handful of lights that can actually affect that region of space."

The key difference from **tiled deferred** is that clustered lighting partitions **3D view space**, not just the 2D screen. That solves one of tiled rendering's biggest weaknesses.

---

# Why tiled deferred isn't enough

Suppose you divide the screen into 16×16 tiles.

```
+----+----+----+
|    |    |    |
+----+----+----+
|    | X  |    |
+----+----+----+
```

A huge light whose sphere overlaps this tile gets assigned.

But what if:

* the light is 100 meters behind the visible geometry?
* or 2 meters in front?
* or only affects distant geometry?

The entire tile still has to consider that light.

The tile has no notion of depth.

---

Clustered lighting adds the missing dimension.

Instead of tiles:

```
(x,y)
```

you build

```
(x,y,z)
```

where z is view-space depth.

Now every region of the camera frustum becomes a little box:

```
+---------+
| cluster |
+---------+
```

A pixel belongs to exactly one cluster.

Each cluster stores

```
list of affecting lights
```

instead of the whole scene.

---

# The rendering pipeline

For a deferred renderer it usually becomes:

```
Frame:

CPU
----
Update light buffer

GPU

Geometry Pass
-------------
Fill GBuffer

Compute Pass #1
---------------
Build cluster bounds (usually once or on resize)

Compute Pass #2
---------------
Cull lights into clusters

Deferred Lighting Pass
----------------------
For each pixel:
    reconstruct position
    find cluster
    shade only cluster lights

Post processing
```

Notice the GBuffer barely changes.

---

# Step 1: Build the clusters

Choose a grid.

Typical numbers:

```
16 x 9 x 24

or

32 x 18 x 24

or

16 x 8 x 32
```

The X/Y dimensions correspond to screen tiles.

For a 1920×1080 screen:

```
16x9

means

120 x 120 pixel tiles
```

More commonly:

```
16 pixel tiles

1920/16 = 120

1080/16 = 68

120 x 68 x 24 clusters
```

≈ 195,000 clusters.

That sounds like a lot, but each cluster is tiny.

---

# Z slicing

This is the important part.

You **don't** use linear depth.

Instead:

```
near

|

|

|

far
```

gets divided logarithmically.

Near the camera:

```
| | | | | |      |       |         |
```

instead of

```
|----|----|----|----|----|
```

Why?

Most lighting complexity is near the camera.

Log slices provide much better distribution. ([mmzala.github.io][1])

---

# Each cluster stores

Usually:

```cpp
struct Cluster
{
    uint offset;
    uint count;
};
```

and a giant array

```cpp
uint lightIndices[MAX_TOTAL_LIGHTS];
```

Example

Cluster 5

```
offset = 128

count = 6
```

means

```
lightIndices

128 -> light 3
129 -> light 14
130 -> light 22
131 -> light 90
132 -> light 91
133 -> light 104
```

Exactly like an index buffer.

---

# Step 2: Cull lights

This is a compute shader.

Each thread processes one cluster.

Pseudo:

```cpp
for every cluster
{
    count = 0;

    for every light
    {
        if(light intersects cluster)
        {
            append(lightIndex);
        }
    }
}
```

The intersection is usually

```
Sphere vs AABB
```

because point lights are spheres and clusters are boxes.

Very cheap.

---

# Cluster AABB

Each cluster knows

```
min corner

max corner
```

in **view space**.

The compute shader computes:

```
Cluster 15

min = (-2,-1,-8)

max = (1,2,-12)
```

Then

```
Sphere-AABB overlap
```

for every light.

---

# Step 3: Lighting pass

Your current deferred shader probably looks like

```cpp
for(int i=0;i<numLights;i++)
{
    Shade(light[i]);
}
```

Clustered lighting becomes

```cpp
cluster = GetCluster(pixel);

ClusterData c = clusters[cluster];

for(i=0;i<c.count;i++)
{
    Light light = lights[ lightIndices[c.offset+i] ];

    Shade(light);
}
```

Instead of

```
500 lights
```

you usually evaluate

```
8–30 lights
```

per pixel.

That's where the speedup comes from.

---

# Finding the cluster

You already reconstruct position from depth.

```
positionVS
```

From that:

```cpp
x = pixel.x / TILE_SIZE;

y = pixel.y / TILE_SIZE;

z = ComputeLogSlice(positionVS.z);

cluster = x
        + y*numXTiles
        + z*numXTiles*numYTiles;
```

Done.

---

# Typical buffers

```cpp
StructuredBuffer<Light>

StructuredBuffer<Cluster>

StructuredBuffer<uint> LightIndices
```

These are read-only during lighting.

During culling they become UAVs.

---

# Compute shader pseudocode

```cpp
[numthreads(64,1,1)]
void CullLightsCS(...)
{
    uint clusterID = DispatchThreadID.x;

    Cluster cluster = clusters[clusterID];

    uint offset = atomicAdd(globalCounter, 0);

    uint count = 0;

    for(light in lights)
    {
        if(Intersects(cluster, light))
        {
            lightIndices[offset + count] = light.id;
            count++;
        }
    }

    clusters[clusterID].offset = offset;
    clusters[clusterID].count  = count;
}
```

Many engines improve on this by using group shared memory, hierarchical culling, or prefix sums to reduce atomics, but the high-level idea stays the same. ([mmzala.github.io][1])

---

# Data flow

```
CPU
 |
 | upload lights
 |
 V

StructuredBuffer<Light>

        |
        V

Compute
--------
cluster generation

        |
        V

Cluster AABBs

        |
        V

Compute
--------
light culling

        |
        V

Cluster table

offset/count

        +
light index list

        |
        V

Deferred Lighting

pixel

↓

cluster

↓

cluster light list

↓

shade
```

---

# Advantages over tiled deferred

| Tiled                                      | Clustered                          |
| ------------------------------------------ | ---------------------------------- |
| 2D tiles                                   | 3D clusters                        |
| Many false-positive lights                 | Much tighter light lists           |
| Performs poorly with depth discontinuities | Handles depth variation naturally  |
| Simple                                     | Slightly more complex              |
| Good                                       | Usually better with lots of lights |

Clustered lighting tends to scale much better in scenes with large depth ranges or many small dynamic lights because clusters better match the actual spatial distribution of geometry. ([humus.name][2])

---

# Integration into your renderer

Given your existing pipeline:

```
Geometry
↓

GBuffer

↓

Deferred Lighting
↓

Post
```

you would minimally change it to:

```
Geometry
↓

GBuffer

↓

Cluster Build (once or resize)

↓

Light Cull (compute every frame)

↓

Deferred Lighting
↓

Post
```

Your G-buffer formats, BRDF, material system, shadows, and post-processing can all remain largely unchanged. The primary change is replacing the "loop over all lights" in your lighting shader with "loop over this cluster's light list."

---

# Excellent open-source references

These are some of the best implementations and learning resources:

* [pezcode/Cluster (bgfx, supports DX11/DX12/Vulkan, forward + deferred + clustered)](https://git.hubp.de/pezcode/Cluster) — One of the clearest complete implementations, with compute-based cluster generation and light culling. ([GitHub][3])
* [Humus Clustered Shading demo](https://humus.name/index.php?ID=90&page=3D&utm_source=chatgpt.com) — Emil Persson's classic Direct3D 11 sample that demonstrates the technique and influenced many production engines. ([humus.name][2])
* [Original clustered forward demo (Olsson et al.)](https://gitlab.com/efficient_shading/clustered_forward_demo) — The reference implementation accompanying the SIGGRAPH 2012 work on tiled and clustered shading. ([GitLab][4])
* [Graphics Study: Clustered Shading walkthrough](https://mmzala.github.io/blog/clustered-shading.html) — A thorough implementation guide that walks through building clusters, culling, and shading with compute shaders. ([mmzala.github.io][1])
* [DaveH355 clustered shading tutorial and sample](https://github.com/DaveH355/clustered-shading) — A compact educational implementation frequently recommended by graphics programmers. ([Reddit][5])
* [Microsoft DirectX Graphics Samples](https://github.com/microsoft/DirectX-Graphics-Samples) — While it doesn't contain clustered lighting directly, it's an excellent reference for modern D3D12 resource management, compute pipelines, barriers, and descriptor handling you'll need for integrating clustered shading. ([GitHub][6])

If you're specifically targeting **modern D3D12**, I'd recommend implementing it in this order:

1. GPU-generated cluster AABBs (once at startup or resize).
2. A compute shader that performs sphere-vs-AABB light culling into a global light index buffer.
3. Modify your deferred lighting shader to fetch the cluster and iterate only its light list.
4. Add a debug visualization that colors clusters by light count—it makes validating the implementation much easier before optimizing.

[1]: https://mmzala.github.io/blog/clustered-shading.html "Clustered Shading - Graphics Study | A Programmer’s Blog"
[2]: https://humus.name/index.php?ID=90&page=3D&utm_source=chatgpt.com "Humus - 3D"
[3]: https://git.hubp.de/pezcode/Cluster "GitHub - pezcode/Cluster: Clustered shading implementation with bgfx · GitHub"
[4]: https://gitlab.com/efficient_shading/clustered_forward_demo "efficient_shading / clustered_forward_demo · GitLab"
[5]: https://www.reddit.com/r/opengl/comments/1bvptj6/i_wrote_a_tutorial_on_clustered_shading_a/ "I wrote a tutorial on clustered shading: a technique for rendering thousands of dynamic lights!"
[6]: https://github.com/microsoft/DirectX-Graphics-Samples "GitHub - microsoft/DirectX-Graphics-Samples: This repo contains the DirectX Graphics samples that demonstrate how to build graphics intensive applications on Windows. · GitHub"
