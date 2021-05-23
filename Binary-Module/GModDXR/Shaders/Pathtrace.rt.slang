#include "Utils/Math/MathConstants.slangh"

#define _USE_LEGACY_SHADING_CODE 0

import Scene.Shading;
import Scene.Raytracing;

import Utils.Helpers;
import Utils.Math.MathHelpers;

import Utils.Sampling.SampleGenerator;

import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;
import Experimental.Scene.Lights.EmissiveLightSampler;

#define MIN_COS_THETA 0.f

cbuffer PerFrameCB
{
	float4x4 invView;
	float2 viewportDims;
	float tanHalfFovY;
	uint sampleIndex;
	bool useDOF;
	float4 kClearColour;
	bool bSampleEmissives;
	EmissiveLightSampler emissiveSampler;
};

struct PrimaryRayData
{
	float4 colour;
	float hitT;
	uint3 launchIndex;
};

struct IndirectRayData
{
	bool terminated;
	float3 colour;
	float3 origin;
	float3 direction;
	float3 throughput;
	SampleGenerator sg;
	float pdfLast;
}

struct ShadowRayData
{
	bool visible;
};

// evalMIS is a simplified version of the one found in PathTracerHelpers.slang
/** Evaluates the currently configured heuristic for multiple importance sampling (MIS).
	This version assumes one sample is taken from each of the sampling strategies.
	\param[in] p0 Pdf for the first sampling strategy.
	\param[in] p1 Pdf for the second sampling strategy.
	\return Weight for the contribution from the first strategy (p0).
*/
float evalMIS(float p0, float p1)
{
	float p02 = p0 * p0;
	return p02 / (p02 + p1 * p1);
}

// traceShadowRay and evalDirectAnalytic taken from MinimalPathTracer.rt.slang
/** Traces a shadow ray towards a light source.
	\param[in] origin Ray origin for the shadow ray.
	\param[in] dir Direction from shading point towards the light source (normalized).
	\param[in] distance Distance to the light source.
	\return True if light is visible, false otherwise.
*/
bool traceShadowRay(float3 origin, float3 dir, float distance)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.f;
	ray.TMax = distance;

	ShadowRayData rayData;
	rayData.visible = false;    // Set to true by miss shader if ray is not terminated before
	TraceRay(gRtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, hitProgramCount, 1 /* missIdx */, ray, rayData);

	return rayData.visible;
}

/** Evaluates the direct illumination from analytic lights.
	This function samples Falcor's light list uniformly with one shadow ray.
	\param[in] sd Shading data.
	\param[in] rayOrigin Ray origin for the shadow ray.
	\param[in] sg SampleGenerator object.
	\return Outgoing radiance in view direction.
*/
float3 evalDirectAnalytic(const ShadingData sd, float3 rayOrigin, inout SampleGenerator sg)
{
	const uint lightCount = gScene.getLightCount();
	if (lightCount == 0) return float3(0);

	// Pick one of the analytic light sources randomly with equal probability.
	const uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);

	// Sample local light source.
	AnalyticLightSample ls;
	bool valid = sampleLight(rayOrigin, gScene.getLight(lightIndex), sg, ls);

	// Reject sample if lower hemisphere.
	if (!valid || dot(ls.dir, sd.N) <= MIN_COS_THETA) return float3(0);

	// Test visibility by tracing a shadow ray.
	bool V = traceShadowRay(rayOrigin, ls.dir, ls.distance);
	return V ? evalBSDFCosine(sd, ls.dir) * ls.Li * lightCount : float3(0);
}

void evalDirect(inout IndirectRayData rayData, const ShadingData sd, float3 origin, uint hitId, uint triId)
{
	// Add emissive contribution
	if (any(sd.emissive > 0.f)) {
		TriangleHit hit;
		hit.triangleIndex = gScene.lightCollection.getTriangleIndex(hitId, triId);
		hit.posW = sd.posW;
		hit.normalW = sd.frontFacing ? sd.faceN : -sd.faceN;

		rayData.colour += rayData.throughput * sd.emissive * evalMIS(rayData.pdfLast, emissiveSampler.evalPdf(origin, sd.N, true, hit) / 2.f);
	}

	// Add direct contribution
	if (bSampleEmissives && sampleNext1D(rayData.sg) < 0.5f) {
		TriangleLightSample lightSample;

		bool valid = emissiveSampler.sampleLight(origin, sd.N, true, rayData.sg, lightSample);
		if (!valid || dot(sd.N, lightSample.dir) <= MIN_COS_THETA) return;

		bool visible = traceShadowRay(lightSample.posW + lightSample.normalW, lightSample.dir, lightSample.distance);
		if (!visible) return;

		float3 emitColour = lightSample.Le * evalMIS(lightSample.pdf / 2.f, evalPdfBSDF(sd, lightSample.dir)) / lightSample.pdf;
		rayData.colour += rayData.throughput * evalBSDFCosine(sd, lightSample.dir) * emitColour;
	} else {
		rayData.colour += rayData.throughput * evalDirectAnalytic(sd, origin, rayData.sg);
	}
}

void sampleIndirect(const ShadingData sd, float3 origin, inout IndirectRayData rayData)
{
	// Sample BSDF
	BSDFSample result;
	const bool valid = sampleBSDF(sd, rayData.sg, result);
	if (!valid || result.pdf == 0.f) {
		rayData.terminated = true;
		return;
	}

	// Update ray data object
	rayData.pdfLast = result.pdf;
	rayData.origin = origin;
	rayData.direction = result.wi;
	rayData.throughput *= result.weight;
}

[shader("miss")]
void shadowMiss(inout ShadowRayData hitData)
{
	hitData.visible = true;
}

[shader("anyhit")]
void shadowAnyHit(
	uniform HitShaderParams hitParams,
	inout ShadowRayData hitData,
	in BuiltInTriangleIntersectionAttributes attribs
) {
	// Alpha test for non-opaque geometry. (will need to remove when I've implemented refraction)
	VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
	uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
	if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) {
		IgnoreHit();
	}
}

[shader("miss")]
void indirectMiss(inout IndirectRayData hitData)
{
	// Add IBL here
	hitData.terminated = true;
}

[shader("anyhit")]
void indirectAnyHit(
	uniform HitShaderParams hitParams,
	inout IndirectRayData rayData,
	BuiltInTriangleIntersectionAttributes attribs
) {
	// Alpha test for non-opaque geometry. (will need to remove when I've implemented refraction)
	VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
	uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
	if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) {
		IgnoreHit();
	}
}

[shader("closesthit")]
void indirectClosestHit(
	uniform HitShaderParams hitParams,
	inout IndirectRayData rayData,
	BuiltInTriangleIntersectionAttributes attribs
) {
	// Get the hit-point data
	float3 rayDirW = WorldRayDirection();
	uint triangleIndex = PrimitiveIndex();

	// prepare the shading data
	VertexData v = getVertexData(hitParams, triangleIndex, attribs);
	
	uint hitIndex = hitParams.getGlobalHitID();
	uint materialID = gScene.getMaterialID(hitIndex);

	ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -rayDirW, 0);
	sd.diffuse = sd.diffuse * gScene.materials[materialID].baseColor.rgb;
	sd.opacity = sd.opacity * gScene.materials[materialID].baseColor.a;
	sd.emissive = sd.emissive + gScene.materials[materialID].emissive * gScene.materials[materialID].emissiveFactor;

	// Calc biased origin
	float3 origin = sd.computeNewRayOrigin();

	// Evaluate direct lighting
	evalDirect(rayData, sd, origin, hitIndex, triangleIndex);

	// Sample next ray
	sampleIndirect(sd, origin, rayData);

	// Russian Roulette
	float p = max(rayData.throughput.x, max(rayData.throughput.y, rayData.throughput.z));
	if (sampleNext1D(rayData.sg) > p) {
		rayData.terminated = true;
	} else {
		rayData.throughput *= 1 / p;
	}
}

[shader("miss")]
void primaryMiss(inout PrimaryRayData hitData)
{
	hitData.colour = kClearColour;
	hitData.hitT = -1;
}

[shader("anyhit")]
void primaryAnyHit(
	uniform HitShaderParams hitParams,
	inout PrimaryRayData rayData,
	BuiltInTriangleIntersectionAttributes attribs
) {
	// Alpha test for non-opaque geometry. (will need to remove when I've implemented refraction)
	VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
	uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
	if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) {
		IgnoreHit();
	}
}

[shader("closesthit")]
void primaryClosestHit(
	uniform HitShaderParams hitParams,
	inout PrimaryRayData hitData, in BuiltInTriangleIntersectionAttributes attribs
) {
	// Get the hit-point data
	float3 rayOrigW = WorldRayOrigin();
	float3 rayDirW = WorldRayDirection();
	float hitT = RayTCurrent();
	uint triangleIndex = PrimitiveIndex();

	// Get vertext data, hit index, and material ID
	VertexData v = getVertexData(hitParams, triangleIndex, attribs);

	uint hitIndex = hitParams.getGlobalHitID();
	uint materialID = gScene.getMaterialID(hitIndex);
	
	// Prepare shading data
	ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -rayDirW, 0);
	sd.diffuse = sd.diffuse * gScene.materials[materialID].baseColor.rgb;
	sd.opacity = sd.opacity * gScene.materials[materialID].baseColor.a;
	sd.emissive = sd.emissive + gScene.materials[materialID].emissive * gScene.materials[materialID].emissiveFactor;

	// Create sample generator
	SampleGenerator generator = SampleGenerator.create(hitData.launchIndex.xy, sampleIndex);

	// Calculate biased hit point
	float3 origin = sd.computeNewRayOrigin();

	// Lighting
	// Prepare reused ray
	RayDesc indirectRay;
	indirectRay.TMin = 0.f;
	indirectRay.TMax = FLT_MAX;

	// Prepare ray payload
	IndirectRayData indRayData;
	indRayData.colour = float3(0);
	indRayData.throughput = float3(1);
	indRayData.sg = generator;
	indRayData.terminated = false;
	indRayData.pdfLast = 1.f;

	// Eval direct lighting at primary ray hit
	evalDirect(indRayData, sd, origin, hitIndex, triangleIndex);

	// Sample first indirect ray
	sampleIndirect(sd, origin, indRayData);

	// Scatter
	[loop]
	for (uint depth = 0; depth < 5 && !indRayData.terminated; depth++) {
		// Trace indirect ray
		indirectRay.Origin = indRayData.origin;
		indirectRay.Direction = indRayData.direction;
		TraceRay(gRtScene, RAY_FLAG_CULL_FRONT_FACING_TRIANGLES, 0xFF, 2, hitProgramCount, 2, indirectRay, indRayData);
	}

	// Assign colour to primary ray payload
	hitData.colour.rgb = indRayData.colour;
	hitData.colour.a = 1;
	hitData.hitT = hitT;
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
	hitData.launchIndex = launchIndex;
	TraceRay(gRtScene, RAY_FLAG_CULL_FRONT_FACING_TRIANGLES, 0xFF, 0, hitProgramCount, 0, ray, hitData);
	gOutput[launchIndex.xy] = hitData.colour;
}
