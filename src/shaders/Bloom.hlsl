#include "Common.hlsli"

#include "srrhi/hlsl/Common.hlsli"
#include "srrhi/hlsl/Bloom.hlsli"

static const srrhi::BloomPrefilterConstants  PrefilterCB    = srrhi::BloomPrefilterInputs::GetPrefilterConstants();
static const Texture2D<float3>               PrefilterInput = srrhi::BloomPrefilterInputs::GetInputTexture();

static const srrhi::BloomDownsampleConstants DownsampleCB    = srrhi::BloomDownsampleInputs::GetDownsampleConstants();
static const Texture2D<float3>               DownsampleInput = srrhi::BloomDownsampleInputs::GetInputTexture();

static const srrhi::BloomUpsampleConstants   UpsampleCB     = srrhi::BloomUpsampleInputs::GetUpsampleConstants();
static const Texture2D<float3>               UpsampleSource = srrhi::BloomUpsampleInputs::GetSourceTexture();
static const Texture2D<float3>               UpsampleBloom  = srrhi::BloomUpsampleInputs::GetBloomTexture();

// --- Prefilter ---

float3 SafeHDR(float3 c)
{
    return min(c, 65504.0); // Clamp to half-float max
}

float3 Prefilter(float3 color)
{
    float brightness = max(color.r, max(color.g, color.b));
    float knee = max(PrefilterCB.m_Knee * PrefilterCB.m_Threshold, 1e-4);

    // Only the part of the pixel above the threshold blooms, eased in over a knee. Without this every pixel
    // reaches the blur, so one very bright emitter (a lamp at a few hundred in linear) smears over the whole
    // frame instead of glowing around itself.
    float soft = clamp(brightness - PrefilterCB.m_Threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    return color * (max(soft, brightness - PrefilterCB.m_Threshold) / max(brightness, 1e-4));
}

float4 Prefilter_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX];
    float3 color = PrefilterInput.SampleLevel(linearClampSampler, input.uv, 0).rgb;
    return float4(Prefilter(SafeHDR(color)) * PrefilterCB.m_Strength, 1.0);
}

// --- Downsample (Jimenez 13-tap) ---

float4 Downsample_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX];
    float2 texelSize = 1.0 / float2(DownsampleCB.m_Width, DownsampleCB.m_Height);
    float2 uv = input.uv;

    // Jimenez 13-tap downsample pattern
    // Sampling at -1.0, 0.0, 1.0 in target texel space
    // corresponds to -2.0, 0.0, 2.0 in source texel space.
    // Sampling at -0.5, 0.5 in target texel space
    // corresponds to -1.0, 1.0 in source texel space.

    float3 a = DownsampleInput.SampleLevel(linearClampSampler, uv + float2(-1, -1) * texelSize, 0).rgb;
    float3 b = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 0, -1) * texelSize, 0).rgb;
    float3 c = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 1, -1) * texelSize, 0).rgb;

    float3 d = DownsampleInput.SampleLevel(linearClampSampler, uv + float2(-0.5, -0.5) * texelSize, 0).rgb;
    float3 e = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 0.5, -0.5) * texelSize, 0).rgb;

    float3 f = DownsampleInput.SampleLevel(linearClampSampler, uv + float2(-1,  0) * texelSize, 0).rgb;
    float3 g = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 0,  0) * texelSize, 0).rgb;
    float3 h = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 1,  0) * texelSize, 0).rgb;

    float3 i = DownsampleInput.SampleLevel(linearClampSampler, uv + float2(-0.5,  0.5) * texelSize, 0).rgb;
    float3 j = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 0.5,  0.5) * texelSize, 0).rgb;

    float3 k = DownsampleInput.SampleLevel(linearClampSampler, uv + float2(-1,  1) * texelSize, 0).rgb;
    float3 l = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 0,  1) * texelSize, 0).rgb;
    float3 m = DownsampleInput.SampleLevel(linearClampSampler, uv + float2( 1,  1) * texelSize, 0).rgb;

    float3 result = g * 0.125;
    result += (a + c + k + m) * 0.03125;
    result += (b + f + h + l) * 0.0625;
    result += (d + e + i + j) * 0.125;

    return float4(result, 1.0);
}

// --- Upsample (9-tap tent filter) ---

float4 Upsample_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX];
    float2 texelSize = 1.0 / float2(UpsampleCB.m_Width, UpsampleCB.m_Height);
    float d = UpsampleCB.m_UpsampleRadius;

    // 9-tap tent filter
    float3 a = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2(-d, -d) * texelSize, 0).rgb;
    float3 b = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2( 0, -d) * texelSize, 0).rgb;
    float3 c = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2( d, -d) * texelSize, 0).rgb;

    float3 d_ = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2(-d,  0) * texelSize, 0).rgb;
    float3 e  = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2( 0,  0) * texelSize, 0).rgb;
    float3 f  = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2( d,  0) * texelSize, 0).rgb;

    float3 g = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2(-d,  d) * texelSize, 0).rgb;
    float3 h = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2( 0,  d) * texelSize, 0).rgb;
    float3 i = UpsampleSource.SampleLevel(linearClampSampler, input.uv + float2( d,  d) * texelSize, 0).rgb;

    float3 upsample = e * 0.25;
    upsample += (b + d_ + f + h) * 0.125;
    upsample += (a + c + g + i) * 0.0625;

    float3 bloom = UpsampleBloom.SampleLevel(linearClampSampler, input.uv, 0).rgb;

    return float4(bloom + upsample, 1.0);
}

// --- Composite (add bloom mip0 into scene color via additive blending) ---

static const srrhi::BloomCompositeConstants CompositeCB     = srrhi::BloomCompositeInputs::GetCompositeConstants();
static const Texture2D<float3>              CompositeBloom  = srrhi::BloomCompositeInputs::GetBloomTexture();

float4 Composite_PSMain(FullScreenVertexOut input) : SV_Target
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX];
    float3 bloom = CompositeBloom.SampleLevel(linearClampSampler, input.uv, 0).rgb;
    return float4(bloom * CompositeCB.m_BloomIntensity, 1.0);
}
