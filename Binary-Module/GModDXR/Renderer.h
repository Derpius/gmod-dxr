#pragma once

#define FALCOR_D3D12

#include "Falcor.h"
#include "Utils/Sampling/SampleGenerator.h"
#include "Experimental/Scene/Lights/EmissivePowerSampler.h"
#include "Experimental/Scene/Lights/EnvMapSampler.h"

namespace GModDXR
{
	struct WorldData
	{
		uint32_t length;
		std::vector<Falcor::float3> pPositions;
		std::vector<Falcor::float3> pNormals;
		Falcor::float3 sunDirection;
	};

	struct TextureDesc
	{
		std::string baseColour;
		std::string normalMap;
		bool alphatest;
	};

	class Renderer : public Falcor::IRenderer
	{
	public:
		void onLoad(Falcor::RenderContext* pRenderContext) override;
		void onFrameRender(Falcor::RenderContext* pRenderContext, const Falcor::Fbo::SharedPtr& pTargetFbo) override;
		void onResizeSwapChain(uint32_t width, uint32_t height) override;
		bool onKeyEvent(const Falcor::KeyboardEvent& keyEvent) override;
		bool onMouseEvent(const Falcor::MouseEvent& mouseEvent) override;
		void onGuiRender(Falcor::Gui* pGui) override;

		void setWorldData(const WorldData* data);
		void setCameraDefaults(const Falcor::float3 pos, const Falcor::float3 target);
		void setEntities(std::vector<Falcor::TriangleMesh::SharedPtr>* meshes, std::vector<Falcor::Material::SharedPtr>* materials, std::vector<Falcor::SceneBuilder::Node>* nodes, std::vector<TextureDesc>* textures);

	private:
		Falcor::SceneBuilder::SharedPtr pBuilder;
		Falcor::Scene::SharedPtr pScene;

		Falcor::Sampler::SharedPtr pLinearSampler;

		Falcor::RtProgram::SharedPtr pRaytraceProgram;

		Falcor::ComputeProgram::SharedPtr pAccProg;
		Falcor::ComputeVars::SharedPtr pAccVars;
		Falcor::ComputeState::SharedPtr pAccState;
		Falcor::Texture::SharedPtr pAccBufferSum;
		Falcor::Texture::SharedPtr pAccBufferCorr;
		Falcor::uint accumulatingSince = 0;
		bool resetAccumulation = false;

		Falcor::FullScreenPass::SharedPtr pAntialiasPass;
		bool                              antialiasToggle = true;
		float                             fxaaQualitySubPix = 0.75f;
		float                             fxaaQualityEdgeThreshold = 0.166f;
		float                             fxaaQualityEdgeThresholdMin = 0.0833f;
		bool                              fxaaEarlyOut = true;

		Falcor::FullScreenPass::SharedPtr pLuminancePass;
		Falcor::FullScreenPass::SharedPtr pTonemapPass;
		Falcor::Texture::SharedPtr        pLutTexture;
		bool                              useLut = false;

		Falcor::Camera::SharedPtr pCamera;
		Falcor::float3 cameraStartPos;
		Falcor::float3 cameraStartTarget;

		bool useDOF = false;
		Falcor::RtProgramVars::SharedPtr pRtVars;
		Falcor::Texture::SharedPtr pRtOut;

		Falcor::uint sampleIndex = 0;
		Falcor::SampleGenerator::SharedPtr pSampleGenerator;
		Falcor::EmissiveLightSampler::SharedPtr pEmissiveSampler;
		Falcor::EnvMapSampler::SharedPtr pEnvMapSampler;

		const WorldData* pWorldData;

		std::vector<Falcor::TriangleMesh::SharedPtr>* pMeshes;
		std::vector<Falcor::Material::SharedPtr>* pMaterials;
		std::vector<Falcor::SceneBuilder::Node>* pNodes;
		std::vector<TextureDesc>* pTextures;

		float zNear = 0.01f;
		float zFar = 100.f;
		float exposureCompensation = 0.f;

		bool useWhiteBalance = false;
		float whitePoint = 6500.f;
		Falcor::float3 currentWhite = Falcor::float3(0.f);
		Falcor::float3x3 colourTransform;

		void setPerFrameVars(const Falcor::Fbo* pTargetFbo);
		void renderRT(Falcor::RenderContext* pContext, const Falcor::Fbo* pTargetFbo);
		void loadScene(Falcor::RenderContext* pRenderContext, const Falcor::Fbo* pTargetFbo);
	};
}