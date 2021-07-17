SamplerState gSampler : register(s0);
Texture2D<float4> gInput;
Texture2D<float4> gLuminance;

Texture2D<float4> gLut;
#define WIDTH 256.f
#define HEIGHT 16.f
#define COLOURS 16.f
#define MAXCOLOUR 15.f

cbuffer PerFrameCB {
	float3x4 gColourTransform;
	bool useLut;
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
    float4 colour = gInput.Sample(gSampler, texC);
    float3 finalColour = colour.rgb;

    // apply auto exposure
    float avgLuminance = exp2(gLuminance.SampleLevel(gSampler, texC, kLuminanceLod).r);
    float pixelLuminance = calcLuminance(finalColour);
    finalColour *= (kExposureKey / avgLuminance);

    // apply white balance
    finalColour = mul(finalColour, (float3x3)gColourTransform);

    // apply tone mapping
    finalColour = saturate(toneMapAces(finalColour));

	// apply lut
	if (useLut) {
		// https://defold.com/tutorials/grading/
		float cell = finalColour.b * MAXCOLOUR;

		float cell_l = floor(cell);
		float cell_h = ceil(cell);

		float half_px_x = 0.5 / WIDTH;
		float half_px_y = 0.5 / HEIGHT;
		float r_offset = half_px_x + finalColour.r / COLOURS * (MAXCOLOUR / COLOURS);
		float g_offset = half_px_y + finalColour.g * (MAXCOLOUR / COLOURS);

		float2 lut_pos_l = float2(cell_l / COLOURS + r_offset, g_offset);
		float2 lut_pos_h = float2(cell_h / COLOURS + r_offset, g_offset);

		float3 graded_color_l = gLut.Sample(gSampler, lut_pos_l).rgb;
		float3 graded_color_h = gLut.Sample(gSampler, lut_pos_h).rgb;

		finalColour = lerp(graded_color_l, graded_color_h, frac(cell));
	}

    return float4(finalColour, colour.a);
}