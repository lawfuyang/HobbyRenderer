/*
 * RTXDIShadeSamples.hlsl
 *
 * Pass 3 of the ReSTIR DI pipeline.
 * Loads the final reservoir (after optional temporal / spatial resampling),
 * evaluates the selected light sample against the surface BRDF, and writes
 * the direct-lighting radiance to g_RTXDIDIOutput.
 */

#include "RTXDIApplicationBridge.hlsli"

// RTXDI SDK — reservoir load/store utilities
#include "Rtxdi/DI/Reservoir.hlsli"

// ============================================================================
RTXDI_ReservoirBufferParameters GetReservoirBufferParams()
{
    RTXDI_ReservoirBufferParameters p;
    p.reservoirBlockRowPitch = g_RTXDIConst.m_ReservoirBlockRowPitch;
    p.reservoirArrayPitch    = g_RTXDIConst.m_ReservoirArrayPitch;
    return p;
}

// ============================================================================
[numthreads(8, 8, 1)]
void RTXDIShadeSamples_CSMain(uint2 GlobalIndex : SV_DispatchThreadID)
{
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(GlobalIndex >= viewportSize))
        return;

    RTXDI_ReservoirBufferParameters rbp = GetReservoirBufferParams();

    uint2 pixelPosition = GlobalIndex;
    int2  iPixel        = int2(pixelPosition);

    // Load the final shading reservoir (temporal or initial output, depending on which passes ran)
    RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(rbp, pixelPosition,
        g_RTXDIConst.m_ShadingInputBufferIndex);

    float3 radiance = float3(0.0, 0.0, 0.0);

    if (RTXDI_IsValidDIReservoir(reservoir))
    {
        // Fetch G-buffer surface
        RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

        if (RAB_IsSurfaceValid(surface))
        {
            // Reconstruct the selected light sample from the reservoir
            uint lightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
            float2 randXY   = RTXDI_GetDIReservoirSampleUV(reservoir);

            RAB_LightInfo   lightInfo     = RAB_LoadLightInfo(lightIndex, false);
            RAB_LightSample lightSample   = RAB_SamplePolymorphicLight(lightInfo, surface, randXY);

            // Visibility test
            bool visible = RAB_GetConservativeVisibility(surface, lightSample);

            if (visible)
            {
                // Evaluate BRDF at the selected light direction
                float3 V = RAB_GetSurfaceViewDirection(surface);
                float3 L = lightSample.direction;

                float3 brdf    = RAB_EvaluateBrdf(surface, L, V);
                float  NdotL   = max(0.0, dot(RAB_GetSurfaceNormal(surface), L));

                // BRDF already contains the NdotL factor; avoid double-counting
                // as RAB_EvaluateBrdf returns brdf * NdotL.
                // Reservoir weight (1/p_hat * W) accounts for the resampling.
                float weight = RTXDI_GetDIReservoirInvPdf(reservoir);

                // Guard against zero / degenerate pdf
                if (weight > 0.0 && NdotL > 0.0)
                {
                    // The reservoir weight already encodes the target pdf normalisation.
                    // radiance = Le * brdf_without_NdotL * NdotL * weight
                    // RAB_EvaluateBrdf returns (diffuse + specular) * NdotL already, so:
                    radiance = lightSample.radiance * brdf * weight;
                }
            }
        }
    }

    // Clamp to prevent fireflies (very large single-sample contributions)
    radiance = min(radiance, float3(100.0, 100.0, 100.0));

    g_RTXDIDIOutput[pixelPosition] = float4(radiance, 1.0);
}
