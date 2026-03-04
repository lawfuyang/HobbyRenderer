// ============================================================================
// SECTION 1: Boiling filter defines (for RTXDITemporalResampling)
// ============================================================================
#define RTXDI_ENABLE_BOILING_FILTER    1
#define RTXDI_BOILING_FILTER_GROUP_SIZE 8
#define RTXDI_PRESAMPLING_GROUP_SIZE    256

// ============================================================================
// SECTION 2: Common includes
// ============================================================================
#include "RTXDIApplicationBridge.hlsli"
#include "ShaderShared.h"
#include "CommonLighting.hlsli"

// ============================================================================
// SECTION 2B: Additional resources for RTXDIBuildLocalLightPDF_Main and RTXDIBuildEnvLightPDF_Main
// ============================================================================
RWTexture2D<float> g_PDFMip0    : register(u4);  // mip 0 of the local-light PDF texture
RWTexture2D<float> g_EnvPDFMip0 : register(u8);  // mip 0 of the environment-light PDF texture

// Note: g_RTXDI_EnvLightPDFTexture (t19) is declared in RTXDIApplicationBridge.hlsli

#include "Rtxdi/Utils/Checkerboard.hlsli"
#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/TemporalResampling.hlsli"
#include "Rtxdi/DI/BoilingFilter.hlsli"
#include "Rtxdi/DI/SpatialResampling.hlsli"
#include "Rtxdi/LightSampling/PresamplingFunctions.hlsli"

// ============================================================================
// SECTION 2C: NRD includes for RELAX denoising (only when permutation active)
// ============================================================================
#if RTXDI_ENABLE_RELAX_DENOISING
#include "NRDConfig.hlsli"
#include "NRD.hlsli"
#endif

// ============================================================================
// SECTION 3: Helper functions shared across multiple passes
// ============================================================================

RTXDI_LightBufferParameters GetLightBufferParams()
{
    RTXDI_LightBufferParameters p;
    p.localLightBufferRegion.firstLightIndex    = g_RTXDIConst.m_LocalLightFirstIndex;
    p.localLightBufferRegion.numLights          = g_RTXDIConst.m_LocalLightCount;
    p.infiniteLightBufferRegion.firstLightIndex = g_RTXDIConst.m_InfiniteLightFirstIndex;
    p.infiniteLightBufferRegion.numLights       = g_RTXDIConst.m_InfiniteLightCount;
    p.environmentLightParams.lightPresent       = g_RTXDIConst.m_EnvLightPresent;
    p.environmentLightParams.lightIndex         = g_RTXDIConst.m_EnvLightIndex;
    return p;
}

RTXDI_RuntimeParameters GetRuntimeParams()
{
    RTXDI_RuntimeParameters p;
    p.neighborOffsetMask      = g_RTXDIConst.m_NeighborOffsetMask;
    p.activeCheckerboardField = g_RTXDIConst.m_ActiveCheckerboardField;
    return p;
}

RTXDI_ReservoirBufferParameters GetReservoirBufferParams()
{
    RTXDI_ReservoirBufferParameters p;
    p.reservoirBlockRowPitch = g_RTXDIConst.m_ReservoirBlockRowPitch;
    p.reservoirArrayPitch    = g_RTXDIConst.m_ReservoirArrayPitch;
    return p;
}

RTXDI_LightBufferRegion GetLocalLightBufferRegion()
{
    RTXDI_LightBufferRegion r;
    r.firstLightIndex = g_RTXDIConst.m_LocalLightFirstIndex;
    r.numLights       = g_RTXDIConst.m_LocalLightCount;
    r.pad1 = r.pad2   = 0u;
    return r;
}

RTXDI_RISBufferSegmentParameters GetLocalLightRISSegmentParams()
{
    RTXDI_RISBufferSegmentParameters p;
    p.bufferOffset = g_RTXDIConst.m_LocalRISBufferOffset;
    p.tileSize     = g_RTXDIConst.m_LocalRISTileSize;
    p.tileCount    = g_RTXDIConst.m_LocalRISTileCount;
    p.pad1         = 0u;
    return p;
}

RTXDI_RISBufferSegmentParameters GetLocalLightRISBufferSegmentParams()
{
    RTXDI_RISBufferSegmentParameters p;
    p.bufferOffset = g_RTXDIConst.m_LocalRISBufferOffset;
    p.tileSize     = g_RTXDIConst.m_LocalRISTileSize;
    p.tileCount    = g_RTXDIConst.m_LocalRISTileCount;
    p.pad1         = 0u;
    return p;
}

RTXDI_RISBufferSegmentParameters GetEnvLightRISBufferSegmentParams()
{
    RTXDI_RISBufferSegmentParameters p;
    p.bufferOffset = g_RTXDIConst.m_EnvRISBufferOffset;
    p.tileSize     = g_RTXDIConst.m_EnvRISTileSize;
    p.tileCount    = g_RTXDIConst.m_EnvRISTileCount;
    p.pad1         = 0u;
    return p;
}

// ============================================================================
// SECTION 4: RTXDIBuildLocalLightPDF
// ============================================================================

[numthreads(8, 8, 1)]
void RTXDI_BuildLocalLightPDF_Main(uint2 gid : SV_DispatchThreadID)
{
    // Bounds check against mip-0 dimensions.
    if (any(gid >= g_RTXDIConst.m_LocalLightPDFTextureSize))
        return;

    // Map texel position → local-light index via Z-curve.
    const uint lightIndex = RTXDI_ZCurveToLinearIndex(gid);

    float weight = 0.0f;
    if (lightIndex < g_RTXDIConst.m_LocalLightCount)
    {
        const uint  globalIndex = lightIndex + g_RTXDIConst.m_LocalLightFirstIndex;
        const GPULight gl       = g_Lights[globalIndex];

        // Pre-computed radiance depends on light type (same logic as RAB_LoadLightInfo).
        // For the PDF we only need relative luminance, so we use color * intensity.
        // Sky-adjusted sun radiance is expensive to compute here; the sun is an
        // infinite light anyway and is handled by a separate path (not presampled).
        const float3 radiance = gl.m_Color * gl.m_Intensity;
        weight = max(Luminance(radiance), 1e-8f); // small epsilon: never fully zero
    }

    g_PDFMip0[gid] = weight;
}

// ============================================================================
// SECTION 4B: RTXDIBuildEnvLightPDF
// Writes luminance(sky) * sin(theta) (or just sin(theta) for BRDF mode)
// into mip-0 of the environment PDF texture so RTXDI_PresampleEnvironmentMap
// can build an importance-sampled env-light RIS buffer.
// ============================================================================

[numthreads(8, 8, 1)]
void RTXDI_BuildEnvLightPDF_Main(uint2 gid : SV_DispatchThreadID)
{
    uint2 pdfSize = g_RTXDIConst.m_EnvPDFTextureSize;
    if (any(gid >= pdfSize))
        return;

    // Map texel to spherical direction (equirectangular lat-long).
    // Matching RTXDI FullSample equirectUVToDirection: 
    //   Azimuth = (uv.x + 0.25) * 2pi
    //   Elevation = (0.5 - uv.y) * pi
    float2 uv    = (float2(gid) + 0.5) / float2(pdfSize);

    float cosElevation;
    float3 dir = equirectUVToDirection(uv, cosElevation);

    // Full luminance * relative solid-angle importance sampling.
    // FullSample's getPixelWeight uses: luma * cos(elevation)
    float3 radiance = GetAtmosphereSkyRadiance(
        float3(0.0, 0.0, 0.0), dir, g_RTXDIConst.m_SunDirection, g_RTXDIConst.m_SunIntensity, true);
    float weight = max(Luminance(radiance) * cosElevation, 1e-8);

    g_EnvPDFMip0[gid] = weight;
}

// ============================================================================
// SECTION 4C: RTXDIPresampleEnvironmentMap
// Fills the env-light RIS tile segment by importance-sampling the env PDF
// mip chain (RTXDI_PresampleEnvironmentMap from PresamplingFunctions.hlsli).
// Dispatch: (DivUp(k_EnvRISTileSize,256), k_EnvRISTileCount, 1)
// ============================================================================

[numthreads(RTXDI_PRESAMPLING_GROUP_SIZE, 1, 1)]
void RTXDI_PresampleEnvironmentMap_Main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    const uint sampleInTile = GlobalIndex.x;
    const uint tileIndex    = GlobalIndex.y;

    if (sampleInTile >= g_RTXDIConst.m_EnvRISTileSize ||
        tileIndex    >= g_RTXDIConst.m_EnvRISTileCount)
        return;

    // Per-entry RNG: mix (tileIndex, sample) with frame index for temporal decorrelation.
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(
        uint2(sampleInTile + tileIndex * g_RTXDIConst.m_EnvRISTileSize,
              g_RTXDIConst.m_FrameIndex), 0u);

    RTXDI_PresampleEnvironmentMap(
        rng,
        g_RTXDI_EnvLightPDFTexture,
        g_RTXDIConst.m_EnvPDFTextureSize,
        tileIndex,
        sampleInTile,
        GetEnvLightRISBufferSegmentParams());
}

// ============================================================================
// SECTION 5: RTXDIGenerateInitialSamples
// ============================================================================

[numthreads(8, 8, 1)]
void RTXDI_GenerateInitialSamples_Main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    RTXDI_RuntimeParameters rtParams = GetRuntimeParams();
    RTXDI_ReservoirBufferParameters rbp = GetReservoirBufferParams();

    uint2 reservoirPosition = GlobalIndex;
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(reservoirPosition, rtParams.activeCheckerboardField);
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(pixelPosition >= viewportSize))
        return;

    int2  iPixel = int2(pixelPosition);

    // Initialise RNG
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 1u);
    RAB_RandomSamplerState coherentRng = RAB_InitRandomSampler(pixelPosition / 16u, 1u);

    // Fetch G-buffer surface for this pixel
    RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

    RTXDI_LightBufferParameters lbp = GetLightBufferParams();

    RTXDI_SampleParameters sampleParams = RTXDI_InitSampleParameters(
        /*numLocalLightSamples=*/  max(1u, lbp.localLightBufferRegion.numLights > 0 ? 1u : 0u),
        /*numInfiniteLightSamples=*/ (lbp.infiniteLightBufferRegion.numLights > 0 ? 1u : 0u),
        /*numEnvironmentMapSamples=*/ (g_RTXDIConst.m_EnvSamplingMode >= 1u && g_RTXDIConst.m_EnvLightPresent != 0u)
                                       ? g_RTXDIConst.m_NumEnvSamples : 0u,
        /*numBrdfSamples=*/          0u);

    // Build RIS segment parameters from the constant buffer.
    RTXDI_RISBufferSegmentParameters localRISParams = GetLocalLightRISBufferSegmentParams();
    RTXDI_RISBufferSegmentParameters envRISParams   = GetEnvLightRISBufferSegmentParams();

    RAB_LightSample selectedSample;
    RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(
        rng, coherentRng, surface, sampleParams, lbp,
        #if RTXDI_ENABLE_PRESAMPLING
            ReSTIRDI_LocalLightSamplingMode_POWER_RIS,
            localRISParams,
            envRISParams,
        #else
            ReSTIRDI_LocalLightSamplingMode_UNIFORM,
        #endif
        selectedSample);

    RTXDI_StoreDIReservoir(reservoir, rbp, reservoirPosition,
        g_RTXDIConst.m_InitialSamplingOutputBufferIndex);
}

// ============================================================================
// SECTION 6: RTXDIPresampleLights
// ============================================================================

[numthreads(RTXDI_PRESAMPLING_GROUP_SIZE, 1, 1)]
void RTXDI_PresampleLights_Main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    const uint sampleInTile = GlobalIndex.x;
    const uint tileIndex    = GlobalIndex.y;

    if (sampleInTile >= g_RTXDIConst.m_LocalRISTileSize ||
        tileIndex    >= g_RTXDIConst.m_LocalRISTileCount)
        return;

    if (g_RTXDIConst.m_LocalLightCount == 0u)
    {
        const uint risBufferPtr = g_RTXDIConst.m_LocalRISBufferOffset
                                + tileIndex * g_RTXDIConst.m_LocalRISTileSize
                                + sampleInTile;
        g_RTXDI_RISBuffer[risBufferPtr] = uint2(RTXDI_INVALID_LIGHT_INDEX, 0u);
        return;
    }

    // Per-entry RNG.  Using a 2D seed so (tile, sample) pairs don't collide.
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(
        uint2(sampleInTile + tileIndex * g_RTXDIConst.m_LocalRISTileSize,
              g_RTXDIConst.m_FrameIndex), 0u);

    // Delegate entirely to the RTXDI SDK function.  It will:
    //   - descend the PDF mip chain to pick a light proportional to its weight
    //   - call RAB_StoreCompactLightInfo to cache the light data
    //   - write the (index, invPdf) pair to RTXDI_RIS_BUFFER
    RTXDI_PresampleLocalLights(
        rng,
        g_RTXDI_LocalLightPDFTexture,               // Texture2D<float> with full mip chain
        g_RTXDIConst.m_LocalLightPDFTextureSize,    // mip-0 dimensions
        tileIndex,
        sampleInTile,
        GetLocalLightBufferRegion(),
        GetLocalLightRISSegmentParams());
}

// ============================================================================
// SECTION 7: RTXDITemporalResampling
// ============================================================================

[numthreads(8, 8, 1)]
void RTXDI_TemporalResampling_Main(
    uint2 GlobalIndex : SV_DispatchThreadID,
    uint2 LocalIndex  : SV_GroupThreadID)
{
    RTXDI_RuntimeParameters        rtParams = GetRuntimeParams();
    RTXDI_ReservoirBufferParameters rbp      = GetReservoirBufferParams();

    uint2 reservoirPosition = GlobalIndex;
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(reservoirPosition, rtParams.activeCheckerboardField);
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(pixelPosition >= viewportSize))
        return;

    int2  iPixel        = int2(pixelPosition);

    RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 2u);

    // Load current surface
    RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

    RTXDI_DIReservoir outReservoir = RTXDI_EmptyDIReservoir();
    if (RAB_IsSurfaceValid(surface))
    {
        // Load the reservoir produced by the initial sampling pass (current frame's new candidates)
        RTXDI_DIReservoir curReservoir = RTXDI_LoadDIReservoir(rbp, reservoirPosition,
            g_RTXDIConst.m_InitialSamplingOutputBufferIndex);

        // Motion vector — stored as pixel-space velocity (dx, dy).
        float3 mv = g_GBufferMV.Load(int3(iPixel, 0)).xyz;
        float3 pixelMotion = ConvertMotionVectorToPixelSpace(g_RTXDIConst.m_View, g_RTXDIConst.m_PrevView, iPixel, mv);

        RTXDI_DITemporalResamplingParameters tparams;
        tparams.screenSpaceMotion        = pixelMotion;
        tparams.sourceBufferIndex        = g_RTXDIConst.m_TemporalResamplingInputBufferIndex;
        tparams.maxHistoryLength         = 20u;
        tparams.biasCorrectionMode       = RTXDI_BIAS_CORRECTION_BASIC;
        tparams.depthThreshold           = 0.1;
        tparams.normalThreshold          = 0.5;
        tparams.enableVisibilityShortcut = false;
        tparams.enablePermutationSampling = true;
        tparams.uniformRandomNumber      = RTXDI_JenkinsHash(g_RTXDIConst.m_FrameIndex);

        RAB_LightSample selectedSample = RAB_EmptyLightSample();
        int2 temporalPixelPos;

        outReservoir = RTXDI_DITemporalResampling(
            pixelPosition, surface, curReservoir, rng,
            rtParams, rbp, tparams,
            temporalPixelPos, selectedSample);
    }

    // Boiling filter (operates within the 8×8 group)
    RTXDI_BoilingFilter(LocalIndex, RAB_GetBoilingFilterStrength(), outReservoir);

    RTXDI_StoreDIReservoir(outReservoir, rbp, reservoirPosition,
        g_RTXDIConst.m_TemporalResamplingOutputBufferIndex);
}

// ============================================================================
// SECTION 8: RTXDISpatialResampling
// ============================================================================

[numthreads(8, 8, 1)]
void RTXDI_SpatialResampling_Main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    RTXDI_RuntimeParameters         rtParams = GetRuntimeParams();
    RTXDI_ReservoirBufferParameters rbp      = GetReservoirBufferParams();

    uint2 reservoirPosition = GlobalIndex;
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(reservoirPosition, rtParams.activeCheckerboardField);
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(pixelPosition >= viewportSize))
        return;

    int2  iPixel        = int2(pixelPosition);

    // Use pass index 3 (distinct from initial=1, temporal=2) for decorrelated RNG
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 3u);

    RAB_Surface surface = RAB_GetGBufferSurface(iPixel, false);

    // Default to an empty reservoir in case the pixel is invalid
    RTXDI_DIReservoir outReservoir = RTXDI_EmptyDIReservoir();

    if (RAB_IsSurfaceValid(surface))
    {
        // Load the centre pixel's reservoir (temporal output, or initial output if temporal is off)
        RTXDI_DIReservoir centerReservoir = RTXDI_LoadDIReservoir(rbp, reservoirPosition,
            g_RTXDIConst.m_SpatialResamplingInputBufferIndex);

        RTXDI_DISpatialResamplingParameters sparams;
        sparams.sourceBufferIndex             = g_RTXDIConst.m_SpatialResamplingInputBufferIndex;
        sparams.numSamples                    = g_RTXDIConst.m_SpatialNumSamples;
        sparams.numDisocclusionBoostSamples   = 0;
        sparams.targetHistoryLength           = 20u;   // match temporal maxHistoryLength
        sparams.biasCorrectionMode            = RTXDI_BIAS_CORRECTION_BASIC;
        sparams.samplingRadius                = g_RTXDIConst.m_SpatialSamplingRadius;
        sparams.depthThreshold                = 0.1f;
        sparams.normalThreshold               = 0.5f;
        sparams.enableMaterialSimilarityTest  = true;
        sparams.discountNaiveSamples          = false;

        RAB_LightSample selectedSample = RAB_EmptyLightSample();
        outReservoir = RTXDI_DISpatialResampling(
            pixelPosition, surface, centerReservoir,
            rng, rtParams, rbp, sparams, selectedSample);
    }

    RTXDI_StoreDIReservoir(outReservoir, rbp, reservoirPosition,
        g_RTXDIConst.m_SpatialResamplingOutputBufferIndex);
}

// ============================================================================
// SECTION 9: RTXDIShadeSamples
// ============================================================================

[numthreads(8, 8, 1)]
void RTXDI_ShadeSamples_Main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    RTXDI_ReservoirBufferParameters rbp = GetReservoirBufferParams();
    RTXDI_RuntimeParameters rtParams;
    rtParams.neighborOffsetMask      = g_RTXDIConst.m_NeighborOffsetMask;
    rtParams.activeCheckerboardField = g_RTXDIConst.m_ActiveCheckerboardField;

    uint2 reservoirPosition = GlobalIndex;
    uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(reservoirPosition, rtParams.activeCheckerboardField);
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(pixelPosition >= viewportSize))
        return;

    int2  iPixel        = int2(pixelPosition);

    // Load the final shading reservoir (temporal or initial output, depending on which passes ran)
    RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(rbp, reservoirPosition,
        g_RTXDIConst.m_ShadingInputBufferIndex);

    float3 radiance = float3(0.0, 0.0, 0.0);
#if RTXDI_ENABLE_RELAX_DENOISING
    float3 diffuseRadiance  = float3(0.0, 0.0, 0.0);
    float3 specularRadiance = float3(0.0, 0.0, 0.0);
    float  hitDistance      = 1e6f;
#endif

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

            // Final-shading visibility: use GetFinalVisibility instead of the conservative
            // variant. GetFinalVisibility properly commits non-opaque triangle hits that RAB_GetConservativeVisibility deliberately
            // skips with RAY_FLAG_CULL_NON_OPAQUE. Without this, non-opaque triangles like
            // are never committed and light leaks straight through them.
            // Pass surface.normal so GetFinalVisibility can apply the same normal-bias
            // self-intersection avoidance used in CalculateRTShadow (deferred path).
            bool visible = GetFinalVisibility(g_SceneAS, surface.worldPos, RAB_GetSurfaceNormal(surface), lightSample.position);

            if (visible)
            {
                // Scale radiance by the reservoir weight and divide by the solid-angle PDF.
                // This matches FullSample's ShadingHelpers.hlsli:
                //   lightSample.radiance *= RTXDI_GetDIReservoirInvPdf(reservoir) / lightSample.solidAnglePdf;
                // The solidAnglePdf (sr^-1) converts the per-steradian radiance into the
                // correct irradiance contribution. Without this division the env-light
                // contribution is inflated by ~(W*H)/(2*pi^2) — thousands of times too bright.
                float invPdf = RTXDI_GetDIReservoirInvPdf(reservoir);
                float solidAnglePdf = max(lightSample.solidAnglePdf, 1e-10);
                float3 scaledRadiance = lightSample.radiance * (invPdf / solidAnglePdf);

                if (any(scaledRadiance > 0.0))
                {
                    float3 V = RAB_GetSurfaceViewDirection(surface);
                    float3 L = lightSample.direction;

#if RTXDI_ENABLE_RELAX_DENOISING
                    // Split BRDF into diffuse and specular so each can be denoised separately.
                    float3 diffBrdf  = RAB_EvaluateBrdfDiffuseOnly(surface, L);
                    float3 specBrdf  = RAB_EvaluateBrdfSpecularOnly(surface, L, V);
                    diffuseRadiance  = scaledRadiance * diffBrdf;
                    specularRadiance = scaledRadiance * specBrdf;
                    // Finite hit distance for point/spot lights; large sentinel for directional.
                    hitDistance      = (lightSample.distance < 1e9f) ? lightSample.distance : 1e6f;
                    radiance         = diffuseRadiance + specularRadiance;
#else
                    // Combined BRDF — RAB_EvaluateBrdf already includes NdotL.
                    float3 brdf = RAB_EvaluateBrdf(surface, L, V);
                    radiance    = scaledRadiance * brdf;
#endif
                }
            }
        }
    }

#if RTXDI_ENABLE_RELAX_DENOISING
    // Pack for RELAX front-end.  hitDistance is in world units.
    g_RTXDIDiffuseOutput[pixelPosition]  = RELAX_FrontEnd_PackRadianceAndHitDist(diffuseRadiance,  hitDistance, true);
    g_RTXDISpecularOutput[pixelPosition] = RELAX_FrontEnd_PackRadianceAndHitDist(specularRadiance, hitDistance, true);
#else
    g_RTXDIDIOutput[pixelPosition] = float4(radiance, 1.0);
#endif
}

// ============================================================================
// SECTION 10: RTXDIGenerateViewZ
// Linear view-space depth (IN_VIEWZ) required by the NRD RELAX denoiser.
// Runs full-screen — sky pixels get a large sentinel so NRD skips them.
// Only compiled when RTXDI_ENABLE_RELAX_DENOISING=1.
// ============================================================================
#if RTXDI_ENABLE_RELAX_DENOISING
[numthreads(8, 8, 1)]
void RTXDI_GenerateViewZ_Main(uint2 GlobalIndex : SV_DispatchThreadID)
{
    uint2 viewportSize = g_RTXDIConst.m_ViewportSize;
    if (any(GlobalIndex >= viewportSize))
        return;

    float depth = g_Depth.Load(int3(GlobalIndex, 0));

    float linearDepth;
    if (depth == 0.0) // Reverse-Z: 0 == far plane (sky / background)
    {
        linearDepth = g_RTXDIConst.m_View.m_MatViewToClip._43; // large positive value from projection
        // Fall back to a large sentinel the denoiser can safely ignore.
        linearDepth = 1e6f;
    }
    else
    {
        float3 worldPos  = ReconstructWorldPos(GlobalIndex, depth, g_RTXDIConst.m_View);
        float3 camPos    = float3(
            g_RTXDIConst.m_View.m_MatViewToWorld._41,
            g_RTXDIConst.m_View.m_MatViewToWorld._42,
            g_RTXDIConst.m_View.m_MatViewToWorld._43);
        float3 camForward = float3(
            g_RTXDIConst.m_View.m_MatViewToWorld._31,
            g_RTXDIConst.m_View.m_MatViewToWorld._32,
            g_RTXDIConst.m_View.m_MatViewToWorld._33);
        // Signed linear view-space Z (positive = in front of camera for LH).
        linearDepth = dot(worldPos - camPos, camForward);
    }

    g_RTXDILinearDepth[GlobalIndex] = linearDepth;
}
#endif
