/*
 * BuildEnvLightPDF.hlsl
 *
 * Writes the Bruneton procedural sky luminance (weighted by cosine of elevation)
 * into mip-0 of the environment-light PDF texture.
 *
 * This pass runs every frame (sky changes with sun direction) before
 * GenerateMipsUsingSPD and PresampleEnvironmentMap, so that the env RIS
 * segment is importance-sampled according to the actual sky distribution.
 *
 * The PDF value stored per texel is:
 *   luminance(GetAtmosphereSkyRadiance(dir)) * cosElevation
 *
 * cosElevation is the Jacobian of the equirectangular projection — it
 * accounts for the fact that texels near the poles subtend a smaller solid
 * angle than texels near the equator.  Multiplying by it makes the stored
 * value proportional to the actual power contribution of each texel.
 *
 * Binding layout:
 *   b0 = ResamplingConstants (g_Const)  — provides sunDirection, sunIntensity, enableEnvironmentMap
 *   u0 = u_EnvLightPdfMip0            — RWTexture2D<float>, mip-0 of the env PDF texture
 */

#pragma pack_matrix(row_major)

// Minimal includes — avoid pulling in the full RtxdiApplicationBridge which
// declares many resources that would fail binding validation in this pass.
#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/ReGIR/ReGIRParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>

#include "../../Atmosphere.hlsli"
#include "../../CommonLighting.hlsli"

#include "srrhi/hlsl/RTXDI.hlsli"

#define g_Const              srrhi::BuildEnvLightPDFInputs::GetConst()
#define u_EnvLightPdfMip0    srrhi::BuildEnvLightPDFInputs::GetEnvLightPdfMip0()

[numthreads(8, 8, 1)]
void main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    uint2 pdfSize = g_Const.environmentPdfTextureSize;

    if (any(GlobalIndex >= pdfSize))
        return;

    // If sky is disabled, write uniform 1.0 so PresampleEnvironmentMap still
    // produces a valid (uniform) distribution.
    if (!g_Const.sceneConstants.enableEnvironmentMap)
    {
        u_EnvLightPdfMip0[GlobalIndex] = 1.0f;
        return;
    }

    // Convert texel index to equirectangular UV in [0,1]^2.
    // Centre of texel: (i + 0.5) / size
    float2 uv = (float2(GlobalIndex) + 0.5f) / float2(pdfSize);

    // Decode UV to world-space direction and cosine of elevation.
    float cosElevation;
    float3 dir = equirectUVToDirection(uv, cosElevation);

    // Sample the Bruneton sky — exclude the sun disk to avoid extreme spikes
    // (the sun is handled as a separate directional RTXDI light).
    float3 skyRadiance = GetAtmosphereSkyRadiance(
        float3(0.0f, 0.0f, 0.0f),
        dir,
        g_Const.sceneConstants.sunDirection,
        g_Const.sceneConstants.sunIntensity,
        false); // bAddSunDisk = false

    // Weight by cosElevation (equirectangular solid-angle Jacobian).
    // Guard against degenerate poles where cosElevation → 0.
    float pdfValue = calcLuminance(skyRadiance) * max(cosElevation, 1e-4f);

    u_EnvLightPdfMip0[GlobalIndex] = pdfValue;
}
