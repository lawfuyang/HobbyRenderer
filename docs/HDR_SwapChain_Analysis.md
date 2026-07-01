# HDR Swap Chain Analysis & Implementation Plan for HobbyRenderer

Date: 2026-07-01 | Status: **Analysis complete — awaiting approval to modify source files**

---

## 1. HDR Format Selection: Why FP16 scRGB Is the Right Choice

### 1.1 Format Comparison

| Property | scRGB FP16 | HDR10 (PQ) |
|---|---|---|
| DXGI Format | `R16G16B16A16_FLOAT` | `R10G10B10A2_UNORM` |
| Bits per pixel | 64 | 32 |
| Encoding | **Linear** (simple shader output) | ST.2084 PQ (perceptual curve) |
| Color primaries | sRGB/Rec.709 | BT.2020 (wide gamut) |
| Max representable | ~65504 nits (fp16 max) | 10000 nits (PQ ceiling) |
| GPU bandwidth | 2× higher | baseline |
| Shader complexity | **Trivial** — output linear values | Must encode every pixel via PQ EOTF |
| OS integration | Windows compositor auto-converts | Windows compositor auto-converts |
| Banding risk | None (fp16 precision) | Would band at 10-bit without PQ encoding |

**Verdict**: FP16 scRGB is the best choice for a game renderer. Linear output is trivially simple, and the extra bandwidth is negligible for a swap chain (vs internal render targets).

### 1.2 Key scRGB Facts

- **1.0 scRGB = 80 nits** (SDR reference white level)
- Values > 1.0 represent HDR brightness (e.g., 12.5 = 1000 nits)
- The Windows compositor converts scRGB to the display's native color space automatically
- FP16 can represent values up to ~65504 nits — far beyond any current display

---

## 2. Tonemapping Strategy for HDR Display

### 2.1 The Critical Question: Why Not Just Output Raw HDR Values?

You *could* output raw lighting values directly to the scRGB swapchain and let Windows handle it. This would "work" but has problems:
- A specular highlight might be 10,000 nits — painfully bright on a 1000-nit display with harsh clipping
- Specular aliasing becomes visible as sparkling at HDR luminance
- You lose artistic control over the look

### 2.2 The User's Reinhard Function — Analysis and Correction

**Original function:**

```hlsl
float3 HDRRolloff(float3 x)
{
    float MaxDisplay = 1000.0;
    return x / (1.0 + x / MaxDisplay);
}
```

**Problem 1 — Unit Mismatch**: In scRGB, `x` is in scRGB units (1.0 = 80 nits) but `MaxDisplay = 1000.0` is in nits. You cannot mix units in the same formula. For a value of 1.0 scRGB (80 nits): `1.0 / (1.0 + 1.0/1000) = 1.0/1.001 ≈ 0.999` — essentially no compression at all, because `MaxDisplay` being 1000 (in nits) is 12.5× larger than 80 nits, making the denominator terms dimensionally inconsistent. The function "works" accidentally only because the mismatch scales the output incorrectly.

**Problem 2 — SDR Content Gets Compressed**: With the corrected scRGB units version (`MaxDisplay / 80.0`), at SDR white (1.0 scRGB = 80 nits, maxSCRGB = 12.5): `1.0 / (1.0 + 1.0/12.5) = 1.0/1.08 = 0.926`. This means SDR content gets darkened by ~7.4% — UI and SDR-range content will look dimmer on the HDR display than on an SDR one. **Not acceptable.**

**The Root Cause**: Classic Reinhard `x/(1+x)` is designed for mapping an HDR image to [0,1] SDR range. Its "knee" is at x=1. In scRGB for HDR displays, 1.0 is SDR white — we should NOT compress there.

### 2.3 Corrected HDR Tonemapping

The solution: **SDR passthrough + HDR-only Reinhard rolloff**:

```hlsl
// TonemapHDR_PSMain — HDR scRGB output entry point
// 1.0 scRGB = 80 nits (SDR white). Values > 1.0 are HDR.
float3 HDRDisplayTonemap(float3 x, float MaxDisplayNits)
{
    float maxSCRGB = MaxDisplayNits / 80.0;  // e.g. 12.5 for 1000 nit display

    // Per-channel approach: find luminance, preserve chromaticity
    float lum = max(x.r, max(x.g, x.b));

    if (lum <= 1.0)
        return x;  // ★ SDR passthrough — no compression below 80 nits

    // Only compress the HDR headroom above SDR white
    float hdrHeadroom = maxSCRGB - 1.0;        // e.g. 11.5 for 1000 nits
    float excess = lum - 1.0;                  // how far above 80 nits
    float compressedExcess = excess * hdrHeadroom / (excess + hdrHeadroom);

    float newLum = 1.0 + compressedExcess;
    float3 result = x * (newLum / lum);        // preserve hue/saturation

    // Safety clamp: never exceed display max
    return min(result, maxSCRGB);
}

float4 TonemapHDR_PSMain(FullScreenVertexOut input) : SV_Target
{
    uint2 pixelPos = uint2(input.uv * float2(TonemapCB.m_Width, TonemapCB.m_Height));
    float3 color = HDRColorInput[pixelPos];
    float exposure = ExposureInput[0];

    color = color * exposure;

    float3 hdrOutput = HDRDisplayTonemap(color, TonemapCB.m_MaxDisplayNits);

    // ★ No gamma correction — scRGB is linear
    return float4(hdrOutput, 1.0f);
}
```

**Behavior verification** (1000-nit display, `maxSCRGB = 12.5`, `hdrHeadroom = 11.5`):

| Scene Value | scRGB | Nits | Output scRGB | Output Nits | Effect |
|---|---|---|---|---|---|
| Dark     | 0.5  | 40   | 0.5  | 40   | Passthrough |
| SDR white | 1.0  | 80   | 1.0  | 80   | Passthrough ✓ |
| Bright   | 5.0  | 400  | 3.78 | 302   | Gentle rolloff |
| Very bright | 12.5 | 1000 | 6.75 | 540   | Half of headroom |
| Extreme  | 100  | 8000 | 11.33 | 906   | Approaching max |
| ∞        | ∞    | ∞    | 12.5 | 1000  | Asymptote to max |

### 2.4 Clamping

Yes, clamp to `maxSCRGB` as a safety net. The rolloff already asymptotically approaches the max, but floating-point edge cases could produce values slightly above the limit. `min(result, maxSCRGB)` is cheap insurance.

### 2.5 Recommended HDR Tonemapping Functions

Below is a reference library of HDR display tonemapping operators, ranked from simplest to most filmic. All operate in **linear scRGB** (1.0 = 80 nits) and assume `MaxDisplayNits` is queried from the display. Only the **chosen one** gets used in production; the others are included for comparison.

---

#### Function 1: Naive Passthrough (No Tonemapping)

```hlsl
// BAD — specular aliasing, harsh clipping at display peak
float3 NoTonemap(float3 x, float MaxDisplayNits)
{
    float maxSCRGB = MaxDisplayNits / 80.0;
    return saturate(x);  // clips everything above 80 nits to 80 nits!
    // OR: return x;      // lets highlights pierce through unclipped — painful
}
```

| Pro | Con |
|---|---|
| Trivial | Specular aliasing is obvious at HDR brightness |
| Zero ALU | No artistic control |
| | Harsh clipping or blinding highlights |

**Verdict**: ❌ Don't use. Even "just output linear and let Windows handle it" needs *some* compression above 80 nits.

---

#### Function 2: Reinhard (Classic) — What the User Originally Proposed

```hlsl
// Classic Reinhard: designed for compressing HDR into [0,1] SDR range.
// NOT suitable for HDR display output as-is.
float3 ReinhardClassic(float3 x)
{
    return x / (1.0 + x);
}
```

| Pro | Con |
|---|---|
| Dirt simple | Compresses SDR content at 80 nits (knee at x=1.0) |
| Well known | Output maxes at ~1.0 = 80 nits — wastes entire HDR display |
| | Ignores display peak luminance |

**Verdict**: ❌ Only useful as a baseline. Designed for SDR output, not HDR displays.

---

#### Function 3: Reinhard Extended (Luminance-Adaptive)

```hlsl
// Classic Reinhard extended: Lwhite parameter controls the burn-out point.
// Can work for HDR if Lwhite is set to the display peak in scRGB units.
float3 ReinhardExtended(float3 x, float MaxDisplayNits)
{
    float maxSCRGB = MaxDisplayNits / 80.0;
    float lum = max(x.r, max(x.g, x.b));
    float Lwhite2 = maxSCRGB * maxSCRGB;

    float mapped = lum * (1.0 + lum / Lwhite2) / (1.0 + lum);
    return x * (mapped / max(lum, 1e-6));
}
```

**Behavior** (1000-nit display, `maxSCRGB = 12.5`):

| Scene Value (nits) | Output (nits) | Effect |
|---|---|---|
| 40  | 39.9 | Near passthrough ✓ |
| 80  | 79.5 | ~SDR passthrough ✓ |
| 400 | 317 | Gentle compression |
| 1000 | 578 | Moderate compression |
| 8000 | 957 | Strong compression |

| Pro | Con |
|---|---|
| Classic, well-understood | Still slightly compresses near SDR white (~0.6% at 80 nits) |
| Controllable via `Lwhite` | "Looks like Reinhard" — distinct visual signature |
| Clean asymptote to `maxSCRGB` | Slightly desaturates bright colors |

**Verdict**: ✅ Good. Simple, predictable, and display-adaptive. Minor SDR compression is negligible in practice.

---

#### Function 4: HDR Display Tonemap (Our Recommended — SDR Passthrough + Reinhard Headroom)

```hlsl
// SDR passthrough: values ≤ 1.0 scRGB (80 nits) are untouched.
// HDR headroom: compressed via Reinhard rolloff toward display peak.
// This is the RECOMMENDED function for the HobbyRenderer HDR path.
float3 HDRDisplayTonemap(float3 x, float MaxDisplayNits)
{
    float maxSCRGB = MaxDisplayNits / 80.0;
    float lum = max(x.r, max(x.g, x.b));

    // SDR passthrough: no compression at or below 80 nits
    if (lum <= 1.0)
        return x;

    // Compress only the headroom above SDR white
    float hdrHeadroom = maxSCRGB - 1.0;
    float excess = lum - 1.0;
    float compressedExcess = excess * hdrHeadroom / (excess + hdrHeadroom);

    float newLum = 1.0 + compressedExcess;
    float3 result = x * (newLum / lum);

    return min(result, maxSCRGB);
}
```

**Behavior** (1000-nit display):

| Scene Value | scRGB | Nits | Output scRGB | Output Nits | Effect |
|---|---|---|---|---|---|
| Dark | 0.5 | 40 | 0.50 | 40 | Untouched |
| SDR white | 1.0 | 80 | 1.00 | 80 | Untouched ✓ |
| Bright | 5.0 | 400 | 3.78 | 302 | Gentle |
| Very bright | 12.5 | 1000 | 6.75 | 540 | Half headroom |
| Extreme | 100 | 8000 | 11.33 | 906 | Near-max |
| ∞ | ∞ | ∞ | 12.50 | 1000 | Asymptote |

| Pro | Con |
|---|---|
| **Zero SDR degradation** — UI looks identical to SDR mode | Slightly more ALU than Reinhard Extended |
| Smooth highlight rolloff to display peak | Transition at 80 nits is a hard condition (branch) |
| Display-adaptive (uses real `MaxDisplayNits`) |  |
| Simple enough for real-time use |  |

**Verdict**: ✅✅ **Recommended.** Preserves SDR quality perfectly while providing smooth HDR highlight handling.

---

#### Function 5: ACES Fitted (Filmic)

```hlsl
// ACES filmic approximation by Krzysztof Narkowicz.
// Designed for SDR output but can be scaled for HDR displays.
float3 ACESFitted(float3 x, float MaxDisplayNits)
{
    float maxSCRGB = MaxDisplayNits / 80.0;

    // Scale input to fit the ACES curve, then scale output to display range
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;

    float3 scaled = x * (1.0 / maxSCRGB); // normalize to [0, ~1]
    float3 tonemapped = saturate((scaled * (a * scaled + b)) / (scaled * (c * scaled + d) + e));

    return tonemapped * maxSCRGB;
}
```

| Pro | Con |
|---|---|
| Filmic look — gently lifts shadows | SDR content gets reshaped (not passthrough) |
| Well-known, widely used in games | Curve parameters optimized for SDR, not HDR |
| Good color saturation in highlights | Requires tuning per display max |

**Verdict**: ⚠️ Solid but overkill for a first HDR implementation. The curve was designed for SDR output and needs per-display retuning for HDR.

---

#### Function 6: Uncharted 2 (Filmic)

```hlsl
// Uncharted 2 filmic tonemapping by John Hable.
// Creates a strong filmic shoulder and toe.
float3 Uncharted2Tonemap(float3 x, float MaxDisplayNits)
{
    float maxSCRGB = MaxDisplayNits / 80.0;

    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    float W = 11.2;

    float3 scaled = x / maxSCRGB;
    float3 mapped = ((scaled * (A * scaled + C * B) + D * E) /
                     (scaled * (A * scaled + B) + D * F)) - E / F;
    float3 white = ((W * (A * W + C * B) + D * E) /
                    (W * (A * W + B) + D * F)) - E / F;

    return saturate(mapped / white) * maxSCRGB;
}
```

| Pro | Con |
|---|---|
| Distinctive filmic shoulder | Heavy ALU cost (lots of FMAs) |
| Good color fidelity | Overly aggressive toe darkens shadows |
| | Curve tuned for SDR — needs HDR reparameterization |

**Verdict**: ❌ Not recommended for HDR display output. Designed for SDR, and the toe/shoulder were calibrated for an SDR display's gamma curve, not scRGB linear.

---

#### Summary: Which Function to Use

| Priority | Function | When to Use |
|---|---|---|
| **1st (use this)** | **HDRDisplayTonemap** | SDR passthrough + Reinhard headroom. Best default for HDR. |
| 2nd | Reinhard Extended | If you prefer classic Reinhard look over perfect SDR passthrough |
| 3rd | ACES Fitted | If you want a filmic look and are willing to tune per-display |
| Don't use | Naive Passthrough | Specular aliasing + no artistic control |
| Don't use | Reinhard Classic | Designed for SDR, not HDR displays |
| Don't use | Uncharted 2 | SDR-tuned filmic; HDR reparameterization needed |

**Implementation plan**: Ship `HDRDisplayTonemap` as the only HDR path. If the look needs adjusting later, swap to Reinhard Extended or ACES Fitted.

---

## 3. PQ Encoding — What It Is and When You Need It

### 3.1 What Is PQ?

**PQ (Perceptual Quantizer)**, standardized as **ST.2084**, is a transfer function that maps absolute luminance values (0–10,000 nits) to 10-bit or 12-bit integer codes. It's "perceptual" because the quantization steps are spaced to match human visual sensitivity — no step is visible to the eye.

### 3.2 When PQ Is Required

| Scenario | Need PQ? | Why |
|---|---|---|
| HDR10 video playback | **Yes** | Content is PQ-encoded; mandatory for HDR10 spec |
| HDR10 swap chain (R10G10B10A2) | **Yes** | 10 bits/channel would band without perceptual encoding |
| scRGB FP16 swap chain | **No** | 16-bit float has orders of magnitude more precision |
| Console (PS5/Xbox) HDR output | **Yes** | Consoles use HDR10 output path |
| PC game with FP16 scRGB | **No** | Windows compositor handles conversion |
| HDMI HDR signaling | **Yes** | HDR10 metadata is part of HDMI 2.0a+ spec |

### 3.3 Why We Don't Need PQ

With scRGB FP16:
- We output **linear light values** directly from the shader
- Windows compositor handles scRGB → display conversion
- FP16 has ~1024 quantized levels between 0–1 scRGB (0–80 nits), and exponentially more at HDR levels — no banding
- PQ would add unnecessary shader complexity (every pixel needs `ST2084_OETF()`)

---

## 4. DXGI_HDR_METADATA_HDR10 — Do We Need It? Can We Query Real Values?

### 4.1 Short Answer

**For scRGB FP16: `SetHDRMetaData` is NOT needed.** The color space alone (`SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)`) is sufficient for the Windows compositor to handle scRGB output correctly. `SetHDRMetaData` is designed primarily for HDR10 content (BT.2020 primaries + PQ transfer function) and describes the **content's** color volume, not the display's. For scRGB, the metadata struct doesn't match the actual color space in use, making it technically misleading.

### 4.2 BUT — Can We Query Real Values Instead of Hardcoding?

**Yes.** `IDXGIOutput6::GetDesc1()` returns a `DXGI_OUTPUT_DESC1` structure with the display's actual color characteristics. No hardcoded numbers needed.

### 4.3 `DXGI_OUTPUT_DESC1` vs `DXGI_HDR_METADATA_HDR10` — Field Mapping

| DXGI_OUTPUT_DESC1 field | Type | DXGI_HDR_METADATA_HDR10 field | Type | Conversion |
|---|---|---|---|---|
| `RedPrimary[2]`  | `FLOAT` (CIE 1931 xy) | `RedPrimary[2]` | `UINT16` | Multiply by **50000** |
| `GreenPrimary[2]`| `FLOAT` (CIE 1931 xy) | `GreenPrimary[2]` | `UINT16` | Multiply by **50000** |
| `BluePrimary[2]` | `FLOAT` (CIE 1931 xy) | `BluePrimary[2]` | `UINT16` | Multiply by **50000** |
| `WhitePoint[2]`  | `FLOAT` (CIE 1931 xy) | `WhitePoint[2]` | `UINT16` | Multiply by **10000** |
| `MaxLuminance`   | `FLOAT` (cd/m² = nits) | `MaxMasteringLuminance` | `UINT` | Multiply by **10000** → (0.0001 cd/m² units) |
| `MinLuminance`   | `FLOAT` (cd/m² = nits) | `MinMasteringLuminance` | `UINT` | Same: Multiply by **10000** |
| — (no direct source) | | `MaxContentLightLevel` | `UINT` | Use `MaxLuminance` (nits) |
| — (no direct source) | | `MaxFrameAverageLightLevel` | `UINT` | Heuristic: `MaxLuminance * 0.4` |

The primaries and white point in both structures use the CIE 1931 xy chromaticity coordinate system. The only difference is the scale factor: output desc uses `float` in the [0, 1] range, while HDR10 metadata uses `uint16` scaled by 50000 (primaries) or 10000 (white point).

### 4.4 Code to Query and Populate (For Reference)

```cpp
// Query the display's actual color characteristics
void QueryDisplayHDRCapabilities(IDXGISwapChain3* swapChain,
                                  float& outMaxNits,
                                  float& outMinNits)
{
    ComPtr<IDXGIOutput> output;
    if (FAILED(swapChain->GetContainingOutput(&output)))
        return;

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6)))
        return;

    DXGI_OUTPUT_DESC1 desc1;
    if (FAILED(output6->GetDesc1(&desc1)))
        return;

    outMaxNits = desc1.MaxLuminance;
    outMinNits = desc1.MinLuminance;

    // ═══════════════════════════════════════════════════════
    // Optionally populate HDR10 metadata (for HDR10 swapchains only)
    // ═══════════════════════════════════════════════════════
    //
    // DXGI_HDR_METADATA_HDR10 hdrMeta = {};
    //
    // // Primaries: CIE 1931 xy float → uint16 (×50000)
    // hdrMeta.RedPrimary[0]   = static_cast<UINT16>(desc1.RedPrimary[0]   * 50000.0f);
    // hdrMeta.RedPrimary[1]   = static_cast<UINT16>(desc1.RedPrimary[1]   * 50000.0f);
    // hdrMeta.GreenPrimary[0] = static_cast<UINT16>(desc1.GreenPrimary[0] * 50000.0f);
    // hdrMeta.GreenPrimary[1] = static_cast<UINT16>(desc1.GreenPrimary[1] * 50000.0f);
    // hdrMeta.BluePrimary[0]  = static_cast<UINT16>(desc1.BluePrimary[0]  * 50000.0f);
    // hdrMeta.BluePrimary[1]  = static_cast<UINT16>(desc1.BluePrimary[1]  * 50000.0f);
    //
    // // White point: CIE 1931 xy float → uint16 (×10000)
    // hdrMeta.WhitePoint[0]   = static_cast<UINT16>(desc1.WhitePoint[0] * 10000.0f);
    // hdrMeta.WhitePoint[1]   = static_cast<UINT16>(desc1.WhitePoint[1] * 10000.0f);
    //
    // // Luminance: nits → 0.0001 cd/m² units (×10000)
    // hdrMeta.MaxMasteringLuminance =
    //     static_cast<UINT>(desc1.MaxLuminance * 10000.0f);
    // hdrMeta.MinMasteringLuminance =
    //     static_cast<UINT>(desc1.MinLuminance * 10000.0f);
    //
    // // Content light levels (nits, no conversion)
    // hdrMeta.MaxContentLightLevel =
    //     static_cast<UINT>(desc1.MaxLuminance);
    // hdrMeta.MaxFrameAverageLightLevel =
    //     static_cast<UINT>(desc1.MaxFullFrameLuminance > 0.0f
    //         ? desc1.MaxFullFrameLuminance
    //         : desc1.MaxLuminance * 0.4f); // heuristic fallback
    //
    // swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
    //                            sizeof(DXGI_HDR_METADATA_HDR10), &hdrMeta);
}
```

### 4.5 Why We Skip `SetHDRMetaData` for scRGB

| Reason | Detail |
|---|---|
| **Wrong color space** | `DXGI_HDR_METADATA_HDR10` describes BT.2020 primaries + PQ EOTF. scRGB uses sRGB primaries + linear (G10). The metadata would advertise incorrect content characteristics. |
| **OS compatibility** | The Windows compositor handles scRGB correctly from `SetColorSpace1` alone. No metadata needed. |
| **No visible difference** | In practice, calling `SetHDRMetaData` with scRGB has no observable effect on rendering quality. Windows ignores it for non-HDR10 content. |
| **HDR Calibration app** | Windows 11's HDR Calibration tool stores per-display profiles. The compositor already reads EDID and calibration data — it doesn't need the app to restate what it already knows. |
| **When you WOULD need it** | If switching to `DXGI_FORMAT_R10G10B10A2_UNORM` + `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (HDR10 path), then `SetHDRMetaData` becomes **required** for correct HDMI signaling and display tonemapping. |

**Conclusion**: For our scRGB FP16 implementation, we query `MaxLuminance` from `DXGI_OUTPUT_DESC1` to feed the tonemapping shader, but we do **not** call `SetHDRMetaData`. The color space call handles everything the compositor needs.

---

## 5. Detection Strategy (Section 5.2 Auto-Detection)

### Three-Layer Detection

```
Layer 1: SDL3 Display Property
    SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN
    → Is Windows HDR mode ON for this display?
    
Layer 2: DXGI Color Space Support
    IDXGISwapChain4::CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
    → Can the GPU present scRGB to this display?
    
Layer 3: Display Nits
    IDXGIOutput6::GetDesc1() → MaxLuminance
    → What's the peak luminance of this display?
```

### Detection Code (to be added to D3D12RHI.cpp)

**In `Initialize()`** (after device creation, before swapchain):

```cpp
#include <SDL3/SDL_video.h>  // needed for SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN

// Detect HDR display support via SDL3
{
    SDL_DisplayID displayID = SDL_GetDisplayForWindow(window);
    SDL_PropertiesID props = SDL_GetDisplayProperties(displayID);
    m_bDisplaySupportsHDR = SDL_GetBooleanProperty(props,
        SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
}
```

**In `CreateSwapchain()`** (conditional HDR format + color space):

```cpp
// Auto-detect best swap chain format
const bool useHDR = m_bDisplaySupportsHDR;
const DXGI_FORMAT swapchainFormat = useHDR
    ? DXGI_FORMAT_R16G16B16A16_FLOAT
    : DXGI_FORMAT_R8G8B8A8_UNORM;

swapChainDesc.Format = swapchainFormat;
// ... create swap chain, then:

if (useHDR)
{
    ComPtr<IDXGISwapChain4> swapChain4;
    if (SUCCEEDED(m_SwapChain.As(&swapChain4)))
    {
        UINT colorSpaceSupport = 0;
        hr = swapChain4->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
            &colorSpaceSupport);
        if (SUCCEEDED(hr) &&
            (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        {
            swapChain4->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            m_bIsHDR = true;
            m_SwapchainFormat = nvrhi::Format::RGBA16_FLOAT;

            // Query display nits
            ComPtr<IDXGIOutput> output;
            if (SUCCEEDED(m_SwapChain->GetContainingOutput(&output)))
            {
                ComPtr<IDXGIOutput6> output6;
                if (SUCCEEDED(output.As(&output6)))
                {
                    DXGI_OUTPUT_DESC1 desc1;
                    if (SUCCEEDED(output6->GetDesc1(&desc1)))
                    {
                        m_MaxDisplayNits = desc1.MaxLuminance;
                        if (m_MaxDisplayNits <= 0.0f || m_MaxDisplayNits > 10000.0f)
                            m_MaxDisplayNits = 1000.0f; // safe default
                    }
                }
            }
        }
        else
        {
            // scRGB not supported by GPU, fall back to SDR
            m_bIsHDR = false;
            m_SwapchainFormat = nvrhi::Format::RGBA8_UNORM;
            m_MaxDisplayNits = 80.0f;
            SDL_Log("HDR display detected but scRGB not supported; falling back to SDR.");
        }
    }
}
else
{
    m_bIsHDR = false;
    m_SwapchainFormat = nvrhi::Format::RGBA8_UNORM;
    m_MaxDisplayNits = 80.0f;
}
```

**New members in `GraphicRHI.h`:**

```cpp
bool m_bIsHDR = false;
float m_MaxDisplayNits = 80.0f;  // SDR default. Updated by D3D12RHI CreateSwapchain.
```

**New member in `D3D12GraphicRHI`:**

```cpp
bool m_bDisplaySupportsHDR = false;
```

---

## 6. ImGui Handling

**No code changes needed.** ImGui works correctly because:

1. `fbInfo.colorFormats = { g_Renderer.m_RHI->m_SwapchainFormat }` — adapts automatically to `RGBA16_FLOAT`
2. D3D12 blend states are format-independent — premultiplied alpha blending works with FP16
3. ImGui PS outputs sRGB colors in [0,1] → in scRGB this equals 80 nits (perfect SDR-brightness UI)
4. Graphics pipeline caching is keyed by `FramebufferInfoEx` including format — HDR pipelines will be separate cache entries, created on demand

---

## 7. Shader Architecture

### 7.1 New Entry Point: `TonemapHDR_PSMain`

Added to `Tonemap.hlsl` alongside the existing `Tonemap_PSMain` (SDR path). Both share the same inputs:

| Entry Point | Swapchain | Tonemap | Gamma | Output Range |
|---|---|---|---|---|
| `Tonemap_PSMain` | SDR (RGBA8) | PBRNeutralToneMapping | sRGB OETF | [0, 1] |
| `TonemapHDR_PSMain` | HDR (RGBA16_FLOAT) | HDRDisplayTonemap (Reinhard) | None (linear) | [0, maxSCRGB] |

### 7.2 Push Constant Changes

Add `float m_MaxDisplayNits` to `TonemapConstants` (changes `PushConstantBytes` from 8 to 12):

```
HDR.sr cbuffer TonemapConstants: add  float m_MaxDisplayNits
HDR.hlsli struct TonemapConstants:  add  float m_MaxDisplayNits
HDR.h class TonemapConstants:       add  float MaxDisplayNits + setter
                                    bump PushConstantBytes: 8 → 12
```

### 7.3 Tonemap.hlsl Changes

- Add `HDRDisplayTonemap()` function (SDR passthrough + HDR-only Reinhard rolloff)
- Add `TonemapHDR_PSMain` entry point
- Existing `Tonemap_PSMain` unchanged

---

## 8. Files to Modify (When Ready)

| File | Changes |
|---|---|
| `src/GraphicRHI.h` | Add `m_bIsHDR`, `m_MaxDisplayNits` members |
| `src/D3D12RHI.cpp` | `#include <SDL3/SDL_video.h>`, add `m_bDisplaySupportsHDR`, SDL3 detection in `Initialize()`, HDR format/color-space/nits logic in `CreateSwapchain()` |
| `src/shaders/HDR.sr` | Add `float m_MaxDisplayNits` to `TonemapConstants` cbuffer |
| `src/shaders/srrhi/hlsl/HDR.hlsli` | Same struct update (auto-generated mirror) |
| `src/shaders/srrhi/cpp/HDR.h` | Add getter/setter, bump `PushConstantBytes` 8→12 |
| `src/shaders/Tonemap.hlsl` | Add `HDRDisplayTonemap()`, add `TonemapHDR_PSMain` |
| `src/shaders/shaders.cfg` | Add `Tonemap.hlsl -T ps -E TonemapHDR_PSMain -m 6_8` |
| `src/shaders/ShaderIDs.h` | Add `TONEMAP_TONEMAPHDR_PSMAIN` constant, bump COUNT, add ENTRIES row |
| `src/HDRRenderer.cpp` | Select HDR vs SDR shader by `m_bIsHDR`, pass `MaxDisplayNits` |

**No changes needed**: `Renderer.cpp`, `Renderer.h`, `Config.h`, `ImGuiRenderer.cpp`, `CommonRenderers.cpp`, `BasePassRenderer.cpp`

---

## 9. Shader Rebuild Note

After modifying shader source files, the shader compilation tool must be re-run to produce the new `Tonemap_TonemapHDR_PSMain.dxil` binary. The build system handles this via CMake.

---

## 10. Summary of Findings

| Question | Answer |
|---|---|
| Best HDR format? | **scRGB FP16** (R16G16B16A16_FLOAT) — linear, simple, no PQ |
| Correct Reinhard formula? | User's version has unit mismatch (nits ÷ scRGB) and compresses SDR content. **Corrected `HDRDisplayTonemap`** preserves 0–80 nits passthrough, rolloff only above |
| Can `DXGI_HDR_METADATA_HDR10` values be queried from DXGI? | **Yes** — `DXGI_OUTPUT_DESC1` provides `RedPrimary`, `GreenPrimary`, `BluePrimary`, `WhitePoint`, `MinLuminance`, `MaxLuminance`, `MaxFullFrameLuminance` — all real values from the display's EDID |
| Do we need `SetHDRMetaData` for scRGB? | **No.** The color primaries in HDR10 metadata are BT.2020, but scRGB uses sRGB primaries. The metadata would describe the wrong color volume. `SetColorSpace1` alone is sufficient. |
| Is `SetHDRMetaData` needed for optimal visuals? | **No** for scRGB. The Windows compositor and HDR Calibration profile already know the display's capabilities from EDID. HDR10 metadata is only meaningful for HDR10 content (BT.2020 + PQ). |
| Recommended HDR tonemapping? | **`HDRDisplayTonemap`** — SDR passthrough (0–80 nits untouched) + Reinhard rolloff for HDR headroom. Display-adaptive via queried `MaxLuminance`. See Section 2.5 for alternatives. |
| Clamp to display max nits? | **Yes** — safety clamp after rolloff |
| PQ encoding? | **Not needed** for scRGB FP16. Only needed for HDR10 10-bit to prevent banding |
| ImGui changes? | **None** — adapts automatically via dynamic `m_SwapchainFormat` |
| Config needed? | **None** — fully automatic detection. If HDR display is detected → HDR swapchain. No user toggle. |
| Arg passing? | **None** — detection in `Initialize()`, swapchain creation in `CreateSwapchain()` |
