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
#include "shaders/rtxdi/SharedShaderInclude/SharedShaderInclude/ShaderParameters.h"

#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>

#include <imgui.h>

// ---- DI output textures (read by DeferredRenderer) ----
RGTextureHandle g_RG_RTXDIDIOutput;           // non-denoised diffuse illumination
RGTextureHandle g_RG_RTXDIDiffuseOutput;      // RELAX denoised diffuse output
RGTextureHandle g_RG_RTXDISpecularOutput;     // non-denoised specular / RELAX denoised specular
RGBufferHandle  g_RG_RTXDILightReservoirBuffer; // u_LightReservoirs — also read by viz renderer
RGTextureHandle g_RG_RTXDIDIComposited;       // CompositingPass output — read by DeferredRenderer

extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferGeoNormals;
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
// Compact buffer stride: 2 x uint4 per RIS entry (see RAB_LightInfo.hlsli).
static constexpr uint32_t k_CompactSlotsPerEntry = 2u;

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
    RGTextureHandle m_GeoNormalsHistory;   // previous-frame geo normals

    // Track if history textures are newly created in current frame
    bool m_AlbedoHistoryIsNew = false;
    bool m_ORMHistoryIsNew    = false;
    bool m_DepthHistoryIsNew  = false;
    bool m_NormalsHistoryIsNew = false;
    bool m_GeoNormalsHistoryIsNew = false;

    // ------------------------------------------------------------------
    // Per-frame transient RG handles (not needed by other renderers)
    // ------------------------------------------------------------------
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
    // Triangle emissive light data — prepared once at PostSceneLoad.
    // Triangle lights are never streamed in/out so tasks are static.
    // ------------------------------------------------------------------
    // One PrepareLightsTask per emissive mesh geometry (covers all its triangles).
    std::vector<PrepareLightsTask>       m_CachedTriangleLightTasks;
    // Total number of emissive triangles across all emissive mesh geometries.
    uint32_t                             m_TotalEmissiveTriangles = 0;
    // Per-instance geometry-to-light mapping: geometryInstanceIndex -> firstLightBufferOffset.
    // RTXDI_INVALID_LIGHT_INDEX (0xFFFFFFFF) means not emissive.
    std::vector<uint32_t>                m_CachedGeometryInstanceToLight;

    // Cached sun direction from last frame — used to detect changes and
    // trigger a rebuild of the analytical light buffer params.
    Vector3                              m_CachedSunDirection{ 0.f, 0.f, 0.f };
    // True when analytical light params need to be rebuilt this frame.
    bool                                 m_AnalyticalLightsDirty = true;

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
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        g_ReSTIRDI_NumLocalLightUniformSamples = 8;
        g_ReSTIRDI_NumLocalLightPowerRISSamples = 8;
        g_ReSTIRDI_NumLocalLightReGIRRISSamples = 8;
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode::Uniform ? g_ReSTIRDI_NumLocalLightUniformSamples : 
            (g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode::Power_RIS ? g_ReSTIRDI_NumLocalLightPowerRISSamples : 
            g_ReSTIRDI_NumLocalLightReGIRRISSamples);
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
        g_ReSTIRDI_InitialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;  // TODO: delete this when ReGIR_RIS once it's working
        g_ReSTIRDI_InitialSamplingParams.numLocalLightSamples = 8; // TODO: delete this when ReGIR_RIS once it's working

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

        // Build and cache light buffer params — analytical scene lights are static
        // so this only needs to happen once after the scene is loaded.
        m_CachedLightBufferParams = BuildLightBufferParams(m_CachedPrimitiveLights);

        // ------------------------------------------------------------------
        // Build triangle emissive light tasks — prepared once at PostSceneLoad
        // because triangle lights are never streamed in/out.
        // ------------------------------------------------------------------
        {
            Scene& scene = renderer->m_Scene;

            // Count total geometry instances (one per primitive across all nodes)
            const uint32_t totalGeometryInstances = static_cast<uint32_t>(scene.m_InstanceData.size());
            m_CachedGeometryInstanceToLight.assign(totalGeometryInstances, RTXDI_INVALID_LIGHT_INDEX);

            // Assign m_FirstGeometryInstanceIndex for each instance.
            // In HobbyRenderer, each PerInstanceData corresponds to exactly one primitive
            // (one geometry sub-mesh), so geometryIndex is always 0 and
            // m_FirstGeometryInstanceIndex == the instance's own index in m_InstanceData.
            for (uint32_t i = 0; i < totalGeometryInstances; ++i)
                scene.m_InstanceData[i].m_FirstGeometryInstanceIndex = i;

            // Upload the updated m_FirstGeometryInstanceIndex values to the GPU buffer.
            scopeCl->writeBuffer(scene.m_InstanceDataBuffer,
                scene.m_InstanceData.data(),
                scene.m_InstanceData.size() * sizeof(PerInstanceData));

            m_CachedTriangleLightTasks.clear();
            m_TotalEmissiveTriangles = 0;

            // Walk all instances and build one PrepareLightsTask per emissive primitive.
            for (uint32_t instanceIdx = 0; instanceIdx < totalGeometryInstances; ++instanceIdx)
            {
                const PerInstanceData& inst = scene.m_InstanceData[instanceIdx];

                const Scene::Material& cpuMat = scene.m_Materials[inst.m_MaterialIndex];
                const Vector3& emissive = cpuMat.m_EmissiveFactor;
                const bool hasEmissiveTexture = (cpuMat.m_EmissiveTexture >= 0);
                const bool isEmissive = hasEmissiveTexture ||
                    (emissive.x > 0.f || emissive.y > 0.f || emissive.z > 0.f);

                if (!isEmissive)
                    continue;

                // Find the MeshData to get the triangle count at LOD 0.
                const MeshData& meshData = scene.m_MeshData[inst.m_MeshDataIndex];
                const uint32_t triangleCount = meshData.m_IndexCounts[0] / 3u;
                if (triangleCount == 0)
                    continue;

                // Record the geometry-to-light mapping for this instance.
                // geometryIndex is always 0 (one primitive per PerInstanceData).
                const uint32_t geometryInstanceIndex = inst.m_FirstGeometryInstanceIndex; // == instanceIdx
                m_CachedGeometryInstanceToLight[geometryInstanceIndex] = m_TotalEmissiveTriangles;

                // Encode instanceIndex in high 19 bits, geometryIndex (0) in low 12 bits.
                SDL_assert(instanceIdx < (1u << 19));
                PrepareLightsTask task{};
                task.instanceAndGeometryIndex = (instanceIdx << 12) | 0u;
                task.lightBufferOffset        = m_TotalEmissiveTriangles;
                task.triangleCount            = triangleCount;
                task.previousLightBufferOffset = -1; // static — no temporal tracking needed

                m_CachedTriangleLightTasks.push_back(task);
                m_TotalEmissiveTriangles += triangleCount;
            }

            SDL_Log("[RTXDI] Triangle emissive lights: %u triangles across %u meshes",
                m_TotalEmissiveTriangles, (uint32_t)m_CachedTriangleLightTasks.size());
        }

        // Mark analytical lights dirty so the first frame rebuilds them.
        m_AnalyticalLightsDirty = true;
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

            auto makeHDR = [&](const char* name, RGTextureHandle& h)
            {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = nvrhi::Format::RGBA16_FLOAT;
                desc.m_NvrhiDesc.isUAV = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName = name;
                renderGraph.DeclareTexture(desc, h);
                };

            makeHDR("RTXDIRawDiffuseOutput", m_RG_RawDiffuseOutput);
            makeHDR("RTXDIRawSpecularOutput", m_RG_RawSpecularOutput);
            makeHDR("RTXDIDiffuseOutput", g_RG_RTXDIDiffuseOutput);
            makeHDR("RTXDISpecularOutput", g_RG_RTXDISpecularOutput);
        }
        else
        {
            // Non-denoised path: DI output textures read by DeferredRenderer
            auto makeHDR = [&](const char* name, RGTextureHandle& h) {
                RGTextureDesc desc;
                desc.m_NvrhiDesc.width = width;
                desc.m_NvrhiDesc.height = height;
                desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT;
                desc.m_NvrhiDesc.isUAV = true;
                desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
                desc.m_NvrhiDesc.debugName = name;
                renderGraph.DeclareTexture(desc, h);
                };
            makeHDR("RTXDIDIOutput", g_RG_RTXDIDIOutput);
            makeHDR("RTXDISpecularOutput", g_RG_RTXDISpecularOutput);
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

        // Previous-frame geo normals history.
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width       = width;
            desc.m_NvrhiDesc.height      = height;
            desc.m_NvrhiDesc.format      = nvrhi::Format::RG16_FLOAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName   = "GeoNormalsHistory";
            m_GeoNormalsHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GeoNormalsHistory);
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
            // Total local lights = triangle emissive lights + analytical local lights.
            // Infinite/env lights are NOT included in the local light PDF texture.
            const uint32_t analyticalLocalLights = static_cast<uint32_t>(m_CachedLightBufferParams.localLightBufferRegion.numLights);
            const uint32_t preparedLightCount = std::max<uint32_t>(1u,
                m_TotalEmissiveTriangles + analyticalLocalLights);

            // Find smallest power-of-2 S such that S*S >= preparedLightCount.
            m_PDFTexSize = 1u;
            while (m_PDFTexSize * m_PDFTexSize < preparedLightCount)
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
        renderGraph.ReadTexture(g_RG_GBufferGeoNormals);
        renderGraph.ReadTexture(g_RG_GBufferORM);
        renderGraph.ReadTexture(g_RG_GBufferMotionVectors);
        renderGraph.ReadTexture(g_RG_GBufferEmissive);

        // ------------------------------------------------------------------
        // FullSample per-frame resources
        // ------------------------------------------------------------------

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

        // Light data buffer (PolymorphicLightInfo per light, written by PrepareLights)
        // Must hold: triangle lights + analytical local lights + infinite lights + env light.
        {
            const uint32_t analyticalLights = static_cast<uint32_t>(m_CachedPrimitiveLights.size()) + 1u; // +1 for env
            const uint32_t maxLights = std::max(
                m_TotalEmissiveTriangles + analyticalLights, 1u);
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
            const uint32_t analyticalLights = static_cast<uint32_t>(m_CachedPrimitiveLights.size()) + 1u;
            const uint32_t maxLights = std::max(
                m_TotalEmissiveTriangles + analyticalLights, 1u);
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

        // PrepareLights task buffer — holds triangle tasks + analytical light tasks.
        // Triangle tasks are static (built at PostSceneLoad); analytical tasks are rebuilt
        // when lights are dirty. Total = triangle mesh tasks + analytical light count.
        {
            const uint32_t maxTasks = std::max(
                static_cast<uint32_t>(m_CachedTriangleLightTasks.size()) +
                static_cast<uint32_t>(m_CachedPrimitiveLights.size()),
                1u);
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
            const uint32_t maxLights = std::max(static_cast<uint32_t>(m_CachedPrimitiveLights.size()), 1u);
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

        // Rebuild analytical light buffer params only when any light node is dirty
        // (includes sun orientation/pitch changes tracked via m_LightsDirty, and
        //  the first frame after PostSceneLoad via m_AnalyticalLightsDirty).
        // Triangle lights are static and never need a per-frame rebuild.
        {
            const Vector3 sunDir = renderer->m_Scene.GetSunDirection();
            const bool sunChanged = (sunDir.x != m_CachedSunDirection.x ||
                                     sunDir.y != m_CachedSunDirection.y ||
                                     sunDir.z != m_CachedSunDirection.z);
            if (m_AnalyticalLightsDirty || renderer->m_Scene.m_LightsDirty || sunChanged)
            {
                m_CachedLightBufferParams = BuildLightBufferParams(m_CachedPrimitiveLights);
                m_CachedSunDirection      = sunDir;
                m_AnalyticalLightsDirty   = false;
            }
        }

        // Merge triangle lights into the light buffer layout.
        // Triangle lights occupy [0, m_TotalEmissiveTriangles) in the local light region.
        // Analytical local lights follow immediately after.
        RTXDI_LightBufferParameters lbp = m_CachedLightBufferParams;
        lbp.localLightBufferRegion.firstLightIndex = 0u;
        lbp.localLightBufferRegion.numLights = m_TotalEmissiveTriangles + m_CachedLightBufferParams.localLightBufferRegion.numLights;
        lbp.infiniteLightBufferRegion.firstLightIndex = lbp.localLightBufferRegion.numLights;
        lbp.infiniteLightBufferRegion.numLights = m_CachedLightBufferParams.infiniteLightBufferRegion.numLights;
        lbp.environmentLightParams.lightIndex = lbp.infiniteLightBufferRegion.firstLightIndex + lbp.infiniteLightBufferRegion.numLights;
        lbp.environmentLightParams.lightPresent = m_CachedLightBufferParams.environmentLightParams.lightPresent;

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
        g_Const.sceneConstants.sunDirection              = renderer->m_Scene.GetSunDirection();

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

            // When RELAX denoising is active and there is no indirect lighting pass,
            // ShadeSamples is the last pass writing to u_DiffuseLighting / u_SpecularLighting.
            // TODO: set this to '0' when we have a separate indirect pass, and then only set this back to '1' for the final shading of indirect
            restirDI.shadingParams.enableDenoiserInputPacking = renderer->m_EnableReSTIRDIRelaxDenoising ? 1u : 0u;

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
        nvrhi::TextureHandle geoNormalsTex    = renderGraph.GetTexture(g_RG_GBufferGeoNormals,    RGResourceAccessMode::Read);
        nvrhi::TextureHandle ormTex          = renderGraph.GetTexture(g_RG_GBufferORM,           RGResourceAccessMode::Read);
        nvrhi::TextureHandle motionTex       = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Read);
        nvrhi::TextureHandle emissiveTex     = renderGraph.GetTexture(g_RG_GBufferEmissive,      RGResourceAccessMode::Read);

        // History textures (persistent, reused from existing members)
        nvrhi::TextureHandle albedoHistoryTex  = renderGraph.GetTexture(m_GBufferAlbedoHistory,  RGResourceAccessMode::Write);
        nvrhi::TextureHandle ormHistoryTex     = renderGraph.GetTexture(m_GBufferORMHistory,     RGResourceAccessMode::Write);
        nvrhi::TextureHandle depthHistoryTex   = renderGraph.GetTexture(m_DepthHistory,          RGResourceAccessMode::Write);
        nvrhi::TextureHandle normalsHistoryTex = renderGraph.GetTexture(m_GbufferNormalsHistory, RGResourceAccessMode::Write);

        // FullSample per-frame textures (member variables)
        nvrhi::TextureHandle denoiserNRTex    = renderGraph.GetTexture(m_RG_DenoiserNormalRoughness, RGResourceAccessMode::Write);
        nvrhi::TextureHandle geoNormalsHistTex= renderGraph.GetTexture(m_GeoNormalsHistory,        RGResourceAccessMode::Write);
        nvrhi::TextureHandle linearDepthTex   = renderGraph.GetTexture(m_RG_LinearDepth,         RGResourceAccessMode::Write);
        nvrhi::TextureHandle compositedTex    = renderGraph.GetTexture(g_RG_RTXDIDIComposited,   RGResourceAccessMode::Write);

        // Light buffers
        nvrhi::BufferHandle  neighborOffsetsBuf  = renderGraph.GetBuffer(m_RG_NeighborOffsetsBuffer, m_AlbedoHistoryIsNew ?  RGResourceAccessMode::Write : RGResourceAccessMode::Read);
        nvrhi::BufferHandle  risBuffer           = renderGraph.GetBuffer(m_RG_RISBuffer,             RGResourceAccessMode::Write);
        nvrhi::BufferHandle  lightReservoirBuf   = renderGraph.GetBuffer(g_RG_RTXDILightReservoirBuffer, RGResourceAccessMode::Write);
        nvrhi::BufferHandle  risLightDataBuf     = renderGraph.GetBuffer(m_RG_RISLightDataBuffer,    RGResourceAccessMode::Write);
        nvrhi::TextureHandle localLightPDFTex    = renderGraph.GetTexture(m_RG_LocalLightPDFTexture, RGResourceAccessMode::Write);
        nvrhi::TextureHandle envLightPDFTex      = renderGraph.GetTexture(m_RG_EnvLightPDFTexture, RGResourceAccessMode::Write);
        nvrhi::BufferHandle  lightDataBuf        = renderGraph.GetBuffer(m_RG_LightDataBuffer,       RGResourceAccessMode::Write);
        nvrhi::BufferHandle  lightIndexMapBuf    = renderGraph.GetBuffer(m_RG_LightIndexMapping,     RGResourceAccessMode::Write);
        nvrhi::BufferHandle  geoInstToLightBuf   = renderGraph.GetBuffer(m_RG_GeometryInstanceToLight, RGResourceAccessMode::Write);

        // Match FullSample: clear temporal light-index mapping and local-light PDF mip0 every frame.
        commandList->clearBufferUInt(lightIndexMapBuf, 0u);
        commandList->clearTextureFloat(
            localLightPDFTex,
            nvrhi::TextureSubresourceSet(0, 1, 0, 1),
            nvrhi::Color(0.f));

        // Build Environment-Light PDF mip-0 from Bruneton sky luminance.
        // This replaces the old uniform clear (1.0) with an importance-sampled PDF
        // that reflects the actual sky distribution (bright horizon, dark zenith, etc.).
        // When sky is disabled, the shader writes uniform 1.0 as a fallback.
        {
            nvrhi::BindingSetDesc buildEnvPDFBset;
            buildEnvPDFBset.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, rtxdiCB),
                nvrhi::BindingSetItem::Texture_UAV(0, envLightPDFTex,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
            };
            renderer->AddComputePass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/LightingPasses/Presampling/BuildEnvLightPDF_main",
                .bindingSetDesc = buildEnvPDFBset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(k_EnvPDFTexSize, 8u),
                    .y = DivideAndRoundUp(k_EnvPDFTexSize, 8u),
                    .z = 1u
                }
            });
        }

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
        if (m_GeoNormalsHistoryIsNew)
            commandList->copyTexture(geoNormalsHistTex, nvrhi::TextureSlice{}, geoNormalsTex, nvrhi::TextureSlice{});

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
        // RAB_Buffers.hlsli binding layout (all passes that use the shared bset)
        // b0  = ResamplingConstants (g_Const)
        // t0  = t_NeighborOffsets
        // t1  = t_GBufferDepth
        // t2  = t_GBufferGeoNormals
        // t3  = t_GBufferAlbedo
        // t4  = t_GBufferORM
        // t5  = t_GBufferNormals
        // t6  = t_PrevGBufferNormals
        // t7  = t_PrevGBufferGeoNormals
        // t8  = t_PrevGBufferAlbedo
        // t9  = t_PrevGBufferORM
        // t10 = t_MotionVectors
        // t11 = t_DenoiserNormalRoughness
        // t12 = t_PrevGBufferDepth
        // t13 = t_LocalLightPdfTexture
        // t14 = t_EnvironmentPdfTexture
        // t15 = SceneBVH
        // t16 = PrevSceneBVH
        // t17 = t_LightDataBuffer
        // t18 = t_GBufferEmissive
        // t19 = t_GeometryInstanceToLight
        // t20 = t_InstanceData
        // t21 = t_GeometryData
        // t22 = t_MaterialConstants
        // t23 = t_SceneIndices
        // t24 = t_SceneVertices
        // u0  = u_LightReservoirs
        // u1  = u_RisBuffer
        // u2  = u_RisLightDataBuffer
        // u3  = u_RestirLuminance         (stub)
        // u4  = u_GIReservoirs            (stub)
        // u5  = u_SecondaryGBuffer        (stub — future GI)
        // u6  = u_DiffuseLighting
        // u7  = u_SpecularLighting
        // ------------------------------------------------------------------

        nvrhi::BindingSetDesc bset;
        bset.bindings = {
            // Constant buffers
            nvrhi::BindingSetItem::ConstantBuffer(0, rtxdiCB),
            // SRVs
            nvrhi::BindingSetItem::TypedBuffer_SRV(0,  neighborOffsetsBuf),
            nvrhi::BindingSetItem::Texture_SRV(1,  depthTex),
            nvrhi::BindingSetItem::Texture_SRV(2,  geoNormalsTex),
            nvrhi::BindingSetItem::Texture_SRV(3,  albedoTex),
            nvrhi::BindingSetItem::Texture_SRV(4,  ormTex),
            nvrhi::BindingSetItem::Texture_SRV(5,  normalsTex),
            nvrhi::BindingSetItem::Texture_SRV(6,  normalsHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(7,  geoNormalsHistTex),
            nvrhi::BindingSetItem::Texture_SRV(8,  albedoHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(9,  ormHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(10, motionTex),
            nvrhi::BindingSetItem::Texture_SRV(11, denoiserNRTex),
            nvrhi::BindingSetItem::Texture_SRV(12, depthHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(13, localLightPDFTex),
            nvrhi::BindingSetItem::Texture_SRV(14, envLightPDFTex),
            nvrhi::BindingSetItem::RayTracingAccelStruct(15, renderer->m_Scene.m_TLAS),
            nvrhi::BindingSetItem::RayTracingAccelStruct(16, m_TLASHistory),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(17, lightDataBuf),
            nvrhi::BindingSetItem::Texture_SRV(18, emissiveTex),
            nvrhi::BindingSetItem::TypedBuffer_SRV(19, geoInstToLightBuf),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(20, renderer->m_Scene.m_InstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(21, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(22, renderer->m_Scene.m_MaterialConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(23, renderer->m_Scene.m_IndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(24, renderer->m_Scene.m_VertexBufferQuantized),
            // UAVs
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, lightReservoirBuf),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, risBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(2, risLightDataBuf),
            nvrhi::BindingSetItem::Texture_UAV(3, cr.DummyUAVTexture),                    // u_RestirLuminance stub
            nvrhi::BindingSetItem::StructuredBuffer_UAV(4, cr.DummyUAVStructuredBuffer),  // u_GIReservoirs stub
            nvrhi::BindingSetItem::StructuredBuffer_UAV(5, cr.DummyUAVStructuredBuffer),  // u_SecondaryGBuffer stub
            nvrhi::BindingSetItem::Texture_UAV(6, bDenoise ? rawDiffuseTex  : diOutputTex),
            nvrhi::BindingSetItem::Texture_UAV(7, bDenoise ? rawSpecularTex : specularOutTex),
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
            ppFbDesc.setDepthAttachment(depthTex);
            nvrhi::FramebufferHandle ppFb = device->createFramebuffer(ppFbDesc);

            // Surfaces Pass (Stencil == 1)
            nvrhi::DepthStencilState ds;
            ds.depthTestEnable = false;
            ds.depthWriteEnable = false;
            ds.stencilEnable = true;
            ds.stencilRefValue = 1;
            ds.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Equal;

            renderer->AddFullScreenPass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/PostprocessGBuffer_main",
                .bindingSetDesc = ppBset,
                .bIncludeBindlessResources = false,
                .framebuffer    = ppFb,
                .depthStencilState = &ds,
            });
        }

        // ------------------------------------------------------------------
        // PrepareLights
        // Combines triangle emissive light tasks (static, built at PostSceneLoad)
        // with analytical light tasks (rebuilt when lights are dirty).
        //
        // Layout in the light data buffer:
        //   [0 .. m_TotalEmissiveTriangles)          — triangle lights
        //   [m_TotalEmissiveTriangles .. numLocal)   — analytical local lights
        //   [numLocal .. numLocal+numInfinite)        — infinite lights (directional)
        //   [numLocal+numInfinite]                    — environment light (written directly)
        //
        // The combined task list is sorted by lightBufferOffset so FindTask()
        // binary search works correctly.
        // ------------------------------------------------------------------
        {
            PROFILE_SCOPED("PrepareLights");

            // ---- Build combined task list ----
            // Triangle tasks are already sorted by lightBufferOffset (built in PostSceneLoad).
            // Analytical tasks follow immediately after triangle lights.
            std::vector<PrepareLightsTask> allTasks;
            allTasks.reserve(m_CachedTriangleLightTasks.size() + m_CachedPrimitiveLights.size());

            // 1. Triangle light tasks (static, lightBufferOffset starts at 0)
            allTasks.insert(allTasks.end(), m_CachedTriangleLightTasks.begin(), m_CachedTriangleLightTasks.end());

            // 2. Analytical light tasks (TASK_PRIMITIVE_LIGHT_BIT set)
            //    lightBufferOffset starts right after all triangle lights.
            const uint32_t analyticalBaseOffset = m_TotalEmissiveTriangles;
            for (uint32_t i = 0; i < static_cast<uint32_t>(m_CachedPrimitiveLights.size()); ++i)
            {
                PrepareLightsTask task{};
                task.instanceAndGeometryIndex  = TASK_PRIMITIVE_LIGHT_BIT | i;
                task.triangleCount             = 1; // one thread per analytical light
                task.lightBufferOffset         = analyticalBaseOffset + i;
                task.previousLightBufferOffset = -1;
                allTasks.push_back(task);
            }

            const uint32_t totalThreads = m_TotalEmissiveTriangles + static_cast<uint32_t>(m_CachedPrimitiveLights.size());

            if (totalThreads > 0)
            {
                // Upload geometry-to-light mapping (static, built at PostSceneLoad).
                if (!m_CachedGeometryInstanceToLight.empty())
                {
                    commandList->writeBuffer(geoInstToLightBuf, m_CachedGeometryInstanceToLight.data(), m_CachedGeometryInstanceToLight.size() * sizeof(uint32_t));
                }

                // Upload combined task buffer.
                commandList->writeBuffer(prepareLightsTaskBuf, allTasks.data(), allTasks.size() * sizeof(PrepareLightsTask));

                // Upload analytical (primitive) lights.
                if (!m_CachedPrimitiveLights.empty())
                {
                    commandList->writeBuffer(primitiveLightBuf, m_CachedPrimitiveLights.data(), m_CachedPrimitiveLights.size() * sizeof(PolymorphicLightInfo));
                }

                PrepareLightsConstants plCB{};
                plCB.numTasks                = static_cast<uint32_t>(allTasks.size());
                plCB.currentFrameLightOffset = 0u;
                plCB.previousFrameLightOffset = 0u;

                nvrhi::BufferHandle plCBHandle = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(PrepareLightsConstants), "PrepareLightsCB", 1));
                commandList->writeBuffer(plCBHandle, &plCB, sizeof(plCB));

            nvrhi::BindingSetDesc plBset;
                plBset.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, plCBHandle),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(0,  prepareLightsTaskBuf),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(1,  primitiveLightBuf),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(2,  renderer->m_Scene.m_InstanceDataBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(3,  renderer->m_Scene.m_MeshDataBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(4,  renderer->m_Scene.m_MaterialConstantsBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(5,  renderer->m_Scene.m_IndexBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(6,  renderer->m_Scene.m_VertexBufferQuantized),
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(0,  lightDataBuf),
                    nvrhi::BindingSetItem::TypedBuffer_UAV(1,       lightIndexMapBuf),
                    nvrhi::BindingSetItem::Texture_UAV(2, localLightPDFTex,
                        nvrhi::Format::UNKNOWN,
                        nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
                };
                renderer->AddComputePass({
                    .commandList    = commandList,
                    .shaderName     = "rtxdi/PrepareLights_main",
                    .bindingSetDesc = plBset,
                    .bIncludeBindlessResources = true,
                    .dispatchParams = {
                        .x = DivideAndRoundUp(totalThreads, 256u),
                        .y = 1u,
                        .z = 1u
                    }
                });
            }
        }

        // ------------------------------------------------------------------
        // Write the kEnvironment light directly to the light data buffer.
        // This bypasses PrepareLights.hlsl (which would write getPower()=0 to
        // the local light PDF texture at the env light's index, corrupting it).
        // The env light entry is at lbp.environmentLightParams.lightIndex.
        // ------------------------------------------------------------------
        if (renderer->m_EnableSky)
        {
            auto f32tof16 = [](float v) -> uint32_t {
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                uint32_t sign     = (bits >> 31u) & 1u;
                uint32_t exponent = (bits >> 23u) & 0xFFu;
                uint32_t mantissa = bits & 0x7FFFFFu;
                if (exponent == 0u) return sign << 15u;
                int32_t  e16 = static_cast<int32_t>(exponent) - 127 + 15;
                if (e16 <= 0) return sign << 15u;
                if (e16 >= 31) return (sign << 15u) | (0x1Fu << 10u);
                return (sign << 15u) | (static_cast<uint32_t>(e16) << 10u) | (mantissa >> 13u);
            };

            PolymorphicLightInfo envInfo{};
            // Type = kEnvironment
            envInfo.colorTypeAndFlags = static_cast<uint32_t>(PolymorphicLightType::kEnvironment)
                                      << kPolymorphicLightTypeShift;
            // radianceScale = white (1,1,1) — actual radiance is computed procedurally in the shader
            PackLightColor({ 1.f, 1.f, 1.f }, envInfo);
            // direction1 = 0xFFFFFFFF → textureIndex = -1 → no texture, use procedural Bruneton sky
            envInfo.direction1 = static_cast<uint32_t>(-1);
            // scalars: low 16 bits = rotation (0.0f), high 16 bits = importanceSampled (1)
            envInfo.scalars = f32tof16(0.0f) | (f32tof16(1.0f) << 16u);
            // direction2: texture size (width in low 16 bits, height in high 16 bits)
            envInfo.direction2 = (k_EnvPDFTexSize & 0xFFFFu) | (k_EnvPDFTexSize << 16u);

            const uint64_t byteOffset = static_cast<uint64_t>(lbp.environmentLightParams.lightIndex)
                                      * sizeof(PolymorphicLightInfo);
            commandList->writeBuffer(lightDataBuf, &envInfo, sizeof(envInfo), byteOffset);
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
        // Mip-0 is now written every frame by BuildEnvLightPDF_main (above),
        // which samples the Bruneton sky to produce an importance-sampled PDF.
        // We generate the remaining mips via SPD and then presample.
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
                nvrhi::BindingSetItem::Texture_SRV(0,  albedoTex),   // t_GBufferAlbedo  (RGBA8_UNORM)
                nvrhi::BindingSetItem::Texture_SRV(1,  ormTex),      // t_GBufferORM     (RG8_UNORM: roughness=.r, metallic=.g)
                nvrhi::BindingSetItem::Texture_SRV(2,  emissiveTex), // t_GBufferEmissive
                // DI illumination inputs (denoised or raw)
                nvrhi::BindingSetItem::Texture_SRV(3,  bDenoise ? denoisedDiffuseTex  : diOutputTex),
                nvrhi::BindingSetItem::Texture_SRV(4,  bDenoise ? denoisedSpecularTex : specularOutTex),
            };

            nvrhi::FramebufferDesc ppFbDesc;
            ppFbDesc.addColorAttachment(compositedTex);
            ppFbDesc.setDepthAttachment(depthTex);
            nvrhi::FramebufferHandle ppFb = device->createFramebuffer(ppFbDesc);

            // Surfaces Pass (Stencil == 1)
            nvrhi::DepthStencilState ds;
            ds.depthTestEnable = false;
            ds.depthWriteEnable = false;
            ds.stencilEnable = true;
            ds.stencilRefValue = 1;
            ds.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Equal;

            renderer->AddFullScreenPass({
                .commandList    = commandList,
                .shaderName     = "rtxdi/CompositingPass_CompositingPass_PSMain",
                .bindingSetDesc = compBset,
                .bIncludeBindlessResources = false,
                .framebuffer    = ppFb,
                .depthStencilState = &ds
            });
        }

        // ------------------------------------------------------------------
        // Copy current G-buffer to history textures for next frame
        // ------------------------------------------------------------------
        {
            PROFILE_GPU_SCOPED("Copy resources to History", commandList);

            commandList->copyTexture(albedoHistoryTex, nvrhi::TextureSlice{}, albedoTex, nvrhi::TextureSlice{});
            commandList->copyTexture(ormHistoryTex, nvrhi::TextureSlice{}, ormTex, nvrhi::TextureSlice{});
            commandList->copyTexture(depthHistoryTex, nvrhi::TextureSlice{}, depthTex, nvrhi::TextureSlice{});
            commandList->copyTexture(normalsHistoryTex, nvrhi::TextureSlice{}, normalsTex, nvrhi::TextureSlice{});
            commandList->copyTexture(geoNormalsHistTex, nvrhi::TextureSlice{}, geoNormalsTex, nvrhi::TextureSlice{});

            // Copy current TLAS to history for next frame
            if (m_TLASHistory && renderer->m_Scene.m_TLAS)
            {
                commandList->copyRaytracingAccelerationStructure(m_TLASHistory, renderer->m_Scene.m_TLAS);
            }
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

    static void PackLightColor(const Vector3& color, PolymorphicLightInfo& info)
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
    static DirectX::XMFLOAT2 UnitVecToOct(const Vector3& n)
    {
        float m = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
        float ox = n.x / m, oy = n.y / m;
        if (n.z <= 0.f)
        {
            float sx = ox >= 0.f ? 1.f : -1.f;
            float sy = oy >= 0.f ? 1.f : -1.f;
            float newOx = (1.f - std::abs(oy)) * sx;
            float newOy = (1.f - std::abs(ox)) * sy;
            ox = newOx;
            oy = newOy;
        }
        return { ox, oy };
    }

    static uint32_t PackNormalizedVector(const Vector3& v)
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

        // Direction from node rotation quaternion (same convention as glTF spot lights)
        auto QuatToDir = [](const DirectX::XMFLOAT4& q) -> Vector3 {
            // Forward = local -Z rotated by q
            float x = q.x, y = q.y, z = q.z, w = q.w;
            return {
                -2.f*(x*z + w*y),
                -2.f*(y*z - w*x),
                -1.f + 2.f*(x*x + y*y)
            };
        };

        auto Normalize3 = [](Vector3 v) -> Vector3 {
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
            Vector3 col = { light.m_Color.x * radiance,
                                      light.m_Color.y * radiance,
                                      light.m_Color.z * radiance };

            out.colorTypeAndFlags = static_cast<uint32_t>(PolymorphicLightType::kDirectional)
                                    << kPolymorphicLightTypeShift;
            PackLightColor(col, out);
            // Use toward-sun convention (+Z forward), consistent with deferred path's GetSunDirection().
            const Vector3 sunDir = Renderer::GetInstance()->m_Scene.GetSunDirection();
            out.direction1 = PackNormalizedVector(sunDir);
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
                Vector3 col = { light.m_Color.x * radiance,
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
                Vector3 flux = { light.m_Color.x * light.m_Intensity,
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
            float softness = (light.m_SpotOuterConeAngle > 0.f)
                ? std::max(0.f, std::min(1.f, 1.f - light.m_SpotInnerConeAngle / light.m_SpotOuterConeAngle))
                : 0.f;

            out.center = { node.m_Translation.x, node.m_Translation.y, node.m_Translation.z };

            if (light.m_Radius > 0.f)
            {
                float projArea = DirectX::XM_PI * light.m_Radius * light.m_Radius;
                float radiance = (projArea > 0.f) ? light.m_Intensity / projArea : 0.f;
                Vector3 col = { light.m_Color.x * radiance,
                                          light.m_Color.y * radiance,
                                          light.m_Color.z * radiance };

                out.colorTypeAndFlags = (static_cast<uint32_t>(PolymorphicLightType::kSphere)
                                         << kPolymorphicLightTypeShift)
                                      | kPolymorphicLightShapingEnableBit;
                PackLightColor(col, out);
                out.scalars = PackFloat2ToUint(light.m_Radius, 0.f);
            }
            else
            {
                Vector3 flux = { light.m_Color.x * light.m_Intensity,
                                           light.m_Color.y * light.m_Intensity,
                                           light.m_Color.z * light.m_Intensity };

                out.colorTypeAndFlags = (static_cast<uint32_t>(PolymorphicLightType::kPoint)
                                         << kPolymorphicLightTypeShift)
                                      | kPolymorphicLightShapingEnableBit;
                PackLightColor(flux, out);
            }

            Vector3 dir = Normalize3(QuatToDir(node.m_Rotation));
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
    // Also fills outPrimitiveLights with converted PolymorphicLightInfo entries,
    // packed in RTXDI expected order: local finite lights first, infinite lights after.
    static RTXDI_LightBufferParameters BuildLightBufferParams(std::vector<PolymorphicLightInfo>& outPrimitiveLights)
    {
        const Renderer* renderer = Renderer::GetInstance();

        RTXDI_LightBufferParameters lbp{};

        std::vector<PolymorphicLightInfo> localLights;
        std::vector<PolymorphicLightInfo> infiniteLights;
        localLights.reserve(renderer->m_Scene.m_LightCount);
        infiniteLights.reserve(renderer->m_Scene.m_LightCount);

        for (uint32_t i = 0; i < renderer->m_Scene.m_LightCount; ++i)
        {
            const Scene::Light& light = renderer->m_Scene.m_Lights[i];
            SDL_assert(light.m_NodeIndex >= 0);
            const Scene::Node& node = renderer->m_Scene.m_Nodes[light.m_NodeIndex];

            PolymorphicLightInfo info{};
            if (!ConvertAnalyticalLight(light, node, info))
                continue;

            if (light.m_Type == Scene::Light::Directional)
            {
                // Node rotation is kept in sync by SetSunPitchYaw, so
                // ConvertAnalyticalLight already produces the correct direction1
                // from GetSunDirection() (toward-sun convention). No override needed.
                infiniteLights.push_back(info);
            }
            else
                localLights.push_back(info);
        }

        outPrimitiveLights.clear();
        outPrimitiveLights.reserve(localLights.size() + infiniteLights.size());
        outPrimitiveLights.insert(outPrimitiveLights.end(), localLights.begin(), localLights.end());
        outPrimitiveLights.insert(outPrimitiveLights.end(), infiniteLights.begin(), infiniteLights.end());

        lbp.localLightBufferRegion.firstLightIndex    = 0u;
        lbp.localLightBufferRegion.numLights          = static_cast<uint32_t>(localLights.size());
        lbp.infiniteLightBufferRegion.firstLightIndex = lbp.localLightBufferRegion.numLights;
        lbp.infiniteLightBufferRegion.numLights       = static_cast<uint32_t>(infiniteLights.size());

        // The kEnvironment light is NOT added to outPrimitiveLights because PrepareLights.hlsl
        // would write getPower()=0 to the local light PDF texture at the env light's index,
        // corrupting the PDF. Instead, the env light PolymorphicLightInfo is written directly
        // to the light data buffer in Execute() via commandList->writeBuffer after PrepareLights.
        const uint32_t envLightIndex = lbp.infiniteLightBufferRegion.firstLightIndex
                                     + lbp.infiniteLightBufferRegion.numLights;
        lbp.environmentLightParams.lightPresent = renderer->m_EnableSky ? 1u : 0u;
        lbp.environmentLightParams.lightIndex   = envLightIndex;

        return lbp;
    }
};

REGISTER_RENDERER(RTXDIRenderer);
