#include "Utils/Math/MathConstants.slangh"

import Scene.Shading;
import Scene.Raytracing;

import Utils.Helpers;
import Utils.Math.MathHelpers;

import Utils.Sampling.SampleGenerator;

import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;
import Experimental.Scene.Lights.EmissiveLightSampler;
import Experimental.Scene.Lights.EnvMapSampler;

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
	bool bSampleEnvMap;
	EmissiveLightSampler emissiveSampler;
	EnvMapSampler envMapSampler;
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

struct LightProbabilities
{
	float analytic;
	float emissive;
	float envmap;
};

LightProbabilities lightSelectionProbs()
{
	LightProbabilities p;
	p.analytic = gScene.getLightCount() > 0 ? 1.f : 0.f;
	p.emissive = bSampleEmissives ? 1.f : 0.f;
	p.envmap = bSampleEnvMap ? 1.f : 0.f;

	float sum = p.analytic + p.emissive + p.envmap;
	if (sum == 0.f) return p;

	p.analytic /= sum;
	p.emissive /= sum;
	p.envmap /= sum;

	return p;
}

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
	LightProbabilities lightPs = lightSelectionProbs();

	// Add emissive contribution
	if (bSampleEmissives && any(sd.emissive > 0.f)) {
		TriangleHit hit;
		hit.triangleIndex = gScene.lightCollection.getTriangleIndex(hitId, triId);
		hit.posW = sd.posW;
		hit.normalW = sd.frontFacing ? sd.faceN : -sd.faceN;

		rayData.colour += rayData.throughput * sd.emissive * evalMIS(rayData.pdfLast, emissiveSampler.evalPdf(origin, sd.N, true, hit) / lightPs.emissive);
	}

	// Add direct contribution
	float u = sampleNext1D(rayData.sg);
	if (bSampleEmissives) {
		if (u < lightPs.emissive) {
			TriangleLightSample lightSample;

			bool valid = emissiveSampler.sampleLight(origin, sd.N, true, rayData.sg, lightSample);
			if (!valid || dot(sd.N, lightSample.dir) <= MIN_COS_THETA) return;

			bool visible = traceShadowRay(origin, lightSample.dir, lightSample.distance);
			if (!visible) return;

			float pdf = lightSample.pdf * lightPs.emissive;
			float3 emitColour = lightSample.Le * evalMIS(pdf, evalPdfBSDF(sd, lightSample.dir)) / pdf;
			rayData.colour += rayData.throughput * evalBSDFCosine(sd, lightSample.dir) * emitColour;
			return;
		}
		u -= lightPs.emissive;
	}

	if (bSampleEnvMap) {
		if (u < lightPs.envmap) {
			EnvMapSample lightSample;
			envMapSampler.sample(sampleNext2D(rayData.sg), lightSample);
			if (dot(sd.N, lightSample.dir) <= MIN_COS_THETA) return;

			bool visible = traceShadowRay(origin, lightSample.dir, FLT_MAX);
			if (!visible) return;

			float pdf = lightSample.pdf * lightPs.envmap;
			float3 emitColour = lightSample.Le * evalMIS(pdf, evalPdfBSDF(sd, lightSample.dir)) / pdf;
			rayData.colour += rayData.throughput * evalBSDFCosine(sd, lightSample.dir) * emitColour;
			return;
		}
	}

	if (gScene.getLightCount() > 0) {
		rayData.colour += rayData.throughput * evalDirectAnalytic(sd, origin, rayData.sg);
	}
}

void sampleIndirect(const ShadingData sd, inout IndirectRayData rayData)
{
	// Sample BSDF
	BSDFSample result;
	const bool valid = sampleBSDF(sd, rayData.sg, result);
	if (!valid) {
		rayData.terminated = true;
		return;
	}

	// Update ray data object
	rayData.pdfLast = result.pdf;
	rayData.origin = sd.computeNewRayOrigin(!result.isLobe(LobeType::Transmission));
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
	// Alpha test for non-opaque geometry
	VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
	uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
	if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) {
		IgnoreHit();
	}
}

[shader("miss")]
void indirectMiss(inout IndirectRayData hitData)
{
	if (bSampleEnvMap)
		hitData.colour += hitData.throughput * envMapSampler.eval(hitData.direction) * evalMIS(hitData.pdfLast, envMapSampler.evalPdf(hitData.direction) / lightSelectionProbs().envmap);
	
	hitData.terminated = true;
}

[shader("anyhit")]
void indirectAnyHit(
	uniform HitShaderParams hitParams,
	inout IndirectRayData rayData,
	BuiltInTriangleIntersectionAttributes attribs
) {
	// Alpha test for non-opaque geometry
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

	// Fix backfacing normals due to normal mapping and vertex normals
	adjustShadingNormal(sd, v);

	// Sample next ray
	sampleIndirect(sd, rayData);

	// Evaluate direct lighting
	evalDirect(rayData, sd, rayData.origin, hitIndex, triangleIndex);

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
	hitData.colour = bSampleEnvMap ? float4(gScene.envMap.eval(WorldRayDirection()), 1.f) : kClearColour;
	hitData.hitT = -1;
}

[shader("anyhit")]
void primaryAnyHit(
	uniform HitShaderParams hitParams,
	inout PrimaryRayData rayData,
	BuiltInTriangleIntersectionAttributes attribs
) {
	// Alpha test for non-opaque geometry
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

	// Create sample generator
	SampleGenerator generator = SampleGenerator.create(hitData.launchIndex.xy, sampleIndex);

	// Fix backfacing normals due to normal mapping and vertex normals
	adjustShadingNormal(sd, v);

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

	// Sample first indirect ray
	sampleIndirect(sd, indRayData);

	// Eval direct lighting at primary ray hit
	evalDirect(indRayData, sd, indRayData.origin, hitIndex, triangleIndex);

	// Scatter
	[loop]
	for (uint depth = 0; depth < 3 && !indRayData.terminated; depth++) {
		// Trace indirect ray
		indirectRay.Origin = indRayData.origin;
		indirectRay.Direction = indRayData.direction;
		TraceRay(gRtScene, 0, 0xFF, 2, hitProgramCount, 2, indirectRay, indRayData);
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
	TraceRay(gRtScene, 0, 0xFF, 0, hitProgramCount, 0, ray, hitData);
	gOutput[launchIndex.xy] = hitData.colour;
}
