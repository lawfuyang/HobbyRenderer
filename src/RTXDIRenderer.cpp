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
#include "shaders/rtxdi/SharedShaderInclude/SharedShaderInclude/ShaderParameters.h"

#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>

#include <imgui.h>

// ---- DI output textures (read by DeferredRenderer) ----
RGTextureHandle g_RG_RTXDIDIOutput;           // non-denoised diffuse illumination
RGTextureHandle g_RG_RTXDIDiffuseOutput;      // RELAX denoised diffuse output
RGTextureHandle g_RG_RTXDISpecularOutput;     // non-denoised specular / RELAX denoised specular
RGTextureHandle g_RG_RTXDIRawDiffuseOutput;   // pre-denoised diffuse (RELAX input)
RGTextureHandle g_RG_RTXDIRawSpecularOutput;  // pre-denoised specular (RELAX input)
RGTextureHandle g_RG_RTXDILinearDepth;        // linear view-space depth (RELAX IN_VIEWZ)
RGBufferHandle  g_RG_RTXDILightReservoirBuffer; // u_LightReservoirs — also read by viz renderer
RGTextureHandle g_RG_RTXDIDIComposited;       // CompositingPass output — read by DeferredRenderer

extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferMotionVectors;
extern RGTextureHandle g_RG_GBufferEmissive;
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

void RTXDIIMGUISettings()
{
    Renderer* renderer = Renderer::GetInstance();

    ImGui::Indent();

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
    // Per-frame transient RG handles (not needed by other renderers)
    // ------------------------------------------------------------------
    RGTextureHandle m_RG_PrevRestirLuminance;
    RGTextureHandle m_RG_DenoiserNormalRoughness;
    RGTextureHandle m_RG_LinearDepth;
    RGTextureHandle m_RG_RawDiffuseOutput;
    RGTextureHandle m_RG_RawSpecularOutput;
    RGBufferHandle  m_RG_LightDataBuffer;
    RGBufferHandle  m_RG_LightIndexMapping;
    RGBufferHandle  m_RG_GeometryInstanceToLight;
    RGBufferHandle  m_RG_PrepareLightsTasks;
    RGBufferHandle  m_RG_SecondaryGBuffer;
    RGBufferHandle  m_RG_PrimitiveLightBuffer;

    // ------------------------------------------------------------------
    // Persistent ray tracing acceleration structures
    // ------------------------------------------------------------------
    nvrhi::rt::AccelStructHandle m_TLASHistory;

    // ------------------------------------------------------------------
    // Cached light buffer data (built once at PostSceneLoad; scene lights
    // are never streamed in/out so this never needs to be rebuilt per-frame)
    // ------------------------------------------------------------------
    RTXDI_LightBufferParameters          m_CachedLightBufferParams{};
    std::vector<PolymorphicLightInfo>    m_CachedPrimitiveLights;

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

        // "medium" preset from FullSample as base, with my own changes
        g_ReSTIRDI_ResamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
        g_ReSTIRDI_NumLocalLightUniformSamples = 8;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numBrdfSamples = 1;
        g_ReSTIRDI_InitialSamplingParams.numInfiniteLightSamples = 1;
        g_ReSTIRDI_TemporalResamplingParams.enableVisibilityShortcut = 1u;
        g_ReSTIRDI_BoilingFilterParams.enableBoilingFilter = 1u;
        g_ReSTIRDI_BoilingFilterParams.boilingFilterStrength = 0.2f;
        g_ReSTIRDI_TemporalResamplingParams.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        g_ReSTIRDI_SpatialResamplingParams.numSamples = 1;
        g_ReSTIRDI_SpatialResamplingParams.numDisocclusionBoostSamples = 8;
        g_ReSTIRDI_ShadingParams.reuseFinalVisibility = 1u;

        g_ReSTIRDI_TemporalResamplingParams.enablePermutationSampling = 0u; // disabling this somehow increases image quality?

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

        // Build and cache light buffer params — scene lights are static so this
        // only needs to happen once after the scene is loaded.
        m_CachedLightBufferParams = BuildLightBufferParams(renderer, m_CachedPrimitiveLights);
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
        renderGraph.ReadTexture(g_RG_GBufferEmissive);

        // ------------------------------------------------------------------
        // FullSample per-frame resources
        // ------------------------------------------------------------------

        // Previous-frame RestirLuminance (persistent history)
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width  = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = nvrhi::Format::RG16_FLOAT; // R=luminance, G=unused
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName    = "RTXDIPrevRestirLuminance";
            renderGraph.DeclarePersistentTexture(desc, m_RG_PrevRestirLuminance);
        }

        // Denoiser normal+roughness (RGBA16F) — PostprocessGBuffer writes, NRD reads
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width  = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = nvrhi::Format::R10G10B10A2_UNORM;
            desc.m_NvrhiDesc.isUAV  = true;
            desc.m_NvrhiDesc.isRenderTarget = true;  // needed for fullscreen pixel shader output
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.debugName    = "RTXDIDenoiserNormalRoughness";
            renderGraph.DeclareTexture(desc, m_RG_DenoiserNormalRoughness);
        }

        // Linear depth (R32F) — written by GenerateViewZ, read by NRD as IN_VIEWZ
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width  = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
            desc.m_NvrhiDesc.isUAV  = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.debugName    = "RTXDILinearDepth";
            renderGraph.DeclareTexture(desc, m_RG_LinearDepth);
        }

        // CompositingPass output (RGBA16F) — final DI + emissive composite, read by DeferredRenderer
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width  = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = nvrhi::Format::RGBA16_FLOAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.isRenderTarget = true;
            desc.m_NvrhiDesc.debugName    = "RTXDIDIComposited";
            renderGraph.DeclareTexture(desc, g_RG_RTXDIDIComposited);
        }

        // RELAX raw inputs (only allocated when denoising enabled)
        if (renderer->m_EnableReSTIRDIRelaxDenoising)
        {
            auto makeHDR = [&](const char* name, RGTextureHandle& h) {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width  = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = nvrhi::Format::RGBA16_FLOAT;
                desc.m_NvrhiDesc.isUAV  = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName    = name;
                renderGraph.DeclareTexture(desc, h);
            };
            makeHDR("RTXDIRawDiffuseOutput",  m_RG_RawDiffuseOutput);
            makeHDR("RTXDIRawSpecularOutput", m_RG_RawSpecularOutput);
            makeHDR("RTXDIDiffuseOutput",     g_RG_RTXDIDiffuseOutput);
            makeHDR("RTXDISpecularOutput",    g_RG_RTXDISpecularOutput);
        }
        else
        {
            // Non-denoised path: DI output textures read by DeferredRenderer
            auto makeHDR = [&](const char* name, RGTextureHandle& h) {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width  = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT;
                desc.m_NvrhiDesc.isUAV  = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName    = name;
                renderGraph.DeclareTexture(desc, h);
            };
            makeHDR("RTXDIDIOutput",       g_RG_RTXDIDIOutput);
            makeHDR("RTXDISpecularOutput", g_RG_RTXDISpecularOutput);
        }

        // Light data buffer (PolymorphicLightInfo per light, written by PrepareLights)
        {
            const uint32_t maxLights = std::max(renderer->m_Scene.m_LightCount * 2u, 1u);
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(maxLights) * sizeof(PolymorphicLightInfo);
            bd.m_NvrhiDesc.structStride = sizeof(PolymorphicLightInfo);
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName    = "RTXDILightDataBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_LightDataBuffer);
        }

        // Light index mapping buffer (uint per light)
        {
            const uint32_t maxLights = std::max(renderer->m_Scene.m_LightCount * 2u, 1u);
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(maxLights) * sizeof(uint32_t);
            bd.m_NvrhiDesc.format       = nvrhi::Format::R32_UINT;
            bd.m_NvrhiDesc.canHaveTypedViews = true;
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName    = "RTXDILightIndexMapping";
            renderGraph.DeclareBuffer(bd, m_RG_LightIndexMapping);
        }

        // GeometryInstanceToLight mapping (uint per instance)
        {
            const uint32_t numInstances = std::max((uint32_t)renderer->m_Scene.m_InstanceData.size(), 1u);
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(numInstances) * sizeof(uint32_t);
            bd.m_NvrhiDesc.format       = nvrhi::Format::R32_UINT;
            bd.m_NvrhiDesc.canHaveTypedViews = true;
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName    = "RTXDIGeometryInstanceToLight";
            renderGraph.DeclareBuffer(bd, m_RG_GeometryInstanceToLight);
        }

        // PrepareLights task buffer (PrepareLightsTask per emissive triangle)
        {
            const uint32_t maxTasks = std::max(renderer->m_Scene.m_LightCount, 1u);
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(maxTasks) * sizeof(PrepareLightsTask);
            bd.m_NvrhiDesc.structStride = sizeof(PrepareLightsTask);
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName    = "RTXDIPrepareLightsTasks";
            renderGraph.DeclareBuffer(bd, m_RG_PrepareLightsTasks);
        }

        // Primitive light buffer — CPU-converted analytical lights (directional/point/spot).
        // Written each frame via commandList->writeBuffer; read by PrepareLights.hlsl at t1.
        {
            const uint32_t maxLights = std::max(renderer->m_Scene.m_LightCount, 1u);
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(maxLights) * sizeof(PolymorphicLightInfo);
            bd.m_NvrhiDesc.structStride = sizeof(PolymorphicLightInfo);
            bd.m_NvrhiDesc.canHaveUAVs  = false;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            bd.m_NvrhiDesc.debugName    = "RTXDIPrimitiveLightBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_PrimitiveLightBuffer);
        }

        // Secondary GBuffer (SecondaryGBufferData per pixel) — used by BrdfRayTracing
        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(width) * height * sizeof(SecondaryGBufferData);
            bd.m_NvrhiDesc.structStride = sizeof(SecondaryGBufferData);
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName    = "RTXDISecondaryGBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_SecondaryGBuffer);
        }

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
        // Build ResamplingConstants (FullSample's g_Const) from context accessors
        // ------------------------------------------------------------------
        const RTXDI_ReservoirBufferParameters rbp = m_Context->GetReservoirBufferParameters();
        const RTXDI_RuntimeParameters         rtp = m_Context->GetRuntimeParams();
        const RTXDI_DIBufferIndices          bix = m_Context->GetBufferIndices();
        // Use the light buffer params cached at PostSceneLoad (scene lights are static).
        const RTXDI_LightBufferParameters&    lbp            = m_CachedLightBufferParams;
        const std::vector<PolymorphicLightInfo>& primitiveLights = m_CachedPrimitiveLights;

        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        ResamplingConstants g_Const{};

        // ---- View matrices ----
        // Fill PlanarViewConstants with Donut-compatible field names.
        // HobbyRenderer's PlanarViewConstants (ShaderShared.h) uses m_Mat* names;
        // the HLSL shim (view_cb.h) maps them to matWorldToView, matViewToWorld, etc.
        // We copy the whole struct — field layout is identical.
        static_assert(sizeof(g_Const.view) == sizeof(renderer->m_Scene.m_View),
            "PlanarViewConstants size mismatch between ShaderShared.h and ShaderParameters.h");
        memcpy(&g_Const.view,     &renderer->m_Scene.m_View,     sizeof(g_Const.view));
        memcpy(&g_Const.prevView, &renderer->m_Scene.m_ViewPrev, sizeof(g_Const.prevView));
        // prevPrevView: use prevView as fallback (no triple-buffering in HobbyRenderer)
        memcpy(&g_Const.prevPrevView, &renderer->m_Scene.m_ViewPrev, sizeof(g_Const.prevPrevView));

        // Fill cameraDirectionOrPosition from matViewToWorld translation row
        auto FillCameraPos = [](PlanarViewConstants& v) {
            v.m_CameraDirectionOrPosition = {
                v.m_MatViewToWorld.m[3][0],
                v.m_MatViewToWorld.m[3][1],
                v.m_MatViewToWorld.m[3][2],
                1.0f
            };
        };
        FillCameraPos(g_Const.view);
        FillCameraPos(g_Const.prevView);
        FillCameraPos(g_Const.prevPrevView);

        // ---- Runtime parameters ----
        g_Const.runtimeParams = rtp;

        // ---- Denoiser mode ----
        g_Const.denoiserMode = renderer->m_EnableReSTIRDIRelaxDenoising
            ? DENOISER_MODE_RELAX : DENOISER_MODE_OFF;

        // ---- Scene constants ----
        g_Const.sceneConstants.enableEnvironmentMap      = renderer->m_EnableSky ? 1u : 0u;
        g_Const.sceneConstants.environmentMapTextureIndex = 0u; // Bruneton sky — no texture index
        g_Const.sceneConstants.environmentScale          = 1.0f;
        g_Const.sceneConstants.environmentRotation       = 0.0f;
        g_Const.sceneConstants.enableAlphaTestedGeometry = 1u;
        g_Const.sceneConstants.enableTransparentGeometry = 0u;
        g_Const.sceneConstants.sunIntensity              = renderer->m_Scene.GetSunIntensity();
        g_Const.sceneConstants.sunDirection              = renderer->m_Scene.m_SunDirection;

        // ---- Light buffer parameters ----
        g_Const.lightBufferParams = lbp;

        // ---- RIS buffer segment parameters ----
        {
            RTXDI_RISBufferSegmentParameters localSeg{};
            localSeg.bufferOffset = 0u;
            localSeg.tileSize     = k_RISTileSize;
            localSeg.tileCount    = k_RISTileCount;
            g_Const.localLightsRISBufferSegmentParams = localSeg;
        }
        if (renderer->m_EnableSky)
        {
            RTXDI_RISBufferSegmentParameters envSeg{};
            envSeg.bufferOffset = k_RISTileSize * k_RISTileCount;
            envSeg.tileSize     = k_EnvRISTileSize;
            envSeg.tileCount    = k_EnvRISTileCount;
            g_Const.environmentLightRISBufferSegmentParams = envSeg;
        }

        // ---- PDF texture sizes ----
        g_Const.localLightPdfTextureSize  = { m_PDFTexSize, m_PDFTexSize };
        g_Const.environmentPdfTextureSize = { k_EnvPDFTexSize, k_EnvPDFTexSize };

        // ---- ReSTIR DI parameters ----
        {
            RTXDI_Parameters restirDI{};
            restirDI.reservoirBufferParams = rbp;
            restirDI.bufferIndices         = bix;
            restirDI.initialSamplingParams = m_Context->GetInitialSamplingParameters();
            restirDI.temporalResamplingParams = m_Context->GetTemporalResamplingParameters();
            restirDI.spatialResamplingParams  = m_Context->GetSpatialResamplingParameters();
            restirDI.shadingParams            = m_Context->GetShadingParameters();
            g_Const.restirDI = restirDI;
        }

        // ---- Misc flags ----
        g_Const.enablePreviousTLAS    = 1u;
        g_Const.discountNaiveSamples  = m_Context->GetSpatialResamplingParameters().discountNaiveSamples;
        g_Const.enableBrdfIndirect    = 0u; // GI/PT stubs not dispatched
        g_Const.enableBrdfAdditiveBlend = 0u;
        g_Const.enableAccumulation    = 0u;
        g_Const.directLightingMode    = DirectLightingMode::ReStir;
        g_Const.visualizeRegirCells   = 0u;
        g_Const.enableDenoiserPSR     = 0u;
        g_Const.usePSRMvecForResampling = 0u;
        g_Const.updatePSRwithResampling = 0u;

        // Upload constant buffers (volatile — recreated every frame)
        const nvrhi::BufferHandle rtxdiCB = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ResamplingConstants), "ResamplingConstantsCB", 1));
        commandList->writeBuffer(rtxdiCB, &g_Const, sizeof(g_Const));

        // Retrieve render graph resources — FullSample RAB_Buffers.hlsli slot layout
        const CommonResources& cr = CommonResources::GetInstance();

        // G-buffer inputs
        nvrhi::TextureHandle depthTex        = renderGraph.GetTexture(g_RG_DepthTexture,         RGResourceAccessMode::Read);
        nvrhi::TextureHandle albedoTex       = renderGraph.GetTexture(g_RG_GBufferAlbedo,        RGResourceAccessMode::Read);
        nvrhi::TextureHandle normalsTex      = renderGraph.GetTexture(g_RG_GBufferNormals,       RGResourceAccessMode::Read);
        nvrhi::TextureHandle ormTex          = renderGraph.GetTexture(g_RG_GBufferORM,           RGResourceAccessMode::Read);
        nvrhi::TextureHandle motionTex       = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read);
        nvrhi::TextureHandle emissiveTex     = renderGraph.GetTexture(g_RG_GBufferEmissive,      RGResourceAccessMode::Read);

        // History textures (persistent, reused from existing members)
        nvrhi::TextureHandle albedoHistoryTex  = renderGraph.GetTexture(m_GBufferAlbedoHistory,  RGResourceAccessMode::Write);
        nvrhi::TextureHandle ormHistoryTex     = renderGraph.GetTexture(m_GBufferORMHistory,     RGResourceAccessMode::Write);
        nvrhi::TextureHandle depthHistoryTex   = renderGraph.GetTexture(m_DepthHistory,          RGResourceAccessMode::Write);
        nvrhi::TextureHandle normalsHistoryTex = renderGraph.GetTexture(m_GbufferNormalsHistory, RGResourceAccessMode::Write);

        // FullSample per-frame textures (member variables)
        nvrhi::TextureHandle prevRestirLumTex = renderGraph.GetTexture(m_RG_PrevRestirLuminance, RGResourceAccessMode::Write);
        nvrhi::TextureHandle denoiserNRTex    = renderGraph.GetTexture(m_RG_DenoiserNormalRoughness, RGResourceAccessMode::Write);
        nvrhi::TextureHandle linearDepthTex   = renderGraph.GetTexture(m_RG_LinearDepth,         RGResourceAccessMode::Write);
        nvrhi::TextureHandle compositedTex    = renderGraph.GetTexture(g_RG_RTXDIDIComposited,   RGResourceAccessMode::Write);

        // Light buffers
        nvrhi::BufferHandle  neighborOffsetsBuf  = renderGraph.GetBuffer(m_RG_NeighborOffsetsBuffer, m_AlbedoHistoryIsNew ?  RGResourceAccessMode::Write : RGResourceAccessMode::Read);
        nvrhi::BufferHandle  risBuffer           = renderGraph.GetBuffer(m_RG_RISBuffer,             RGResourceAccessMode::Write);
        nvrhi::BufferHandle  lightReservoirBuf   = renderGraph.GetBuffer(g_RG_RTXDILightReservoirBuffer, RGResourceAccessMode::Write);
        nvrhi::BufferHandle  risLightDataBuf     = renderGraph.GetBuffer(m_RG_RISLightDataBuffer,    RGResourceAccessMode::Write);
        nvrhi::TextureHandle localLightPDFTex    = renderGraph.GetTexture(m_RG_LocalLightPDFTexture, RGResourceAccessMode::Write);
        nvrhi::TextureHandle envLightPDFTex      = renderGraph.GetTexture(m_RG_EnvLightPDFTexture,   RGResourceAccessMode::Write);
        nvrhi::BufferHandle  lightDataBuf        = renderGraph.GetBuffer(m_RG_LightDataBuffer,       RGResourceAccessMode::Write);
        nvrhi::BufferHandle  lightIndexMapBuf    = renderGraph.GetBuffer(m_RG_LightIndexMapping,     RGResourceAccessMode::Write);
        nvrhi::BufferHandle  geoInstToLightBuf   = renderGraph.GetBuffer(m_RG_GeometryInstanceToLight, RGResourceAccessMode::Write);
        nvrhi::BufferHandle  prepareLightsTaskBuf= renderGraph.GetBuffer(m_RG_PrepareLightsTasks,    RGResourceAccessMode::Write);
        nvrhi::BufferHandle  primitiveLightBuf   = renderGraph.GetBuffer(m_RG_PrimitiveLightBuffer,   RGResourceAccessMode::Write);
        // secondaryGBufBuf removed — ReSTIR GI / BrdfRayTracing not dispatched

        // Denoising path outputs
        const bool bDenoise = renderer->m_EnableReSTIRDIRelaxDenoising;
        nvrhi::TextureHandle rawDiffuseTex      = bDenoise ? renderGraph.GetTexture(m_RG_RawDiffuseOutput,  RGResourceAccessMode::Write) : cr.DummyUAVTexture;
        nvrhi::TextureHandle rawSpecularTex     = bDenoise ? renderGraph.GetTexture(m_RG_RawSpecularOutput, RGResourceAccessMode::Write) : cr.DummyUAVTexture;
        nvrhi::TextureHandle denoisedDiffuseTex = bDenoise ? renderGraph.GetTexture(g_RG_RTXDIDiffuseOutput,  RGResourceAccessMode::Write) : cr.DummyUAVTexture;
        nvrhi::TextureHandle denoisedSpecularTex= bDenoise ? renderGraph.GetTexture(g_RG_RTXDISpecularOutput, RGResourceAccessMode::Write) : cr.DummyUAVTexture;
        // Non-denoised path outputs
        nvrhi::TextureHandle diOutputTex    = !bDenoise ? renderGraph.GetTexture(g_RG_RTXDIDIOutput,       RGResourceAccessMode::Write) : cr.DummyUAVTexture;
        nvrhi::TextureHandle specularOutTex = !bDenoise ? renderGraph.GetTexture(g_RG_RTXDISpecularOutput, RGResourceAccessMode::Write) : cr.DummyUAVTexture;

        // ------------------------------------------------------------------
        // Initialize history textures on first frame
        // ------------------------------------------------------------------
        if (m_AlbedoHistoryIsNew)
            commandList->copyTexture(albedoHistoryTex, nvrhi::TextureSlice{}, albedoTex,   nvrhi::TextureSlice{});
        if (m_ORMHistoryIsNew)
            commandList->copyTexture(ormHistoryTex,    nvrhi::TextureSlice{}, ormTex,      nvrhi::TextureSlice{});
        if (m_DepthHistoryIsNew)
            commandList->copyTexture(depthHistoryTex,  nvrhi::TextureSlice{}, depthTex,    nvrhi::TextureSlice{});
        if (m_NormalsHistoryIsNew)
            commandList->copyTexture(normalsHistoryTex,nvrhi::TextureSlice{}, normalsTex,  nvrhi::TextureSlice{});

        // ------------------------------------------------------------------
        // Initialize neighbor offsets buffer on first allocation
        // ------------------------------------------------------------------
        if (m_NeighborOffsetsBufferIsNew)
        {
            const uint32_t count = m_Context->GetStaticParameters().NeighborOffsetCount;
            std::vector<uint8_t> offsets(count * 2);
            rtxdi::FillNeighborOffsetBuffer(offsets.data(), count);
            commandList->writeBuffer(neighborOffsetsBuf, offsets.data(), offsets.size());
        }

        // ------------------------------------------------------------------
        // FullSample RAB_Buffers.hlsli binding layout
        // b0 = ResamplingConstants (g_Const)
        // b1 = PerPassConstants    (g_PerPassConstants)
        // t0  = t_NeighborOffsets
        // t1  = t_GBufferDepth
        // t2  = t_GBufferGeoNormals
        // t3  = t_GBufferDiffuseAlbedo
        // t4  = t_GBufferSpecularRough
        // t5  = t_GBufferNormals
        // t6  = t_PrevGBufferNormals
        // t7  = t_PrevGBufferGeoNormals
        // t8  = t_PrevGBufferDiffuseAlbedo
        // t9  = t_PrevGBufferSpecularRough
        // t10 = t_PrevRestirLuminance
        // t11 = t_MotionVectors
        // t12 = t_DenoiserNormalRoughness
        // t13 = t_PrevDepth
        // t14 = t_LocalLightPdfTexture
        // t15 = t_EnvironmentPdfTexture
        // t16 = t_RisBuffer
        // t17 = t_RisLightDataBuffer
        // t18 = t_SceneBVH
        // t19 = t_PrevSceneBVH
        // t20 = t_LightDataBuffer
        // t21 = t_LightIndexMappingBuffer
        // t22 = t_GBufferEmissive
        // t25 = t_GeometryInstanceToLight
        // t26 = t_InstanceData
        // t27 = t_GeometryData
        // t28 = t_MaterialConstants
        // t29 = t_BindlessBuffers (handled by bIncludeBindlessResources)
        // t30 = t_BindlessTextures (handled by bIncludeBindlessResources)
        // u0  = u_LightReservoirs
        // u1  = u_RisBuffer
        // u2  = u_RisLightDataBuffer
        // u3  = u_TemporalSamplePositions
        // u4  = u_Gradients
        // u5  = u_RestirLuminance
        // u6  = u_GIReservoirs
        // u7  = u_PTReservoirs
        // u8  = u_DiffuseLighting
        // u9  = u_SpecularLighting
        // u10 = u_DiffuseConfidence (unused stub)
        // u11 = u_SpecularConfidence (unused stub)
        // u12 = u_RayCountBuffer
        // u13 = u_SecondaryGBuffer
        // u14 = u_SecondarySurfaces (unused stub)
        // u15 = u_DebugColor (unused stub)
        // u16 = u_DebugPrintBuffer
        // u17 = u_DirectLightingRaw
        // u18 = u_IndirectLightingRaw
        // u20 = u_PSRDepth
        // u21 = u_PSRNormalRoughness (= DenoiserNormalRoughness)
        // u22 = u_PSRMotionVectors
        // u23 = u_PSRHitT
        // u24 = u_PSRDiffuseAlbedo
        // u25 = u_PSRSpecularF0
        // u26 = u_PSRLightDir
        // u27 = u_EnvLightPdfMip0 (used only in BuildEnvLightPDF)
        // ------------------------------------------------------------------

        nvrhi::BindingSetDesc bset;
        bset.bindings = {
            // Constant buffers
            nvrhi::BindingSetItem::ConstantBuffer(0, rtxdiCB),
            // SRVs
            nvrhi::BindingSetItem::TypedBuffer_SRV(0,  neighborOffsetsBuf),
            nvrhi::BindingSetItem::Texture_SRV(1,  depthTex),
            nvrhi::BindingSetItem::Texture_SRV(2,  cr.DummySRVTexture),  // t_GBufferGeoNormals — removed, use normals directly
            nvrhi::BindingSetItem::Texture_SRV(3,  albedoTex),
            nvrhi::BindingSetItem::Texture_SRV(4,  ormTex),
            nvrhi::BindingSetItem::Texture_SRV(5,  normalsTex),
            nvrhi::BindingSetItem::Texture_SRV(6,  normalsHistoryTex),  // prev shading normals
            nvrhi::BindingSetItem::Texture_SRV(7,  cr.DummySRVTexture),  // t_PrevGBufferGeoNormals — removed
            nvrhi::BindingSetItem::Texture_SRV(8,  albedoHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(9,  ormHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(10, prevRestirLumTex),
            nvrhi::BindingSetItem::Texture_SRV(11, motionTex),
            nvrhi::BindingSetItem::Texture_SRV(12, denoiserNRTex),
            nvrhi::BindingSetItem::Texture_SRV(13, depthHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(14, localLightPDFTex),
            nvrhi::BindingSetItem::Texture_SRV(15, envLightPDFTex),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(16, cr.DummySRVStructuredBuffer), // dummy SRV for t_RisBuffer — actual buffer read via UAV slot (u1) to avoid bindless resource handling
            nvrhi::BindingSetItem::StructuredBuffer_SRV(17, cr.DummySRVStructuredBuffer), // dummy SRV for t_RisLightDataBuffer — actual buffer read via UAV slot (u2) to avoid bindless resource handling
            nvrhi::BindingSetItem::RayTracingAccelStruct(18, renderer->m_Scene.m_TLAS),
            nvrhi::BindingSetItem::RayTracingAccelStruct(19, m_TLASHistory),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(20, lightDataBuf),
            nvrhi::BindingSetItem::TypedBuffer_SRV(21, lightIndexMapBuf),
            nvrhi::BindingSetItem::Texture_SRV(22, emissiveTex),
            nvrhi::BindingSetItem::TypedBuffer_SRV(25, geoInstToLightBuf),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(26, renderer->m_Scene.m_InstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(27, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(28, renderer->m_Scene.m_MaterialConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(29, renderer->m_Scene.m_IndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(30, renderer->m_Scene.m_VertexBufferQuantized),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0,  lightReservoirBuf),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1,  risBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(2,  risLightDataBuf),
            nvrhi::BindingSetItem::Texture_UAV(3,  cr.DummyUAVTexture),  // u_TemporalSamplePositions — not used
            nvrhi::BindingSetItem::Texture_UAV(4,  cr.DummyUAVTexture),  // u_Gradients — not used
            nvrhi::BindingSetItem::Texture_UAV(5,  cr.DummyUAVTexture),  // u_RestirLuminance — not used
            nvrhi::BindingSetItem::StructuredBuffer_UAV(6,  cr.DummyUAVStructuredBuffer),  // u_GIReservoirs stub
            nvrhi::BindingSetItem::StructuredBuffer_UAV(7,  cr.DummyUAVStructuredBuffer),  // u_PTReservoirs stub
            nvrhi::BindingSetItem::Texture_UAV(8,  bDenoise ? rawDiffuseTex  : diOutputTex),
            nvrhi::BindingSetItem::Texture_UAV(9,  bDenoise ? rawSpecularTex : specularOutTex),
            nvrhi::BindingSetItem::Texture_UAV(10, cr.DummyUAVTexture),  // u_DiffuseConfidence stub
            nvrhi::BindingSetItem::Texture_UAV(11, cr.DummyUAVTexture),  // u_SpecularConfidence stub
            nvrhi::BindingSetItem::TypedBuffer_UAV(12, cr.DummyUAVTypedBuffer),  // u_RayCountBuffer — not used
            nvrhi::BindingSetItem::StructuredBuffer_UAV(13, cr.DummyUAVStructuredBuffer),  // u_SecondaryGBuffer — not used (ReSTIR GI not dispatched)
            nvrhi::BindingSetItem::StructuredBuffer_UAV(14, cr.DummyUAVStructuredBuffer),  // u_SecondarySurfaces stub
            nvrhi::BindingSetItem::Texture_UAV(15, cr.DummyUAVTexture),  // u_DebugColor stub
            nvrhi::BindingSetItem::RawBuffer_UAV(16, cr.DummyUAVByteAddressBuffer),  // u_DebugPrintBuffer — not used
            nvrhi::BindingSetItem::Texture_UAV(17, cr.DummyUAVTexture),  // u_DirectLightingRaw — not used
            nvrhi::BindingSetItem::Texture_UAV(18, cr.DummyUAVTexture),  // u_IndirectLightingRaw — not used
            nvrhi::BindingSetItem::Texture_UAV(20, cr.DummyUAVTexture),  // u_PSRDepth stub
            nvrhi::BindingSetItem::Texture_UAV(21, cr.DummyUAVTexture),  // dummy (no LightIndexMapping — lights don't stream in/out)
            nvrhi::BindingSetItem::Texture_UAV(22, cr.DummyUAVTexture),  // u_PSRMotionVectors stub
            nvrhi::BindingSetItem::Texture_UAV(23, cr.DummyUAVTexture),  // u_PSRHitT stub
            nvrhi::BindingSetItem::Texture_UAV(24, cr.DummyUAVTexture),  // u_PSRDiffuseAlbedo stub
            nvrhi::BindingSetItem::Texture_UAV(25, cr.DummyUAVTexture),  // u_PSRSpecularF0 stub
            nvrhi::BindingSetItem::Texture_UAV(26, cr.DummyUAVTexture),  // u_PSRLightDir stub
        };

        // ------------------------------------------------------------------
        // GenerateViewZ — always run (unconditional), writes linear view-space
        // depth to linearDepthTex.  PostprocessGBuffer reads it at t1.
        // ------------------------------------------------------------------
        {
            PROFILE_SCOPED("GenerateViewZ");

            nvrhi::BindingSetDesc vzBset;
            vzBset.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, rtxdiCB),
                nvrhi::BindingSetItem::Texture_SRV(1,  depthTex),
                nvrhi::BindingSetItem::Texture_UAV(0,  linearDepthTex),
            };
            renderer->AddComputePass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/GenerateViewZ_main",
                .bindingSetDesc = vzBset,
                .bIncludeBindlessResources = false,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .y = DivideAndRoundUp(height, RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .z = 1u
                }
            });
        }

        // ------------------------------------------------------------------
        // PostprocessGBuffer
        // Reads LinearZ (t1) + packed normals/ORM, writes denoiser
        // normal+roughness (NRD IN_NORMAL_ROUGHNESS) as render target.
        // ------------------------------------------------------------------
        {
            PROFILE_SCOPED("PostprocessGBuffer");

            nvrhi::BindingSetDesc ppBset;
            ppBset.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, rtxdiCB),
                nvrhi::BindingSetItem::Texture_SRV(1,  linearDepthTex),  // LinearZ (not raw depth)
                nvrhi::BindingSetItem::Texture_SRV(5,  normalsTex),
                nvrhi::BindingSetItem::Texture_SRV(4,  ormTex),
            };

            nvrhi::FramebufferDesc ppFbDesc;
            ppFbDesc.addColorAttachment(denoiserNRTex);
            nvrhi::FramebufferHandle ppFb = device->createFramebuffer(ppFbDesc);

            renderer->AddFullScreenPass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/PostprocessGBuffer_main",
                .bindingSetDesc = ppBset,
                .bIncludeBindlessResources = false,
                .framebuffer    = ppFb,
            });
        }

        // ------------------------------------------------------------------
        // PrepareLights
        // Uploads CPU-converted analytical lights (directional/point/spot) to
        // the primitive light buffer, then dispatches PrepareLights.hlsl which
        // copies them into the main light data buffer and writes mip-0 PDF flux.
        // ------------------------------------------------------------------
        if (renderer->m_Scene.m_LightCount > 0)
        {
            PROFILE_SCOPED("PrepareLights");

            // Upload CPU-converted analytical lights to the primitive light buffer.
            if (!primitiveLights.empty())
            {
                commandList->writeBuffer(primitiveLightBuf,
                    primitiveLights.data(),
                    primitiveLights.size() * sizeof(PolymorphicLightInfo));
            }

            // Build one PrepareLightsTask per analytical light with TASK_PRIMITIVE_LIGHT_BIT set.
            // instanceAndGeometryIndex is unused for primitive lights; we only need lightBufferOffset.
            {
                const uint32_t numLights = renderer->m_Scene.m_LightCount;
                std::vector<PrepareLightsTask> tasks(numLights);
                for (uint32_t i = 0; i < numLights; ++i)
                {
                    tasks[i].instanceAndGeometryIndex = TASK_PRIMITIVE_LIGHT_BIT; // marks this as a primitive (analytical) light
                    tasks[i].triangleCount            = 0;
                    tasks[i].lightBufferOffset        = i;
                    tasks[i].previousLightBufferOffset = -1; // no temporal tracking yet
                }
                commandList->writeBuffer(prepareLightsTaskBuf,
                    tasks.data(),
                    tasks.size() * sizeof(PrepareLightsTask));
            }

            PrepareLightsConstants plCB{};
            plCB.numTasks                  = renderer->m_Scene.m_LightCount;
            plCB.currentFrameLightOffset   = 0u;
            plCB.previousFrameLightOffset  = 0u;

            nvrhi::BufferHandle plCBHandle = device->createBuffer(
                nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(PrepareLightsConstants), "PrepareLightsCB", 1));
            commandList->writeBuffer(plCBHandle, &plCB, sizeof(plCB));

            nvrhi::BindingSetDesc plBset;
            plBset.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, plCBHandle),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(0,  prepareLightsTaskBuf),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(1,  primitiveLightBuf),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(26, renderer->m_Scene.m_InstanceDataBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(27, renderer->m_Scene.m_MeshDataBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(28, renderer->m_Scene.m_MaterialConstantsBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(29, renderer->m_Scene.m_IndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(30, renderer->m_Scene.m_VertexBufferQuantized),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(0,  lightDataBuf),
                nvrhi::BindingSetItem::TypedBuffer_UAV(1,       lightIndexMapBuf),
                nvrhi::BindingSetItem::TypedBuffer_UAV(2,       geoInstToLightBuf),
                nvrhi::BindingSetItem::Texture_UAV(4, localLightPDFTex,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
            };
            renderer->AddComputePass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/PrepareLights_main",
                .bindingSetDesc = plBset,
                .bIncludeBindlessResources = true,
                .dispatchParams = {
                    .x = DivideAndRoundUp(renderer->m_Scene.m_LightCount, 256u),
                    .y = 1u,
                    .z = 1u
                }
            });
        }

        // ------------------------------------------------------------------
        // Build Local-Light PDF mip chain + presample
        // Mip-0 is now written directly by PrepareLights.hlsl (u4).
        // We only need to generate the remaining mips via SPD and presample.
        // ------------------------------------------------------------------
        if (lbp.localLightBufferRegion.numLights > 0)
        {
            if (m_PDFMipCount > 1u)
            {
                nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
                renderer->GenerateMipsUsingSPD(localLightPDFTex, spdAtomicCounter, commandList, "Generate Local Light PDF Mips", SPD_REDUCTION_AVERAGE);
            }

            {
                PROFILE_SCOPED("Presample Local Lights");

                const uint32_t presampleGroupsX = DivideAndRoundUp(k_RISTileSize, RTXDI_PRESAMPLING_GROUP_SIZE);
                renderer->AddComputePass({
                    .commandList    = commandList,
                    .shaderName     = "rtxdi/LightingPasses/Presampling/PresampleLights_main",
                    .bindingSetDesc = bset,
                    .bIncludeBindlessResources = false,
                    .dispatchParams = { .x = presampleGroupsX, .y = k_RISTileCount, .z = 1u }
                });
            }
        }

        // ------------------------------------------------------------------
        // Build Environment-Light PDF mip chain + presample
        // The env PDF texture is initialized to uniform (1.0) by the GPU clear
        // on first use; we only need to generate mips and presample.
        // (Bruneton sky has no texture to sample from, so uniform env sampling
        //  is used — no custom BuildEnvironmentLightPDF pass needed.)
        // ------------------------------------------------------------------
        if (renderer->m_EnableSky)
        {
            if (m_EnvPDFMipCount > 1u)
            {
                nvrhi::BufferHandle spdEnvCounter = renderGraph.GetBuffer(m_RG_SPDEnvAtomicCounter, RGResourceAccessMode::Write);
                renderer->GenerateMipsUsingSPD(envLightPDFTex, spdEnvCounter, commandList, "Generate Env Light PDF Mips", SPD_REDUCTION_AVERAGE);
            }

            {
                PROFILE_SCOPED("Presample Environment Light");

                const uint32_t presampleGroupsX = DivideAndRoundUp(k_EnvRISTileSize, RTXDI_PRESAMPLING_GROUP_SIZE);
                renderer->AddComputePass({
                    .commandList    = commandList,
                    .shaderName     = "rtxdi/LightingPasses/Presampling/PresampleEnvironmentMap_main",
                    .bindingSetDesc = bset,
                    .bIncludeBindlessResources = false,
                    .dispatchParams = { .x = presampleGroupsX, .y = k_EnvRISTileCount, .z = 1u }
                });
            }
        }

        // ------------------------------------------------------------------
        // Generate Initial Samples
        // ------------------------------------------------------------------
        {
            PROFILE_SCOPED("Generate Initial Samples");

            renderer->AddComputePass({
                .commandList    = commandList,
                // TODO: change to 'GenerateInitialSamples_main_RTXDI_REGIR_MODE=RTXDI_REGIR_ONION' when ReGIR is implemented
                .shaderName     = "rtxdi/LightingPasses/DI/GenerateInitialSamples_main_RTXDI_REGIR_MODE=RTXDI_REGIR_DISABLED",
                .bindingSetDesc = bset,
                .bIncludeBindlessResources = true,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .y = DivideAndRoundUp(height, RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .z = 1u
                }
            });
        }

        // ------------------------------------------------------------------
        // Temporal Resampling (conditional)
        // ------------------------------------------------------------------
        const bool doTemporal = (effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::Temporal ||
                      effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);
        if (doTemporal)
        {
            PROFILE_SCOPED("Temporal Resampling");

            renderer->AddComputePass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/LightingPasses/DI/TemporalResampling_main",
                .bindingSetDesc = bset,
                .bIncludeBindlessResources = true,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .y = DivideAndRoundUp(height, RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .z = 1u
                }
            });
        }

        // ------------------------------------------------------------------
        // Spatial Resampling (conditional)
        // ------------------------------------------------------------------
        const bool doSpatial = (effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::Spatial ||
                     effectiveResamplingMode == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);
        if (doSpatial)
        {
            PROFILE_SCOPED("Spatial Resampling");

            renderer->AddComputePass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/LightingPasses/DI/SpatialResampling_main",
                .bindingSetDesc = bset,
                .bIncludeBindlessResources = true,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .y = DivideAndRoundUp(height, RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .z = 1u
                }
            });
        }

        // ------------------------------------------------------------------
        // Shade Samples → write to DI output (u8/u9)
        // ------------------------------------------------------------------
        {
            PROFILE_SCOPED(bDenoise ? "Shade Samples (RELAX)" : "Shade Samples");

            renderer->AddComputePass({
                .commandList    = commandList,
                // TODO: change to 'ShadeSamples_main_RTXDI_REGIR_MODE=RTXDI_REGIR_ONION' when ReGIR is implemented
                .shaderName     = "rtxdi/LightingPasses/DI/ShadeSamples_main_RTXDI_REGIR_MODE=RTXDI_REGIR_DISABLED",
                .bindingSetDesc = bset,
                .bIncludeBindlessResources = true,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .y = DivideAndRoundUp(height, RTXDI_SCREEN_SPACE_GROUP_SIZE),
                    .z = 1u
                }
            });
        }

        // ------------------------------------------------------------------
        // RELAX denoising (when enabled)
        // ------------------------------------------------------------------
        if (bDenoise)
        {
            // linearDepthTex was already written by GenerateViewZ above (unconditional)
            nrd::CommonSettings commonSettings{};
            FillNRDCommonSettings(commonSettings);

            m_NrdIntegration->RunDenoiserPasses(
                commandList,
                renderGraph,
                denoiserNRTex,          // IN_NORMAL_ROUGHNESS (packed by PostprocessGBuffer)
                ormTex,                 // roughness source fallback
                rawDiffuseTex,          // IN_DIFF_RADIANCE_HITDIST
                rawSpecularTex,         // IN_SPEC_RADIANCE_HITDIST
                linearDepthTex,         // IN_VIEWZ (written by GenerateViewZ above)
                motionTex,              // IN_MV (g_RG_GBufferMotionVectors — used directly)
                denoisedDiffuseTex,     // OUT_DIFF_RADIANCE_HITDIST
                denoisedSpecularTex,    // OUT_SPEC_RADIANCE_HITDIST
                commonSettings,
                &m_NRDRelaxSettings);
        }

        // ------------------------------------------------------------------
        // CompositingPass
        // Combines denoised/raw DI illumination with emissive, sky background,
        // and albedo to produce the final HDR composite.
        // ------------------------------------------------------------------
        {
            PROFILE_SCOPED("CompositingPass");

            nvrhi::BindingSetDesc compBset;
            compBset.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, rtxdiCB),
                nvrhi::BindingSetItem::Texture_SRV(3,  albedoTex),   // t_GBufferAlbedo  (RGBA8_UNORM)
                nvrhi::BindingSetItem::Texture_SRV(4,  ormTex),      // t_GBufferORM     (RG8_UNORM: roughness=.r, metallic=.g)
                nvrhi::BindingSetItem::Texture_SRV(22, emissiveTex), // t_GBufferEmissive
                // DI illumination inputs (denoised or raw)
                nvrhi::BindingSetItem::Texture_SRV(23, bDenoise ? denoisedDiffuseTex  : diOutputTex),
                nvrhi::BindingSetItem::Texture_SRV(24, bDenoise ? denoisedSpecularTex : specularOutTex),
            };

            nvrhi::FramebufferDesc ppFbDesc;
            ppFbDesc.addColorAttachment(compositedTex);
            nvrhi::FramebufferHandle ppFb = device->createFramebuffer(ppFbDesc);

            renderer->AddFullScreenPass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/CompositingPass_CompositingPass_PSMain",
                .bindingSetDesc = compBset,
                .bIncludeBindlessResources = false,
                .framebuffer    = ppFb,
            });
        }

        // ------------------------------------------------------------------
        // Copy current G-buffer to history textures for next frame
        // ------------------------------------------------------------------
        commandList->copyTexture(albedoHistoryTex,  nvrhi::TextureSlice{}, albedoTex,     nvrhi::TextureSlice{});
        commandList->copyTexture(ormHistoryTex,     nvrhi::TextureSlice{}, ormTex,        nvrhi::TextureSlice{});
        commandList->copyTexture(depthHistoryTex,   nvrhi::TextureSlice{}, depthTex,      nvrhi::TextureSlice{});
        commandList->copyTexture(normalsHistoryTex, nvrhi::TextureSlice{}, normalsTex,    nvrhi::TextureSlice{});

        // Copy current TLAS to history for next frame
        if (m_TLASHistory && renderer->m_Scene.m_TLAS)
        {
            commandList->copyRaytracingAccelerationStructure(m_TLASHistory, renderer->m_Scene.m_TLAS);
        }
    }

    const char* GetName() const override { return "RTXDIRenderer"; }

private:
    // ------------------------------------------------------------------
    // CPU-side helpers: pack scene lights into PolymorphicLightInfo.
    // Mirrors the ConvertLight() logic from RTXDI FullSample PrepareLightsPass.cpp.
    // ------------------------------------------------------------------

    static uint16_t Fp32ToFp16(float v)
    {
        // Multiplying by 2^-112 causes exponents below -14 to denormalize
        union FU { uint32_t ui; float f; };
        FU biased; biased.f = v * 1.9259299444e-34f; // 2^-112
        const uint32_t u    = biased.ui;
        const uint32_t sign = u & 0x80000000u;
        const uint32_t body = u & 0x0fffffffu;
        return static_cast<uint16_t>((sign >> 16u) | (body >> 13u));
    }

    static uint32_t PackFloat2ToUint(float a, float b)
    {
        return (static_cast<uint32_t>(Fp32ToFp16(a))) |
               (static_cast<uint32_t>(Fp32ToFp16(b)) << 16u);
    }

    static uint32_t FloatToUInt(float v, float scale)
    {
        return static_cast<uint32_t>(std::floor(v * scale + 0.5f));
    }

    static uint32_t PackR8G8B8Unorm(float r, float g, float b)
    {
        return  (FloatToUInt(std::max(0.f, std::min(1.f, r)), 255.f) & 0xFFu)
             | ((FloatToUInt(std::max(0.f, std::min(1.f, g)), 255.f) & 0xFFu) << 8u)
             | ((FloatToUInt(std::max(0.f, std::min(1.f, b)), 255.f) & 0xFFu) << 16u);
    }

    static void PackLightColor(const DirectX::XMFLOAT3& color, PolymorphicLightInfo& info)
    {
        float maxR = std::max({ color.x, color.y, color.z });
        if (maxR <= 0.f) return;

        float logR = (std::log2f(maxR) - kPolymorphicLightMinLog2Radiance)
                   / (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance);
        logR = std::max(0.f, std::min(1.f, logR));
        uint32_t packed = std::min(static_cast<uint32_t>(std::ceil(logR * 65534.f)) + 1u, 0xffffu);
        float unpacked = std::exp2f((static_cast<float>(packed - 1u) / 65534.f)
                         * (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance)
                         + kPolymorphicLightMinLog2Radiance);

        info.colorTypeAndFlags |= PackR8G8B8Unorm(color.x / unpacked, color.y / unpacked, color.z / unpacked);
        info.logRadiance       |= packed;
    }

    // Octahedral encoding of a unit vector → two floats in [-1,1]
    static DirectX::XMFLOAT2 UnitVecToOct(const DirectX::XMFLOAT3& n)
    {
        float m = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
        float ox = n.x / m, oy = n.y / m;
        if (n.z <= 0.f)
        {
            float sx = ox >= 0.f ? 1.f : -1.f;
            float sy = oy >= 0.f ? 1.f : -1.f;
            ox = (1.f - std::abs(oy)) * sx;
            oy = (1.f - std::abs(ox)) * sy; // note: uses updated ox
        }
        return { ox, oy };
    }

    static uint32_t PackNormalizedVector(const DirectX::XMFLOAT3& v)
    {
        auto oct = UnitVecToOct(v);
        oct.x = oct.x * 0.5f + 0.5f;
        oct.y = oct.y * 0.5f + 0.5f;
        uint32_t X = FloatToUInt(std::max(0.f, std::min(1.f, oct.x)), float((1u << 16u) - 1u));
        uint32_t Y = FloatToUInt(std::max(0.f, std::min(1.f, oct.y)), float((1u << 16u) - 1u));
        return X | (Y << 16u);
    }

    // Convert a single scene light to PolymorphicLightInfo.
    // Returns false if the light type is unsupported.
    static bool ConvertAnalyticalLight(const Scene::Light& light,
                                       const Scene::Node&  node,
                                       PolymorphicLightInfo& out)
    {
        out = {};

        // Direction from node rotation quaternion (same as SceneLoader)
        auto QuatToDir = [](const DirectX::XMFLOAT4& q) -> DirectX::XMFLOAT3 {
            // Forward = (0,0,-1) rotated by q
            float x = q.x, y = q.y, z = q.z, w = q.w;
            return {
                2.f*(x*z + w*y),
                2.f*(y*z - w*x),
                1.f - 2.f*(x*x + y*y)
            };
        };

        auto Normalize3 = [](DirectX::XMFLOAT3 v) -> DirectX::XMFLOAT3 {
            float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
            if (len < 1e-7f) return {0,0,1};
            return { v.x/len, v.y/len, v.z/len };
        };

        switch (light.m_Type)
        {
        case Scene::Light::Directional:
        {
            float halfAngRad  = light.m_AngularSize * 0.5f * (DirectX::XM_PI / 180.f);
            float solidAngle  = 2.f * DirectX::XM_PI * (1.f - std::cos(halfAngRad));
            float irradiance  = light.m_Intensity;
            float radiance    = (solidAngle > 0.f) ? irradiance / solidAngle : 0.f;
            DirectX::XMFLOAT3 col = { light.m_Color.x * radiance,
                                      light.m_Color.y * radiance,
                                      light.m_Color.z * radiance };

            out.colorTypeAndFlags = static_cast<uint32_t>(PolymorphicLightType::kDirectional)
                                    << kPolymorphicLightTypeShift;
            PackLightColor(col, out);
            DirectX::XMFLOAT3 dir = Normalize3(QuatToDir(node.m_Rotation));
            out.direction1 = PackNormalizedVector(dir);
            out.scalars    = PackFloat2ToUint(halfAngRad, solidAngle);
            return true;
        }
        case Scene::Light::Point:
        {
            if (light.m_Radius > 0.f)
            {
                // Sphere light
                float projArea = DirectX::XM_PI * light.m_Radius * light.m_Radius;
                float radiance = (projArea > 0.f) ? light.m_Intensity / projArea : 0.f;
                DirectX::XMFLOAT3 col = { light.m_Color.x * radiance,
                                          light.m_Color.y * radiance,
                                          light.m_Color.z * radiance };
                out.colorTypeAndFlags = static_cast<uint32_t>(PolymorphicLightType::kSphere)
                                        << kPolymorphicLightTypeShift;
                PackLightColor(col, out);
                out.center  = { node.m_Translation.x, node.m_Translation.y, node.m_Translation.z };
                out.scalars = PackFloat2ToUint(light.m_Radius, 0.f);
            }
            else
            {
                // Point light (zero radius)
                DirectX::XMFLOAT3 flux = { light.m_Color.x * light.m_Intensity,
                                           light.m_Color.y * light.m_Intensity,
                                           light.m_Color.z * light.m_Intensity };
                out.colorTypeAndFlags = static_cast<uint32_t>(PolymorphicLightType::kPoint)
                                        << kPolymorphicLightTypeShift;
                PackLightColor(flux, out);
                out.center = { node.m_Translation.x, node.m_Translation.y, node.m_Translation.z };
            }
            return true;
        }
        case Scene::Light::Spot:
        {
            float projArea = DirectX::XM_PI * light.m_Radius * light.m_Radius;
            float radiance = (projArea > 0.f) ? light.m_Intensity / projArea : light.m_Intensity;
            DirectX::XMFLOAT3 col = { light.m_Color.x * radiance,
                                      light.m_Color.y * radiance,
                                      light.m_Color.z * radiance };
            float softness = (light.m_SpotOuterConeAngle > 0.f)
                ? std::max(0.f, std::min(1.f, 1.f - light.m_SpotInnerConeAngle / light.m_SpotOuterConeAngle))
                : 0.f;

            out.colorTypeAndFlags = (static_cast<uint32_t>(PolymorphicLightType::kSphere)
                                     << kPolymorphicLightTypeShift)
                                  | kPolymorphicLightShapingEnableBit;
            PackLightColor(col, out);
            out.center  = { node.m_Translation.x, node.m_Translation.y, node.m_Translation.z };
            out.scalars = PackFloat2ToUint(light.m_Radius, 0.f);
            DirectX::XMFLOAT3 dir = Normalize3(QuatToDir(node.m_Rotation));
            out.primaryAxis = PackNormalizedVector(dir);
            out.cosConeAngleAndSoftness = PackFloat2ToUint(
                std::cos(light.m_SpotOuterConeAngle), softness);
            return true;
        }
        default:
            return false;
        }
    }

    // Build the CPU-side primitive light array and return RTXDI_LightBufferParameters.
    // Also fills outPrimitiveLights with the converted PolymorphicLightInfo entries.
    static RTXDI_LightBufferParameters BuildLightBufferParams(
        const Renderer* renderer,
        std::vector<PolymorphicLightInfo>& outPrimitiveLights)
    {
        RTXDI_LightBufferParameters lbp{};
        const uint32_t totalLights = renderer->m_Scene.m_LightCount;

        // Environment light: present and presampled when sky is enabled.
        const bool bEnvPresent = renderer->m_EnableSky;
        lbp.environmentLightParams.lightPresent = bEnvPresent ? 1u : 0u;
        lbp.environmentLightParams.lightIndex   = bEnvPresent ? totalLights : 0u;

        if (totalLights == 0)
            return lbp;

        outPrimitiveLights.resize(totalLights);

        uint32_t numInfinite = 0;
        uint32_t numLocal    = 0;

        for (uint32_t i = 0; i < totalLights; ++i)
        {
            const Scene::Light& light = renderer->m_Scene.m_Lights[i];
            SDL_assert(light.m_NodeIndex >= 0);
            const Scene::Node& node = renderer->m_Scene.m_Nodes[light.m_NodeIndex];

            PolymorphicLightInfo info{};
            ConvertAnalyticalLight(light, node, info);
            outPrimitiveLights[i] = info;

            if (light.m_Type == Scene::Light::Directional)
                ++numInfinite;
            else
                ++numLocal;
        }

        // Infinite lights (directional) come first in the buffer
        lbp.infiniteLightBufferRegion.firstLightIndex = 0;
        lbp.infiniteLightBufferRegion.numLights       = numInfinite;

        // Local lights (point/spot) follow
        lbp.localLightBufferRegion.firstLightIndex = numInfinite;
        lbp.localLightBufferRegion.numLights       = numLocal;

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

        // Build a minimal ResamplingConstants CB for the viz shader
        // (only view and reservoir params are needed)
        ResamplingConstants resCB{};
        memcpy(&resCB.view, &renderer->m_Scene.m_View, sizeof(resCB.view));
        resCB.view.m_CameraDirectionOrPosition = {
            resCB.view.m_MatViewToWorld.m[3][0],
            resCB.view.m_MatViewToWorld.m[3][1],
            resCB.view.m_MatViewToWorld.m[3][2],
            1.0f
        };
        resCB.runtimeParams = rtp;
        resCB.restirDI.reservoirBufferParams = rbp;
        resCB.restirDI.bufferIndices         = bix;

        nvrhi::BufferHandle resCBHandle = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ResamplingConstants), "ResamplingConstantsCB_Viz", 1));
        commandList->writeBuffer(resCBHandle, &resCB, sizeof(resCB));

        PerPassConstants perPassCB{};
        perPassCB.rayCountBufferIndex = -1;
        nvrhi::BufferHandle perPassCBHandle = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(PerPassConstants), "PerPassConstantsCB_Viz", 1));
        commandList->writeBuffer(perPassCBHandle, &perPassCB, sizeof(perPassCB));

        // Retrieve render graph resources
        nvrhi::BufferHandle  reservoirBuffer = renderGraph.GetBuffer(g_RG_RTXDILightReservoirBuffer, RGResourceAccessMode::Read);
        nvrhi::TextureHandle vizOutput       = renderGraph.GetTexture(m_RG_RTXDIVizOutput, RGResourceAccessMode::Write);

        // Dispatch the visualization compute shader
        // DIReservoirViz_main uses b0=ResamplingConstants, b1=PerPassConstants,
        // u0=LightReservoirs, u1=vizOutput, plus the DIReservoirVizParameters CB at b2.
        nvrhi::BindingSetDesc bsetDesc;
        bsetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, resCBHandle),
            nvrhi::BindingSetItem::ConstantBuffer(1, perPassCBHandle),
            nvrhi::BindingSetItem::ConstantBuffer(2, vizCBHandle),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, reservoirBuffer),
            nvrhi::BindingSetItem::Texture_UAV(1, vizOutput),
        };

        renderer->AddComputePass({
            .commandList    = commandList,
                .shaderName     = "rtxdi/ShaderDebug/ReservoirSubfieldVizPasses/DIReservoirViz_main",
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
