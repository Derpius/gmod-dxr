#define FALCOR_D3D12
#define GM_MODULE

#include "Main.h"
#include "GarrysMod/Lua/Interface.h"

namespace GModDXR
{
	static const float4 kClearColour(0.50f, 0.48f, 0.45f, 1);
	void Renderer::onGuiRender(Gui* pGui)
	{
		Gui::Window w(pGui, "GModDXR Settings", { 300, 400 }, { 10, 80 });

		w.checkbox("Use Depth of Field", useDOF);
		if (w.var("Z Near", zNear, 0.f, std::numeric_limits<float>::max(), 0.1f) || w.var("Z Far", zFar, 0.1f, std::numeric_limits<float>::max(), 0.1f, true))
			pScene->getCamera()->setDepthRange(zNear, zFar);

		pScene->renderUI(w);
	}

	void Renderer::loadScene(const Fbo* pTargetFbo)
	{
		// Create the scene
		pBuilder = SceneBuilder::create();

		DirectionalLight::SharedPtr pSun = DirectionalLight::create("Sun");
		pSun->setWorldDirection(pWorldData->sunDirection);
		pBuilder->addLight(pSun);

		// Load the game world into the scene
		SceneBuilder::Mesh world;
		world.name = "World";
		world.faceCount = pWorldData->length / 3U;
		world.vertexCount = pWorldData->length;
		world.indexCount = pWorldData->length;

		std::vector<uint32_t> indices;
		for (size_t i = 0; i < pWorldData->length; i++)
			indices.push_back(i);
		world.pIndices = indices.data();

		world.topology = Vao::Topology::TriangleList;
		
		Material::SharedPtr pWorldMat = Material::create("World");
		pWorldMat->setShadingModel(ShadingModelMetalRough);
		pWorldMat->setBaseColor(float4(0.9f));
		pWorldMat->setRoughness(1.f);
		pWorldMat->setMetallic(0.f);
		world.pMaterial = pWorldMat;

		world.positions = SceneBuilder::Mesh::Attribute<float3>{ pWorldData->pPositions.data(), SceneBuilder::Mesh::AttributeFrequency::FaceVarying };
		world.normals = SceneBuilder::Mesh::Attribute<float3>{ pWorldData->pNormals.data(), SceneBuilder::Mesh::AttributeFrequency::FaceVarying };

		SceneBuilder::Node worldNode;
		worldNode.name = "World";
		worldNode.transform = glm::mat4(
			float4(1.f, 0.f, 0.f, 0.f),
			float4(0.f, 1.f, 0.f, 0.f),
			float4(0.f, 0.f, 1.f, 0.f),
			float4(0.f, 0.f, 0.f, 1.f)
		);

		pBuilder->addMeshInstance(pBuilder->addMesh(world), pBuilder->addNode(worldNode));

		pScene = pBuilder->getScene();
		if (!pScene) logError("Failed to load scene");

		pCamera = pScene->getCamera();

		// Update the controllers
		float radius = pScene->getSceneBounds().radius();
		pScene->setCameraSpeed(radius * 0.25f);
		zNear = std::max(0.1f, radius / 750.0f);
		zFar = radius * 10.f;

		pCamera->setDepthRange(zNear, zFar);
		pCamera->setAspectRatio(static_cast<float>(pTargetFbo->getWidth()) / static_cast<float>(pTargetFbo->getHeight()));
		pCamera->setPosition(cameraStartPos);
		pCamera->setUpVector(cameraStartUp);

		RtProgram::Desc rtProgDesc;
		rtProgDesc.addShaderLibrary(std::string(_SHADER_DIR) + "debug.rt.slang").setRayGen("rayGen");
		rtProgDesc.addHitGroup(0, "primaryClosestHit", "primaryAnyHit").addMiss(0, "primaryMiss");
		rtProgDesc.addHitGroup(1, "", "shadowAnyHit").addMiss(1, "shadowMiss");
		rtProgDesc.addDefines(pScene->getSceneDefines());
		rtProgDesc.setMaxTraceRecursionDepth(3); // 1 for calling TraceRay from RayGen, 1 for calling it from the primary-ray ClosestHitShader for reflections, 1 for reflection ray tracing a shadow ray

		pRaytraceProgram = RtProgram::create(rtProgDesc);
		pRtVars = RtProgramVars::create(pRaytraceProgram, pScene);
		pRaytraceProgram->setScene(pScene);
	}

	void Renderer::onLoad(RenderContext* pRenderContext)
	{
		if (!gpDevice->isFeatureSupported(Device::SupportedFeatures::Raytracing)) {
			logFatal("Device does not support raytracing!");
		}

		loadScene(gpFramework->getTargetFbo().get());
	}

	void Renderer::setPerFrameVars(const Fbo* pTargetFbo)
	{
		PROFILE("setPerFrameVars");
		auto cb = pRtVars["PerFrameCB"];
		cb["invView"] = glm::inverse(pCamera->getViewMatrix());
		cb["viewportDims"] = float2(pTargetFbo->getWidth(), pTargetFbo->getHeight());
		float fovY = focalLengthToFovY(pCamera->getFocalLength(), Camera::kDefaultFrameHeight);
		cb["tanHalfFovY"] = std::tan(fovY * 0.5f);
		cb["sampleIndex"] = sampleIndex++;
		cb["useDOF"] = useDOF;
		cb["kClearColour"] = kClearColour;
		pRtVars->getRayGenVars()["gOutput"] = pRtOut;
	}

	void Renderer::renderRT(RenderContext* pContext, const Fbo* pTargetFbo)
	{
		PROFILE("renderRT");
		setPerFrameVars(pTargetFbo);

		pContext->clearUAV(pRtOut->getUAV().get(), kClearColour);
		pScene->raytrace(pContext, pRaytraceProgram.get(), pRtVars, uint3(pTargetFbo->getWidth(), pTargetFbo->getHeight(), 1));
		pContext->blit(pRtOut->getSRV(), pTargetFbo->getRenderTargetView(0));
	}

	void Renderer::onFrameRender(RenderContext* pRenderContext, const Fbo::SharedPtr& pTargetFbo)
	{
		pRenderContext->clearFbo(pTargetFbo.get(), kClearColour, 1.0f, 0, FboAttachmentType::All);

		if (pScene) {
			pScene->update(pRenderContext, gpFramework->getGlobalClock().getTime());
			renderRT(pRenderContext, pTargetFbo.get());
		}

		TextRenderer::render(pRenderContext, gpFramework->getFrameRate().getMsg(), pTargetFbo, { 20, 20 });
	}

	bool Renderer::onKeyEvent(const KeyboardEvent& keyEvent)
	{
		return pScene && pScene->onKeyEvent(keyEvent);
	}

	bool Renderer::onMouseEvent(const MouseEvent& mouseEvent)
	{
		return pScene && pScene->onMouseEvent(mouseEvent);
	}

	void Renderer::onResizeSwapChain(uint32_t width, uint32_t height)
	{
		float h = static_cast<float>(height);
		float w = static_cast<float>(width);

		if (pCamera) {
			pCamera->setFocalLength(18);
			float aspectRatio = w / h;
			pCamera->setAspectRatio(aspectRatio);
		}

		pRtOut = Texture::create2D(width, height, ResourceFormat::RGBA16Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
	}

	void Renderer::setWorldData(const WorldData* data)
	{
		pWorldData = data;
	}

	void Renderer::setCameraDefaults(const float3 pos, const float3 up)
	{
		cameraStartPos = pos;
		cameraStartUp = up;
	}

	std::vector<float3> computeBrushNormals(const float3* positions, const size_t count)
	{
		std::vector<float3> normals;
		// Iterate tris
		for (size_t i = 0; i < count; i += 3) {
			// Compute normalised cross product of v0 and v1 localised to v2 and appened to normals vector
			// (may need to invert the normal here depending on what winding my triangulation code spits out)
			float3 normal = glm::normalize(glm::cross(positions[i] - positions[i + 2], positions[i + 1] - positions[i + 2]));
			normals.push_back(normal);
			normals.push_back(normal);
			normals.push_back(normal);
		}
		return normals;
	}
}

static std::thread mainThread;
static std::mutex mut;
static GModDXR::WorldData worldData;
static bool TRACING = false;
void falcorThreadWrapper(const Vector camPos, const Vector camUp) {
	// Create renderer
	GModDXR::Renderer::UniquePtr pRenderer = std::make_unique<GModDXR::Renderer>();

	// Get raw casted pointer to GModDXR::Renderer to access custom methods in the derived class
	GModDXR::Renderer* pRendererRaw = reinterpret_cast<GModDXR::Renderer*>(pRenderer.get());
	pRendererRaw->setWorldData(&worldData);
	pRendererRaw->setCameraDefaults(Falcor::float3(camPos.x, camPos.z, camPos.y), Falcor::float3(camUp.x, camUp.z, camUp.y));
	pRendererRaw = nullptr;

	// Create window config
	Falcor::SampleConfig config;
	config.windowDesc.title = "Garry's Mod DXR";
	config.windowDesc.resizableWindow = true;

	Falcor::Sample::run(config, pRenderer);

	// Let the main thread know we're done
	mut.lock();
	TRACING = false;
	mut.unlock();
}

/*
	Entrypoint for the application when loaded from GLua
	
	Parameters
	- table<Vector> World surface positions
	- number        Number of world vertices
	- Vector        Camera start location
	- Vector        Camera up vector
	- Vector        Sun direction
*/
LUA_FUNCTION(LaunchFalcor)
{
	if (TRACING) return 0;

	// Read camera details and world vert count
	const size_t worldVertCount = LUA->GetNumber(2);
	const Vector camPos = LUA->GetVector(3);
	const Vector camUp = LUA->GetVector(4);
	const Vector sunDir = LUA->GetVector(5);
	LUA->Pop(4);

	// Read world verts into data structure
	std::vector<Falcor::float3> worldPositions;
	for (size_t i = 0; i < worldVertCount; i++) {
		// Get next vert
		LUA->PushNumber(static_cast<double>(i + 1U));
		LUA->GetTable(1);

		// Get and pop the vector
		const Vector vertPos = LUA->GetVector();
		LUA->Pop();

		// Add vertex position to world vetices vector as a float3
		worldPositions.push_back(Falcor::float3(vertPos.x, vertPos.z, vertPos.y));
	}
	LUA->Pop();

	// Create world data
	worldData.length = worldVertCount;
	worldData.pPositions = worldPositions;
	worldData.pNormals = GModDXR::computeBrushNormals(worldPositions.data(), worldVertCount);
	worldData.sunDirection = Falcor::float3(sunDir.x, sunDir.z, sunDir.y);

	// Run the sample
	TRACING = true;
	mainThread = std::thread(falcorThreadWrapper, camPos, camUp);
	mainThread.detach();
	return 0;
}

GMOD_MODULE_OPEN()
{
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
		LUA->PushCFunction(LaunchFalcor);
		LUA->SetField(-2, "LaunchFalcor");
	LUA->Pop();

	return 0;
}

GMOD_MODULE_CLOSE()
{
	while (true) {
		mut.lock();
		if (!TRACING) {
			mut.unlock();
			return 0;
		}
		mut.unlock();
	}
}

int main()
{
	return 0;
}