SamplerState gSampler : register(s0);

Texture2D<float4> gInput;
RWTexture2D<float4> gOutput;
RWTexture2D<float4> gSumBuffer;

cbuffer PerFrameCB {
	uint gSamples;
	uint2 gResolution;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (any(dispatchThreadId.xy >= gResolution)) return;
	const uint2 pixel = dispatchThreadId.xy;

	float4 sum = gInput[pixel] + gSumBuffer[pixel];
	gOutput[pixel] = sum / (gSamples + 1);
	gSumBuffer[pixel] = sum;
}