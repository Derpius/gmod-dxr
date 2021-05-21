SamplerState gSampler : register(s0);

Texture2D<float4> gInput;
RWTexture2D<float4> gOutput;
RWTexture2D<float4> gSumBuffer;
RWTexture2D<float4> gCorrectionBuffer;

cbuffer PerFrameCB {
	uint gSamples;
	uint2 gResolution;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	// Compensated accumulation taken from Falcor's accumulation render pass
    if (any(dispatchThreadId.xy >= gResolution)) return;
    const uint2 pixelPos = dispatchThreadId.xy;
    const float4 curColor = gInput[pixelPos];

    // Fetch the previous sum and running compensation term.
    float4 sum = gSumBuffer[pixelPos];
    float4 c = gCorrectionBuffer[pixelPos];                // c measures how large (+) or small (-) the current sum is compared to what it should be.

    // Adjust current value to minimize the running error.
    // Compute the new sum by adding the adjusted current value.
    float4 y = curColor - c;
    float4 sumNext = sum + y;                           // The value we'll see in 'sum' on the next iteration.
    float4 output = sumNext / (gSamples + 1);

    gSumBuffer[pixelPos] = sumNext;
    gCorrectionBuffer[pixelPos] = (sumNext - sum) - y;     // Store new correction term.
    gOutput[pixelPos] = output;
}