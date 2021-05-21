#define FALCOR_D3D12

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
		TriangleMesh::SharedPtr pWorld = TriangleMesh::create();
		pWorld->setName("World");
		for (size_t i = 0; i < pWorldData->length; i++) {
			pWorld->addVertex(pWorldData->pPositions[i], pWorldData->pNormals[i], float2(0.f, 0.f));
			if (i % 3 == 2) pWorld->addTriangle(i - 2U, i - 1U, i);
		}
		
		Material::SharedPtr pWorldMat = Material::create("World");
		pWorldMat->setShadingModel(ShadingModelMetalRough);
		pWorldMat->setBaseColor(float4(0.9f));
		pWorldMat->setRoughness(1.f);
		pWorldMat->setMetallic(0.f);

		SceneBuilder::Node worldNode;
		worldNode.name = "World";
		worldNode.transform = glm::identity<glm::mat4>();

		pBuilder->addMeshInstance(pBuilder->addNode(worldNode), pBuilder->addTriangleMesh(pWorld, pWorldMat));

		// Iterate over all entities
		for (size_t i = 0; i < pMeshes->size(); i++) {
			// Load image textures
			std::string filename = std::string("Overrides/materials/") + pTextures->at(i).baseColour;
			std::string fullPath;
			if (!findFileInDataDirectories(filename, fullPath)) {
				filename = std::string("Overrides/materials/missingtexture.png");
			}
			pBuilder->loadMaterialTexture(pMaterials->at(i), Material::TextureSlot::BaseColor, filename);

			if (!pTextures->at(i).normalMap.empty())
				pBuilder->loadMaterialTexture(pMaterials->at(i), Material::TextureSlot::Normal, std::string("Overrides/materials/") + pTextures->at(i).normalMap);

			// Add mesh instance
			pBuilder->addMeshInstance(pBuilder->addNode(pNodes->at(i)), pBuilder->addTriangleMesh(pMeshes->at(i), pMaterials->at(i)));
		}

		pScene = pBuilder->getScene();
		if (!pScene) logError("Failed to load scene");

		pCamera = pScene->getCamera();

		// Update the controllers
		float radius = pScene->getSceneBounds().radius();
		pScene->setCameraSpeed(500.f);
		zNear = std::max(0.1f, radius / 750.0f);
		zFar = radius * 10.f;

		pCamera->setDepthRange(zNear, zFar);
		pCamera->setAspectRatio(static_cast<float>(pTargetFbo->getWidth()) / static_cast<float>(pTargetFbo->getHeight()));
		pCamera->setPosition(cameraStartPos);
		pCamera->setTarget(cameraStartTarget);

		RtProgram::Desc rtProgDesc;
		rtProgDesc.addShaderLibrary(std::string(_SHADER_DIR) + "Pathtrace.rt.slang").setRayGen("rayGen");
		rtProgDesc.addHitGroup(0, "primaryClosestHit", "primaryAnyHit").addMiss(0, "primaryMiss");
		rtProgDesc.addHitGroup(1, "", "shadowAnyHit").addMiss(1, "shadowMiss");
		rtProgDesc.addHitGroup(2, "indirectClosestHit", "indirectAnyHit").addMiss(2, "indirectMiss");
		rtProgDesc.addDefines(pScene->getSceneDefines());
		rtProgDesc.setMaxTraceRecursionDepth(3);

		pSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);

		pRaytraceProgram = RtProgram::create(rtProgDesc, 80U);
		pRaytraceProgram->addDefines(pSampleGenerator->getDefines());

		pRtVars = RtProgramVars::create(pRaytraceProgram, pScene);

		auto pGlobalVars = pRtVars->getRootVar();
		bool success = pSampleGenerator->setShaderData(pGlobalVars);
		if (!success) logError("Failed to bind sample generator");

		pRaytraceProgram->setScene(pScene);

		pAccProg = ComputeProgram::createFromFile(std::string(_SHADER_DIR) + "Accumulate.cs.slang", "main");
		pAccVars = ComputeVars::create(pAccProg->getReflector());
		pAccState = ComputeState::create();

		//pTonemapPass = ComputeProgram::createFromFile(std::string(_SHADER_DIR) + "Tonemap.cs.slang", "main");
	}

	void Renderer::onLoad(RenderContext* pRenderContext)
	{
		if (!gpDevice->isFeatureSupported(Device::SupportedFeatures::Raytracing)) {
			logFatal("Device does not support raytracing!");
		}

		Sampler::Desc samplerDesc;
		samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);

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
		cb["sampleIndex"] = sampleIndex;
		cb["useDOF"] = useDOF;
		cb["kClearColour"] = kClearColour;
		pRtVars->getRayGenVars()["gOutput"] = pRtOut;
	}

	void Renderer::renderRT(RenderContext* pContext, const Fbo* pTargetFbo)
	{
		PROFILE("renderRT");
		setPerFrameVars(pTargetFbo);

		const uint2 resolution = uint2(pTargetFbo->getWidth(), pTargetFbo->getHeight());

		pContext->clearUAV(pRtOut->getUAV().get(), kClearColour);
		pScene->raytrace(pContext, pRaytraceProgram.get(), pRtVars, uint3(resolution, 1));

		// Accumulation pass (temporal denoising)
		bool bReset = false;

		// Reset code taken from Falcor's accumulation render pass (designed for use in mogwai)
		auto sceneUpdates = pScene->getUpdates();
		if ((sceneUpdates & ~Scene::UpdateFlags::CameraPropertiesChanged) != Scene::UpdateFlags::None) {
			bReset = true;
		} else if (is_set(sceneUpdates, Scene::UpdateFlags::CameraPropertiesChanged)) {
			auto excluded = Camera::Changes::Jitter | Camera::Changes::History;
			auto cameraChanges = pScene->getCamera()->getChanges();
			if ((cameraChanges & ~excluded) != Camera::Changes::None) bReset = true;
		}

		if (bReset) {
			pContext->clearUAV(pAccBufferSum->getUAV().get(), float4(0.f));
			accumulatingSince = sampleIndex;
		}

		Texture::SharedPtr pAccOutput = Texture::create2D(
			resolution.x, resolution.y,
			ResourceFormat::RGBA16Float, 1, 1, nullptr,
			ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
		);

		pAccVars["PerFrameCB"]["gSamples"] = sampleIndex - accumulatingSince;
		pAccVars["PerFrameCB"]["gResolution"] = resolution;
		pAccVars["gInput"] = pRtOut;
		pAccVars["gOutput"] = pAccOutput;
		pAccVars["gSumBuffer"] = pAccBufferSum;

		uint3 numGroups = div_round_up(uint3(resolution.x, resolution.y, 1u), pAccProg->getReflector()->getThreadGroupSize());
		pAccState->setProgram(pAccProg);
		pContext->dispatch(pAccState.get(), pAccVars.get(), numGroups);
		pContext->blit(pAccOutput->getSRV(), pTargetFbo->getRenderTargetView(0));

		// Increment sample index
		sampleIndex++;
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
		pAccBufferSum = Texture::create2D(width, height, ResourceFormat::RGBA16Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
	}

	void Renderer::setWorldData(const WorldData* data)
	{
		pWorldData = data;
	}

	void Renderer::setCameraDefaults(const float3 pos, const float3 target)
	{
		cameraStartPos = pos;
		cameraStartTarget = target;
	}

	void Renderer::setEntities(std::vector<TriangleMesh::SharedPtr>* meshes, std::vector<Material::SharedPtr>* materials, std::vector<SceneBuilder::Node>* nodes, std::vector<TextureList>* textures)
	{
		pMeshes = meshes;
		pMaterials = materials;
		pNodes = nodes;
		pTextures = textures;
	}

	std::vector<float3> computeBrushNormals(const float3* positions, const size_t count)
	{
		std::vector<float3> normals;
		// Iterate tris
		for (size_t i = 0; i < count; i += 3) {
			// Compute normalised cross product of v0 and v1 localised to v2 and appened to normals vector
			// (may need to invert the normal here depending on what winding my triangulation code spits out)
			float3 normal = -glm::normalize(glm::cross(positions[i] - positions[i + 2], positions[i + 1] - positions[i + 2]));
			normals.push_back(normal);
			normals.push_back(normal);
			normals.push_back(normal);
		}
		return normals;
	}
}

Falcor::float3 gmodToGLMVec(Vector vec) { return Falcor::float3(vec.x, vec.z, -vec.y); }

static std::thread mainThread;
static std::mutex mut;
static GModDXR::WorldData worldData;
static bool TRACING = false;
void falcorThreadWrapper(
	const Vector camPos, const Vector camTarget,
	std::vector<Falcor::TriangleMesh::SharedPtr> meshes, std::vector<Falcor::Material::SharedPtr> materials, std::vector<Falcor::SceneBuilder::Node> nodes, std::vector<GModDXR::TextureList> textures
) {
	// Create renderer
	GModDXR::Renderer::UniquePtr pRenderer = std::make_unique<GModDXR::Renderer>();

	// Get raw casted pointer to GModDXR::Renderer to access custom methods in the derived class
	GModDXR::Renderer* pRendererRaw = reinterpret_cast<GModDXR::Renderer*>(pRenderer.get());
	pRendererRaw->setWorldData(&worldData);
	pRendererRaw->setCameraDefaults(gmodToGLMVec(camPos), gmodToGLMVec(camTarget));
	pRendererRaw->setEntities(&meshes, &materials, &nodes, &textures);
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

void printLua(GarrysMod::Lua::ILuaBase* inst, const char text[])
{
	inst->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	inst->GetField(-1, "print");
	inst->PushString(text);
	inst->Call(1, 0);
	inst->Pop();
}

/*
	Entrypoint for the application when loaded from GLua
	
	Parameters
	- table<Vector> World surface positions
	- number        Number of world vertices
	- Vector        Camera start location
	- Vector        Camera up vector
	- Vector        Sun direction
	- table<table>  Table of entity data:
	-    string         Name
	-    table          Mesh data
	-    string         Base colour texture
	-    table          4x4 transformation matrix
	-    Vector         Base colour
	-    number         Alpha
*/
LUA_FUNCTION(LaunchFalcor)
{
	if (TRACING) return 0;

	auto meshes = std::vector<Falcor::TriangleMesh::SharedPtr>();
	auto materials = std::vector<Falcor::Material::SharedPtr>();
	auto nodes = std::vector<Falcor::SceneBuilder::Node>();
	auto textures = std::vector<GModDXR::TextureList>();

	for (size_t i = 0; i < LUA->ObjLen(6); i++) {
		Falcor::TriangleMesh::SharedPtr pMesh = Falcor::TriangleMesh::create();

		Falcor::Material::SharedPtr pMaterial = Falcor::Material::create("Entity");
		pMaterial->setShadingModel(ShadingModelMetalRough);
		pMaterial->setRoughness(1.f); // Placeholder
		pMaterial->setMetallic(0.f);  // Placeholder

		LUA->PushNumber(static_cast<double>(i + 1U));
		LUA->GetTable(6);

		LUA->GetField(7, "name");
		pMesh->setName(LUA->GetString());
		LUA->Pop();

		LUA->GetField(7, "model");
		for (size_t j = 0; j < LUA->ObjLen(8); j++) {
			LUA->PushNumber(static_cast<double>(j + 1U));
			LUA->GetTable(8);

			LUA->GetField(-1, "pos");
			Falcor::float3 pos = gmodToGLMVec(LUA->GetVector());
			LUA->Pop();

			LUA->GetField(-1, "normal");
			Falcor::float3 normal = gmodToGLMVec(LUA->GetVector());
			LUA->Pop();

			LUA->GetField(-1, "u");
			LUA->GetField(-2, "v");
			Falcor::float2 uv = Falcor::float2(LUA->GetNumber(-2), LUA->GetNumber());
			LUA->Pop(3);

			pMesh->addVertex(pos, normal, uv);
			if (j % 3 == 2) pMesh->addTriangle(j - 2U, j - 1U, j);
		}
		LUA->Pop();

		LUA->GetField(7, "baseTexture");
		LUA->GetField(7, "normalTexture");
		textures.emplace_back(GModDXR::TextureList{ std::string(LUA->GetString(-2)), std::string(LUA->GetString(-1)) });
		LUA->Pop(2);

		LUA->GetField(7, "colour");
		LUA->GetField(7, "alpha");
		pMaterial->setBaseColor(Falcor::float4(LUA->GetVector(-2).x, LUA->GetVector(-2).y, LUA->GetVector(-2).z, LUA->GetNumber()));
		LUA->Pop(2);

		float ang[4][4];

		LUA->GetField(7, "transform");
		for (size_t row = 0; row < 4; row++) {
			LUA->PushNumber(static_cast<double>(row + 1U));
			LUA->GetTable(8);

			for (size_t col = 0; col < 4; col++) {
				LUA->PushNumber(static_cast<double>(col + 1U));
				LUA->GetTable(9);

				ang[row][col] = static_cast<float>(LUA->GetNumber());

				LUA->Pop();
			}

			LUA->Pop();
		}

		glm::mat4 zToYUp = glm::mat4(
			Falcor::float4(1, 0, 0, 0),
			Falcor::float4(0, 0, -1, 0),
			Falcor::float4(0, 1, 0, 0),
			Falcor::float4(0, 0, 0, 1)
		);

		meshes.emplace_back(pMesh);
		materials.emplace_back(pMaterial);
		nodes.emplace_back(Falcor::SceneBuilder::Node{
			"Entity",
			(zToYUp * glm::mat4(
				Falcor::float4(ang[0][0], ang[1][0], ang[2][0], ang[3][0]),
				Falcor::float4(ang[0][1], ang[1][1], ang[2][1], ang[3][1]),
				Falcor::float4(ang[0][2], ang[1][2], ang[2][2], ang[3][2]),
				Falcor::float4(ang[0][3], ang[1][3], ang[2][3], ang[3][3])
			)) * glm::transpose(zToYUp),
			glm::identity<glm::mat4>()
		});
		LUA->Pop(2);
	}

	// Read camera details and world vert count
	const size_t worldVertCount = LUA->GetNumber(2);
	const Vector camPos = LUA->GetVector(3);
	const Vector camTarget = LUA->GetVector(4);
	const Vector sunDir = LUA->GetVector(5);
	LUA->Pop(4);

	// Read world verts into data structure
	std::vector<Falcor::float3> worldPositions;
	for (size_t i = 0; i < worldVertCount; i++) {
		// Get next vert
		LUA->PushNumber(static_cast<double>(i + 1U));
		LUA->GetTable(1);

		// Add vertex position to world vetices vector as a float3
		worldPositions.push_back(gmodToGLMVec(LUA->GetVector()));
		LUA->Pop();
	}
	LUA->Pop();

	// Create world data
	worldData.length = worldVertCount;
	worldData.pPositions = worldPositions;
	worldData.pNormals = GModDXR::computeBrushNormals(worldPositions.data(), worldVertCount);
	worldData.sunDirection = gmodToGLMVec(sunDir);

	// Run the sample
	TRACING = true;
	mainThread = std::thread(falcorThreadWrapper, camPos, camTarget, meshes, materials, nodes, textures);
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
