#pragma once

#include "Falcor.h"
#include "Utils/Sampling/SampleGenerator.h"
#include "Experimental/Scene/Lights/EmissivePowerSampler.h"

namespace GModDXR
{
	using namespace Falcor;

	struct WorldData
	{
		uint32_t length;
		std::vector<float3> pPositions;
		std::vector<float3> pNormals;
		float3 sunDirection;
	};

	struct TextureList
	{
		std::string baseColour;
		std::string normalMap;
	};

	class Renderer : public IRenderer
	{
	public:
		void onLoad(RenderContext* pRenderContext) override;
		void onFrameRender(RenderContext* pRenderContext, const Fbo::SharedPtr& pTargetFbo) override;
		void onResizeSwapChain(uint32_t width, uint32_t height) override;
		bool onKeyEvent(const KeyboardEvent& keyEvent) override;
		bool onMouseEvent(const MouseEvent& mouseEvent) override;
		void onGuiRender(Gui* pGui) override;

		void setWorldData(const WorldData* data);
		void setCameraDefaults(const float3 pos, const float3 target);
		void setEntities(std::vector<TriangleMesh::SharedPtr>* meshes, std::vector<Material::SharedPtr>* materials, std::vector<SceneBuilder::Node>* nodes, std::vector<TextureList>* textures);

	private:
		SceneBuilder::SharedPtr pBuilder;
		Scene::SharedPtr pScene;

		RtProgram::SharedPtr pRaytraceProgram;

		ComputeProgram::SharedPtr pAccProg;
		ComputeVars::SharedPtr pAccVars;
		ComputeState::SharedPtr pAccState;
		Texture::SharedPtr pAccBufferSum;
		Texture::SharedPtr pAccBufferCorr;
		uint accumulatingSince = 0;

		//ComputeProgram::SharedPtr pTonemapProg;

		Camera::SharedPtr pCamera;
		float3 cameraStartPos;
		float3 cameraStartTarget;

		bool useDOF = false;
		RtProgramVars::SharedPtr pRtVars;
		Texture::SharedPtr pRtOut;

		uint sampleIndex = 0;
		SampleGenerator::SharedPtr pSampleGenerator;
		EmissiveLightSampler::SharedPtr pEmissiveSampler;

		const WorldData* pWorldData;

		std::vector<TriangleMesh::SharedPtr>* pMeshes;
		std::vector<Material::SharedPtr>* pMaterials;
		std::vector<SceneBuilder::Node>* pNodes;
		std::vector<TextureList>* pTextures;

		float zNear = 0.01f;
		float zFar = 100.f;

		void setPerFrameVars(const Fbo* pTargetFbo);
		void renderRT(RenderContext* pContext, const Fbo* pTargetFbo);
		void loadScene(RenderContext* pRenderContext, const Fbo* pTargetFbo);
	};
}
