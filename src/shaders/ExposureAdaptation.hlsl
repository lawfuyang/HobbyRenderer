#include "srrhi/hlsl/HDR.hlsli"

static const srrhi::AdaptationConstants AdaptationCB = srrhi::ExposureAdaptationInputs::GetAdaptationConstants();

static const RWBuffer<float> Exposure = srrhi::ExposureAdaptationInputs::GetExposure();
static const StructuredBuffer<uint> HistogramInput = srrhi::ExposureAdaptationInputs::GetHistogramInput();

groupshared float SharedWeights[256];

[numthreads(256, 1, 1)]
void ExposureAdaptation_CSMain(uint threadIdx : SV_GroupIndex)
{
    uint count = HistogramInput[threadIdx];
    
    float range = AdaptationCB.m_MaxLogLuminance - AdaptationCB.m_MinLogLuminance;
    float logLum = threadIdx == 0 ? AdaptationCB.m_MinLogLuminance : (AdaptationCB.m_MinLogLuminance + (float(threadIdx - 1) / 254.0f) * range);
    SharedWeights[threadIdx] = float(count) * logLum;

    GroupMemoryBarrierWithGroupSync();

    for (uint i = 128; i > 0; i >>= 1)
    {
        if (threadIdx < i)
            SharedWeights[threadIdx] += SharedWeights[threadIdx + i];
        GroupMemoryBarrierWithGroupSync();
    }

    if (threadIdx == 0)
    {
        float avgLogLum = SharedWeights[0] / max(float(AdaptationCB.m_NumPixels), 1.0f);
        float avgLum = exp2(avgLogLum);
        
        // Convert Luminance -> EV100
        // EV100 = log2(L * S / K) where S=100, K=12.5
        float EV100 = log2(avgLum * 100.0f / 12.5f);
        
        // Clamp EV instead of luminance
        EV100 = clamp(EV100, AdaptationCB.m_ExposureValueMin, AdaptationCB.m_ExposureValueMax);
        
        // Add exposure compensation (subtract because lower EV = more exposure)
        EV100 -= AdaptationCB.m_ExposureCompensation;
        
        // Convert EV -> Multiplier (using 1.2 calibration factor)
        float targetExposure = 1.0f / (pow(2.0f, EV100) * 1.2f);
        
        float currentExposure = Exposure[0];
        float exposure = currentExposure + (targetExposure - currentExposure) * (1.0f - exp(-AdaptationCB.m_DeltaTime * AdaptationCB.m_AdaptationSpeed));
        
        Exposure[0] = exposure;
    }
}
