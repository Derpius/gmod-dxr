#include "Utils/Math/MathConstants.slangh"

import Scene.Shading;
import Scene.Raytracing;

import Utils.Helpers;
import Utils.Math.MathHelpers;

import Utils.Sampling.SampleGenerator;

import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;

#define BIAS 0.01f

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
}

struct ShadowRayData
{
	bool visible;
};

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
    float invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

    // Sample local light source.
    AnalyticLightSample ls;
    bool valid = sampleLight(rayOrigin, gScene.getLight(lightIndex), sg, ls);

    // Reject sample if lower hemisphere.
    if (!valid || dot(ls.dir, sd.N) <= kMinCosTheta) return float3(0);

    // Test visibility by tracing a shadow ray.
    bool V = traceShadowRay(rayOrigin, ls.dir, ls.distance);
    return V ? evalBSDFCosine(sd, ls.dir) * ls.Li * invPdf : float3(0);
}

void sampleIndirect(const ShadingData sd, float3 origin, inout IndirectRayData rayData)
{
	float pdf = 0.f;
	float3 wi = sample_cosine_hemisphere_concentric(sampleNext2D(rayData.sg), pdf);
	wi = fromLocal(wi, sd);

	rayData.origin = origin;
	rayData.direction = wi;
	rayData.throughput *= pdf > 0.f ? evalBSDFCosine(sd, wi) / pdf : float3(0);
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
	uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
	ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -rayDirW, 0);
	sd.diffuse = sd.diffuse * gScene.materials[materialID].baseColor.rgb;
	sd.opacity = sd.opacity * gScene.materials[materialID].baseColor.a;
	sd.emissive = sd.emissive + gScene.materials[materialID].emissive * gScene.materials[materialID].emissiveFactor;

	// Calc biased origin
	float3 origin = sd.computeNewRayOrigin();

	// Add emissive contribution
	rayData.colour += rayData.throughput * sd.emissive;

	// Add direct contribution
	rayData.colour += rayData.throughput * evalDirectAnalytic(sd, origin, rayData.sg);

	// Sample next ray
	sampleIndirect(sd, origin, rayData);
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
	// Get the hit-point data
	float3 rayOrigW = WorldRayOrigin();
	float3 rayDirW = WorldRayDirection();
	float hitT = RayTCurrent();
	uint triangleIndex = PrimitiveIndex();

	// prepare the shading data
	VertexData v = getVertexData(hitParams, triangleIndex, attribs);
	uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
	ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -rayDirW, 0);
	sd.diffuse = sd.diffuse * gScene.materials[materialID].baseColor.rgb;
	sd.opacity = sd.opacity * gScene.materials[materialID].baseColor.a;
	sd.emissive = sd.emissive + gScene.materials[materialID].emissive * gScene.materials[materialID].emissiveFactor;

	// Create sample generator
	SampleGenerator generator = SampleGenerator.create(hitData.launchIndex.xy, sampleIndex);

	// Calculate biased hit point
	float3 origin = sd.computeNewRayOrigin();

	// Output colour
	float3 colour = sd.emissive + evalDirectAnalytic(sd, origin, generator);

	// Indirect lighting
	RayDesc indirectRay;
	indirectRay.TMin = 0.f;
	indirectRay.TMax = FLT_MAX;

	IndirectRayData indRayData;
	indRayData.colour = float3(0);
	indRayData.throughput = float3(1);
	indRayData.sg = generator;
	indRayData.terminated = false;

	sampleIndirect(sd, origin, indRayData);

	for (uint depth = 0; depth < 3 && !indRayData.terminated; depth ++) {
		// Trace indirect ray
		indirectRay.Origin = indRayData.origin;
		indirectRay.Direction = indRayData.direction;
		TraceRay(gRtScene, 0, 0xFF, 2, hitProgramCount, 2, indirectRay, indRayData);
	}
	colour += indRayData.colour;

	hitData.colour.rgb = colour;
	hitData.colour.a = 1;
	hitData.hitT = hitT;
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
