Since you're already using **D3D12 Reserved Resources + Hardware Sampler Feedback + RTXTS-TTM**, you're actually at the point where DirectStorage becomes genuinely useful. Most tutorials explain DirectStorage as a faster loading API, but for a virtual texture renderer it's better thought of as **a high-throughput asynchronous page streaming system**.

The biggest win is **not raw SSD bandwidth**—it's reducing CPU overhead while issuing tens of thousands of tiny page reads every frame. That's exactly the workload virtual texturing creates. ([GitHub][1])

## Official SDK

Microsoft maintains everything here:

* [DirectStorage GitHub](https://github.com/microsoft/DirectStorage?utm_source=chatgpt.com)
* [DirectStorage landing page (Microsoft)](https://devblogs.microsoft.com/directx/directstorage-api-downloads/?utm_source=chatgpt.com)

The GitHub repo includes:

* HelloDirectStorage
* BulkLoadDemo
* GPU Decompression Benchmark
* EnqueueRequestsDemo
* GDeflate reference implementation

These are the best starting point because they use the real SDK APIs instead of wrapping them. ([GitHub][1])

---

# What DirectStorage actually is

Without DirectStorage, a streaming request typically looks like

```
Game
 │
ReadFile()
 │
Windows File System
 │
CPU copies
 │
RAM
 │
CPU decompresses
 │
Copy to upload heap
 │
Copy to GPU
 │
Texture ready
```

Every page requires

* Win32 IO
* completion callbacks
* CPU scheduling
* decompression
* upload copies

With thousands of 64 KB pages, CPU overhead becomes surprisingly large.

---

With DirectStorage:

```
Game

Submit 5000 requests

      ↓

DirectStorage Queue

      ↓

NVMe SSD

      ↓

(optional GPU decompression)

      ↓

GPU memory

      ↓

Texture tile
```

The important differences are

* batched IO
* very low CPU overhead
* optimized NVMe queue usage
* optional GPU decompression (GDeflate, Zstd in newer previews)
* fence-based synchronization with D3D12

It only performs **reads**—it's not a replacement for writing files. ([GitHub][1])

---

# Why this matches virtual texturing perfectly

Your renderer already does something like

```
Sampler Feedback

↓

Determine missing tiles

↓

Tile request queue

↓

Read page from disk

↓

Upload to GPU

↓

UpdateTileMappings()

↓

Render next frame
```

DirectStorage only replaces the highlighted portion:

```
Sampler Feedback

↓

Determine missing tiles

↓

Tile request queue

↓

DirectStorage Queue
      │
      │
      ├── thousands of reads
      │
      ▼
GPU upload

↓

UpdateTileMappings()

↓

Render
```

Your feedback algorithm stays exactly the same.

RTXTS-TTM continues deciding *which* tiles are needed.

DirectStorage changes *how* they're loaded.

---

# High-level architecture

```
Frame N

Sampler Feedback

↓

Missing Tiles

↓

Page Cache Lookup

↓

Build IO Requests

↓

Enqueue DirectStorage Requests

↓

Signal Fence

↓

GPU copies/decompression

↓

UpdateTileMappings()

↓

Tile becomes resident

↓

Frame N+1
```

Notice there is almost no CPU work after enqueueing.

---

# Typical initialization

```cpp
CreateFactory();

CreateQueue();

OpenTextureFile();

CreateStatusArray();

CreateFence();
```

Usually you'll have

* one DirectStorage factory
* one queue
* many files
* one completion fence

---

# Streaming pseudocode

```cpp
for (TileID tile : MissingTiles)
{
    Request req{};

    req.Source.File = textureFile;
    req.Source.Offset = tile.diskOffset;
    req.Source.Size = tile.compressedSize;

    req.Destination.Texture = reservedTexture;
    req.Destination.Subresource = tile.subresource;

    queue.EnqueueRequest(req);
}

queue.EnqueueSignal(fence, fenceValue);

queue.Submit();
```

Then later

```cpp
WaitForFence(fenceValue);

UpdateTileMappings(...);
```

That's basically the streaming loop.

---

# Integrating with your existing renderer

Current

```
Sampler Feedback

↓

CPU reads tile.bin

↓

Upload heap

↓

CopyTextureRegion

↓

UpdateTileMappings()
```

After DirectStorage

```
Sampler Feedback

↓

DirectStorage request

↓

GPU destination

↓

Fence

↓

UpdateTileMappings()
```

Much cleaner.

---

# Compression

One of the nicest features is GPU decompression.

Instead of

```
SSD

↓

Compressed

↓

CPU Inflate

↓

GPU
```

you get

```
SSD

↓

Compressed

↓

GPU Decompress

↓

Texture
```

This saves CPU time while also reducing storage bandwidth because the assets stay compressed on disk. The SDK includes GDeflate support, and newer preview releases add additional codec support such as Zstandard. ([Microsoft for Developers][2])

---

# Threading model

Instead of

```
Streaming Thread
    ReadFile()

Worker Thread
    memcpy()

Worker Thread
    decompress()

Render Thread
    upload
```

you usually have

```
Render Thread

↓

Build requests

↓

Submit queue

↓

Continue rendering
```

The runtime handles the rest.

---

# Queue model

DirectStorage resembles D3D12 command queues.

```
Queue

enqueue(req1)

enqueue(req2)

enqueue(req3)

enqueue(signal)

submit()
```

The idea is to batch hundreds or thousands of requests together instead of performing individual `ReadFile()` calls. ([GitHub][1])

---

# Best practices for virtual texturing

For a feedback-based texture streamer:

* Keep one large packed texture file rather than many tiny files.
* Store tile offsets in a page table so requests are simple offset+size lookups.
* Batch every page request for the current frame into a single DirectStorage submission.
* Issue reads as early as possible after processing sampler feedback.
* Delay `UpdateTileMappings()` until the completion fence signals.
* Keep multiple frames of requests in flight to hide storage latency.
* If you already compress texture pages offline, evaluate GPU decompression to reduce CPU work and disk bandwidth.

---

# Open-source samples

## Microsoft DirectStorage

The official repository should be your first stop:

* [DirectStorage GitHub](https://github.com/microsoft/DirectStorage?utm_source=chatgpt.com)

Interesting samples include:

* **HelloDirectStorage** — minimal example
* **BulkLoadDemo** — loading many assets
* **GpuDecompressionBenchmark** — compare CPU vs GPU decompression
* **EnqueueRequestsDemo** — modern batched request API
* **GDeflate** reference implementation ([GitHub][1])

---

## NVIDIA RTXTS-TTM

You're already using:

* [RTXTS-TTM](https://github.com/NVIDIA-RTX/RTXTS-TTM?utm_source=chatgpt.com)

DirectStorage fits almost exactly between RTXTS's tile request generation and its residency updates.

---

# How I'd integrate it

Given your renderer already has:

```
Hardware Sampler Feedback
        ↓
Page request generation
        ↓
Reserved Resources
        ↓
UpdateTileMappings
```

I would change only the streaming backend:

```
Hardware Feedback
        ↓
Tile Requests
        ↓
Page Cache
        ↓
DirectStorage Batch Queue
        ↓
GPU Decompression (optional)
        ↓
Fence
        ↓
UpdateTileMappings()
        ↓
Resident
```

Everything above the "Tile Requests" stage remains unchanged.

In other words, DirectStorage is **not** a replacement for hardware sampler feedback or tiled resources—it complements them. Sampler feedback decides **what** to stream, tiled resources decide **where** it lives in virtual memory, and DirectStorage determines **how** those pages get from storage to the GPU as efficiently as possible. For a renderer like yours, that's exactly the combination Microsoft designed it to support.

[1]: https://github.com/microsoft/DirectStorage "GitHub - microsoft/DirectStorage: DirectStorage for Windows is an API that allows game developers to unlock the full potential of high speed NVMe drives for loading game assets. · GitHub"
[2]: https://devblogs.microsoft.com/directx/directstorage-api-downloads "DirectStorage SDK & API - DirectX Developer Blog"
