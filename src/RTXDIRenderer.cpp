/*
 * RTXDIRenderer.cpp
 *
 * Implements the ReSTIR DI pipeline:
 *   1. GenerateInitialSamples  — one thread per pixel, picks candidate light
 *   2. TemporalResampling      — combines with previous-frame reservoir
 *   3. ShadeSamples            — evaluates BRDF and writes demodulated DI illumination
 *
 * Controlled by Renderer::m_EnableReSTIRDI.  When disabled, Setup() bails out early
 * and the DeferredRenderer uses its normal AccumulateDirectLighting() path.
 */

#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "NrdIntegration.h"

#include "shaders/ShaderShared.h"
#include "shaders/DIReservoirVizParameters.h"

#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>

#include <imgui.h>

RGTextureHandle g_RG_RTXDIDIOutput;       // non-denoised: demodulated diffuse illumination
RGTextureHandle g_RG_RTXDIDiffuseOutput;  // RELAX: denoised diffuse output (OUT_DIFF_RADIANCE_HITDIST)
RGTextureHandle g_RG_RTXDISpecularOutput; // non-denoised: demodulated specular, RELAX: denoised specular output
RGTextureHandle g_RG_RTXDILinearDepth;    // RELAX: linear view-space depth (IN_VIEWZ)
RGTextureHandle g_RG_RTXDIRawDiffuseOutput;
RGTextureHandle g_RG_RTXDIRawSpecularOutput;
RGBufferHandle  g_RG_RTXDILightReservoirBuffer;
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_HDRColor;

// ============================================================================

// Tile parameters for the presampling pass --------------------------------
// These are fixed at startup. Increasing tileCount improves spatial diversity;
// increasing tileSize reduces variance within a tile. Values match FullSample defaults.
static constexpr uint32_t k_RISTileSize  = 1024u;  // local-light samples per tile
static constexpr uint32_t k_RISTileCount = 128u;   // number of local-light tiles
// Environment RIS: same tile geometry as local lights.
static constexpr uint32_t k_EnvRISTileSize  = 1024u;
static constexpr uint32_t k_EnvRISTileCount = 128u;
// Environment PDF texture: square, power-of-2, equirectangular lat-long.
static constexpr uint32_t k_EnvPDFTexSize = 1024u;
// Compact buffer stride: 3 × uint4 per RIS entry (see RTXDIApplicationBridge.hlsli).
static constexpr uint32_t k_CompactSlotsPerEntry = 3u;

// ============================================================================
// ReservoirSubfieldVizMode — selects which reservoir type to visualize.
// GIReservoir and PTReservoir are stubs (not yet implemented).
// ============================================================================
enum class ReservoirSubfieldVizMode : uint32_t
{
    Off         = 0,
    DIReservoir = 1,
    GIReservoir = 2, // stub — not yet implemented
    PTReservoir = 3  // stub — not yet implemented
};

// Global state for reservoir subfield visualization
uint32_t         g_ReservoirSubfieldVizMode = (uint32_t)ReservoirSubfieldVizMode::Off;
DIReservoirField g_DIReservoirVizField      = DIReservoirField::DI_RESERVOIR_FIELD_LIGHT_DATA;

// ============================================================================

enum class ReSTIRDIQualityPreset : uint32_t
{
    Custom = 0,
    Fast = 1,
    Medium = 2,
    Unbiased = 3,
    Ultra = 4,
    Reference = 5
};

bool g_ReSTIRDI_ShowAdvancedSettings = false;
rtxdi::ReSTIRDI_ResamplingMode          g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
RTXDI_DIInitialSamplingParameters      g_ReSTIRDI_InitialSamplingParams  = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
RTXDI_DITemporalResamplingParameters   g_ReSTIRDI_TemporalResamplingParams = rtxdi::GetDefaultReSTIRDITemporalResamplingParams();
RTXDI_BoilingFilterParameters          g_ReSTIRDI_BoilingFilterParams      = rtxdi::GetDefaultReSTIRDIBoilingFilterParams();
RTXDI_DISpatialResamplingParameters    g_ReSTIRDI_SpatialResamplingParams  = rtxdi::GetDefaultReSTIRDISpatialResamplingParams();
RTXDI_ShadingParameters                g_ReSTIRDI_ShadingParams            = rtxdi::GetDefaultReSTIRDIShadingParams();
uint32_t g_ReSTIRDI_NumLocalLightUniformSamples = 8;
uint32_t g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
uint32_t g_ReSTIRDI_NumLocalLightReGIRRISSamples = 8;
ReSTIRDIQualityPreset g_ReSTIRDI_CurrentPreset = ReSTIRDIQualityPreset::Custom;

void ApplyReSTIRDIPreset(ReSTIRDIQualityPreset preset)
{
    g_ReSTIRDI_CurrentPreset = preset;

    switch (preset)
    {
    case ReSTIRDIQualityPreset::Fast:
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
        g_ReSTIRDI_NumLocalLightUniformSamples = 4;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 4;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 4;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 4;
        g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 0;
        g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples = 1;
        g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut = 1u;
        g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = 1u;
        g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength = 0.2f;
        g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Off;
        g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Off;
        g_ReSTIRDI_SpatialResamplingParams.numSamples = 1;
        g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 2;
        g_ReSTIRDI_ShadingParams.reuseFinalVisibility = 1u;
        break;

    case ReSTIRDIQualityPreset::Medium:
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        g_ReSTIRDI_NumLocalLightUniformSamples = 8;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
        g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples = 2;
        g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut = 1u;
        g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = 1u;
        g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength = 0.2f;
        g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
        g_ReSTIRDI_SpatialResamplingParams.numSamples = 1;
        g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 8;
        g_ReSTIRDI_ShadingParams.reuseFinalVisibility = 1u;
        break;

    case ReSTIRDIQualityPreset::Unbiased:
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        g_ReSTIRDI_NumLocalLightUniformSamples = 8;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 16;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
        g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples = 2;
        g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut = 0u;
        g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = 0u;
        g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength = 0.0f;
        g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.numSamples = 1;
        g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 8;
        g_ReSTIRDI_ShadingParams.reuseFinalVisibility = 0u;
        break;

    case ReSTIRDIQualityPreset::Ultra:
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        g_ReSTIRDI_NumLocalLightUniformSamples = 16;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 16;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 16;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 16;
        g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
        g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples = 16;
        g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut = 0u;
        g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = 0u;
        g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength = 0.0f;
        g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.numSamples = 4;
        g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 16;
        g_ReSTIRDI_ShadingParams.reuseFinalVisibility = 0u;
        break;

    case ReSTIRDIQualityPreset::Reference:
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::None;
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        g_ReSTIRDI_NumLocalLightUniformSamples = 16;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 16;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 0;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 16;
        g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
        g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples = 16;
        g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = 0u;
        g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength = 0.0f;
        break;

        // my own settings
    case ReSTIRDIQualityPreset::Custom:
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
        g_ReSTIRDI_NumLocalLightUniformSamples = 8;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
        g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples = 2;
        g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut = 1u;
        g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = 1u;
        g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength = 0.2f;
        g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_TemporalResamplingParams.enablePermutationSampling = 0u; // disabling this somehow increases image quality?
        g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.numSamples = 1;
        g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 8;
        g_ReSTIRDI_ShadingParams.reuseFinalVisibility = 1u;
    default:
        break;
    }
}

void RTXDIIMGUISettings()
{
    Renderer* renderer = Renderer::GetInstance();

    ImGui::Indent();

    // ---- Preset Selection -----------------------------------------------
    ImGui::PushItemWidth(200.f);
    if (ImGui::Combo("Preset", (int*)&g_ReSTIRDI_CurrentPreset,
        "(Custom)\0Fast\0Medium\0Unbiased\0Ultra\0Reference\0"))
    {
        ApplyReSTIRDIPreset(g_ReSTIRDI_CurrentPreset);
    }
    ImGui::PopItemWidth();

    // ---- Resampling mode -------------------------------------------------------
    ImGui::PushItemWidth(200.f);
    if (g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::FusedSpatiotemporal)
    {
        // Fused spatiotemporal is not implemented in this renderer path.
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
    }
    ImGui::Combo("Resampling Mode", (int*)&g_ReSTIRDI_ResamplingMode,
        "None\0"
        "Temporal\0"
        "Spatial\0"
        "Temporal + Spatial\0");
    ImGui::PopItemWidth();

    // ---- RELAX Denoising toggle -------------------------------------------
    ImGui::Separator();
    ImGui::Checkbox("Enable RELAX Denoising (NRD)", &renderer->m_EnableReSTIRDIRelaxDenoising);
    if (renderer->m_EnableReSTIRDIRelaxDenoising)
        ImGui::TextDisabled("  Diffuse + specular denoised separately via RELAX D+S.");

    if (renderer->m_EnableReSTIRDIRelaxDenoising)
    {
        ImGui::SliderFloat("Noise Mix-in", &renderer->m_ReSTIRDINoiseMix, 0.f, 1.f);
        ImGui::SliderFloat("Noise Clamp Low", &renderer->m_ReSTIRDINoiseClampLow, 0.f, 1.f);
        ImGui::SliderFloat("Noise Clamp High", &renderer->m_ReSTIRDINoiseClampHigh, 1.f, 4.f);
    }
    ImGui::Separator();

    ImGui::Checkbox("Show Advanced Settings", &g_ReSTIRDI_ShowAdvancedSettings);
    ImGui::Separator();

    // ---- Initial Sampling -----------------------------------------------------
    if (ImGui::TreeNode("Initial Sampling"))
    {
        if (ImGui::TreeNode("Local Light Sampling"))
        {
            int* localLightMode = (int*)&g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode;

            ImGui::RadioButton("Uniform Sampling##llMode", localLightMode, (int)ReSTIRDI_LocalLightSamplingMode::Uniform);
            ImGui::SliderInt("Uniform Samples", (int*)&g_ReSTIRDI_NumLocalLightUniformSamples, 0, 32);

            ImGui::RadioButton("Power RIS##llMode", localLightMode, (int)ReSTIRDI_LocalLightSamplingMode::Power_RIS);
            ImGui::SliderInt("Power RIS Samples", (int*)&g_ReSTIRDI_NumLocalLightPowerRISSamples, 0, 32);

            ImGui::RadioButton("ReGIR RIS##llMode", localLightMode, (int)ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS);
            ImGui::SliderInt("ReGIR RIS Samples", (int*)&g_ReSTIRDI_NumLocalLightReGIRRISSamples, 0, 32);

            // Keep numLocalLightSamples in sync with the active mode
            switch (g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode)
            {
            case ReSTIRDI_LocalLightSamplingMode::Uniform:
                g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = g_ReSTIRDI_NumLocalLightUniformSamples;
                break;
            case ReSTIRDI_LocalLightSamplingMode::Power_RIS:
                g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = g_ReSTIRDI_NumLocalLightPowerRISSamples;
                break;
            case ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS:
                g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = g_ReSTIRDI_NumLocalLightReGIRRISSamples;
                break;
            default: break;
            }

            ImGui::TreePop();
        }

        ImGui::SliderInt("Initial BRDF Samples",
            (int*)&g_ReSTIRDI_InitialSamplingParams.numBrdfSamples, 0, 8);
        ImGui::SliderInt("Initial Infinite Light Samples",
            (int*)&g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples, 0, 32);
        ImGui::SliderInt("Initial Environment Samples",
            (int*)&g_ReSTIRDI_InitialSamplingParams.numEnvironmentSamples, 0, 32);

        bool enableInitVis = g_ReSTIRDI_InitialSamplingParams.enableInitialVisibility != 0;
        if (ImGui::Checkbox("Enable Initial Visibility", &enableInitVis))
            g_ReSTIRDI_InitialSamplingParams.enableInitialVisibility = enableInitVis ? 1u : 0u;

        ImGui::SliderFloat("BRDF Sample Cutoff",
            &g_ReSTIRDI_InitialSamplingParams.brdfCutoff, 0.0f, 0.1f);

        ImGui::TreePop();
    }

    // ---- Temporal Resampling --------------------------------------------------
    const bool hasTemporalMode =
        g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::Temporal ||
        g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
    if (hasTemporalMode && ImGui::TreeNode("Temporal Resampling"))
    {
        bool enablePermutation = g_ReSTIRDI_TemporalResamplingParams.enablePermutationSampling != 0;
        if (ImGui::Checkbox("Enable Permutation Sampling", &enablePermutation))
            g_ReSTIRDI_TemporalResamplingParams.enablePermutationSampling = enablePermutation ? 1u : 0u;

        ImGui::Combo("Temporal Bias Correction",
            (int*)&g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode,
            "Off\0Basic\0Pairwise\0Ray Traced\0");

        if (g_ReSTIRDI_ShowAdvancedSettings)
        {
            ImGui::SliderFloat("Temporal Depth Threshold",
                &g_ReSTIRDI_TemporalResamplingParams.depthThreshold, 0.0f, 1.0f);
            ImGui::SliderFloat("Temporal Normal Threshold",
                &g_ReSTIRDI_TemporalResamplingParams.normalThreshold, 0.0f, 1.0f);
            ImGui::SliderFloat("Permutation Sampling Threshold",
                &g_ReSTIRDI_TemporalResamplingParams.permutationSamplingThreshold, 0.8f, 1.0f);
        }

        ImGui::SliderInt("Max History Length",
            (int*)&g_ReSTIRDI_TemporalResamplingParams.maxHistoryLength, 1, 100);

        bool boilingEnabled = g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter != 0;
        if (ImGui::Checkbox("##enableBoilingFilter", &boilingEnabled))
            g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = boilingEnabled ? 1u : 0u;
        ImGui::SameLine();
        ImGui::PushItemWidth(90.f);
        ImGui::SliderFloat("Boiling Filter",
            &g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength, 0.0f, 1.0f);
        ImGui::PopItemWidth();

        ImGui::TreePop();
    }

    // ---- Spatial Resampling ---------------------------------------------------
    const bool hasSpatialMode =
        g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::Spatial ||
        g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
    if (hasSpatialMode && ImGui::TreeNode("Spatial Resampling"))
    {
        if (g_ReSTIRDI_ResamplingMode != rtxdi::ReSTIRDI_ResamplingMode::FusedSpatiotemporal)
        {
            ImGui::Combo("Spatial Bias Correction",
                (int*)&g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode,
                "Off\0Basic\0Pairwise\0Ray Traced\0");
        }

        ImGui::SliderInt("Spatial Samples",
            (int*)&g_ReSTIRDI_SpatialResamplingParams.numSamples, 1, 32);

        if (g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial ||
            g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial)
        {
            ImGui::SliderInt("Disocclusion Boost Samples",
                (int*)&g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples, 1, 32);
        }

        ImGui::SliderFloat("Spatial Sampling Radius",
            &g_ReSTIRDI_SpatialResamplingParams.samplingRadius, 1.0f, 32.0f);

        if (g_ReSTIRDI_ShowAdvancedSettings &&
            g_ReSTIRDI_ResamplingMode != rtxdi::ReSTIRDI_ResamplingMode::FusedSpatiotemporal)
        {
            ImGui::SliderFloat("Spatial Depth Threshold",
                &g_ReSTIRDI_SpatialResamplingParams.depthThreshold, 0.0f, 1.0f);
            ImGui::SliderFloat("Spatial Normal Threshold",
                &g_ReSTIRDI_SpatialResamplingParams.normalThreshold, 0.0f, 1.0f);
            bool discountNaive = g_ReSTIRDI_SpatialResamplingParams.discountNaiveSamples != 0;
            if (ImGui::Checkbox("Discount Naive Samples", &discountNaive))
                g_ReSTIRDI_SpatialResamplingParams.discountNaiveSamples = discountNaive ? 1u : 0u;
        }

        ImGui::TreePop();
    }

    // ---- Final Shading --------------------------------------------------------
    if (ImGui::TreeNode("Final Shading"))
    {
        bool enableFinalVis = g_ReSTIRDI_ShadingParams.enableFinalVisibility != 0;
        if (ImGui::Checkbox("Enable Final Visibility", &enableFinalVis))
            g_ReSTIRDI_ShadingParams.enableFinalVisibility = enableFinalVis ? 1u : 0u;

        bool discardInvisible = g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut != 0;
        if (ImGui::Checkbox("Discard Invisible Samples", &discardInvisible))
            g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut = discardInvisible ? 1u : 0u;

        bool reuseFinalVis = g_ReSTIRDI_ShadingParams.reuseFinalVisibility != 0;
        if (ImGui::Checkbox("Reuse Final Visibility", &reuseFinalVis))
            g_ReSTIRDI_ShadingParams.reuseFinalVisibility = reuseFinalVis ? 1u : 0u;

        if (reuseFinalVis && g_ReSTIRDI_ShowAdvancedSettings)
        {
            ImGui::SliderFloat("Final Visibility Max Distance",
                &g_ReSTIRDI_ShadingParams.finalVisibilityMaxDistance, 0.0f, 32.0f);
            ImGui::SliderInt("Final Visibility Max Age",
                (int*)&g_ReSTIRDI_ShadingParams.finalVisibilityMaxAge, 0, 16);
        }

        ImGui::TreePop();
    }

    // ---- Reservoir Subfield Visualization ----------------------------------------
    ImGui::Separator();
    if (ImGui::TreeNode("Reservoir Subfield Viz"))
    {
        ImGui::PushItemWidth(220.f);
        ImGui::Combo("Reservoir Type", (int*)&g_ReservoirSubfieldVizMode,
            "Off\0"
            "DI Reservoirs\0"
            "GI Reservoirs (N/A)\0"
            "PT Reservoirs (N/A)\0");
        ImGui::PopItemWidth();

        if (g_ReservoirSubfieldVizMode != (uint32_t)ReservoirSubfieldVizMode::Off)
        {
            ImGui::TextDisabled("  Note: RELAX denoising output is bypassed while viz mode is active.");
        }

        switch ((ReservoirSubfieldVizMode)g_ReservoirSubfieldVizMode)
        {
        case ReservoirSubfieldVizMode::DIReservoir:
            ImGui::PushItemWidth(220.f);
            ImGui::Combo("DI Reservoir Field", (int*)&g_DIReservoirVizField,
                "Light data\0"
                "UV data\0"
                "Target Pdf\0"
                "M\0"
                "Packed visibility\0"
                "Spatial distance\0"
                "Age\0"
                "Canonical weight\0");
            ImGui::PopItemWidth();
            break;
        case ReservoirSubfieldVizMode::GIReservoir:
            ImGui::TextDisabled("  GI Reservoir visualization is not yet implemented.");
            break;
        case ReservoirSubfieldVizMode::PTReservoir:
            ImGui::TextDisabled("  PT Reservoir visualization is not yet implemented.");
            break;
        default:
            break;
        }

        ImGui::TreePop();
    }

    ImGui::Unindent();
}

class RTXDIRenderer : public IRenderer
{
public:
    // ------------------------------------------------------------------
    // Persistent GPU buffers (survive across frames)
    // ------------------------------------------------------------------
    RGBufferHandle m_RG_NeighborOffsetsBuffer;
    RGBufferHandle m_RG_RISBuffer;
    // Light reservoir buffer — stored via global g_RG_RTXDILightReservoirBuffer
    // so the RTXDIVisualizationRenderer can sample reservoirs after shading.

    // Track if neighbor offsets buffer was newly allocated (needs initial fill)
    bool m_NeighborOffsetsBufferIsNew = false;

    // Compact light info buffer written by the presample pass and read by
    // the initial sampling pass (via RAB_LoadCompactLightInfo).
    // Layout: k_CompactSlotsPerEntry × uint4 per RIS tile entry.
    RGBufferHandle m_RG_RISLightDataBuffer;

    // Local-light PDF texture for power-importance presampling.
    // Square, power-of-2, R32_FLOAT with a full mip chain.
    // Mip 0 has one texel per local light (Z-curve mapped), written every frame
    // by RTXDIBuildLocalLightPDF; the mip chain is then regenerated.
    // RTXDI_PresampleLocalLights consumes the full chain for power-weighted
    // tile filling — this is the actual mechanism behind Power-RIS.
    RGTextureHandle      m_RG_LocalLightPDFTexture;
    uint32_t             m_PDFTexSize  = 0; // side length of the square texture
    uint32_t             m_PDFMipCount = 0; // number of mip levels

    // SPD atomic counter for mip generation
    RGBufferHandle       m_RG_SPDAtomicCounter;

    // Environment-light PDF texture for sky importance sampling.
    // Square (k_EnvPDFTexSize × k_EnvPDFTexSize), R32_FLOAT, full mip chain.
    // Mip-0 is rebuilt every frame by RTXDI_BuildEnvLightPDF_Main;
    // the mip chain feeds RTXDI_PresampleEnvironmentMap which fills the env RIS segment.
    RGTextureHandle      m_RG_EnvLightPDFTexture;
    uint32_t             m_EnvPDFMipCount = 0;

    // SPD atomic counter for env PDF mip generation (separate from local PDF counter).
    RGBufferHandle       m_RG_SPDEnvAtomicCounter;

    // ------------------------------------------------------------------
    // Persistent textures (G-buffer history for previous frame)
    // ------------------------------------------------------------------
    RGTextureHandle m_DepthHistory;
    RGTextureHandle m_GBufferAlbedoHistory;
    RGTextureHandle m_GbufferNormalsHistory;
    RGTextureHandle m_GBufferORMHistory;

    // Track if history textures are newly created in current frame
    bool m_AlbedoHistoryIsNew = false;
    bool m_ORMHistoryIsNew    = false;
    bool m_DepthHistoryIsNew  = false;
    bool m_NormalsHistoryIsNew = false;

    // ------------------------------------------------------------------
    // Persistent ray tracing acceleration structures
    // ------------------------------------------------------------------
    nvrhi::rt::AccelStructHandle m_TLASHistory;

    // ------------------------------------------------------------------
    // RTXDI context (owns frame-index tracking and buffer-index bookkeeping)
    // ------------------------------------------------------------------
    std::unique_ptr<rtxdi::ReSTIRDIContext> m_Context;

    std::unique_ptr<NrdIntegration> m_NrdIntegration;
    nrd::RelaxSettings m_NRDRelaxSettings;

    void CreateRTXDIContext()
    {
        Renderer* renderer = Renderer::GetInstance();

        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Create the ReSTIRDI context
        rtxdi::ReSTIRDIStaticParameters staticParams;
        staticParams.RenderWidth = width;
        staticParams.RenderHeight = height;
        m_Context = std::make_unique<rtxdi::ReSTIRDIContext>(staticParams);
    }

    void Initialize() override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Initialize ReSTIR DI parameter structs to library defaults
        g_ReSTIRDI_InitialSamplingParams = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
        g_ReSTIRDI_TemporalResamplingParams = rtxdi::GetDefaultReSTIRDITemporalResamplingParams();
        g_ReSTIRDI_BoilingFilterParams = rtxdi::GetDefaultReSTIRDIBoilingFilterParams();
        g_ReSTIRDI_SpatialResamplingParams = rtxdi::GetDefaultReSTIRDISpatialResamplingParams();
        g_ReSTIRDI_ShadingParams = rtxdi::GetDefaultReSTIRDIShadingParams();

        // Sync per-mode sample counts with initial sampling params default
        g_ReSTIRDI_NumLocalLightUniformSamples = g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples;

        ApplyReSTIRDIPreset(g_ReSTIRDI_CurrentPreset);

        CreateRTXDIContext();

        m_NrdIntegration = std::make_unique<NrdIntegration>(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR);
        m_NrdIntegration->Initialize();

        // default settings: follow RTXDI sample
        m_NRDRelaxSettings.diffuseMaxFastAccumulatedFrameNum = 1;
        m_NRDRelaxSettings.specularMaxFastAccumulatedFrameNum = 1;
        m_NRDRelaxSettings.diffusePhiLuminance = 1.0f;
        m_NRDRelaxSettings.spatialVarianceEstimationHistoryThreshold = 1;
        m_NRDRelaxSettings.enableAntiFirefly = true;
        m_NRDRelaxSettings.diffusePrepassBlurRadius = 30.0f;
        m_NRDRelaxSettings.specularPrepassBlurRadius = 30.0f;
    }

    void PostSceneLoad() override
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;

        // ---- TLAS History (for temporal effects) -------------------
        const uint32_t maxInstances = static_cast<uint32_t>(renderer->m_Scene.m_InstanceData.size());

        nvrhi::rt::AccelStructDesc tlasHistoryDesc;
        tlasHistoryDesc.topLevelMaxInstances = maxInstances;
        tlasHistoryDesc.debugName = "RTXDI_TLAS_History";
        tlasHistoryDesc.isTopLevel = true;
        m_TLASHistory = device->createAccelStruct(tlasHistoryDesc);

        nvrhi::CommandListHandle cl = renderer->AcquireCommandList();
        ScopedCommandList scopeCl{ cl, "RTXDI::Initialize" };
        scopeCl->buildTopLevelAccelStructFromBuffer(m_TLASHistory, renderer->m_Scene.m_RTInstanceDescBuffer, 0, (uint32_t)renderer->m_Scene.m_RTInstanceDescs.size());
    }

    // ------------------------------------------------------------------
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Only run in normal / IBL mode; skip for path tracer
        if (renderer->m_Mode == RenderingMode::ReferencePathTracer)
            return false;

        // If the user turned RTXDI off, we want the DeferredRenderer to take
        // the classic path — don't participate in the render graph at all.
        if (!renderer->m_EnableReSTIRDI)
            return false;

        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;
        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // RIS buffer capacity covers local-light tiles and env-light tiles.
        // The env segment always reserves k_EnvRISTileSize × k_EnvRISTileCount
        // even when env sampling is disabled, so the binding stays stable.
        const uint32_t k_LocalRISEntries = k_RISTileSize  * k_RISTileCount;
        const uint32_t k_EnvRISEntries   = k_EnvRISTileSize * k_EnvRISTileCount;
        const uint32_t totalRISEntries   = k_LocalRISEntries + k_EnvRISEntries;

        // ------------------------------------------------------------------
        // RELAX denoising output textures (only when denoising is enabled)
        // ------------------------------------------------------------------
        if (renderer->m_EnableReSTIRDIRelaxDenoising)
        {
            m_NrdIntegration->Setup(renderGraph);

            // Raw RELAX inputs from RTXDI front-end packing.
            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width  = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = nvrhi::Format::RGBA16_FLOAT;
                desc.m_NvrhiDesc.isUAV  = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName    = "RTXDIRawDiffuseOutput";
                renderGraph.DeclareTexture(desc, g_RG_RTXDIRawDiffuseOutput);
            }
            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width  = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = nvrhi::Format::RGBA16_FLOAT;
                desc.m_NvrhiDesc.isUAV  = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName    = "RTXDIRawSpecularOutput";
                renderGraph.DeclareTexture(desc, g_RG_RTXDIRawSpecularOutput);
            }

            // Denoised RELAX outputs consumed by DeferredRenderer.
            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width  = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = nvrhi::Format::RGBA16_FLOAT;
                desc.m_NvrhiDesc.isUAV  = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName    = "RTXDIDiffuseOutput";
                renderGraph.DeclareTexture(desc, g_RG_RTXDIDiffuseOutput);
            }
            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width  = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = nvrhi::Format::RGBA16_FLOAT;
                desc.m_NvrhiDesc.isUAV  = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName    = "RTXDISpecularOutput";
                renderGraph.DeclareTexture(desc, g_RG_RTXDISpecularOutput);
            }
            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width  = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
                desc.m_NvrhiDesc.isUAV  = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName    = "RTXDILinearDepth";
                renderGraph.DeclareTexture(desc, g_RG_RTXDILinearDepth);
            }
        }
        else
        {
            // ------------------------------------------------------------------
            // Declare / retrieve non-denoised DI illumination textures
            // ------------------------------------------------------------------
            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT; // R11G11B10_FLOAT
                desc.m_NvrhiDesc.isUAV = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName = "RTXDIDiffuseOutput";
                renderGraph.DeclareTexture(desc, g_RG_RTXDIDIOutput);
            }

            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT; // R11G11B10_FLOAT
                desc.m_NvrhiDesc.isUAV = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName = "RTXDISpecularOutput";
                renderGraph.DeclareTexture(desc, g_RG_RTXDISpecularOutput);
            }
        }

        // ------------------------------------------------------------------
        // Declare / retrieve persistent G-buffer history textures
        // ------------------------------------------------------------------
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::GBUFFER_ALBEDO_FORMAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName = "GBufferAlbedoHistory";
            m_AlbedoHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GBufferAlbedoHistory);
        }

        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width  = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::GBUFFER_ORM_FORMAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName = "GBufferORMHistory";
            m_ORMHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GBufferORMHistory);
        }

        // Previous-frame depth history (copy of depth buffer: same format, D24S8).
        // isRenderTarget=true so NVRHI creates it with ALLOW_DEPTH_STENCIL, which is
        // required to copy from the main depth buffer (also ALLOW_DEPTH_STENCIL) and
        // to create a proper depth-component SRV.
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width        = width;
            desc.m_NvrhiDesc.height       = height;
            desc.m_NvrhiDesc.format       = Renderer::DEPTH_FORMAT;
            desc.m_NvrhiDesc.isRenderTarget = true; // needed for ALLOW_DEPTH_STENCIL flag
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName    = "DepthHistory";
            m_DepthHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_DepthHistory);
        }

        // Previous-frame G-buffer normals history.
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width       = width;
            desc.m_NvrhiDesc.height      = height;
            desc.m_NvrhiDesc.format      = Renderer::GBUFFER_NORMALS_FORMAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName   = "GBufferNormalsHistory";
            m_NormalsHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GbufferNormalsHistory);
        }

        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize = m_Context->GetStaticParameters().NeighborOffsetCount * 2;
            bd.m_NvrhiDesc.format   = nvrhi::Format::RG8_SNORM; // 2 × int8 normalized [-1,1] per entry
            bd.m_NvrhiDesc.canHaveTypedViews = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            bd.m_NvrhiDesc.debugName    = "RTXDI_NeighborOffsets";
            m_NeighborOffsetsBufferIsNew = renderGraph.DeclarePersistentBuffer(bd, m_RG_NeighborOffsetsBuffer);
        }

        // ---- RIS buffer (transient) -----------------------------------------------
        // Sized for all presampled local light tiles.
        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(totalRISEntries) * sizeof(uint32_t) * 2;
            bd.m_NvrhiDesc.structStride = sizeof(uint32_t) * 2;
            bd.m_NvrhiDesc.format       = nvrhi::Format::RG32_UINT;
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName    = "RTXDI_RISBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_RISBuffer);
        }

        // ---- Light reservoir buffer (transient) --------------------------------
        {
            RTXDI_ReservoirBufferParameters rbp = m_Context->GetReservoirBufferParameters();
            const uint32_t totalReservoirs = rbp.reservoirArrayPitch * rtxdi::c_NumReSTIRDIReservoirBuffers;

            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize = static_cast<uint64_t>(totalReservoirs) * sizeof(RTXDI_PackedDIReservoir);
            bd.m_NvrhiDesc.structStride = sizeof(RTXDI_PackedDIReservoir);
            bd.m_NvrhiDesc.canHaveUAVs = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName = "RTXDI_LightReservoirBuffer";
            renderGraph.DeclareBuffer(bd, g_RG_RTXDILightReservoirBuffer);
        }

        // ---- Compact light data buffer --------------------------------
        // Stores k_CompactSlotsPerEntry × uint4 per RIS tile entry.
        // Written by the presample pass, read by the initial sampling pass.
        // Must be updated every frame because light properties can change.
        {
            const uint64_t numUint4s = static_cast<uint64_t>(totalRISEntries) * k_CompactSlotsPerEntry;
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize = numUint4s * sizeof(uint32_t) * 4; // each uint4 = 16 bytes
            bd.m_NvrhiDesc.structStride = sizeof(uint32_t) * 4;             // stride = 1 uint4
            bd.m_NvrhiDesc.canHaveUAVs = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.keepInitialState = true;
            bd.m_NvrhiDesc.debugName = "RTXDI_RISLightDataBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_RISLightDataBuffer);
        }

        // ---- Local-light PDF texture -----------------------------------------------
        // Sized so the Z-curve can address every local light in the scene.
        // Light count is fixed post-load, so this is created once.
        {
            // Local lights = all lights except index 0 (the sun/directional).
            const uint32_t localLightCount = (renderer->m_Scene.m_LightCount > 1u)
                ? renderer->m_Scene.m_LightCount - 1u
                : 0u;

            // Find smallest power-of-2 S such that S*S >= localLightCount.
            m_PDFTexSize = 1u;
            while (m_PDFTexSize* m_PDFTexSize < localLightCount)
                m_PDFTexSize <<= 1u;

            // Mip count: from full-res down to 1×1.
            m_PDFMipCount = 1u;
            for (uint32_t s = m_PDFTexSize; s > 1u; s >>= 1u)
                ++m_PDFMipCount;

            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = m_PDFTexSize;
            desc.m_NvrhiDesc.height = m_PDFTexSize;
            desc.m_NvrhiDesc.mipLevels = m_PDFMipCount;
            desc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.debugName = "RTXDI_LocalLightPDFTexture";
            renderGraph.DeclareTexture(desc, m_RG_LocalLightPDFTexture);
        }

        // ---- SPD atomic counter for mip generation ------
        {
            renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc("RTXDI PDF Mip SPD Atomic Counter"), m_RG_SPDAtomicCounter);
            renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);
        }

        // ---- Environment-light PDF texture (always declared, rebuilt when env enabled) ---
        // Square, power-of-2, R32_FLOAT with a full mip chain.
        // Mip-0 written each frame by RTXDI_BuildEnvLightPDF_Main;
        // the full chain feeds RTXDI_PresampleEnvironmentMap.
        {
            m_EnvPDFMipCount = 1u;
            for (uint32_t s = k_EnvPDFTexSize; s > 1u; s >>= 1u)
                ++m_EnvPDFMipCount;

            RGTextureDesc desc;
            desc.m_NvrhiDesc.width     = k_EnvPDFTexSize;
            desc.m_NvrhiDesc.height    = k_EnvPDFTexSize;
            desc.m_NvrhiDesc.mipLevels = m_EnvPDFMipCount;
            desc.m_NvrhiDesc.format    = nvrhi::Format::R32_FLOAT;
            desc.m_NvrhiDesc.isUAV     = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.debugName = "RTXDI_EnvLightPDFTexture";
            renderGraph.DeclareTexture(desc, m_RG_EnvLightPDFTexture);
        }

        // ---- SPD atomic counter for env PDF mip generation ----
        {
            renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc("RTXDI Env PDF SPD Counter"), m_RG_SPDEnvAtomicCounter);
            renderGraph.WriteBuffer(m_RG_SPDEnvAtomicCounter);
        }

        // Register accesses
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferAlbedo);
        renderGraph.ReadTexture(g_RG_GBufferNormals);
        renderGraph.ReadTexture(g_RG_GBufferORM);
        renderGraph.ReadTexture(g_RG_GBufferMotionVectors);

        return true;
    }

    // ------------------------------------------------------------------
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;

        // Advance the frame index so the context produces fresh buffer indices
        m_Context->SetFrameIndex(renderer->m_FrameNumber);

        // Apply all ReSTIR DI parameters from renderer settings to the context.
        // Fused spatiotemporal mode is not implemented in this shader path, so map
        // it to TemporalAndSpatial to keep runtime/UI and dispatch behavior consistent.
        const rtxdi::ReSTIRDI_ResamplingMode effectiveResamplingMode =
            (g_ReSTIRDI_ResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::FusedSpatiotemporal)
            ? rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial
            : g_ReSTIRDI_ResamplingMode;

        m_Context->SetResamplingMode(effectiveResamplingMode);
        m_Context->SetInitialSamplingParameters(g_ReSTIRDI_InitialSamplingParams);
        m_Context->SetTemporalResamplingParameters(g_ReSTIRDI_TemporalResamplingParams);
        m_Context->SetBoilingFilterParameters(g_ReSTIRDI_BoilingFilterParams);
        m_Context->SetSpatialResamplingParameters(g_ReSTIRDI_SpatialResamplingParams);
        m_Context->SetShadingParameters(g_ReSTIRDI_ShadingParams);

        // ------------------------------------------------------------------
        // Build RTXDIConstants from context accessors
        // ------------------------------------------------------------------
        const RTXDI_ReservoirBufferParameters rbp = m_Context->GetReservoirBufferParameters();
        const RTXDI_RuntimeParameters         rtp = m_Context->GetRuntimeParams();
        const RTXDI_DIBufferIndices          bix = m_Context->GetBufferIndices();
        const RTXDI_LightBufferParameters     lbp = BuildLightBufferParams(renderer);

        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        RTXDIConstants cb{};
        cb.m_ViewportSize                        = { width, height };
        cb.m_FrameIndex                          = renderer->m_FrameNumber;
        cb.m_LightCount                          = renderer->m_Scene.m_LightCount;

        // RTXDI_RuntimeParameters
        cb.m_NeighborOffsetMask                  = rtp.neighborOffsetMask;
        cb.m_ActiveCheckerboardField             = rtp.activeCheckerboardField;

        // Local light region
        cb.m_LocalLightFirstIndex                = lbp.localLightBufferRegion.firstLightIndex;
        cb.m_LocalLightCount                     = lbp.localLightBufferRegion.numLights;

        // Infinite light region (sun = index 0 maps to infinite)
        cb.m_InfiniteLightFirstIndex             = lbp.infiniteLightBufferRegion.firstLightIndex;
        cb.m_InfiniteLightCount                  = lbp.infiniteLightBufferRegion.numLights;

        // Environment light
        cb.m_EnvLightPresent                     = lbp.environmentLightParams.lightPresent;
        cb.m_EnvLightIndex                       = lbp.environmentLightParams.lightIndex;

        // Reservoir buffer params
        cb.m_ReservoirBlockRowPitch              = rbp.reservoirBlockRowPitch;
        cb.m_ReservoirArrayPitch                 = rbp.reservoirArrayPitch;

        // Buffer indices
        cb.m_InitialSamplingOutputBufferIndex    = bix.initialSamplingOutputBufferIndex;
        cb.m_TemporalResamplingInputBufferIndex  = bix.temporalResamplingInputBufferIndex;
        cb.m_TemporalResamplingOutputBufferIndex = bix.temporalResamplingOutputBufferIndex;
        cb.m_SpatialResamplingInputBufferIndex   = bix.spatialResamplingInputBufferIndex;
        cb.m_SpatialResamplingOutputBufferIndex  = bix.spatialResamplingOutputBufferIndex;
        cb.m_ShadingInputBufferIndex             = bix.shadingInputBufferIndex;
        cb.m_EnableSky                           = renderer->m_EnableSky ? 1u : 0u;

        // ---- Local-light PDF texture size ----
        cb.m_LocalLightPDFTextureSize = { m_PDFTexSize, m_PDFTexSize };

        // ---- RIS buffer segment parameters for local-light presampling ----
        cb.m_LocalRISBufferOffset = 0u;
        cb.m_LocalRISTileSize     = k_RISTileSize;
        cb.m_LocalRISTileCount    = k_RISTileCount;

        // ---- Environment-light sampling ----
        cb.m_EnvPDFTextureSize = { k_EnvPDFTexSize, k_EnvPDFTexSize };
        cb.m_EnvSamplingMode   = renderer->m_EnableSky ? 2u : 0u;
        cb.m_NumEnvSamples          = renderer->m_EnableSky ? g_ReSTIRDI_InitialSamplingParams.numEnvironmentSamples : 0u;
        cb.m_NumLocalLightSamples   = g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples;
        cb.m_NumInfiniteLightSamples= g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples;
        cb.m_NumBrdfSamples         = g_ReSTIRDI_InitialSamplingParams.numBrdfSamples;
        cb.m_LocalLightSamplingMode = static_cast<uint32_t>(g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode);
        cb.m_BrdfCutoff             = g_ReSTIRDI_InitialSamplingParams.brdfCutoff;

        if (renderer->m_EnableSky)
        {
            // Env RIS segment immediately follows the local-light RIS segment.
            cb.m_EnvRISBufferOffset = k_RISTileSize * k_RISTileCount;
            cb.m_EnvRISTileSize     = k_EnvRISTileSize;
            cb.m_EnvRISTileCount    = k_EnvRISTileCount;
        }
        else
        {
            // Leave env RIS slot empty — initial sampling sees tileSize=0 and skips env.
            cb.m_EnvRISBufferOffset = 0u;
            cb.m_EnvRISTileSize     = 0u;
            cb.m_EnvRISTileCount    = 0u;
        }

        // Spatial resampling parameters
        const RTXDI_DISpatialResamplingParameters spatialParams = m_Context->GetSpatialResamplingParameters();
        cb.m_SpatialNumSamples        = spatialParams.numSamples;
        cb.m_SpatialSamplingRadius    = spatialParams.samplingRadius;

        // Temporal resampling parameters — forwarded from the ReSTIRDI context
        // (the context already calls JenkinsHash(frameIndex) to compute uniformRandomNumber)
        const RTXDI_DITemporalResamplingParameters temporalParams = m_Context->GetTemporalResamplingParameters();
        const RTXDI_BoilingFilterParameters boilingParams = m_Context->GetBoilingFilterParameters();
        cb.m_TemporalMaxHistoryLength          = temporalParams.maxHistoryLength;
        cb.m_TemporalBiasCorrectionMode        = static_cast<uint32_t>(temporalParams.biasCorrectionMode);
        cb.m_TemporalDepthThreshold            = temporalParams.depthThreshold;
        cb.m_TemporalNormalThreshold           = temporalParams.normalThreshold;
        cb.m_TemporalEnableVisibilityShortcut  = temporalParams.enableVisibilityShortcut;
        cb.m_TemporalEnablePermutationSampling = temporalParams.enablePermutationSampling;
        cb.m_TemporalPermutationSamplingThreshold = temporalParams.permutationSamplingThreshold;
        cb.m_TemporalUniformRandomNumber       = temporalParams.uniformRandomNumber;
        cb.m_TemporalEnableBoilingFilter       = boilingParams.enableBoilingFilter;
        cb.m_TemporalBoilingFilterStrength     = boilingParams.boilingFilterStrength;

        // Spatial resampling parameters — forwarded from the ReSTIRDI context
        cb.m_SpatialNumDisocclusionBoostSamples = spatialParams.numDisocclusionBoostSamples;
        cb.m_SpatialBiasCorrectionMode          = static_cast<uint32_t>(spatialParams.biasCorrectionMode);
        cb.m_SpatialDepthThreshold              = spatialParams.depthThreshold;
        cb.m_SpatialNormalThreshold             = spatialParams.normalThreshold;
        cb.m_SpatialDiscountNaiveSamples        = spatialParams.discountNaiveSamples;

        // Shading parameters — forwarded from the ReSTIRDI context
        const RTXDI_ShadingParameters shadingParams = m_Context->GetShadingParameters();
        cb.m_EnableInitialVisibility    = g_ReSTIRDI_InitialSamplingParams.enableInitialVisibility;
        cb.m_EnableFinalVisibility      = shadingParams.enableFinalVisibility;
        cb.m_ReuseFinalVisibility       = shadingParams.reuseFinalVisibility;
        cb.m_DiscardInvisibleSamples    = temporalParams.enableVisibilityShortcut;
        cb.m_FinalVisibilityMaxAge      = shadingParams.finalVisibilityMaxAge;
        cb.m_FinalVisibilityMaxDistance = shadingParams.finalVisibilityMaxDistance;
        cb.m_EnableRTShadows            = renderer->m_EnableRTShadows ? 1u : 0u;

        // View matrices
        cb.m_View     = renderer->m_Scene.m_View;
        cb.m_PrevView = renderer->m_Scene.m_ViewPrev;

        cb.m_SunDirection = renderer->m_Scene.m_SunDirection;
        cb.m_SunIntensity = renderer->m_Scene.GetSunIntensity();

        // Upload constant buffer (volatile — recreated every frame)
        const nvrhi::BufferHandle rtxdiCB = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(RTXDIConstants), "RTXDIConstantsCB", 1));
        commandList->writeBuffer(rtxdiCB, &cb, sizeof(cb));

        // Retrieve render graph resources
        nvrhi::TextureHandle depthTex = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Read);
        nvrhi::TextureHandle albedoTex = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Read);
        nvrhi::TextureHandle normalsTex = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Read);
        nvrhi::TextureHandle ormTex = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Read);
        nvrhi::TextureHandle motionTex = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read);
        const bool bDenoise = renderer->m_EnableReSTIRDIRelaxDenoising;
        nvrhi::TextureHandle diOutput = !bDenoise
            ? renderGraph.GetTexture(g_RG_RTXDIDIOutput, RGResourceAccessMode::Write)
            : CommonResources::GetInstance().DummyUAVTexture;
        nvrhi::TextureHandle specularOutputUav = bDenoise
            ? renderGraph.GetTexture(g_RG_RTXDIRawSpecularOutput, RGResourceAccessMode::Write)
            : renderGraph.GetTexture(g_RG_RTXDISpecularOutput, RGResourceAccessMode::Write);
        nvrhi::TextureHandle albedoHistoryTex = renderGraph.GetTexture(m_GBufferAlbedoHistory, RGResourceAccessMode::Write);
        nvrhi::TextureHandle ormHistoryTex = renderGraph.GetTexture(m_GBufferORMHistory, RGResourceAccessMode::Write);
        nvrhi::TextureHandle depthHistoryTex = renderGraph.GetTexture(m_DepthHistory, RGResourceAccessMode::Write);
        nvrhi::TextureHandle normalsHistoryTex = renderGraph.GetTexture(m_GbufferNormalsHistory, RGResourceAccessMode::Write);
        nvrhi::BufferHandle neighborOffsetsBuffer = renderGraph.GetBuffer(m_RG_NeighborOffsetsBuffer, RGResourceAccessMode::Read);
        nvrhi::BufferHandle risBuffer = renderGraph.GetBuffer(m_RG_RISBuffer, RGResourceAccessMode::Read);
        nvrhi::BufferHandle lightReservoirBuffer = renderGraph.GetBuffer(g_RG_RTXDILightReservoirBuffer, RGResourceAccessMode::Read);
        nvrhi::BufferHandle risLightDataBuffer = renderGraph.GetBuffer(m_RG_RISLightDataBuffer, RGResourceAccessMode::Write);
        nvrhi::TextureHandle localLightPDFTex = renderGraph.GetTexture(m_RG_LocalLightPDFTexture, RGResourceAccessMode::Write);
        nvrhi::TextureHandle envLightPDFTex   = renderGraph.GetTexture(m_RG_EnvLightPDFTexture,   RGResourceAccessMode::Write);

        // ------------------------------------------------------------------
        // Initialize history textures on first frame
        // ------------------------------------------------------------------
        if (m_AlbedoHistoryIsNew)
        {
            commandList->copyTexture(albedoHistoryTex, nvrhi::TextureSlice{}, albedoTex, nvrhi::TextureSlice{});
        }
        if (m_ORMHistoryIsNew)
        {
            commandList->copyTexture(ormHistoryTex, nvrhi::TextureSlice{}, ormTex, nvrhi::TextureSlice{});
        }
        if (m_DepthHistoryIsNew)
        {
            commandList->copyTexture(depthHistoryTex, nvrhi::TextureSlice{}, depthTex, nvrhi::TextureSlice{});
        }
        if (m_NormalsHistoryIsNew)
        {
            commandList->copyTexture(normalsHistoryTex, nvrhi::TextureSlice{}, normalsTex, nvrhi::TextureSlice{});
        }

        // ------------------------------------------------------------------
        // Initialize neighbor offsets buffer on first allocation
        // ------------------------------------------------------------------
        if (m_NeighborOffsetsBufferIsNew)
        {
            const uint32_t count = m_Context->GetStaticParameters().NeighborOffsetCount;
            std::vector<uint8_t> offsets(count * 2); // two int8/uint8 per entry
            rtxdi::FillNeighborOffsetBuffer(offsets.data(), count);
            commandList->writeBuffer(neighborOffsetsBuffer, offsets.data(), offsets.size());
        }

        nvrhi::BindingSetDesc bset;
        bset.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(1, rtxdiCB),
            nvrhi::BindingSetItem::Texture_SRV(1,  depthTex),
            nvrhi::BindingSetItem::Texture_SRV(2,  normalsTex),
            nvrhi::BindingSetItem::Texture_SRV(3,  albedoTex),
            nvrhi::BindingSetItem::Texture_SRV(4,  ormTex),
            nvrhi::BindingSetItem::Texture_SRV(5,  motionTex),
            nvrhi::BindingSetItem::Texture_SRV(8,  albedoHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(9,  ormHistoryTex),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, renderer->m_Scene.m_LightBuffer),
            nvrhi::BindingSetItem::RayTracingAccelStruct(7, renderer->m_Scene.m_TLAS),
            nvrhi::BindingSetItem::RayTracingAccelStruct(10, m_TLASHistory),
            nvrhi::BindingSetItem::TypedBuffer_SRV(0, neighborOffsetsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, risBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, lightReservoirBuffer),
            nvrhi::BindingSetItem::Texture_UAV(2, diOutput),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(3, risLightDataBuffer),
            nvrhi::BindingSetItem::Texture_UAV(6, specularOutputUav),
            nvrhi::BindingSetItem::Texture_SRV(11, localLightPDFTex),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, renderer->m_Scene.m_InstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(13, renderer->m_Scene.m_MaterialConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(14, renderer->m_Scene.m_VertexBufferQuantized),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(15, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(16, renderer->m_Scene.m_IndexBuffer),
            nvrhi::BindingSetItem::Texture_SRV(17, depthHistoryTex),   // previous-frame depth
            nvrhi::BindingSetItem::Texture_SRV(18, normalsHistoryTex), // previous-frame normals
            nvrhi::BindingSetItem::Texture_SRV(19, envLightPDFTex),    // environment PDF mip chain
        };

        // ------------------------------------------------------------------
        // Build Local-Light PDF (mip 0)
        // Writes luminance(light.radiance) into the PDF texture's mip 0 at
        // each local light's Z-curve position.  Must run every frame because
        // light intensities can change between frames.
        // ------------------------------------------------------------------
        if (lbp.localLightBufferRegion.numLights > 0)
        {
            {
                PROFILE_SCOPED("Build Local Light PDF Mip 0");

                nvrhi::BindingSetDesc buildPDFBset;
                buildPDFBset.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(1, rtxdiCB),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(6, renderer->m_Scene.m_LightBuffer),
                    // u4: mip-0 UAV — written by the build shader
                    nvrhi::BindingSetItem::Texture_UAV(4, localLightPDFTex,
                        nvrhi::Format::UNKNOWN,
                        nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
                };
                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderName = "RTXDI_Master_RTXDI_BuildLocalLightPDF_Main",
                    .bindingSetDesc = buildPDFBset,
                    .dispatchParams = {
                        .x = DivideAndRoundUp(m_PDFTexSize, 8u),
                        .y = DivideAndRoundUp(m_PDFTexSize, 8u),
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }

            // ------------------------------------------------------------------
            // Generate PDF mip chain using SPD
            // Required so RTXDI_PresampleLocalLights can do hierarchical CDF
            // traversal across all mip levels.
            // ------------------------------------------------------------------
            if (m_PDFMipCount > 1u)
            {
                nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
                renderer->GenerateMipsUsingSPD(localLightPDFTex, spdAtomicCounter, commandList, "Generate Local Light PDF Mips", SPD_REDUCTION_AVERAGE);
            }

            // ------------------------------------------------------------------
            // Presample Local Lights (Power-RIS via RTXDI SDK)
            // Reads the full PDF mip chain via RTXDI_PresampleLocalLights to fill
            // the RIS tiles with importance-sampled lights, storing compact data.
            // ------------------------------------------------------------------
            {
                PROFILE_SCOPED("Presample Local Lights");

                const uint32_t presampleGroupsX = DivideAndRoundUp(k_RISTileSize, 256u);
                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderName = "RTXDI_Master_RTXDI_PresampleLights_Main",
                    .bindingSetDesc = bset,
                    .dispatchParams = {
                        .x = presampleGroupsX,
                        .y = k_RISTileCount,
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }
        }

        // ------------------------------------------------------------------
        // Build Environment-Light PDF, generate mip chain, and presample
        // the env RIS segment (runs only when env sampling is enabled).
        // ------------------------------------------------------------------
        if (renderer->m_EnableSky)
        {
            // Step 1: fill mip-0 with luminance × sin(θ) weights (ReSTIR-DI)
            //         or solid-angle-only weights (BRDF mode).
            {
                PROFILE_SCOPED("Build Environment Light PDF Mip 0");

                nvrhi::BindingSetDesc buildEnvPDFBset;
                buildEnvPDFBset.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(1, rtxdiCB),
                    // u8: env PDF mip-0 UAV
                    nvrhi::BindingSetItem::Texture_UAV(8, envLightPDFTex,
                        nvrhi::Format::UNKNOWN,
                        nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
                };
                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderName  = "RTXDI_Master_RTXDI_BuildEnvLightPDF_Main",
                    .bindingSetDesc = buildEnvPDFBset,
                    .dispatchParams = {
                        .x = DivideAndRoundUp(k_EnvPDFTexSize, 8u),
                        .y = DivideAndRoundUp(k_EnvPDFTexSize, 8u),
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }

            // Step 2: generate env PDF mip chain via SPD.
            if (m_EnvPDFMipCount > 1u)
            {
                nvrhi::BufferHandle spdEnvCounter = renderGraph.GetBuffer(
                    m_RG_SPDEnvAtomicCounter, RGResourceAccessMode::Write);
                renderer->GenerateMipsUsingSPD(
                    envLightPDFTex, spdEnvCounter, commandList,
                    "Generate Env Light PDF Mips", SPD_REDUCTION_AVERAGE);
            }

            // Step 3: presample the env RIS segment.
            {
                PROFILE_SCOPED("Presample Environment Light");

                const uint32_t presampleGroupsX = DivideAndRoundUp(k_EnvRISTileSize, 256u);
                Renderer::RenderPassParams params{
                    .commandList    = commandList,
                    .shaderName     = "RTXDI_Master_RTXDI_PresampleEnvironmentMap_Main",
                    .bindingSetDesc = bset,
                    .dispatchParams = {
                        .x = presampleGroupsX,
                        .y = k_EnvRISTileCount,
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }
        }

        // ------------------------------------------------------------------
        // Generate Initial Samples
        // ------------------------------------------------------------------
        {
            PROFILE_SCOPED("Generate Initial Samples");

            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDI_Master_RTXDI_GenerateInitialSamples_Main",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width, 8u),
                    .y = DivideAndRoundUp(height, 8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Temporal Resampling (conditional)
        // ------------------------------------------------------------------
        const bool doTemporal = (effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::Temporal ||
                      effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);
        if (doTemporal)
        {
            PROFILE_SCOPED("Temporal Resampling");

            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDI_Master_RTXDI_TemporalResampling_Main",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width, 8u),
                    .y = DivideAndRoundUp(height, 8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Spatial Resampling (conditional)
        // ------------------------------------------------------------------
        const bool doSpatial = (effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::Spatial ||
                     effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);
        if (doSpatial)
        {
            PROFILE_SCOPED("Spatial Resampling");

            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDI_Master_RTXDI_SpatialResampling_Main",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width, 8u),
                    .y = DivideAndRoundUp(height, 8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Shade Samples → write to DI output
        // ------------------------------------------------------------------
        if (renderer->m_EnableReSTIRDIRelaxDenoising)
        {
            // ---- Denoising path: split BRDF, pack diffuse/specular, then RELAX ----
            nvrhi::TextureHandle rawDiffuseOutputTex  = renderGraph.GetTexture(g_RG_RTXDIRawDiffuseOutput,  RGResourceAccessMode::Write);
            nvrhi::TextureHandle rawSpecularOutputTex = renderGraph.GetTexture(g_RG_RTXDIRawSpecularOutput, RGResourceAccessMode::Write);
            nvrhi::TextureHandle denoisedDiffuseOutputTex  = renderGraph.GetTexture(g_RG_RTXDIDiffuseOutput,  RGResourceAccessMode::Write);
            nvrhi::TextureHandle denoisedSpecularOutputTex = renderGraph.GetTexture(g_RG_RTXDISpecularOutput, RGResourceAccessMode::Write);
            nvrhi::TextureHandle linearDepthTex    = renderGraph.GetTexture(g_RG_RTXDILinearDepth,    RGResourceAccessMode::Write);

            // Extend the common binding set with denoising-only UAVs (u5, u7).
            nvrhi::BindingSetDesc denoiseBset = bset;
            denoiseBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(5, rawDiffuseOutputTex));
            denoiseBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(7, linearDepthTex));

            // ShadeSamples (denoising permutation): writes packed diffuse/specular to u5/u6.
            {
                PROFILE_SCOPED("Shade Samples with RELAX permutation");

                Renderer::RenderPassParams params{
                    .commandList  = commandList,
                    .shaderName   = "RTXDI_Master_RTXDI_ShadeSamples_Main_RTXDI_ENABLE_RELAX_DENOISING=1",
                    .bindingSetDesc = denoiseBset,
                    .dispatchParams = {
                        .x = DivideAndRoundUp(width, 8u),
                        .y = DivideAndRoundUp(height, 8u),
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }

            // GenerateViewZ: full-screen pass that writes linear view-space depth to u7.
            {
                PROFILE_SCOPED("Generate ViewZ for RELAX");
                
                Renderer::RenderPassParams params{
                    .commandList  = commandList,
                    .shaderName   = "RTXDI_Master_RTXDI_GenerateViewZ_Main_RTXDI_ENABLE_RELAX_DENOISING=1",
                    .bindingSetDesc = denoiseBset,
                    .dispatchParams = {
                        .x = DivideAndRoundUp(width,  8u),
                        .y = DivideAndRoundUp(height, 8u),
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }

            // Execute RELAX denoiser via NrdIntegration.
            // Separate textures for IN_* and OUT_* avoid SRV+UAV aliasing.
            // NrdIntegration packs normals+roughness internally before dispatching.
            {
                nrd::CommonSettings commonSettings{};
                FillNRDCommonSettings(commonSettings);

                m_NrdIntegration->RunDenoiserPasses(
                    commandList,
                    renderGraph,
                    normalsTex,                 // IN_NORMAL_ROUGHNESS (packed internally)
                    ormTex,                     // roughness source for pack pass
                    rawDiffuseOutputTex,        // IN_DIFF_RADIANCE_HITDIST
                    rawSpecularOutputTex,       // IN_SPEC_RADIANCE_HITDIST
                    linearDepthTex,             // IN_VIEWZ
                    motionTex,                  // IN_MV
                    denoisedDiffuseOutputTex,   // OUT_DIFF_RADIANCE_HITDIST
                    denoisedSpecularOutputTex,  // OUT_SPEC_RADIANCE_HITDIST
                    commonSettings,
                    &m_NRDRelaxSettings);
            }
        }
        else
        {
            PROFILE_SCOPED("Shade Samples without RELAX");

            // ---- Non-denoising path: demodulated diffuse/specular illumination ----
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDI_Master_RTXDI_ShadeSamples_Main_RTXDI_ENABLE_RELAX_DENOISING=0",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width, 8u),
                    .y = DivideAndRoundUp(height, 8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Copy current G-buffer to history textures for next frame
        // ------------------------------------------------------------------
        commandList->copyTexture(albedoHistoryTex,  nvrhi::TextureSlice{}, albedoTex,   nvrhi::TextureSlice{});
        commandList->copyTexture(ormHistoryTex,     nvrhi::TextureSlice{}, ormTex,      nvrhi::TextureSlice{});
        commandList->copyTexture(depthHistoryTex,   nvrhi::TextureSlice{}, depthTex,    nvrhi::TextureSlice{});
        commandList->copyTexture(normalsHistoryTex, nvrhi::TextureSlice{}, normalsTex,  nvrhi::TextureSlice{});

        // Copy current TLAS to history for next frame
        if (m_TLASHistory && renderer->m_Scene.m_TLAS)
        {
            commandList->copyRaytracingAccelerationStructure(m_TLASHistory, renderer->m_Scene.m_TLAS);
        }
    }

    const char* GetName() const override { return "RTXDIRenderer"; }

private:
    // ------------------------------------------------------------------
    // Helper: build RTXDI_LightBufferParameters from current scene lights.
    // Light index 0 is always the directional/sun → infinite light slot.
    // All others are local (point/spot).
    // ------------------------------------------------------------------
    static RTXDI_LightBufferParameters BuildLightBufferParams(const Renderer* renderer)
    {
        RTXDI_LightBufferParameters lbp{};
        const uint32_t totalLights = renderer->m_Scene.m_LightCount;

        // Environment light: present and presampled when sky is enabled.
        // The virtual env-light index is one
        // past the end of the real lights array so it never aliases a GPULight.
        const bool bEnvPresent = renderer->m_EnableSky;
        lbp.environmentLightParams.lightPresent = bEnvPresent ? 1u : 0u;
        lbp.environmentLightParams.lightIndex = bEnvPresent ? totalLights : 0u;

        if (totalLights == 0)
            return lbp;

        // Sun (always index 0) → infinite light region
        lbp.infiniteLightBufferRegion.firstLightIndex = 0;
        lbp.infiniteLightBufferRegion.numLights       = 1;

        // Remaining lights → local light region
        if (totalLights > 1)
        {
            lbp.localLightBufferRegion.firstLightIndex = 1;
            lbp.localLightBufferRegion.numLights       = totalLights - 1;
        }



        return lbp;
    }
};

REGISTER_RENDERER(RTXDIRenderer);

class RTXDIVisualizationRenderer : public IRenderer
{
    RGTextureHandle m_RG_RTXDIVizOutput;

public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Only run when RTXDI is enabled and DI visualization mode is active.
        if (!renderer->m_EnableReSTIRDI)
            return false;

        if (g_ReservoirSubfieldVizMode == (uint32_t)ReservoirSubfieldVizMode::Off)
            return false;

        if (g_ReservoirSubfieldVizMode != (uint32_t)ReservoirSubfieldVizMode::DIReservoir)
            return false; // GI/PT stubs — not yet implemented
    
        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Declare the viz output texture (RGBA16F, same dimensions as swapchain)
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width        = width;
            desc.m_NvrhiDesc.height       = height;
            desc.m_NvrhiDesc.format       = Renderer::HDR_COLOR_FORMAT;
            desc.m_NvrhiDesc.isUAV        = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.debugName    = "RTXDIVizOutput";
            renderGraph.DeclareTexture(desc, m_RG_RTXDIVizOutput);
        }

        // Read the DI reservoir buffer (populated by RTXDIRenderer)
        renderGraph.ReadBuffer(g_RG_RTXDILightReservoirBuffer);

        // We will blit the viz output to HDR color in Render()
        renderGraph.WriteTexture(g_RG_HDRColor);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;

        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Retrieve the RTXDI context from the RTXDIRenderer instance
        extern IRenderer* g_RTXDIRenderer;
        RTXDIRenderer* rtxdiRenderer = static_cast<RTXDIRenderer*>(g_RTXDIRenderer);

        const RTXDI_ReservoirBufferParameters rbp = rtxdiRenderer->m_Context->GetReservoirBufferParameters();
        const RTXDI_RuntimeParameters         rtp = rtxdiRenderer->m_Context->GetRuntimeParams();
        const RTXDI_DIBufferIndices           bix = rtxdiRenderer->m_Context->GetBufferIndices();

        // Build the DIReservoirVizParameters constant buffer
        DIReservoirVizParameters vizCB{};
        vizCB.view                 = renderer->m_Scene.m_View;
        vizCB.runtimeParams        = rtp;
        vizCB.reservoirBufferParams = rbp;
        vizCB.bufferIndices        = bix;
        vizCB.diReservoirField     = g_DIReservoirVizField;
        vizCB.maxLightsInBuffer    = renderer->m_Scene.m_LightCount;
        vizCB.pad1                 = 0;
        vizCB.pad2                 = 0;

        nvrhi::BufferHandle vizCBHandle = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(DIReservoirVizParameters), "DIReservoirVizCB", 1));
        commandList->writeBuffer(vizCBHandle, &vizCB, sizeof(vizCB));

        // Build the RTXDIConstants constant buffer (for maxHistoryLength)
        RTXDIConstants rtxdiCB{};
        rtxdiCB.m_View                    = renderer->m_Scene.m_View;
        rtxdiCB.m_TemporalMaxHistoryLength = rtxdiRenderer->m_Context->GetTemporalResamplingParameters().maxHistoryLength;

        nvrhi::BufferHandle rtxdiCBHandle = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(RTXDIConstants), "RTXDIConstantsCB_Viz", 1));
        commandList->writeBuffer(rtxdiCBHandle, &rtxdiCB, sizeof(rtxdiCB));

        // Retrieve render graph resources
        nvrhi::BufferHandle  reservoirBuffer = renderGraph.GetBuffer(g_RG_RTXDILightReservoirBuffer, RGResourceAccessMode::Read);
        nvrhi::TextureHandle vizOutput       = renderGraph.GetTexture(m_RG_RTXDIVizOutput, RGResourceAccessMode::Write);

        // Dispatch the visualization compute shader
        nvrhi::BindingSetDesc bsetDesc;
        bsetDesc.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, reservoirBuffer),
            nvrhi::BindingSetItem::Texture_UAV(1, vizOutput),
            nvrhi::BindingSetItem::ConstantBuffer(0, vizCBHandle),
            nvrhi::BindingSetItem::ConstantBuffer(1, rtxdiCBHandle)
        };

        renderer->AddComputePass({
            .commandList    = commandList,
            .shaderName     = "DIReservoirViz_main",
            .bindingSetDesc = bsetDesc,
            .bIncludeBindlessResources = false,
            .dispatchParams = {
                .x = (width  + 15) / 16,
                .y = (height + 15) / 16,
                .z = 1
            }
        });

        // Blit the viz output to HDR color so downstream tone-mapping sees it
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);
        commandList->copyTexture(hdrColor, nvrhi::TextureSlice{}, vizOutput, nvrhi::TextureSlice{});
    }

    const char* GetName() const override { return "RTXDIVisualization"; }
};

REGISTER_RENDERER(RTXDIVisualizationRenderer);
