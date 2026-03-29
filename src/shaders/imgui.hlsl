// Include shared types
#include "ShaderShared.h"
#include "srrhi/hlsl/ImGui.hlsli"

static const srrhi::ImGuiConstants ImGuiCB      = srrhi::ImGuiInputs::GetImGuiConstants();
static const Texture2D<float4>     ImGuiTexture = srrhi::ImGuiInputs::GetFontTexture();

struct VSInput
{
    float2 aPos    : POSITION;
    float2 aUV     : TEXCOORD0;
    float4 aColor  : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float4 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Color = input.aColor;
    output.UV = input.aUV;
    output.Position = float4(input.aPos * ImGuiCB.uScale + ImGuiCB.uTranslate, 0.0f, 1.0f);
    return output;
}

struct PSInput
{
    float4 Position : SV_POSITION;
    float4 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    SamplerState linearClampSampler = SamplerDescriptorHeap[srrhi::CommonConsts::SAMPLER_LINEAR_CLAMP_INDEX];
    return input.Color * ImGuiTexture.Sample(linearClampSampler, input.UV);
}
