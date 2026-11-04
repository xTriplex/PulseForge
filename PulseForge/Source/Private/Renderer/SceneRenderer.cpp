#include "Renderer/SceneRenderer.h"
#include "Renderer/SceneRendererAmbientOcclusionPass.h"
#include "Renderer/SceneRendererHdrTargetCache.h"
#include "Renderer/SceneRendererLocalLightingState.h"
#include "Renderer/SceneRendererPipelineCache.h"
#include "Renderer/SceneRendererShadowTargetCache.h"
#include "Renderer/SceneRendererShadowPass.h"
#include "Renderer/RenderTargetFrameScope.h"
#include "Renderer/SceneRendererToneMappingStage.h"
#include "Renderer/SceneRendererMaterialBindingCache.h"
#include "Renderer/Detail/RuntimeUIRenderPass.h"

#include "Assets/MaterialAssetCache.h"
#include "Assets/AssetPathResolver.h"
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/AssetPreparationService.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/Project.h"
#endif
#include "Renderer/AssetCache/MeshAssetCache.h"
#include "Runtime/CookedContentProvider.h"
#include "Renderer/AssetCache/TextureAssetCache.h"
#include "Renderer/EnvironmentLightingCache.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Renderer/Mesh.h"
#include "Scene/Scene.h"
#include "Scene/Components/EnvironmentLightComponent.h"

#include <cmath>
#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <exception>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace PulseForge
{
	struct SceneRenderer::TransientMaterialOverride final
	{
		AssetID Material;
		MaterialAssetDesc Description;
		uint64_t Revision = 0;
	};

	namespace
	{
		std::expected<std::vector<std::byte>, SceneRendererError> ReadShaderBytecode(
			const std::filesystem::path& Path)
		{
			std::ifstream File(Path, std::ios::binary | std::ios::ate);
			if (!File)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::ShaderLoadFailed,
					{},
					{},
					"Could not open compiled shader bytecode: " + Path.string()
				});
			}

			const std::streampos End = File.tellg();
			if (End <= 0 || static_cast<uint64_t>(End) > 64ull * 1024ull * 1024ull)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::ShaderLoadFailed,
					{},
					{},
					"Compiled shader bytecode is empty, unreadable, or exceeds 64 MiB: " + Path.string()
				});
			}

			std::vector<std::byte> Bytecode(static_cast<size_t>(End));
			File.seekg(0, std::ios::beg);
			File.read(reinterpret_cast<char*>(Bytecode.data()), static_cast<std::streamsize>(Bytecode.size()));
			if (!File)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::ShaderLoadFailed,
					{},
					{},
					"Could not read compiled shader bytecode: " + Path.string()
				});
			}
			return Bytecode;
		}

		bool IsFinite(const glm::mat4& Matrix)
		{
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row]))
						return false;
			return true;
		}

		const Mesh* FindPreparedMesh(const PreparedMeshResourceMap& PreparedMeshes, const AssetID& Asset)
		{
			const auto Found = PreparedMeshes.find(Asset);
			return Found == PreparedMeshes.end() ? nullptr : Found->second;
		}

		SceneRendererError MakeResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}

		SceneRendererError MakeDrawError(std::string Message)
		{
			return { SceneRendererErrorCode::DrawFailed, {}, {}, std::move(Message) };
		}

		std::expected<TextureHandle, TextureError> CreateBlackCube(RenderDevice& Runtime, std::string DebugName)
		{
			const std::array<float, 4> Black{ 0.0f, 0.0f, 0.0f, 1.0f };
			const std::span<const float> Pixel(Black);
			std::array<TextureSubresourceData, 6> Faces;
			for (uint32_t Face = 0; Face < 6; ++Face)
				Faces[Face] = { 0, Face, std::as_bytes(Pixel), sizeof(Black) };
			TextureDesc Description;
			Description.Width = 1;
			Description.Height = 1;
			Description.Format = TextureFormat::RGBA32_Float;
			Description.Dimension = TextureDimension::TextureCube;
			Description.DebugName = std::move(DebugName);
			return Runtime.CreateTexture(Description, Faces);
		}

		std::expected<TextureHandle, TextureError> CreateWhiteCube(RenderDevice& Runtime, std::string DebugName)
		{
			const std::array<float, 4> White{ 1.0f, 1.0f, 1.0f, 1.0f };
			const std::span<const float> Pixel(White);
			std::array<TextureSubresourceData, 6> Faces;
			for (uint32_t Face = 0; Face < Faces.size(); ++Face)
				Faces[Face] = { 0, Face, std::as_bytes(Pixel), sizeof(White) };
			TextureDesc Description;
			Description.Width = 1;
			Description.Height = 1;
			Description.Format = TextureFormat::RGBA32_Float;
			Description.Dimension = TextureDimension::TextureCube;
			Description.DebugName = std::move(DebugName);
			return Runtime.CreateTexture(Description, Faces);
		}

	} // namespace

	struct SceneRenderer::ProjectAssetCaches final
	{
#if !defined(PF_COOKED_RUNTIME_ONLY)
		DerivedDataArtifactStore Artifacts;
		AssetPreparationService Preparation;
#endif
		AssetUploadBudget UploadBudget;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		std::string SessionKey;
		std::shared_ptr<const AssetRegistry> Registry;
#endif
		std::shared_ptr<const CookedContentProvider> CookedContent;
		MeshAssetCache Meshes;
		TextureAssetCache Textures;
		MaterialAssetCache Materials;
		EnvironmentLightingCache EnvironmentLighting;

#if !defined(PF_COOKED_RUNTIME_ONLY)
		static std::string MakeSessionKey(const std::filesystem::path& ProjectRoot)
		{
			static std::atomic<uint64_t> NextSession{ 1 };
			const uint64_t Session = NextSession.fetch_add(1, std::memory_order_relaxed);
			return ProjectRoot.lexically_normal().generic_string() + "#session-" + std::to_string(Session);
		}

		ProjectAssetCaches(RenderDevice& Runtime, const Project& SourceProject)
			: Artifacts(DerivedDataArtifactStoreOptions{ SourceProject.GetRootPath() / ".pulseforge" / "DerivedData" }),
			  SessionKey(MakeSessionKey(SourceProject.GetRootPath())),
			  Registry(std::make_shared<const AssetRegistry>(SourceProject.GetAssetRegistry())),
			  Meshes(Runtime, SourceProject.GetRootPath(), Registry, Preparation, &UploadBudget,
				  Artifacts, SessionKey,
				  Runtime.GetDiagnosticRecorder()),
			  Textures(Runtime, SourceProject.GetRootPath(), Registry, Preparation, &UploadBudget,
				  Artifacts, SessionKey,
				  Runtime.GetDiagnosticRecorder()),
			  Materials(SourceProject.GetRootPath(), SourceProject.GetAssetRegistry(), Runtime.GetDiagnosticRecorder()),
			  EnvironmentLighting(Runtime, SourceProject.GetRootPath(), Registry, Preparation,
				  &UploadBudget, Artifacts, SessionKey, Runtime.GetDiagnosticRecorder())
		{
		}

		ProjectAssetCaches(RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content)
			: Artifacts(DerivedDataArtifactStoreOptions{ .Enabled = false }),
			  SessionKey(Content ? Content->GetProjectID().ToString() : std::string{}),
			  Registry(std::make_shared<const AssetRegistry>()), CookedContent(std::move(Content)),
			  Meshes(Runtime, CookedContent, Runtime.GetDiagnosticRecorder()),
			  Textures(Runtime, CookedContent, Runtime.GetDiagnosticRecorder()),
			  Materials(CookedContent, Runtime.GetDiagnosticRecorder()),
			  EnvironmentLighting(Runtime, CookedContent, Runtime.GetDiagnosticRecorder())
		{
			if (!CookedContent)
				throw std::invalid_argument("Cooked scene renderer requires a content provider");
		}

		void Clear(const AssetRegistry& CurrentRegistry) noexcept
		{
			try
			{
				Registry = std::make_shared<const AssetRegistry>(CurrentRegistry);
			}
			catch (...)
			{
				PF_ERROR("Could not retain the refreshed project asset registry snapshot");
				return;
			}
			Meshes.SetRegistrySnapshot(Registry);
			Textures.SetRegistrySnapshot(Registry);
			EnvironmentLighting.SetRegistrySnapshot(Registry);
			SessionKey = MakeSessionKey(Artifacts.GetOptions().Root.parent_path().parent_path());
			Meshes.SetSessionKey(SessionKey);
			Textures.SetSessionKey(SessionKey);
			EnvironmentLighting.SetSessionKey(SessionKey);
			Materials.Clear();
		}
#else
		ProjectAssetCaches(RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content)
			: CookedContent(std::move(Content)), Meshes(Runtime, CookedContent, Runtime.GetDiagnosticRecorder()),
			  Textures(Runtime, CookedContent, Runtime.GetDiagnosticRecorder()), Materials(CookedContent, Runtime.GetDiagnosticRecorder()),
			  EnvironmentLighting(Runtime, CookedContent, Runtime.GetDiagnosticRecorder())
		{
			if (!CookedContent)
				throw std::invalid_argument("Cooked scene renderer requires a content provider");
		}
#endif
	};

#if !defined(PF_COOKED_RUNTIME_ONLY)
	SceneRenderer::SceneRenderer(RenderDevice& Runtime, const Project& SourceProject)
		: m_Runtime(Runtime), m_Project(&SourceProject), m_ProjectAssetCaches(std::make_unique<ProjectAssetCaches>(Runtime, SourceProject)),
		  m_PipelineCache(std::make_unique<SceneRendererPipelineCache>(Runtime)),
		  m_ToneMappingStage(std::make_unique<ToneMappingStage>(Runtime)), m_HdrTargetCache(std::make_unique<HdrTargetCache>(Runtime)),
		  m_ShadowTargetCache(std::make_unique<ShadowTargetCache>(Runtime)), m_LocalLightingState(std::make_unique<LocalLightingState>()),
		  m_AmbientOcclusionPass(std::make_unique<AmbientOcclusionPass>(Runtime)),
		  m_MaterialBindingCache(std::make_unique<SceneRendererMaterialBindingCache>(Runtime)), m_ShadowPass(std::make_unique<ShadowPass>()),
		  m_RuntimeUIRenderPass(std::make_unique<RuntimeUIRenderPass>(Runtime))
	{
	}
#endif

	SceneRenderer::SceneRenderer(RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content)
		: m_Runtime(Runtime), m_ProjectAssetCaches(std::make_unique<ProjectAssetCaches>(Runtime, std::move(Content))),
		  m_PipelineCache(std::make_unique<SceneRendererPipelineCache>(Runtime)),
		  m_ToneMappingStage(std::make_unique<ToneMappingStage>(Runtime)), m_HdrTargetCache(std::make_unique<HdrTargetCache>(Runtime)),
		  m_ShadowTargetCache(std::make_unique<ShadowTargetCache>(Runtime)), m_LocalLightingState(std::make_unique<LocalLightingState>()),
		  m_AmbientOcclusionPass(std::make_unique<AmbientOcclusionPass>(Runtime)),
		  m_MaterialBindingCache(std::make_unique<SceneRendererMaterialBindingCache>(Runtime)), m_ShadowPass(std::make_unique<ShadowPass>()),
		  m_RuntimeUIRenderPass(std::make_unique<RuntimeUIRenderPass>(Runtime))
	{
	}

	SceneRenderer::~SceneRenderer() = default;

	AssetPreparationServiceStats SceneRenderer::GetAssetPreparationStats() const noexcept
	{
#if defined(PF_COOKED_RUNTIME_ONLY)
		return {};
#else
		return m_ProjectAssetCaches->Preparation.GetStats();
#endif
	}

#if defined(PF_DEBUG)
	const Texture* SceneRenderer::GetHdrColorTextureForValidation() const noexcept
	{
		return m_HdrTargetCache ? m_HdrTargetCache->GetColorTextureForValidation() : nullptr;
	}

	const Texture* SceneRenderer::GetHdrDepthTextureForValidation() const noexcept
	{
		return m_HdrTargetCache ? m_HdrTargetCache->GetDepthTextureForValidation() : nullptr;
	}
#endif

#if defined(PF_G11_GPU_VALIDATION_TESTS)
#if !defined(PF_COOKED_RUNTIME_ONLY)
	std::expected<AssetPreparationService::Subscription, AssetPreparationError>
	SceneRenderer::EnqueuePreparationForValidation(AssetPreparationService::Processor Processor)
	{
		if (!Processor)
			return std::unexpected(AssetPreparationError{ AssetPreparationErrorCode::InvalidRequest, "Validation processor is empty" });
		const std::string RecipeKey = "gpu-validation-gate/" + std::to_string(++m_ValidationPreparationRequest);
		return m_ProjectAssetCaches->Preparation.Request(
			{ m_ProjectAssetCaches->SessionKey, RecipeKey }, AssetPreparationPriority::Interactive, 1, std::move(Processor));
	}
#endif
#endif

	const Texture* SceneRenderer::GetPointShadowFaceTextureForValidation(size_t Slot, uint32_t Face) const noexcept
	{
		return m_ShadowTargetCache->PointShadowFaceTexture(Slot, Face);
	}

	const Texture* SceneRenderer::GetDirectionalShadowTextureForValidation(size_t Cascade) const noexcept
	{
		return m_ShadowTargetCache->DirectionalShadowTexture(Cascade);
	}

	const Texture* SceneRenderer::GetSpotlightShadowTextureForValidation(size_t Slot) const noexcept
	{
		return m_ShadowTargetCache->SpotlightShadowTexture(Slot);
	}

	const Texture* SceneRenderer::GetAmbientOcclusionDepthTextureForValidation() const noexcept
	{
		return m_AmbientOcclusionPass->GetPrepassDepthTextureForValidation();
	}

#if !defined(PF_COOKED_RUNTIME_ONLY)
	std::expected<std::unique_ptr<SceneRenderer>, SceneRendererError> SceneRenderer::Create(
		RenderDevice& Runtime, const Project& SourceProject, const std::filesystem::path& CompiledShaderDirectory)
	{
		ScopedDiagnosticTimer Timer(Runtime.GetDiagnosticRecorder(), "Scene renderer initialization", "SceneRenderer");
		auto Renderer = std::unique_ptr<SceneRenderer>(new SceneRenderer(Runtime, SourceProject));
		if (auto Result = Renderer->Initialize(CompiledShaderDirectory); !Result)
			return std::unexpected(std::move(Result.error()));
		return Renderer;
	}
#endif

	std::expected<std::unique_ptr<SceneRenderer>, SceneRendererError> SceneRenderer::Create(
		RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content,
		const std::filesystem::path& CompiledShaderDirectory)
	{
		if (!Content)
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, {}, {},
				"Cooked scene renderer requires a content provider" });
		ScopedDiagnosticTimer Timer(Runtime.GetDiagnosticRecorder(), "Cooked scene renderer initialization", "SceneRenderer");
		auto Renderer = std::unique_ptr<SceneRenderer>(new SceneRenderer(Runtime, std::move(Content)));
		if (auto Result = Renderer->Initialize(CompiledShaderDirectory); !Result)
			return std::unexpected(std::move(Result.error()));
		return Renderer;
	}

	std::expected<void, SceneRendererError> SceneRenderer::Initialize(
		const std::filesystem::path& CompiledShaderDirectory)
	{
		ShaderHandle SceneVertexShader;
		ShaderHandle SceneFragmentShader;
		ShaderHandle BackgroundVertexShader;
		ShaderHandle BackgroundFragmentShader;
		ShaderHandle ShadowVertexShader;
		ShaderHandle ShadowFragmentShader;
		ShaderHandle PointShadowVertexShader;
		ShaderHandle PointShadowFragmentShader;
		auto VertexBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Triangle.vs.spv");
		if (!VertexBytecode)
			return std::unexpected(std::move(VertexBytecode.error()));
		auto FragmentBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Triangle.ps.spv");
		if (!FragmentBytecode)
			return std::unexpected(std::move(FragmentBytecode.error()));

		ShaderDesc VertexShaderDescription;
		VertexShaderDescription.Stage = ShaderStage::Vertex;
		VertexShaderDescription.EntryPoint = "VSMain";
		VertexShaderDescription.DebugName = "PulseForge scene vertex shader";
		auto VertexShader = m_Runtime.CreateShader(VertexShaderDescription, *VertexBytecode);
		if (!VertexShader)
			return std::unexpected(MakeResourceError("Could not create the scene vertex shader: " + VertexShader.error().Message));
		SceneVertexShader = std::move(VertexShader.value());

		ShaderDesc FragmentShaderDescription;
		FragmentShaderDescription.Stage = ShaderStage::Fragment;
		FragmentShaderDescription.EntryPoint = "PSMain";
		FragmentShaderDescription.DebugName = "PulseForge scene fragment shader";
		auto FragmentShader = m_Runtime.CreateShader(FragmentShaderDescription, *FragmentBytecode);
		if (!FragmentShader)
			return std::unexpected(MakeResourceError("Could not create the scene fragment shader: " + FragmentShader.error().Message));
		SceneFragmentShader = std::move(FragmentShader.value());

		auto BackgroundVertexBytecode = ReadShaderBytecode(CompiledShaderDirectory / "EnvironmentBackground.vs.spv");
		if (!BackgroundVertexBytecode)
			return std::unexpected(std::move(BackgroundVertexBytecode.error()));
		auto BackgroundFragmentBytecode = ReadShaderBytecode(CompiledShaderDirectory / "EnvironmentBackground.ps.spv");
		if (!BackgroundFragmentBytecode)
			return std::unexpected(std::move(BackgroundFragmentBytecode.error()));
		ShaderDesc BackgroundVertexDescription;
		BackgroundVertexDescription.Stage = ShaderStage::Vertex;
		BackgroundVertexDescription.EntryPoint = "VSMain";
		BackgroundVertexDescription.DebugName = "PulseForge environment background vertex shader";
		auto BackgroundVertex = m_Runtime.CreateShader(BackgroundVertexDescription, *BackgroundVertexBytecode);
		if (!BackgroundVertex)
			return std::unexpected(
				MakeResourceError("Could not create environment background vertex shader: " + BackgroundVertex.error().Message));
		BackgroundVertexShader = std::move(*BackgroundVertex);
		ShaderDesc BackgroundFragmentDescription;
		BackgroundFragmentDescription.Stage = ShaderStage::Fragment;
		BackgroundFragmentDescription.EntryPoint = "PSMain";
		BackgroundFragmentDescription.DebugName = "PulseForge environment background fragment shader";
		auto BackgroundFragment = m_Runtime.CreateShader(BackgroundFragmentDescription, *BackgroundFragmentBytecode);
		if (!BackgroundFragment)
			return std::unexpected(
				MakeResourceError("Could not create environment background fragment shader: " + BackgroundFragment.error().Message));
		BackgroundFragmentShader = std::move(*BackgroundFragment);

		auto ShadowVertexBytecode = ReadShaderBytecode(CompiledShaderDirectory / "DirectionalShadow.vs.spv");
		if (!ShadowVertexBytecode)
			return std::unexpected(std::move(ShadowVertexBytecode.error()));
		ShaderDesc ShadowVertexDescription;
		ShadowVertexDescription.Stage = ShaderStage::Vertex;
		ShadowVertexDescription.EntryPoint = "VSMain";
		ShadowVertexDescription.DebugName = "PulseForge directional shadow vertex shader";
		auto ShadowVertex = m_Runtime.CreateShader(ShadowVertexDescription, *ShadowVertexBytecode);
		if (!ShadowVertex)
			return std::unexpected(MakeResourceError("Could not create directional shadow vertex shader: " + ShadowVertex.error().Message));
		ShadowVertexShader = std::move(*ShadowVertex);

		const auto LoadAuxiliaryShader = [&](const char* FileName, ShaderStage Stage, const char* DebugName)
			-> std::expected<ShaderHandle, SceneRendererError>
		{
			auto Bytecode = ReadShaderBytecode(CompiledShaderDirectory / FileName);
			if (!Bytecode)
				return std::unexpected(std::move(Bytecode.error()));
			ShaderDesc Description;
			Description.Stage = Stage;
			Description.EntryPoint = Stage == ShaderStage::Vertex ? "VSMain" : "PSMain";
			Description.DebugName = DebugName;
			auto Shader = m_Runtime.CreateShader(Description, *Bytecode);
			if (!Shader)
				return std::unexpected(MakeResourceError(std::string("Could not create shader '") + DebugName + "': " + Shader.error().Message));
			return std::move(*Shader);
		};
		auto ShadowFragment =
			LoadAuxiliaryShader("DirectionalShadow.ps.spv", ShaderStage::Fragment, "PulseForge alpha-tested shadow fragment shader");
		if (!ShadowFragment) return std::unexpected(std::move(ShadowFragment.error()));
		ShadowFragmentShader = std::move(*ShadowFragment);
		auto AoPrepassVs = LoadAuxiliaryShader("SsaoNormalDepth.vs.spv", ShaderStage::Vertex, "PulseForge SSAO normal-depth vertex shader");
		auto AoPrepassPs =
			LoadAuxiliaryShader("SsaoNormalDepth.ps.spv", ShaderStage::Fragment, "PulseForge SSAO normal-depth fragment shader");
		auto AoVs = LoadAuxiliaryShader("Ssao.vs.spv", ShaderStage::Vertex, "PulseForge SSAO fullscreen vertex shader");
		auto AoPs = LoadAuxiliaryShader("Ssao.ps.spv", ShaderStage::Fragment, "PulseForge SSAO evaluation fragment shader");
		auto AoBlurVs = LoadAuxiliaryShader("SsaoBlur.vs.spv", ShaderStage::Vertex, "PulseForge SSAO blur vertex shader");
		auto AoBlurPs = LoadAuxiliaryShader("SsaoBlur.ps.spv", ShaderStage::Fragment, "PulseForge SSAO blur fragment shader");
		if (!AoPrepassVs || !AoPrepassPs || !AoVs || !AoPs || !AoBlurVs || !AoBlurPs)
			return std::unexpected(MakeResourceError("Could not load the SSAO shader set"));
		auto PointShadowVs = LoadAuxiliaryShader("PointShadow.vs.spv", ShaderStage::Vertex, "PulseForge point-shadow vertex shader");
		auto PointShadowPs =
			LoadAuxiliaryShader("PointShadow.ps.spv", ShaderStage::Fragment, "PulseForge point-shadow radial-depth fragment shader");
		if (!PointShadowVs || !PointShadowPs) return std::unexpected(MakeResourceError("Could not load the point-shadow shader set"));
		PointShadowVertexShader = std::move(*PointShadowVs);
		PointShadowFragmentShader = std::move(*PointShadowPs);

		auto FallbackEnvironment = CreateBlackCube(m_Runtime, "PulseForge fallback environment cube");
		auto FallbackIrradiance = CreateBlackCube(m_Runtime, "PulseForge fallback irradiance cube");
		auto FallbackPrefiltered = CreateBlackCube(m_Runtime, "PulseForge fallback prefiltered cube");
		if (!FallbackEnvironment || !FallbackIrradiance || !FallbackPrefiltered)
			return std::unexpected(MakeResourceError("Could not create fallback environment cubemaps"));
		m_FallbackEnvironmentTextures.Environment = std::move(*FallbackEnvironment);
		m_FallbackEnvironmentTextures.DiffuseIrradiance = std::move(*FallbackIrradiance);
		m_FallbackEnvironmentTextures.PrefilteredSpecular = std::move(*FallbackPrefiltered);
		const std::array<float, 4> BrdfFallback{ 0.0f, 0.0f, 0.0f, 0.0f };
		TextureDesc BrdfFallbackDescription;
		BrdfFallbackDescription.Width = 1;
		BrdfFallbackDescription.Height = 1;
		BrdfFallbackDescription.Format = TextureFormat::RGBA32_Float;
		BrdfFallbackDescription.DebugName = "PulseForge fallback BRDF LUT";
		auto FallbackBrdf = m_Runtime.CreateTexture(BrdfFallbackDescription,
			std::as_bytes(std::span<const float>(BrdfFallback)));
		if (!FallbackBrdf)
			return std::unexpected(MakeResourceError("Could not create fallback BRDF LUT: " + FallbackBrdf.error().Message));
		m_FallbackEnvironmentTextures.BrdfIntegrationLut = std::move(*FallbackBrdf);

		auto FallbackPointShadow = CreateWhiteCube(m_Runtime, "PulseForge fully-lit point-shadow fallback cube");
		if (!FallbackPointShadow)
			return std::unexpected(MakeResourceError("Could not create fallback point-shadow cubemap: " + FallbackPointShadow.error().Message));
		m_FallbackPointShadowTexture = std::move(*FallbackPointShadow);
		const std::array<float, 6> BackgroundVertices{ -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
		BufferDesc BackgroundBufferDescription;
		BackgroundBufferDescription.ByteSize = sizeof(BackgroundVertices);
		BackgroundBufferDescription.Usage = BufferUsage::Vertex;
		BackgroundBufferDescription.DebugName = "PulseForge environment background triangle";
		auto BackgroundBuffer = m_Runtime.CreateBuffer(
			BackgroundBufferDescription, std::as_bytes(std::span<const float>(BackgroundVertices)));
		if (!BackgroundBuffer)
			return std::unexpected(MakeResourceError("Could not create environment background geometry: " + BackgroundBuffer.error().Message));
		m_BackgroundTriangleBuffer = std::move(*BackgroundBuffer);

		auto ToneMappingVertexBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Tonemapping.vs.spv");
		if (!ToneMappingVertexBytecode)
			return std::unexpected(std::move(ToneMappingVertexBytecode.error()));
		auto ToneMappingFragmentBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Tonemapping.ps.spv");
		if (!ToneMappingFragmentBytecode)
			return std::unexpected(std::move(ToneMappingFragmentBytecode.error()));
		if (auto ToneMappingInitialized = m_ToneMappingStage->Initialize(*ToneMappingVertexBytecode, *ToneMappingFragmentBytecode);
			!ToneMappingInitialized)
			return std::unexpected(std::move(ToneMappingInitialized.error()));
		auto RuntimeUIVertexBytecode = ReadShaderBytecode(CompiledShaderDirectory / "RuntimeUI.vs.spv");
		if (!RuntimeUIVertexBytecode) return std::unexpected(std::move(RuntimeUIVertexBytecode.error()));
		auto RuntimeUIFragmentBytecode = ReadShaderBytecode(CompiledShaderDirectory / "RuntimeUI.ps.spv");
		if (!RuntimeUIFragmentBytecode) return std::unexpected(std::move(RuntimeUIFragmentBytecode.error()));
		if (auto Initialized = m_RuntimeUIRenderPass->Initialize(*RuntimeUIVertexBytecode, *RuntimeUIFragmentBytecode); !Initialized)
			return std::unexpected(MakeResourceError("Could not initialize runtime UI renderer: " + Initialized.error().Message));
		const std::array<uint8_t, 4> TransparentUiPixel{ 0, 0, 0, 0 };
		TextureDesc TransparentUiDescription;
		TransparentUiDescription.Width = 1;
		TransparentUiDescription.Height = 1;
		TransparentUiDescription.Format = TextureFormat::RGBA8_UNorm;
		TransparentUiDescription.DebugName = "PulseForge transparent runtime UI fallback";
		auto TransparentUi = m_Runtime.CreateTexture(TransparentUiDescription, std::as_bytes(std::span(TransparentUiPixel)));
		if (!TransparentUi)
			return std::unexpected(MakeResourceError("Could not create transparent runtime UI fallback: " + TransparentUi.error().Message));
		m_TransparentRuntimeUITexture = std::move(*TransparentUi);

		BindingLayoutDesc BindingLayoutDescription;
		BindingLayoutDescription.Visibility = ShaderVisibility::AllGraphics;
		BindingLayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 },
			{ BindingResourceType::TextureCube, 1 },
			{ BindingResourceType::TextureCube, 2 },
			{ BindingResourceType::TextureCube, 4 },
			{ BindingResourceType::Texture2D, 3 },
			{ BindingResourceType::Texture2D, 5 },
			{ BindingResourceType::Texture2D, 6 },
			{ BindingResourceType::Texture2D, 7 },
			{ BindingResourceType::Texture2D, 8 },
			{ BindingResourceType::Sampler, 0 },
			{ BindingResourceType::Sampler, 1 },
			{ BindingResourceType::ConstantBuffer, 0 },
			{ BindingResourceType::ConstantBuffer, 1 },
			{ BindingResourceType::ConstantBuffer, 2 },
			{ BindingResourceType::ConstantBuffer, 3 }
		};
		BindingLayoutDescription.DebugName = "PulseForge scene resources";
		auto BindingLayout = m_Runtime.CreateBindingLayout(BindingLayoutDescription);
		if (!BindingLayout)
			return std::unexpected(MakeResourceError("Could not create the scene binding layout: " + BindingLayout.error().Message));
		m_BindingLayout = std::move(BindingLayout.value());

		BindingLayoutDesc ShadowBindingLayoutDescription;
		ShadowBindingLayoutDescription.Visibility = ShaderVisibility::Fragment;
		ShadowBindingLayoutDescription.ShaderRegisterSpace = 1;
		ShadowBindingLayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 }, { BindingResourceType::Texture2D, 1 },
			{ BindingResourceType::Texture2D, 2 }, { BindingResourceType::Texture2D, 3 },
			{ BindingResourceType::Texture2D, 4 }, { BindingResourceType::Texture2D, 5 },
			{ BindingResourceType::Texture2D, 6 }, { BindingResourceType::Texture2D, 7 },
			{ BindingResourceType::Sampler, 0 }
		};
		ShadowBindingLayoutDescription.DebugName = "PulseForge directional and spotlight shadow maps (space1)";
		auto ShadowBindingLayout = m_Runtime.CreateBindingLayout(ShadowBindingLayoutDescription);
		if (!ShadowBindingLayout)
			return std::unexpected(MakeResourceError("Could not create combined shadow binding layout: " + ShadowBindingLayout.error().Message));
		m_ShadowBindingLayout = std::move(*ShadowBindingLayout);

		BindingLayoutDesc PointShadowBindingLayoutDescription;
		PointShadowBindingLayoutDescription.Visibility = ShaderVisibility::Fragment;
		PointShadowBindingLayoutDescription.ShaderRegisterSpace = 3;
		PointShadowBindingLayoutDescription.Items = {
			{ BindingResourceType::TextureCube, 0 }, { BindingResourceType::TextureCube, 1 },
			{ BindingResourceType::Sampler, 0 }
		};
		PointShadowBindingLayoutDescription.DebugName = "PulseForge point-shadow cubemaps (space3)";
		auto PointShadowBindingLayout = m_Runtime.CreateBindingLayout(PointShadowBindingLayoutDescription);
		if (!PointShadowBindingLayout)
			return std::unexpected(MakeResourceError("Could not create point-shadow binding layout: " + PointShadowBindingLayout.error().Message));
		m_PointShadowBindingLayout = std::move(*PointShadowBindingLayout);

		BindingLayoutDesc ShadowObjectLayoutDescription;
		ShadowObjectLayoutDescription.Visibility = ShaderVisibility::Vertex;
		ShadowObjectLayoutDescription.ShaderRegisterSpace = 3;
		ShadowObjectLayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		ShadowObjectLayoutDescription.DebugName = "PulseForge directional shadow object constants";
		auto ShadowObjectLayout = m_Runtime.CreateBindingLayout(ShadowObjectLayoutDescription);
		if (!ShadowObjectLayout)
			return std::unexpected(MakeResourceError("Could not create shadow object layout: " + ShadowObjectLayout.error().Message));
		m_ShadowObjectBindingLayout = std::move(*ShadowObjectLayout);

		BindingLayoutDesc PointShadowObjectLayoutDescription;
		PointShadowObjectLayoutDescription.Visibility = ShaderVisibility::AllGraphics;
		PointShadowObjectLayoutDescription.ShaderRegisterSpace = 3;
		PointShadowObjectLayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		PointShadowObjectLayoutDescription.DebugName = "PulseForge point-shadow pass/object constants";
		auto PointShadowObjectLayout = m_Runtime.CreateBindingLayout(PointShadowObjectLayoutDescription);
		if (!PointShadowObjectLayout)
			return std::unexpected(MakeResourceError("Could not create point-shadow object layout: " + PointShadowObjectLayout.error().Message));
		m_PointShadowObjectBindingLayout = std::move(*PointShadowObjectLayout);

		const ObjectConstants InitialObject{};
		BufferDesc ObjectBufferDescription;
		ObjectBufferDescription.ByteSize = sizeof(InitialObject);
		ObjectBufferDescription.Usage = BufferUsage::Constant;
		ObjectBufferDescription.DebugName = "PulseForge scene object constants";
		auto ObjectBuffer = m_Runtime.CreateBuffer(ObjectBufferDescription, std::as_bytes(std::span(&InitialObject, 1)));
		if (!ObjectBuffer)
			return std::unexpected(MakeResourceError("Could not create scene object constants: " + ObjectBuffer.error().Message));
		m_ObjectConstantsBuffer = std::move(ObjectBuffer.value());

		const FrameConstants InitialFrame{};
		BufferDesc FrameBufferDescription;
		FrameBufferDescription.ByteSize = sizeof(InitialFrame);
		FrameBufferDescription.Usage = BufferUsage::Constant;
		FrameBufferDescription.DebugName = "PulseForge scene frame and light constants";
		auto FrameBuffer = m_Runtime.CreateBuffer(FrameBufferDescription, std::as_bytes(std::span(&InitialFrame, 1)));
		if (!FrameBuffer)
			return std::unexpected(MakeResourceError("Could not create scene frame constants: " + FrameBuffer.error().Message));
		m_FrameConstantsBuffer = std::move(FrameBuffer.value());

		const LocalLightingConstants InitialLocalLighting{};
		BufferDesc LocalLightingBufferDescription;
		LocalLightingBufferDescription.ByteSize = sizeof(InitialLocalLighting);
		LocalLightingBufferDescription.Usage = BufferUsage::Constant;
		LocalLightingBufferDescription.DebugName = "PulseForge bounded point/spot light constants";
		auto LocalLightingBuffer = m_Runtime.CreateBuffer(
			LocalLightingBufferDescription, std::as_bytes(std::span(&InitialLocalLighting, 1)));
		if (!LocalLightingBuffer)
			return std::unexpected(MakeResourceError("Could not create local-light constants: " + LocalLightingBuffer.error().Message));
		m_LocalLightingConstantsBuffer = std::move(*LocalLightingBuffer);

		const glm::mat4 InitialShadowObject(1.0f);
		BufferDesc ShadowObjectDescription;
		ShadowObjectDescription.ByteSize = sizeof(InitialShadowObject);
		ShadowObjectDescription.Usage = BufferUsage::Constant;
		ShadowObjectDescription.DebugName = "PulseForge shadow object constants";
		auto ShadowObjectBuffer = m_Runtime.CreateBuffer(
			ShadowObjectDescription, std::as_bytes(std::span(&InitialShadowObject, 1)));
		if (!ShadowObjectBuffer)
			return std::unexpected(MakeResourceError("Could not create shadow object constants: " + ShadowObjectBuffer.error().Message));
		m_ShadowObjectConstantsBuffer = std::move(*ShadowObjectBuffer);
		BindingSetDesc ShadowObjectSetDescription;
		ShadowObjectSetDescription.Layout = m_ShadowObjectBindingLayout;
		ShadowObjectSetDescription.Buffers.push_back({ 0, std::cref(*m_ShadowObjectConstantsBuffer) });
		auto ShadowObjectSet = m_Runtime.CreateBindingSet(ShadowObjectSetDescription);
		if (!ShadowObjectSet)
			return std::unexpected(MakeResourceError("Could not create shadow object binding set: " + ShadowObjectSet.error().Message));
		m_ShadowObjectBindingSet = std::move(*ShadowObjectSet);
		const PointShadowObjectConstants InitialPointShadow{};
		BufferDesc PointShadowObjectBufferDescription;
		PointShadowObjectBufferDescription.ByteSize = sizeof(PointShadowObjectConstants);
		PointShadowObjectBufferDescription.Usage = BufferUsage::Constant;
		PointShadowObjectBufferDescription.DebugName = "PulseForge point-shadow pass/object constants";
		auto PointShadowObjectBuffer = m_Runtime.CreateBuffer(
			PointShadowObjectBufferDescription, std::as_bytes(std::span(&InitialPointShadow, 1)));
		if (!PointShadowObjectBuffer)
			return std::unexpected(MakeResourceError("Could not create point-shadow object constants: " + PointShadowObjectBuffer.error().Message));
		m_PointShadowObjectConstantsBuffer = std::move(*PointShadowObjectBuffer);
		BindingSetDesc PointShadowObjectSetDescription;
		PointShadowObjectSetDescription.Layout = m_PointShadowObjectBindingLayout;
		PointShadowObjectSetDescription.Buffers.push_back({ 0, std::cref(*m_PointShadowObjectConstantsBuffer) });
		auto PointShadowObjectSet = m_Runtime.CreateBindingSet(PointShadowObjectSetDescription);
		if (!PointShadowObjectSet)
			return std::unexpected(MakeResourceError("Could not create point-shadow object binding set: " + PointShadowObjectSet.error().Message));
		m_PointShadowObjectBindingSet = std::move(*PointShadowObjectSet);

		SamplerDesc ShadowSamplerDescription;
		ShadowSamplerDescription.Minification = SamplerFilter::Nearest;
		ShadowSamplerDescription.Magnification = SamplerFilter::Nearest;
		ShadowSamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
		ShadowSamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
		ShadowSamplerDescription.DebugName = "PulseForge shadow PCF sampler";
		auto ShadowSampler = m_Runtime.CreateSampler(ShadowSamplerDescription);
		if (!ShadowSampler)
			return std::unexpected(MakeResourceError("Could not create shadow sampler: " + ShadowSampler.error().Message));
		m_ShadowSampler = std::move(*ShadowSampler);

		if (auto ShadowTargets = m_ShadowTargetCache->Initialize(m_ShadowBindingLayout,
				m_PointShadowBindingLayout, *m_ShadowSampler, *m_FallbackPointShadowTexture);
			!ShadowTargets)
			return std::unexpected(std::move(ShadowTargets.error()));
		if (auto Initialized = m_AmbientOcclusionPass->Initialize(std::move(*AoPrepassVs), std::move(*AoPrepassPs), std::move(*AoVs),
				std::move(*AoPs), std::move(*AoBlurVs), std::move(*AoBlurPs));
			!Initialized)
			return std::unexpected(std::move(Initialized.error()));
		const SceneRendererMaterialBindingCache::Resources MaterialBindingResources{ m_ProjectAssetCaches->Materials,
			m_ProjectAssetCaches->Textures, m_BindingLayout, *m_AmbientOcclusionPass->GetFallbackOcclusionTexture(),
			m_ObjectConstantsBuffer, m_FrameConstantsBuffer, m_LocalLightingConstantsBuffer };
		if (auto Initialized = m_MaterialBindingCache->Initialize(MaterialBindingResources); !Initialized)
			return std::unexpected(std::move(Initialized.error()));
		SceneRendererPipelineCache::Resources PipelineResources;
		PipelineResources.SceneVertexShader = std::move(SceneVertexShader);
		PipelineResources.SceneFragmentShader = std::move(SceneFragmentShader);
		PipelineResources.BackgroundVertexShader = std::move(BackgroundVertexShader);
		PipelineResources.BackgroundFragmentShader = std::move(BackgroundFragmentShader);
		PipelineResources.ShadowVertexShader = std::move(ShadowVertexShader);
		PipelineResources.ShadowFragmentShader = std::move(ShadowFragmentShader);
		PipelineResources.PointShadowVertexShader = std::move(PointShadowVertexShader);
		PipelineResources.PointShadowFragmentShader = std::move(PointShadowFragmentShader);
		PipelineResources.SceneMaterialLayout = m_BindingLayout;
		PipelineResources.DirectionalShadowLayout = m_ShadowBindingLayout;
		PipelineResources.AmbientOcclusionLayout = m_AmbientOcclusionPass->GetFinalLayout();
		PipelineResources.PointLightShadowLayout = m_PointShadowBindingLayout;
		PipelineResources.ShadowObjectLayout = m_ShadowObjectBindingLayout;
		PipelineResources.PointShadowObjectLayout = m_PointShadowObjectBindingLayout;
		if (auto PipelineCache = m_PipelineCache->Initialize(std::move(PipelineResources)); !PipelineCache)
			return std::unexpected(std::move(PipelineCache.error()));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareScene(
		const Scene& Source,
		UUID CameraEntity,
		float AspectRatio)
	{
		return PrepareSnapshot(SceneRenderSnapshotBuilder::Build(Source, CameraEntity, AspectRatio));
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareScene(
		const Scene& Source,
		float AspectRatio)
	{
		return PrepareSnapshot(SceneRenderSnapshotBuilder::Build(Source, AspectRatio));
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareScene(
		const Scene& Source,
		const glm::mat4& ViewProjection,
		const glm::vec3& CameraWorldPosition)
	{
		return PrepareSnapshot(SceneRenderSnapshotBuilder::BuildForView(Source, ViewProjection, CameraWorldPosition));
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareScene(
		const Scene& Source,
		const glm::mat4& View,
		const glm::mat4& Projection,
		const glm::vec3& CameraWorldPosition,
		float NearClipPlane,
		float FarClipPlane)
	{
		return PrepareSnapshot(SceneRenderSnapshotBuilder::BuildForView(
			Source, View, Projection, CameraWorldPosition, NearClipPlane, FarClipPlane));
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareSceneFiltered(
		const Scene& Source,
		const glm::mat4& View,
		const glm::mat4& Projection,
		const glm::vec3& CameraWorldPosition,
		float NearClipPlane,
		float FarClipPlane,
		std::span<const UUID> ExcludedEntities)
	{
		return PrepareSnapshot(SceneRenderSnapshotBuilder::BuildForViewFiltered(
			Source, View, Projection, CameraWorldPosition, NearClipPlane, FarClipPlane, ExcludedEntities));
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareSnapshot(
		std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> Snapshot)
	{
		std::optional<ScopedDiagnosticTimer> FirstPrepareTimer;
		if (!m_LoggedFirstSnapshotPreparation)
		{
			m_LoggedFirstSnapshotPreparation = true;
			FirstPrepareTimer.emplace(m_Runtime.GetDiagnosticRecorder(), "Scene renderer first preparation", "SceneRenderer");
		}
		m_PreparedSnapshot.reset();
		m_PreparedMeshResources.clear();
		m_PreparedRuntimeUITextures.clear();
		m_PreparedMaterialBindings.clear();
		m_PreparedEnvironmentTextures = &m_FallbackEnvironmentTextures;
		m_PreparedEnvironmentBinding = nullptr;
		m_PreparedEnvironmentRevision = 0;
		m_EnvironmentLightingPending = false;
		m_AssetPreparationPending = false;
		m_PreparedEnvironmentReady = false;
		m_ProjectAssetCaches->UploadBudget.BeginFrame();
		// Publish completed GPU replacements before any resource or binding is pinned for this frame.
		(void)m_ProjectAssetCaches->Meshes.AdvancePending();
		(void)m_ProjectAssetCaches->Textures.AdvancePending();
		(void)m_ProjectAssetCaches->EnvironmentLighting.AdvancePending();
		if (!Snapshot)
		{
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::SnapshotBuildFailed,
				Snapshot.error().Entity,
				{},
				Snapshot.error().Message
			});
		}
		const auto ResolveImage = [this](AssetID Asset) -> const Texture*
		{
			if (Asset.IsNil()) return nullptr;
			auto Loaded = m_ProjectAssetCaches->Textures.GetOrLoad(Asset, TextureFormat::RGBA8_Srgb);
			if (!Loaded) return nullptr;
			const Texture* Resource = &Loaded->get();
			m_PreparedRuntimeUITextures.push_back(Resource);
			return Resource;
		};
		const auto ResolveFont = [this](AssetID Asset) -> std::expected<std::vector<std::byte>, std::string>
		{
			constexpr uint64_t MaximumFontBytes = 32ull * 1024ull * 1024ull;
			if (Asset.IsNil()) return std::unexpected("Font asset identity is missing");
			if (m_ProjectAssetCaches->CookedContent)
			{
				auto Bytes = m_ProjectAssetCaches->CookedContent->ReadFont(Asset);
				if (!Bytes) return std::unexpected(Bytes.error().Message);
				if (Bytes->empty() || Bytes->size() > MaximumFontBytes)
					return std::unexpected("Cooked font is empty or exceeds the 32 MiB runtime limit");
				return std::move(*Bytes);
			}
#if !defined(PF_COOKED_RUNTIME_ONLY)
			if (!m_Project) return std::unexpected("No managed project is active for font resolution");
			const auto Record = m_Project->GetAssetRegistry().Find(Asset);
			if (!Record) return std::unexpected("Font asset is not present in the active project registry");
			std::string Extension = Record->ProjectRelativePath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension != ".ttf" && Extension != ".otf")
				return std::unexpected("Runtime fonts must be managed TrueType or OpenType assets");
			auto Path = AssetPathResolver::ResolveManagedSourcePath(m_Project->GetRootPath(), Record->ProjectRelativePath);
			if (!Path) return std::unexpected(Path.error().Message);
			std::error_code FileError;
			const uint64_t Size = std::filesystem::file_size(*Path, FileError);
			if (FileError || Size == 0 || Size > MaximumFontBytes)
				return std::unexpected("Managed font is empty, unreadable, or exceeds the 32 MiB runtime limit");
			std::ifstream File(*Path, std::ios::binary);
			if (!File) return std::unexpected("Could not open managed font asset");
			std::vector<std::byte> Bytes(static_cast<size_t>(Size));
			File.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!File) return std::unexpected("Could not read complete managed font asset");
			return Bytes;
#else
			return std::unexpected("Cooked font provider is unavailable");
#endif
		};
		if (auto Prepared = m_RuntimeUIRenderPass->Prepare(m_RuntimeUIDrawCommands, m_RuntimeUIView, ResolveImage, ResolveFont);
			!Prepared)
			return std::unexpected(MakeDrawError("Could not prepare runtime UI resources: " + Prepared.error().Message));
		for (SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			Instance.Orientation = ClassifyWorldTransform(Instance.WorldTransform);
			if (Instance.Orientation == WorldTransformOrientation::Singular)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::SnapshotBuildFailed, Instance.Entity, Instance.MeshAsset,
					"Cannot prepare a mesh with a singular or non-finite world transform" });
		}
		m_PreparedMeshResources.reserve(Snapshot->Meshes.size());
		m_PreparedMaterialBindings.reserve(Snapshot->Meshes.size());
		const std::optional<AssetID> EnvironmentAsset = Snapshot->EnvironmentLight
			? std::optional<AssetID>{ Snapshot->EnvironmentLight->HdrImage }
			: std::nullopt;
		const EnvironmentLightingTextures* EnvironmentTextures = &m_FallbackEnvironmentTextures;
		std::optional<AssetID> BindingEnvironmentAsset = EnvironmentAsset;
		uint64_t BindingEnvironmentRevision = 0;
		if (EnvironmentAsset)
		{
			auto LoadedEnvironment = m_ProjectAssetCaches->EnvironmentLighting.GetOrLoad(*EnvironmentAsset);
			if (!LoadedEnvironment)
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Snapshot->EnvironmentLight->Entity,
					*EnvironmentAsset,
					"Could not prepare HDR environment: " + LoadedEnvironment.error().Message
				});
			if (!LoadedEnvironment->has_value())
			{
				m_EnvironmentLightingPending = true;
				m_AssetPreparationPending = true;
				BindingEnvironmentAsset.reset();
			}
			else
			{
				EnvironmentTextures = &LoadedEnvironment->value().get();
				m_PreparedEnvironmentReady = true;
			}
			if (m_PreparedEnvironmentReady)
				BindingEnvironmentRevision = m_ProjectAssetCaches->EnvironmentLighting.GetResourceRevision(*EnvironmentAsset);
			if (auto Bindings =
					m_MaterialBindingCache->EnsureEnvironmentBindings(*EnvironmentAsset, *EnvironmentTextures, m_PreparedEnvironmentReady,
						BindingEnvironmentRevision);
				!Bindings)
				return std::unexpected(std::move(Bindings.error()));
			m_PreparedEnvironmentRevision = BindingEnvironmentRevision;
			m_PreparedEnvironmentBinding = m_MaterialBindingCache->FindEnvironmentBindings(
				*EnvironmentAsset, m_PreparedEnvironmentReady, BindingEnvironmentRevision);
			if (!m_PreparedEnvironmentBinding)
				return std::unexpected(MakeDrawError("Prepared environment binding was not retained for this frame"));
		}

		std::optional<VertexLayoutDesc> SceneVertexLayout;
		std::vector<SceneMeshInstance> ReadyMeshes;
		ReadyMeshes.reserve(Snapshot->Meshes.size());
		for (const SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			if (Instance.MaterialAsset && !m_ProjectAssetCaches->Materials.IsRegistered(*Instance.MaterialAsset))
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
					*Instance.MaterialAsset, "Referenced material asset is not present in the project registry" });

			auto Mesh = m_ProjectAssetCaches->Meshes.GetOrLoad(Instance.MeshAsset);
			if (!Mesh)
			{
				if (Mesh.error().Code == MeshAssetErrorCode::Pending)
				{
					m_AssetPreparationPending = true;
					continue;
				}
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Instance.Entity,
					Instance.MeshAsset,
					"Could not load mesh asset: " + Mesh.error().Message
				});
			}

			const VertexLayoutDesc& Layout = Mesh->get().GetVertexLayout();
			if (SceneVertexLayout && !SceneRendererPipelineCache::HasSameVertexLayout(*SceneVertexLayout, Layout))
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::UnsupportedVertexLayout,
					Instance.Entity,
					Instance.MeshAsset,
					"This scene renderer instance requires all visible meshes to share one vertex layout"
				});
			}
			SceneVertexLayout = Layout;
			m_PreparedMeshResources.try_emplace(Instance.MeshAsset, &Mesh->get());

			std::expected<void, SceneRendererError> Bindings;
			if (Instance.MaterialAsset)
			{
				const bool HasOverride = m_TransientMaterialOverride &&
					m_TransientMaterialOverride->Material == *Instance.MaterialAsset;
				const MaterialAssetDesc* OverrideDescription = HasOverride ? &m_TransientMaterialOverride->Description : nullptr;
				const uint64_t OverrideRevision = HasOverride ? m_TransientMaterialOverride->Revision : 0;
				Bindings = m_MaterialBindingCache->EnsureMaterialBindings(*Instance.MaterialAsset, BindingEnvironmentAsset,
					BindingEnvironmentRevision, *EnvironmentTextures, OverrideDescription, OverrideRevision);
			}
			else if (!BindingEnvironmentAsset)
				Bindings = m_MaterialBindingCache->EnsureDefaultMaterialBindings(*EnvironmentTextures, BindingEnvironmentRevision);
			if (!Bindings)
			{
				if (Bindings.error().Code == SceneRendererErrorCode::AssetLoadPending)
				{
					m_AssetPreparationPending = true;
					continue;
				}
				Bindings.error().Entity = Instance.Entity;
				return std::unexpected(std::move(Bindings.error()));
			}
			const bool HasOverride = Instance.MaterialAsset && m_TransientMaterialOverride &&
				m_TransientMaterialOverride->Material == *Instance.MaterialAsset;
			const MaterialAssetDesc* OverrideDescription = HasOverride ? &m_TransientMaterialOverride->Description : nullptr;
			const uint64_t OverrideRevision = HasOverride ? m_TransientMaterialOverride->Revision : 0;
			auto MaterialBinding = m_MaterialBindingCache->ResolveMaterialBinding(
				Instance, BindingEnvironmentAsset, BindingEnvironmentRevision, OverrideDescription, OverrideRevision);
			if (!MaterialBinding)
			{
				if (MaterialBinding.error().Code == SceneRendererErrorCode::AssetLoadPending)
				{
					m_AssetPreparationPending = true;
					continue;
				}
				return std::unexpected(std::move(MaterialBinding.error()));
			}
			m_PreparedMaterialBindings.insert_or_assign(Instance.Entity, *MaterialBinding);
			ReadyMeshes.push_back(Instance);
		}
		Snapshot->Meshes = std::move(ReadyMeshes);
#if !defined(PF_COOKED_RUNTIME_ONLY)
		const AssetPreparationServiceStats PreparationStats = m_ProjectAssetCaches->Preparation.GetStats();
		m_AssetPreparationPending |= PreparationStats.QueuedJobs > 0 || PreparationStats.ActiveJobs > 0 ||
			PreparationStats.CpuReadyJobs > 0 || m_ProjectAssetCaches->Meshes.HasPendingWork() ||
			m_ProjectAssetCaches->Textures.HasPendingWork() || m_ProjectAssetCaches->EnvironmentLighting.HasPendingWork();
#endif

		if (SceneVertexLayout)
		{
			if (auto Layout = m_PipelineCache->SetVertexLayout(*SceneVertexLayout); !Layout)
				return std::unexpected(std::move(Layout.error()));
			if (auto Pipeline = m_PipelineCache->EnsureScenePipeline(ColorTargetFormat::Swapchain); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
			if (Snapshot->DirectionalLight && Snapshot->DirectionalLight->CastShadows)
				if (auto ShadowPipeline = m_PipelineCache->EnsureShadowPipeline(Snapshot->DirectionalLight->ShadowBias); !ShadowPipeline)
					return std::unexpected(std::move(ShadowPipeline.error()));
		}

		m_PreparedCascades.reset();
		if (Snapshot->HasCameraFrustum && Snapshot->DirectionalLight &&
			Snapshot->DirectionalLight->CastShadows && !Snapshot->Meshes.empty())
		{
			const SceneDirectionalLight& Light = *Snapshot->DirectionalLight;
			auto Cascades = BuildDirectionalShadowCascades(
				Snapshot->View, Snapshot->Projection, Snapshot->NearClipPlane, Snapshot->FarClipPlane,
				Light.ShadowDistance, Light.RayDirection, 1024);
			if (!Cascades)
				return std::unexpected(MakeDrawError("Could not build directional shadow cascades: " + Cascades.error().Message));
			m_PreparedCascades = std::move(*Cascades);
		}
		m_PreparedSnapshot = std::move(*Snapshot);
		m_PreparedEnvironmentTextures = EnvironmentTextures;
		return {};
	}

	std::expected<const BindingSet*, SceneRendererError> SceneRenderer::ResolveMaterialBinding(const SceneMeshInstance& Instance)
	{
		const BindingSet* Binding = FindPreparedMaterialBinding(Instance.Entity);
		if (!Binding)
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, Instance.Entity,
				Instance.MaterialAsset, "Prepared material binding is unavailable for this frame" });
		return Binding;
	}

	const BindingSet* SceneRenderer::FindPreparedMaterialBinding(UUID Entity) const noexcept
	{
		const auto Found = m_PreparedMaterialBindings.find(Entity);
		return Found == m_PreparedMaterialBindings.end() ? nullptr : Found->second;
	}

	std::expected<const MaterialAssetDesc*, SceneRendererError> SceneRenderer::ResolveMaterialDescription(const SceneMeshInstance& Instance)
	{
		const MaterialAssetDesc* OverrideDescription = Instance.MaterialAsset && m_TransientMaterialOverride &&
			m_TransientMaterialOverride->Material == *Instance.MaterialAsset ? &m_TransientMaterialOverride->Description : nullptr;
		return m_MaterialBindingCache->ResolveMaterialDescription(Instance, OverrideDescription);
	}

	std::expected<void, SceneRendererError> SceneRenderer::RenderShadowCascades()
	{
		const ShadowPass::Resources Resources{ m_Runtime, m_PreparedSnapshot ? &*m_PreparedSnapshot : nullptr,
			m_PreparedCascades ? &*m_PreparedCascades : nullptr,
			*m_LocalLightingState, *m_ShadowTargetCache,
			*m_PipelineCache, *m_MaterialBindingCache, m_PreparedMeshResources, m_PreparedMaterialBindings,
			m_ShadowBindingLayout, m_PointShadowBindingLayout,
			*m_ShadowSampler, m_ShadowObjectConstantsBuffer, m_ShadowObjectBindingSet, m_PointShadowObjectConstantsBuffer,
			m_PointShadowObjectBindingSet };
		return m_ShadowPass->Render(Resources);
	}
	std::expected<void, SceneRendererError> SceneRenderer::RenderAmbientOcclusion(uint32_t Width, uint32_t Height)
	{
		if (!m_PreparedSnapshot)
		{
			m_AmbientOcclusionPass->InvalidateFrame();
			return {};
		}

		const auto ResolveMaterial = [this](const SceneMeshInstance& Instance) { return ResolveMaterialDescription(Instance); };
		const auto ResolveBindings = [this](const SceneMeshInstance& Instance) -> std::expected<const BindingSet*, SceneRendererError>
		{
			auto Bindings = ResolveMaterialBinding(Instance);
			if (!Bindings) return std::unexpected(std::move(Bindings.error()));
			return *Bindings;
		};

		const AmbientOcclusionPass::RenderInputs Inputs{ *m_PreparedSnapshot, m_PipelineCache->GetVertexLayout(), m_BindingLayout,
			m_PreparedMeshResources, *m_BackgroundTriangleBuffer, ResolveMaterial, ResolveBindings };
		return m_AmbientOcclusionPass->Render(Inputs, Width, Height);
	}
	bool SceneRenderer::SetAmbientOcclusionSettings(const AmbientOcclusionSettings& Settings) noexcept
	{
		return m_AmbientOcclusionPass->SetSettings(Settings);
	}

	const AmbientOcclusionSettings& SceneRenderer::GetAmbientOcclusionSettings() const noexcept
	{
		return m_AmbientOcclusionPass->GetSettings();
	}

	void SceneRenderer::RefreshMaterialAssets() noexcept
	{
		m_PreparedSnapshot.reset();
		m_PreparedMaterialBindings.clear();
		m_MaterialBindingCache->RefreshMaterialAssets();
	}

	void SceneRenderer::RefreshMaterialAsset(const AssetID& Material) noexcept
	{
		m_PreparedSnapshot.reset();
		m_PreparedMaterialBindings.clear();
		m_MaterialBindingCache->RefreshMaterialAsset(Material);
	}

	std::expected<void, SceneRendererError> SceneRenderer::SetTransientMaterialOverride(
		const AssetID& Material, const MaterialAssetDesc& Description, uint64_t Revision)
	{
		if (Material.IsNil() || Revision == 0)
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, {}, Material,
				"A transient material preview requires a valid asset and nonzero draft revision" });
		try
		{
			auto Replacement = std::make_unique<TransientMaterialOverride>(TransientMaterialOverride{ Material, Description, Revision });
			m_TransientMaterialOverride = std::move(Replacement);
			m_PreparedSnapshot.reset();
			m_PreparedMaterialBindings.clear();
			m_MaterialBindingCache->RefreshMaterialAssets();
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, {}, Material,
				std::string("Could not retain the transient material draft: ") + Exception.what() });
		}
	}

	void SceneRenderer::ClearTransientMaterialOverride() noexcept
	{
		if (!m_TransientMaterialOverride) return;
		m_TransientMaterialOverride.reset();
		m_PreparedSnapshot.reset();
		m_PreparedMaterialBindings.clear();
		m_MaterialBindingCache->RefreshMaterialAssets();
	}

	void SceneRenderer::RefreshProjectAssets() noexcept
	{
		m_TransparentDrawOrder.clear();
		m_PreparedSnapshot.reset();
		m_PreparedMeshResources.clear();
		m_PreparedRuntimeUITextures.clear();
		m_PreparedMaterialBindings.clear();
		m_PreparedCascades.reset();
		m_PreparedEnvironmentTextures = &m_FallbackEnvironmentTextures;
		m_PreparedEnvironmentBinding = nullptr;
		m_PreparedEnvironmentRevision = 0;
		m_EnvironmentLightingPending = false;
		m_AssetPreparationPending = false;
		m_PreparedEnvironmentReady = false;
		m_MaterialBindingCache->RefreshProjectAssets();
		m_RuntimeUIRenderPass->ResetAssetResources();
#if !defined(PF_COOKED_RUNTIME_ONLY)
		if (m_Project)
			m_ProjectAssetCaches->Clear(m_Project->GetAssetRegistry());
		else
#endif
		{
			m_ProjectAssetCaches->Meshes.Clear();
			m_ProjectAssetCaches->Textures.Clear();
			m_ProjectAssetCaches->EnvironmentLighting.Clear();
			m_ProjectAssetCaches->Materials.Clear();
		}
	}

	std::expected<HdrSceneRenderResult, SceneRendererError> SceneRenderer::RenderPreparedSceneToHdr(uint32_t Width, uint32_t Height)
	{
		if (!m_PreparedSnapshot)
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::SnapshotBuildFailed,
				{},
				{},
				"HDR scene rendering requires a successfully prepared scene snapshot" });

		if (m_HdrTargetCache->NeedsReplacement(Width, Height)) m_ToneMappingStage->InvalidateBindings();
		auto Target = m_HdrTargetCache->Ensure(Width, Height);
		if (!Target) return std::unexpected(std::move(Target.error()));

		RenderTargetClearValue ClearValue;
		ClearValue.Color = { 0.0f, 0.0f, 0.0f, 1.0f };
		ClearValue.Depth = 1.0f;
		auto Rendered = RenderPreparedScene(*Target->Target, ClearValue);
		if (!Rendered) return std::unexpected(std::move(Rendered.error()));

		const Texture* Color = Target->Target->GetColorTexture();
		if (!Color) return std::unexpected(MakeResourceError("HDR scene target color texture is unavailable after rendering"));
		const ShadowPass::Statistics& ShadowStatistics = m_ShadowPass->GetStatistics();
		return HdrSceneRenderResult{ Color, Target->Target->GetDepthTexture(), Width, Height, *Rendered,
			ShadowStatistics.SpotlightPassCount, ShadowStatistics.SpotlightCasterDrawCount,
			m_PreparedSnapshot->EnvironmentLight.has_value(), Target->Generation, m_ShadowTargetCache->SpotlightResourceGeneration(),
			ShadowStatistics.PointPassCount, ShadowStatistics.PointCasterDrawCount, m_ShadowTargetCache->PointLightResourceGeneration(),
			m_LocalLightingState->PointSlotOwners() };
	}

	std::expected<const Texture*, SceneRendererError> SceneRenderer::PrepareRuntimeUIOverlay(uint32_t Width, uint32_t Height)
	{
		if (Width == 0 || Height == 0)
			return std::unexpected(MakeResourceError("Runtime UI overlay requires a non-zero output size"));
		if (!m_RuntimeUIRenderPass || !m_TransparentRuntimeUITexture)
			return std::unexpected(MakeResourceError("Runtime UI overlay resources are not initialized"));
		if (m_RuntimeUIRenderPass->GetStatistics().QuadCount == 0)
			return m_TransparentRuntimeUITexture.get();
		if (!m_RuntimeUIOverlayTarget || m_RuntimeUIOverlayWidth != Width || m_RuntimeUIOverlayHeight != Height)
		{
			RenderTargetDesc Description;
			Description.Width = Width;
			Description.Height = Height;
			Description.ColorFormat = ColorTargetFormat::RGBA16_Float;
			Description.DepthMode = DepthAttachmentMode::None;
			Description.DebugName = "PulseForge prepared linear runtime UI overlay";
			auto Target = m_Runtime.CreateRenderTarget(Description);
			if (!Target)
				return std::unexpected(MakeDrawError("Could not create linear runtime UI overlay: " + Target.error().Message));
			m_ToneMappingStage->InvalidateBindings();
			m_RuntimeUIOverlayTarget = std::move(*Target);
			m_RuntimeUIOverlayWidth = Width;
			m_RuntimeUIOverlayHeight = Height;
			++m_RuntimeUIOverlayTargetCreationCount;
		}
		const Texture* Overlay = m_RuntimeUIOverlayTarget->GetColorTexture();
		if (!Overlay) return std::unexpected(MakeResourceError("Runtime UI overlay has no color texture"));

		RenderTargetClearValue Clear;
		Clear.Color = { 0.0f, 0.0f, 0.0f, 0.0f };
		Clear.Depth = 1.0f;
		if (const GraphicsResult Begin = m_Runtime.BeginRenderTarget(*m_RuntimeUIOverlayTarget, Clear); !Begin)
			return std::unexpected(MakeDrawError("Could not begin linear runtime UI overlay: " + Begin.error().Message));
		++m_RuntimeUIOverlayClearCount;
		RenderTargetFrameScope Scope(m_Runtime);
		auto Draw = m_RuntimeUIRenderPass->Draw(m_RuntimeUIDrawCommands, m_RuntimeUIView, Width, Height,
			ColorTargetFormat::RGBA16_Float, false);
		const GraphicsResult End = Scope.End();
		if (!Draw)
		{
			if (!End) PF_CORE_ERROR("Could not close runtime UI overlay after draw failure: {0}", End.error().Message);
			return std::unexpected(MakeDrawError("Could not render linear runtime UI overlay: " + Draw.error().Message));
		}
		if (!End) return std::unexpected(MakeDrawError("Could not end linear runtime UI overlay: " + End.error().Message));
		return Overlay;
	}

	RuntimeUIOverlayFrameStats SceneRenderer::GetRuntimeUIOverlayFrame(const Texture& BoundTexture) const noexcept
	{
		const TextureDesc& Description = BoundTexture.GetDescription();
		const uint64_t PixelCount = static_cast<uint64_t>(Description.Width) * Description.Height;
		const uint64_t BytesPerPixel = GetTextureFormatBytesPerPixel(Description.Format).value_or(0);
		const uint64_t PayloadBytes = BytesPerPixel != 0 && PixelCount <= (std::numeric_limits<uint64_t>::max)() / BytesPerPixel
			? PixelCount * BytesPerPixel : 0;
		const uint64_t OverlayPixelCount = static_cast<uint64_t>(m_RuntimeUIOverlayWidth) * m_RuntimeUIOverlayHeight;
		constexpr uint64_t OverlayBytesPerPixel = sizeof(uint16_t) * 4;
		const uint64_t OverlayPayloadBytes = OverlayPixelCount <=
			(std::numeric_limits<uint64_t>::max)() / OverlayBytesPerPixel ? OverlayPixelCount * OverlayBytesPerPixel : 0;
		return { .CanvasPresent = m_RuntimeUICanvasPresent,
			.TransparentFallbackBound = &BoundTexture == m_TransparentRuntimeUITexture.get(),
			.BoundTextureWidth = Description.Width, .BoundTextureHeight = Description.Height,
			.BoundTextureLogicalPayloadBytes = PayloadBytes,
			.RetainedOverlayWidth = m_RuntimeUIOverlayWidth, .RetainedOverlayHeight = m_RuntimeUIOverlayHeight,
			.RetainedOverlayLogicalPayloadBytes = OverlayPayloadBytes,
			.OverlayTargetCreationCount = m_RuntimeUIOverlayTargetCreationCount,
			.OverlayClearCount = m_RuntimeUIOverlayClearCount,
			.ToneMappingBindingCreationCount = m_ToneMappingStage->BindingSetCreationCount(),
			.RuntimeUITextureBindingCreationCount = m_RuntimeUIRenderPass->GetStatistics().TextureBindingCreationCount };
	}

	std::expected<SceneRenderOutputResult, SceneRendererError> SceneRenderer::RenderPreparedSceneToOutput()
	{
		const auto [Width, Height] = m_Runtime.GetOutputFramebufferSize();
		if (Width == 0 || Height == 0)
			return SceneRenderOutputResult{};
		auto Hdr = RenderPreparedSceneToHdr(Width, Height);
		if (!Hdr)
			return std::unexpected(std::move(Hdr.error()));
		auto Overlay = PrepareRuntimeUIOverlay(Width, Height);
		if (!Overlay) return std::unexpected(std::move(Overlay.error()));
		if (auto Tonemapped = m_ToneMappingStage->Draw(
				*Hdr->ColorTexture, Hdr->TargetGeneration, **Overlay, ColorTargetFormat::Swapchain, true, *m_BackgroundTriangleBuffer);
			!Tonemapped)
			return std::unexpected(std::move(Tonemapped.error()));
		const RuntimeUIRenderStatistics UIStats = m_RuntimeUIRenderPass->GetStatistics();
		SceneRenderOutputResult Result{ Hdr->GeometryDrawCount, Hdr->TargetGeneration, ColorTargetFormat::Swapchain,
			ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, m_Runtime.GetOutputColorEncoding()), Hdr->SpotlightShadowPassCount,
			Hdr->SpotlightShadowCasterDrawCount, Hdr->SpotlightShadowResourceGeneration, m_LocalLightingState->SpotlightSlotOwners(),
			Hdr->PointShadowPassCount, Hdr->PointShadowCasterDrawCount, Hdr->PointShadowResourceGeneration, Hdr->PointShadowSlotOwners,
			UIStats.QuadCount, UIStats.BatchCount, UIStats.TextureCount, UIStats.FontAtlasCount };
		Result.RuntimeUIOverlay = GetRuntimeUIOverlayFrame(**Overlay);
		return Result;
	}

	std::expected<SceneRenderOutputResult, SceneRendererError> SceneRenderer::RenderPreparedSceneToOutput(const RenderTarget& Target)
	{
		const RenderTargetDesc& Description = Target.GetDescription();
		if (Description.Width == 0 || Description.Height == 0 ||
			(Description.ColorFormat != ColorTargetFormat::RGBA8_UNorm &&
			 Description.ColorFormat != ColorTargetFormat::RGBA8_Srgb) || !Target.GetColorTexture())
			return std::unexpected(MakeResourceError("Tone mapping requires a valid RGBA8_UNorm or RGBA8_Srgb color target"));

		auto Hdr = RenderPreparedSceneToHdr(Description.Width, Description.Height);
		if (!Hdr)
			return std::unexpected(std::move(Hdr.error()));
		auto Overlay = PrepareRuntimeUIOverlay(Description.Width, Description.Height);
		if (!Overlay) return std::unexpected(std::move(Overlay.error()));
		if (auto Prepared = m_ToneMappingStage->Prepare(
				*Hdr->ColorTexture, Hdr->TargetGeneration, **Overlay, Description.ColorFormat,
				Description.DepthMode != DepthAttachmentMode::None);
			!Prepared)
			return std::unexpected(std::move(Prepared.error()));

		RenderTargetClearValue Clear;
		Clear.Color = { 0.0f, 0.0f, 0.0f, 1.0f };
		Clear.Depth = 1.0f;
		if (const GraphicsResult Begin = m_Runtime.BeginRenderTarget(Target, Clear); !Begin)
			return std::unexpected(MakeDrawError("Could not begin tone-mapped output target: " + Begin.error().Message));
		RenderTargetFrameScope Scope(m_Runtime);
		auto Draw = m_ToneMappingStage->Draw(*Hdr->ColorTexture, Hdr->TargetGeneration, **Overlay, Description.ColorFormat,
			Description.DepthMode != DepthAttachmentMode::None, *m_BackgroundTriangleBuffer);
		const GraphicsResult End = Scope.End();
		if (!Draw)
		{
			if (!End)
				PF_CORE_ERROR("Could not close tone-mapped output target after draw failure: {0}", End.error().Message);
			return std::unexpected(std::move(Draw.error()));
		}
		if (!End)
			return std::unexpected(MakeDrawError("Could not end tone-mapped output target: " + End.error().Message));
		const RuntimeUIRenderStatistics UIStats = m_RuntimeUIRenderPass->GetStatistics();
		SceneRenderOutputResult Result{ Hdr->GeometryDrawCount, Hdr->TargetGeneration, Description.ColorFormat,
			ShouldShaderEncodeSrgb(Description.ColorFormat, m_Runtime.GetOutputColorEncoding()), Hdr->SpotlightShadowPassCount,
			Hdr->SpotlightShadowCasterDrawCount, Hdr->SpotlightShadowResourceGeneration, m_LocalLightingState->SpotlightSlotOwners(),
			Hdr->PointShadowPassCount, Hdr->PointShadowCasterDrawCount, Hdr->PointShadowResourceGeneration, Hdr->PointShadowSlotOwners,
			UIStats.QuadCount, UIStats.BatchCount, UIStats.TextureCount, UIStats.FontAtlasCount };
		Result.RuntimeUIOverlay = GetRuntimeUIOverlayFrame(**Overlay);
		return Result;
	}

	void SceneRenderer::SetRuntimeUIFrame(
		std::span<const RuntimeUIDrawCommand> Commands, const RuntimeUIViewport& Viewport, bool CanvasPresent)
	{
		m_RuntimeUIDrawCommands.assign(Commands.begin(), Commands.end());
		m_RuntimeUIView = Viewport;
		m_RuntimeUICanvasPresent = CanvasPresent;
	}

	bool SceneRenderer::SetToneMappingSettings(const ToneMappingSettings& Settings) noexcept
	{
		return m_ToneMappingStage->SetSettings(Settings);
	}

	const ToneMappingSettings& SceneRenderer::GetToneMappingSettings() const noexcept
	{
		return m_ToneMappingStage->GetSettings();
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedScene()
	{
		if (auto Shadows = RenderShadowCascades(); !Shadows)
			return std::unexpected(std::move(Shadows.error()));
		const auto [Width, Height] = m_Runtime.GetOutputFramebufferSize();
		if (auto AmbientOcclusion = RenderAmbientOcclusion(Width, Height); !AmbientOcclusion)
			return std::unexpected(std::move(AmbientOcclusion.error()));
		return RenderPreparedSceneForFormat(ColorTargetFormat::Swapchain, Width, Height);
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedScene(
		const RenderTarget& Target,
		const RenderTargetClearValue& ClearValue)
	{
		const ColorTargetFormat ColorFormat = Target.GetDescription().ColorFormat;
		if (m_PreparedSnapshot && m_PreparedSnapshot->EnvironmentLight)
		{
			if (auto Pipeline = m_PipelineCache->EnsureBackgroundPipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}
		if (m_PreparedSnapshot && !m_PreparedSnapshot->Meshes.empty())
		{
			if (auto Pipeline = m_PipelineCache->EnsureScenePipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}
		if (auto Shadows = RenderShadowCascades(); !Shadows)
			return std::unexpected(std::move(Shadows.error()));
		if (auto AmbientOcclusion = RenderAmbientOcclusion(
			Target.GetDescription().Width, Target.GetDescription().Height); !AmbientOcclusion)
			return std::unexpected(std::move(AmbientOcclusion.error()));

		const GraphicsResult BeginResult = m_Runtime.BeginRenderTarget(Target, ClearValue);
		if (!BeginResult)
			return std::unexpected(MakeDrawError("Could not begin scene render target: " + BeginResult.error().Message));

		RenderTargetFrameScope TargetScope(m_Runtime);
		auto Rendered = RenderPreparedSceneForFormat(
			ColorFormat, Target.GetDescription().Width, Target.GetDescription().Height);
		const GraphicsResult EndResult = TargetScope.End();
		if (!Rendered)
		{
			if (!EndResult)
				PF_CORE_ERROR("Could not close scene render target after draw failure: {0}", EndResult.error().Message);
			return std::unexpected(std::move(Rendered.error()));
		}
		if (!EndResult)
			return std::unexpected(MakeDrawError("Could not end scene render target: " + EndResult.error().Message));

		return Rendered;
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedSceneForFormat(
		ColorTargetFormat ColorFormat,
		uint32_t Width,
		uint32_t Height)
	{
		if (!m_PreparedSnapshot || (m_PreparedSnapshot->Meshes.empty() && !m_PreparedSnapshot->EnvironmentLight))
			return size_t{ 0 };
		if (m_PreparedSnapshot->EnvironmentLight)
			if (auto Pipeline = m_PipelineCache->EnsureBackgroundPipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		size_t SubmittedDraws = 0;
		FrameConstants Frame{};
		Frame.CameraWorldPosition[0] = m_PreparedSnapshot->CameraWorldPosition.x;
		Frame.CameraWorldPosition[1] = m_PreparedSnapshot->CameraWorldPosition.y;
		Frame.CameraWorldPosition[2] = m_PreparedSnapshot->CameraWorldPosition.z;
		Frame.InverseViewProjection = glm::inverse(m_PreparedSnapshot->ViewProjection);
		Frame.View = m_PreparedSnapshot->View;
		Frame.Projection = m_PreparedSnapshot->Projection;
		if (Width > 0 && Height > 0)
			Frame.OutputSize = glm::vec4(static_cast<float>(Width), static_cast<float>(Height),
				1.0f / static_cast<float>(Width), 1.0f / static_cast<float>(Height));
		if (m_PreparedSnapshot->EnvironmentLight && !IsFinite(Frame.InverseViewProjection))
			return std::unexpected(MakeDrawError("Environment background requires an invertible view-projection matrix"));
		if (m_PreparedSnapshot->EnvironmentLight)
		{
			const SceneEnvironmentLight& Environment = *m_PreparedSnapshot->EnvironmentLight;
			const glm::quat InverseRotation{
				Environment.WorldRotation.w,
				-Environment.WorldRotation.x,
				-Environment.WorldRotation.y,
				-Environment.WorldRotation.z };
			Frame.EnvironmentInverseRotation[0] = InverseRotation.x;
			Frame.EnvironmentInverseRotation[1] = InverseRotation.y;
			Frame.EnvironmentInverseRotation[2] = InverseRotation.z;
			Frame.EnvironmentInverseRotation[3] = InverseRotation.w;
			Frame.EnvironmentParameters[0] = Environment.Intensity;
			Frame.EnvironmentParameters[1] = static_cast<float>(m_PreparedEnvironmentTextures->PrefilteredSpecular->GetDescription().MipLevels - 1);
		}
		if (m_PreparedSnapshot->DirectionalLight)
		{
			const SceneDirectionalLight& Light = *m_PreparedSnapshot->DirectionalLight;
			Frame.LightRayDirection[0] = Light.RayDirection.x;
			Frame.LightRayDirection[1] = Light.RayDirection.y;
			Frame.LightRayDirection[2] = Light.RayDirection.z;
			Frame.LightColorIntensity[0] = Light.Color.r;
			Frame.LightColorIntensity[1] = Light.Color.g;
			Frame.LightColorIntensity[2] = Light.Color.b;
			Frame.LightColorIntensity[3] = Light.Intensity;
			if (Light.CastShadows && m_PreparedCascades)
			{
				Frame.ShadowParameters[0] = 1.0f;
				Frame.ShadowParameters[1] = Light.ShadowNormalBias;
				Frame.ShadowParameters[2] = Light.ShadowSoftness;
				Frame.ShadowParameters[3] = m_PreparedCascades->ShadowDistance;
				for (size_t CascadeIndex = 0; CascadeIndex < m_PreparedCascades->Cascades.size(); ++CascadeIndex)
				{
					Frame.CascadeViewProjection[CascadeIndex] = m_PreparedCascades->Cascades[CascadeIndex].ViewProjection;
					Frame.CascadeSplitDepths[CascadeIndex] = m_PreparedCascades->Cascades[CascadeIndex].FarDistance;
				}
			}
		}
		Frame.SpotShadowViewProjection = m_LocalLightingState->SpotlightMatrices();
		Frame.SpotShadowParameters = m_LocalLightingState->SpotlightParameters();
		Frame.PointShadowParameters = m_LocalLightingState->PointParameters();
		const auto LocalLightingUpdate =
			m_Runtime.WriteBuffer(*m_LocalLightingConstantsBuffer, 0, std::as_bytes(std::span(&m_LocalLightingState->Constants(), 1)));
		if (!LocalLightingUpdate)
			return std::unexpected(MakeDrawError("Could not update local-light constants: " + LocalLightingUpdate.error().Message));

		const auto FrameUpdate = m_Runtime.WriteBuffer(*m_FrameConstantsBuffer, 0, std::as_bytes(std::span(&Frame, 1)));
		if (!FrameUpdate)
			return std::unexpected(MakeDrawError("Could not update scene frame constants: " + FrameUpdate.error().Message));

		if (m_PreparedSnapshot->EnvironmentLight)
		{
			const BindingSet* BackgroundBindings = m_PreparedEnvironmentBinding;
			if (!BackgroundBindings) return std::unexpected(MakeDrawError("Prepared environment background bindings are unavailable"));
			const std::array<const BindingSet*, 1> BindingSets = { BackgroundBindings };
			const DrawArguments BackgroundArguments{ 3, 1, 0, 0 };
			const GraphicsPipeline* BackgroundPipeline = m_PipelineCache->FindBackgroundPipeline(ColorFormat);
			if (!BackgroundPipeline) return std::unexpected(MakeDrawError("Prepared environment background pipeline is unavailable"));
			const GraphicsResult BackgroundDraw =
				m_Runtime.Draw(*BackgroundPipeline, *m_BackgroundTriangleBuffer, BackgroundArguments, BindingSets);
			if (!BackgroundDraw)
				return std::unexpected(MakeDrawError("Could not draw environment background: " + BackgroundDraw.error().Message));
		}
		if (m_PreparedSnapshot->Meshes.empty()) return size_t{ 0 };

		const auto DrawMaterialInstance = [this, ColorFormat, &SubmittedDraws](const SceneMeshInstance& Instance,
											  const MaterialAssetDesc& Material) -> std::expected<void, SceneRendererError>
		{
			const bool AlphaBlend = Material.AlphaMode == MaterialAlphaMode::Blend;
			if (auto PipelineResult =
					m_PipelineCache->EnsureScenePipeline(ColorFormat, AlphaBlend, Material.DoubleSided, Instance.Orientation);
				!PipelineResult)
				return std::unexpected(std::move(PipelineResult.error()));
			const GraphicsPipeline* Pipeline =
				m_PipelineCache->FindScenePipeline(ColorFormat, AlphaBlend, Material.DoubleSided, Instance.Orientation);
			if (!Pipeline) return std::unexpected(MakeResourceError("Prepared material graphics pipeline is unavailable"));
			auto MaterialBinding = ResolveMaterialBinding(Instance);
			if (!MaterialBinding) return std::unexpected(std::move(MaterialBinding.error()));
			const Mesh* MeshResource = FindPreparedMesh(m_PreparedMeshResources, Instance.MeshAsset);
			if (!MeshResource)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity, Instance.MeshAsset,
					"Prepared mesh resource is unavailable" });

			const auto NormalTransform = BuildNormalTransform(Instance.WorldTransform);
			if (!NormalTransform)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::DrawFailed, Instance.Entity, Instance.MeshAsset,
					"Cannot render a mesh with a singular/non-finite normal transform" });
			const ObjectConstants Object{
				Instance.WorldTransform,
				m_PreparedSnapshot->ViewProjection * Instance.WorldTransform,
				*NormalTransform };
			const auto Update = m_Runtime.WriteBuffer(
				*m_ObjectConstantsBuffer,
				0,
				std::as_bytes(std::span(&Object, 1)));
			if (!Update)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::DrawFailed,
					Instance.Entity,
					Instance.MeshAsset,
					"Could not update per-object scene constants: " + Update.error().Message
				});
			}

			const DrawIndexedArguments Arguments{ MeshResource->GetIndexCount(), 1, 0, 0 };
			const BindingSet* AmbientOcclusionBindings = m_AmbientOcclusionPass->GetSceneBindingSet();
			const std::array<const BindingSet*, 4> BindingSets = { *MaterialBinding, &m_ShadowTargetCache->ShadowBindings(),
				AmbientOcclusionBindings, &m_ShadowTargetCache->PointLightBindings() };
			const GraphicsResult Draw = m_Runtime.DrawIndexed(*Pipeline, *MeshResource, Arguments, BindingSets);
			if (!Draw)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::DrawFailed,
					Instance.Entity,
					Instance.MeshAsset,
					"Could not draw scene mesh: " + Draw.error().Message
				});
			}
			++SubmittedDraws;
			return {};
		};

		m_TransparentDrawOrder.clear();
		m_TransparentDrawOrder.reserve(std::max(m_TransparentDrawOrder.capacity(), m_PreparedSnapshot->Meshes.size()));
		for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
		{
			auto Material = ResolveMaterialDescription(Instance);
			if (!Material)
				return std::unexpected(std::move(Material.error()));
			if ((*Material)->AlphaMode == MaterialAlphaMode::Blend)
			{
				m_TransparentDrawOrder.push_back(&Instance);
				continue;
			}
			if (auto Draw = DrawMaterialInstance(Instance, **Material); !Draw)
				return std::unexpected(std::move(Draw.error()));
		}
		const auto CameraDepth = [this](const SceneMeshInstance* Instance)
		{
			const glm::vec3 WorldOrigin = glm::vec3(Instance->WorldTransform[3]);
			return -(m_PreparedSnapshot->View * glm::vec4(WorldOrigin, 1.0f)).z;
		};
		std::sort(m_TransparentDrawOrder.begin(), m_TransparentDrawOrder.end(),
			[&CameraDepth](const SceneMeshInstance* Left, const SceneMeshInstance* Right)
			{
				const float LeftDepth = CameraDepth(Left);
				const float RightDepth = CameraDepth(Right);
				if (LeftDepth != RightDepth)
					return LeftDepth > RightDepth;
				return Left->Entity < Right->Entity;
			});
		for (const SceneMeshInstance* Instance : m_TransparentDrawOrder)
		{
			auto Material = ResolveMaterialDescription(*Instance);
			if (!Material)
				return std::unexpected(std::move(Material.error()));
			if (auto Draw = DrawMaterialInstance(*Instance, **Material); !Draw)
				return std::unexpected(std::move(Draw.error()));
		}
		return SubmittedDraws;
	}
} // namespace PulseForge
