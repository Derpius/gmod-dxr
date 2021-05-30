SamplerState gSampler : register(s0);
Texture2D<float4> gInput;
Texture2D<float4> gLuminance;

cbuffer PerFrameCB {
	float3x4 gColourTransform;
}

// Simplified and slightly modified version of https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/RenderPasses/ToneMapper/ToneMapping.ps.slang
static const float kExposureKey = 0.042;
static const float kLuminanceLod = 16.0; // Lookup highest mip level to get average luminance

float calcLuminance(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

float3 toneMapAces(float3 color)
{
    // Cancel out the pre-exposure mentioned in
    // https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
    color *= 0.6;

    float A = 2.51;
    float B = 0.03;
    float C = 2.43;
    float D = 0.59;
    float E = 0.14;

    color = saturate((color * (A * color + B)) / (color * (C * color + D) + E));
    return color;
}

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float4 color = gInput.Sample(gSampler, texC);
    float3 finalColor = color.rgb;

    // apply auto exposure
    float avgLuminance = exp2(gLuminance.SampleLevel(gSampler, texC, kLuminanceLod).r);
    float pixelLuminance = calcLuminance(finalColor);
    finalColor *= (kExposureKey / avgLuminance);

    // apply color grading
    finalColor = mul(finalColor, (float3x3)gColourTransform);

    // apply tone mapping
    finalColor = saturate(toneMapAces(finalColor));

    return float4(finalColor, color.a);
}