// ImGui Shaders
// Converted from GLSL (imgui_impl_vulkan.cpp) to HLSL for DXC compilation

//-----------------------------------------------------------------------------
// VERTEX SHADER
//-----------------------------------------------------------------------------

// Include shared types
#include "ShaderShared.h"

// Instantiate the push-constant variable used by the ImGui shaders
PUSH_CONSTANT
ImGuiPushConstants pushConstants;

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
    output.Position = float4(input.aPos * pushConstants.uScale + pushConstants.uTranslate, 0.0f, 1.0f);
    return output;
}

//-----------------------------------------------------------------------------
// PIXEL SHADER
//-----------------------------------------------------------------------------

Texture2D sTexture : register(t0);
SamplerState sSampler : register(s0);

struct PSInput
{
    float4 Position : SV_POSITION;
    float4 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.Color * sTexture.Sample(sSampler, input.UV);
}
