/***************************************************************************
 # Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
import Scene.Lights.Lights;
import Scene.Shading;
import Scene.Raytracing;
import Utils.Helpers;

cbuffer PerFrameCB
{
	float4x4 invView;
	float2 viewportDims;
	float tanHalfFovY;
	uint sampleIndex;
	bool useDOF;
	float4 kClearColour;
};

struct PrimaryRayData
{
	float4 colour;
	uint depth;
	float hitT;
};

struct ShadowRayData
{
	bool hit;
};

[shader("miss")]
void shadowMiss(inout ShadowRayData hitData)
{
	hitData.hit = false;
}

[shader("anyhit")]
void shadowAnyHit(inout ShadowRayData hitData, in BuiltInTriangleIntersectionAttributes attribs)
{
	hitData.hit = true;
}

[shader("miss")]
void primaryMiss(inout PrimaryRayData hitData)
{
	hitData.colour = kClearColour;
	hitData.hitT = -1;
}

[shader("closesthit")]
void primaryClosestHit(
	uniform HitShaderParams hitParams,
	inout PrimaryRayData hitData, in BuiltInTriangleIntersectionAttributes attribs
) {
	VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);

	hitData.colour.rgb = gScene.materials[gScene.getMaterialID(hitParams.getGlobalHitID())].baseColor.rgb * max(dot(gScene.getLight(0).dirW, v.normalW), 0.3);
	hitData.colour.a = 1;
	hitData.hitT = RayTCurrent();
}

[shader("anyhit")]
void primaryAnyHit(
	uniform HitShaderParams hitParams,
	inout PrimaryRayData rayData,
	BuiltInTriangleIntersectionAttributes attribs
) {
	// Alpha test for non-opaque geometry.
	VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
	uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
	if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) {
		IgnoreHit();
	}
}

[shader("raygeneration")]
void rayGen(
	uniform RWTexture2D<float4> gOutput)
{
	uint3 launchIndex = DispatchRaysIndex();
	uint randSeed = rand_init(launchIndex.x + launchIndex.y * viewportDims.x, sampleIndex, 16);

	RayDesc ray;
	if (!useDOF) {
		ray = gScene.camera.computeRayPinhole(launchIndex.xy, viewportDims).toRayDesc();
	}
	else {
		float2 u = float2(rand_next(randSeed), rand_next(randSeed));
		ray = gScene.camera.computeRayThinlens(launchIndex.xy, viewportDims, u).toRayDesc();
	}

	PrimaryRayData hitData;
	hitData.depth = 0;
	TraceRay(gRtScene, 0 /*rayFlags*/, 0xFF, 0 /* ray index*/, hitProgramCount, 0, ray, hitData);
	gOutput[launchIndex.xy] = hitData.colour;
}
