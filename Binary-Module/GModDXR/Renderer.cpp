#include "Renderer.h"
#include "Utils/Color/ColorUtils.h"

namespace GModDXR
{
	using namespace Falcor;
	static const float4 kClearColour(0.361f, 0.361f, 0.361f, 1);
	void Renderer::onGuiRender(Gui* pGui)
	{
		Gui::Window w(pGui, "GModDXR Settings", { 300, 400 }, { 10, 80 });

		if (w.checkbox("Use Depth of Field", useDOF)) resetAccumulation = true;
		if (w.var("Z Near", zNear, 0.f, std::numeric_limits<float>::max(), 0.1f) || w.var("Z Far", zFar, 0.1f, std::numeric_limits<float>::max(), 0.1f, true)) {
			pScene->getCamera()->setDepthRange(zNear, zFar);
			resetAccumulation = true;
		}

		if (auto group = w.group("Colour Grading", true)) {
			group.var("Exposure Compensation", exposureCompensation, -12.f, 12.f, 0.1f, false, "%.1f");
			group.checkbox("Use White Balance", useWhiteBalance, false);
			group.var("White Point (in kelvin)", whitePoint, 1905.f, 25000.f, 5.f, false, "%.0f");

			float3 white = currentWhite;
			white = white / std::max(std::max(white.r, white.g), white.b);
			group.rgbColor("", white, false);

			group.checkbox("Use LUT", useLut);
			if (group.button("Load")) {
				std::string filename;
				if (openFileDialog(Bitmap::getFileDialogFilters(), filename))
					pLutTexture = Texture::createFromFile(filename, false, false, ResourceBindFlags::ShaderResource);
			}

			if (pLutTexture && group.button("Clear", true)) pLutTexture = nullptr;

			if (pLutTexture) {
				group.text("LUT Path: " + pLutTexture->getSourceFilename());
				group.image("LUT Texture", pLutTexture, float2(512.f));
			}
		}

		if (auto group = w.group("Antialiasing")) {
			group.checkbox("Enabled", antialiasToggle);
			group.var("Sub-Pixel Quality", fxaaQualitySubPix, 0.f, 1.f, 0.001f);
			group.var("Edge Threshold", fxaaQualityEdgeThreshold, 0.f, 1.f, 0.001f);
			group.var("Edge Threshold Min", fxaaQualityEdgeThresholdMin, 0.f, 1.f, 0.001f);
			group.checkbox("Early out", fxaaEarlyOut);
		}

		if (auto sceneGroup = w.group("Scene", true)) pScene->renderUI(w);
	}

	void Renderer::loadScene(RenderContext* pRenderContext, const Fbo* pTargetFbo)
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
		pWorldMat->setBaseColor(float4(float3(0.9f), 1.f));
		pWorldMat->setRoughness(1.f);
		pWorldMat->setMetallic(0.f);

		SceneBuilder::Node worldNode;
		worldNode.name = "World";
		worldNode.transform = glm::identity<glm::mat4>();

		pBuilder->addMeshInstance(pBuilder->addNode(worldNode), pBuilder->addTriangleMesh(pWorld, pWorldMat));

		// Iterate over all entities
		for (size_t i = 0; i < pMeshes->size(); i++) {
			// Load image textures
			// Diffuse
			std::string filename = std::string("Overrides/materials/") + pTextures->at(i).baseColour;
			std::string fullPath;
			if (!findFileInDataDirectories(filename + ".png", fullPath)) {
				filename = "Overrides/materials/gmoddxr_missingtexture";
			}
			pMaterials->at(i)->loadTexture(Material::TextureSlot::BaseColor, filename + ".png");

			if (findFileInDataDirectories(filename + "_mrao.png", fullPath))
				pBuilder->loadMaterialTexture(pMaterials->at(i), Material::TextureSlot::Specular, filename + "_mrao.png");

			if (findFileInDataDirectories(filename + "_emission.png", fullPath)) {
				pMaterials->at(i)->setEmissiveFactor(1.f);
				pBuilder->loadMaterialTexture(pMaterials->at(i), Material::TextureSlot::Emissive, filename + "_emission.png");
			}

			if (findFileInDataDirectories(filename + "_transmission.png", fullPath)) {
				pMaterials->at(i)->setDoubleSided(true);
				pBuilder->loadMaterialTexture(pMaterials->at(i), Material::TextureSlot::SpecularTransmission, filename + "_transmission.png");
			}

			// Normal map
			filename = std::string("Overrides/materials/") + pTextures->at(i).normalMap;
			if (!pTextures->at(i).normalMap.empty() && findFileInDataDirectories(filename + ".png", fullPath))
				pBuilder->loadMaterialTexture(pMaterials->at(i), Material::TextureSlot::Normal, filename + ".png");

			pMaterials->at(i)->setAlphaMode(pTextures->at(i).alphatest ? AlphaModeMask : AlphaModeOpaque);

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

		// Create texture sampler(s)
		Sampler::Desc samplerDesc;
		samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point);
		pLinearSampler = Sampler::create(samplerDesc);

		// Create RT program
		RtProgram::Desc rtProgDesc;
		rtProgDesc.addShaderLibrary("Pathtrace.rt.slang").setRayGen("rayGen");
		rtProgDesc.addHitGroup(0, "primaryClosestHit", "primaryAnyHit").addMiss(0, "primaryMiss");
		rtProgDesc.addHitGroup(1, "", "shadowAnyHit").addMiss(1, "shadowMiss");
		rtProgDesc.addHitGroup(2, "indirectClosestHit", "indirectAnyHit").addMiss(2, "indirectMiss");
		rtProgDesc.addDefines(pScene->getSceneDefines());
		rtProgDesc.setMaxTraceRecursionDepth(3);

		pSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
		pEmissiveSampler = EmissivePowerSampler::create(pRenderContext, pScene);

		pRaytraceProgram = RtProgram::create(rtProgDesc, 80U);
		pRaytraceProgram->addDefines(pSampleGenerator->getDefines());
		pRaytraceProgram->addDefines(pEmissiveSampler->getDefines());

		Program::DefineList defines;
		defines.add("_USE_LEGACY_SHADING_CODE", "0");
		pRaytraceProgram->addDefines(defines);

		pRtVars = RtProgramVars::create(pRaytraceProgram, pScene);

		auto pGlobalVars = pRtVars->getRootVar();
		bool success = pSampleGenerator->setShaderData(pGlobalVars);
		if (!success) logError("Failed to bind sample generator");

		pRaytraceProgram->setScene(pScene);

		pAccProg = ComputeProgram::createFromFile("Accumulate.cs.slang", "main");
		pAccVars = ComputeVars::create(pAccProg->getReflector());
		pAccState = ComputeState::create();

		pAntialiasPass = FullScreenPass::create("FXAA.slang");

		pLuminancePass = FullScreenPass::create("Luminance.ps.slang");
		pTonemapPass = FullScreenPass::create("Tonemap.ps.slang");
	}

	void Renderer::onLoad(RenderContext* pRenderContext)
	{
		if (!gpDevice->isFeatureSupported(Device::SupportedFeatures::Raytracing)) {
			logFatal("Device does not support raytracing!");
		}

		loadScene(pRenderContext, gpFramework->getTargetFbo().get());
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
		// Reset code taken from Falcor's accumulation render pass (designed for use in mogwai)
		auto sceneUpdates = pScene->getUpdates();
		if ((sceneUpdates & ~Scene::UpdateFlags::CameraPropertiesChanged) != Scene::UpdateFlags::None) {
			resetAccumulation = true;
		} else if (is_set(sceneUpdates, Scene::UpdateFlags::CameraPropertiesChanged)) {
			auto excluded = Camera::Changes::Jitter | Camera::Changes::History;
			auto cameraChanges = pScene->getCamera()->getChanges();
			if ((cameraChanges & ~excluded) != Camera::Changes::None) resetAccumulation = true;
		}

		if (resetAccumulation) {
			pContext->clearUAV(pAccBufferSum->getUAV().get(), float4(0.f));
			pContext->clearUAV(pAccBufferCorr->getUAV().get(), float4(0.f));
			accumulatingSince = sampleIndex;
			resetAccumulation = false;
		}

		Texture::SharedPtr pPostProcessingOutput = Texture::create2D(
			resolution.x, resolution.y,
			ResourceFormat::RGBA16Float, 1, 1, nullptr,
			ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
		);

		pAccVars["PerFrameCB"]["gSamples"] = sampleIndex - accumulatingSince;
		pAccVars["PerFrameCB"]["gResolution"] = resolution;
		pAccVars["gInput"] = pRtOut;
		pAccVars["gOutput"] = pPostProcessingOutput;
		pAccVars["gSumBuffer"] = pAccBufferSum;
		pAccVars["gCorrectionBuffer"] = pAccBufferCorr;

		uint3 numGroups = div_round_up(uint3(resolution.x, resolution.y, 1u), pAccProg->getReflector()->getThreadGroupSize());
		pAccState->setProgram(pAccProg);
		pContext->dispatch(pAccState.get(), pAccVars.get(), numGroups);

		Fbo::Desc fboDesc;
		fboDesc.setColorTarget(0, ResourceFormat::RGBA32Float);
		Fbo::SharedPtr pPostProcessingFbo = Fbo::create2D(resolution.x, resolution.y, fboDesc, 1, Fbo::kAttachEntireMipLevel);

		// Antialiasing pass
		if (antialiasToggle) {
			pAntialiasPass["gSampler"] = pLinearSampler;
			pAntialiasPass["gSrc"] = pPostProcessingOutput;

			float2 rcpFrame = 1.f / float2(resolution.x, resolution.y);

			auto pCB = pAntialiasPass["PerFrameCB"];
			pCB["rcpTexDim"] = rcpFrame;
			pCB["qualitySubPix"] = fxaaQualitySubPix;
			pCB["qualityEdgeThreshold"] = fxaaQualityEdgeThreshold;
			pCB["qualityEdgeThresholdMin"] = fxaaQualityEdgeThresholdMin;
			pCB["earlyOut"] = fxaaEarlyOut;

			pAntialiasPass->execute(pContext, pPostProcessingFbo);
			pPostProcessingOutput = pPostProcessingFbo->getColorTexture(0);
			pPostProcessingFbo = Fbo::create2D(resolution.x, resolution.y, fboDesc, 1, Fbo::kAttachEntireMipLevel); // Redefine FBO to break link to texture ptr
		}

		// Luminance pass
		pLuminancePass["gColorSampler"] = pLinearSampler;
		pLuminancePass["gColorTex"] = pPostProcessingOutput;
		pLuminancePass->execute(pContext, pPostProcessingFbo);
		pPostProcessingFbo->getColorTexture(0)->generateMips(pContext);

		// Tonemapping pass
		// Calculate white balance transform for colour transform
		float3x3 whiteBalanceTransform = useWhiteBalance ? calculateWhiteBalanceTransformRGB_Rec709(whitePoint) : glm::identity<float3x3>();
		currentWhite = glm::inverse(whiteBalanceTransform) * float3(1.f);

		auto pCB = pTonemapPass["PerFrameCB"];
		pTonemapPass["gSampler"]   = pLinearSampler;
		pTonemapPass["gInput"]     = pPostProcessingOutput;
		pTonemapPass["gLuminance"] = pPostProcessingFbo->getColorTexture(0);
		pTonemapPass["gLut"]       = pLutTexture;
		pCB["useLut"]              = useLut;
		pCB["gColourTransform"]    = static_cast<float3x4>(whiteBalanceTransform * pow(2.f, exposureCompensation));
		pTonemapPass->execute(pContext, std::make_shared<Fbo>(*pTargetFbo));

		// Increment sample index
		sampleIndex++;
	}

	void Renderer::onFrameRender(RenderContext* pRenderContext, const Fbo::SharedPtr& pTargetFbo)
	{
		pRenderContext->clearFbo(pTargetFbo.get(), kClearColour, 1.0f, 0, FboAttachmentType::All);

		if (pScene) {
			pScene->getLightCollection(pRenderContext);
			pScene->update(pRenderContext, gpFramework->getGlobalClock().getTime());

			if (pScene->useEmissiveLights()) {
				pEmissiveSampler->update(pRenderContext);
				bool success = pEmissiveSampler->setShaderData(pRtVars["PerFrameCB"]["emissiveSampler"]);
				if (!success) logError("Failed to bind emissive sampler");
				pRtVars["PerFrameCB"]["bSampleEmissives"] = true;
			} else {
				pRtVars["PerFrameCB"]["bSampleEmissives"] = false;
			}

			if (is_set(pScene->getUpdates(), Scene::UpdateFlags::EnvMapChanged))
				pEnvMapSampler = nullptr;

			if (pScene->useEnvLight()) {
				if (!pEnvMapSampler) {
					pEnvMapSampler = EnvMapSampler::create(pRenderContext, pScene->getEnvMap());
					pEnvMapSampler->setShaderData(pRtVars["PerFrameCB"]["envMapSampler"]);
				}
				pRtVars["PerFrameCB"]["bSampleEnvMap"] = true;
			} else {
				if (pEnvMapSampler) pEnvMapSampler = nullptr;
				pRtVars["PerFrameCB"]["bSampleEnvMap"] = false;
			}

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

		pRtOut = Texture::create2D(width, height, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
		pAccBufferSum = Texture::create2D(width, height, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
		pAccBufferCorr = Texture::create2D(width, height, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
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

	void Renderer::setEntities(std::vector<TriangleMesh::SharedPtr>* meshes, std::vector<Material::SharedPtr>* materials, std::vector<SceneBuilder::Node>* nodes, std::vector<TextureDesc>* textures)
	{
		pMeshes = meshes;
		pMaterials = materials;
		pNodes = nodes;
		pTextures = textures;
	}
}