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
#include "Assets/CookedProductCodec.h"
#include "Assets/MaterialGraphArtifact.h"
#include "Assets/GltfMeshImportCoordinator.h"
#include "Animation/GltfSkinning.h"
#include "Assets/AssetPathResolver.h"
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/AssetPreparationService.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/MaterialGraphCompiler.h"
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
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <limits>
#include <span>
#include <exception>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace PulseForge
{
	namespace
	{
		constexpr size_t MaximumRendererSkeletalVerticesPerSnapshot = 2'000'000;
		constexpr size_t MaximumRendererSkeletalProductBytes = 256ull * 1024ull * 1024ull;

		[[nodiscard]] std::string MakeSkeletalProductCacheKey(const AssetID& Asset, std::string_view ProductKey)
		{
			return Asset.ToString() + "/" + std::string(ProductKey);
		}

		[[nodiscard]] std::optional<size_t> EstimateSkeletalProductBytes(const ImportedGltfSkeletalMesh& Product)
		{
			size_t Total = sizeof(Product);
			const auto Add = [&Total](size_t Count, size_t ElementSize)
			{
				if (ElementSize != 0 && Count > (MaximumRendererSkeletalProductBytes - Total) / ElementSize) return false;
				Total += Count * ElementSize;
				return true;
			};
			if (!Add(Product.Geometry.Vertices.capacity(), sizeof(GltfMeshVertex)) ||
				!Add(Product.Geometry.Indices.capacity(), sizeof(uint32_t)) || !Add(Product.Geometry.Name.capacity(), sizeof(char)) ||
				!Add(Product.SkeletonNodes.capacity(), sizeof(ImportedGltfSkeletonNode)) ||
				!Add(Product.JointNodeIndices.capacity(), sizeof(uint16_t)) ||
				!Add(Product.InverseBindMatrices.capacity(), sizeof(std::array<float, 16>)) ||
				!Add(Product.VertexInfluences.capacity(), sizeof(GltfVertexInfluences)) ||
				!Add(Product.Animations.capacity(), sizeof(ImportedGltfAnimationClip)))
				return std::nullopt;
			for (const ImportedGltfSkeletonNode& Node : Product.SkeletonNodes)
				if (!Add(Node.StableKey.capacity(), sizeof(char)) || !Add(Node.Name.capacity(), sizeof(char))) return std::nullopt;
			for (const ImportedGltfAnimationClip& Clip : Product.Animations)
			{
				if (!Add(Clip.StableKey.capacity(), sizeof(char)) || !Add(Clip.Name.capacity(), sizeof(char)) ||
					!Add(Clip.Channels.capacity(), sizeof(ImportedGltfAnimationChannel))) return std::nullopt;
				for (const ImportedGltfAnimationChannel& Channel : Clip.Channels)
					if (!Add(Channel.TargetNodeKey.capacity(), sizeof(char)) || !Add(Channel.Times.capacity(), sizeof(float)) ||
						!Add(Channel.Values.capacity(), sizeof(std::array<float, 4>))) return std::nullopt;
			}
			return Total;
		}

		[[nodiscard]] std::optional<SceneAxisAlignedBounds> ComputeSkeletalPoseBounds(
			std::span<const GltfMeshVertex> Vertices)
		{
			if (Vertices.empty()) return std::nullopt;
			glm::vec3 Minimum(std::numeric_limits<float>::max());
			glm::vec3 Maximum(std::numeric_limits<float>::lowest());
			for (const GltfMeshVertex& Vertex : Vertices)
			{
				const glm::vec3 Position(Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]);
				if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z))
					return std::nullopt;
				Minimum = glm::min(Minimum, Position);
				Maximum = glm::max(Maximum, Position);
			}
			return SceneAxisAlignedBounds{ Minimum, Maximum };
		}
	}

	struct GraphMaterialShaderRuntime final
	{
		std::string RecipeFingerprint;
		ShaderHandle FragmentShader;
		BindingLayoutHandle MaterialLayout;
	};

	struct GraphMaterialRuntime final
	{
		std::string RecipeFingerprint;
		std::shared_ptr<GraphMaterialShaderRuntime> ShaderResources;
		BufferHandle ParameterBuffer;
		std::vector<std::byte> ParameterBytes;
		std::vector<std::byte> PendingParameterBytes;
		BindingSetHandle MaterialBindings;
		uint64_t PointShadowResourceGeneration = (std::numeric_limits<uint64_t>::max)();
	};

	struct SceneRenderer::TransientMaterialOverride final
	{
		AssetID Material;
		MaterialAssetDesc Description;
		std::optional<CookedGraphMaterialProduct> GraphProduct;
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

		const Mesh* FindPreparedMesh(const PreparedMeshResourceMap& PreparedMeshes, UUID Entity)
		{
			const auto Found = PreparedMeshes.find(Entity);
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

		CookedMaterialProduct MakeCookedMaterialProduct(const MaterialAssetDesc& Material)
		{
			CookedMaterialProduct Product;
			Product.BaseColorTexture = Material.BaseColorTexture;
			Product.MetallicRoughnessTexture = Material.MetallicRoughnessTexture;
			Product.NormalTexture = Material.NormalTexture;
			Product.OcclusionTexture = Material.OcclusionTexture;
			Product.EmissiveTexture = Material.EmissiveTexture;
			for (size_t Component = 0; Component < Product.BaseColorFactor.size(); ++Component)
				Product.BaseColorFactor[Component] = Material.BaseColorFactor[static_cast<int>(Component)];
			Product.MetallicFactor = Material.MetallicFactor;
			Product.RoughnessFactor = Material.RoughnessFactor;
			Product.NormalScale = Material.NormalScale;
			Product.OcclusionStrength = Material.OcclusionStrength;
			for (size_t Component = 0; Component < Product.EmissiveFactor.size(); ++Component)
				Product.EmissiveFactor[Component] = Material.EmissiveFactor[static_cast<int>(Component)];
			Product.AlphaMode = static_cast<CookedMaterialAlphaMode>(Material.AlphaMode);
			Product.AlphaCutoff = Material.AlphaCutoff;
			Product.DoubleSided = Material.DoubleSided;
			return Product;
		}

		std::vector<std::byte> BuildGraphParameterBytes(
			std::span<const CookedGraphMaterialProduct::Parameter> InputParameters)
		{
			std::vector<CookedGraphMaterialProduct::Parameter> Parameters(InputParameters.begin(), InputParameters.end());
			std::ranges::sort(Parameters, {}, &CookedGraphMaterialProduct::Parameter::Name);
			const size_t RegisterCount = (std::max)(size_t{ 1 }, (Parameters.size() + 3) / 4);
			std::vector<std::byte> Bytes(RegisterCount * 16, std::byte{ 0 });
			for (size_t Index = 0; Index < Parameters.size(); ++Index)
				std::memcpy(Bytes.data() + Index * sizeof(float), &Parameters[Index].DefaultValue, sizeof(float));
			return Bytes;
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
		static uint64_t AllocateGeneration() noexcept
		{
			static std::atomic<uint64_t> Next{ 1 };
			uint64_t Generation = Next.fetch_add(1, std::memory_order_relaxed);
			if (Generation == 0) Generation = Next.fetch_add(1, std::memory_order_relaxed);
			return Generation;
		}

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
		UUID ProjectID;
		uint64_t Generation = 0;
		MeshAssetCache Meshes;
		TextureAssetCache Textures;
		MaterialAssetCache Materials;
		EnvironmentLightingCache EnvironmentLighting;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		std::unordered_map<AssetID,
			std::expected<MaterialGraphCompileIdentity, MaterialGraphCompileError>, UUIDHash> GraphMaterialIdentities;
		size_t GraphMaterialIdentityDescribeCalls = 0;
		size_t GraphMaterialIdentityCacheHits = 0;
#endif

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
			  ProjectID(SourceProject.GetDescription().ID), Generation(AllocateGeneration()),
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
			  ProjectID(CookedContent ? CookedContent->GetProjectID() : UUID{}), Generation(AllocateGeneration()),
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
			GraphMaterialIdentities.clear();
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
			Generation = AllocateGeneration();
			Meshes.SetSessionKey(SessionKey);
			Textures.SetSessionKey(SessionKey);
			EnvironmentLighting.SetSessionKey(SessionKey);
			Materials.Clear();
		}
#else
		ProjectAssetCaches(RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content)
			: CookedContent(std::move(Content)), ProjectID(CookedContent ? CookedContent->GetProjectID() : UUID{}),
			  Generation(AllocateGeneration()), Meshes(Runtime, CookedContent, Runtime.GetDiagnosticRecorder()),
			  Textures(Runtime, CookedContent, Runtime.GetDiagnosticRecorder()), Materials(CookedContent, Runtime.GetDiagnosticRecorder()),
			  EnvironmentLighting(Runtime, CookedContent, Runtime.GetDiagnosticRecorder())
		{
			if (!CookedContent)
				throw std::invalid_argument("Cooked scene renderer requires a content provider");
		}
#endif
	};

	struct SceneRenderer::PreparedSkeletalMeshResource final
	{
		AssetID SourceAsset;
		std::string ProductKey;
		std::shared_ptr<const ImportedGltfSkeletalMesh> Product;
		std::vector<std::array<float, 16>> LastLocalTransforms;
		std::vector<std::byte> PendingVertexData;
		std::optional<SceneAxisAlignedBounds> CurrentPoseBounds;
		MeshHandle Resource;
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

#ifndef PF_COOKED_RUNTIME_ONLY
	SceneRendererResourceCacheStats SceneRenderer::GetResourceCacheStats() const noexcept
	{
		SceneRendererResourceCacheStats Stats;
		if (!m_ProjectAssetCaches) return Stats;
		Stats.ProjectID = m_ProjectAssetCaches->ProjectID;
		Stats.Generation = m_ProjectAssetCaches->Generation;
		Stats.Meshes = m_ProjectAssetCaches->Meshes.GetInventoryStats();
		Stats.Textures = m_ProjectAssetCaches->Textures.GetInventoryStats();
		Stats.EnvironmentLighting = m_ProjectAssetCaches->EnvironmentLighting.GetInventoryStats();
		Stats.MaterialDescriptions = m_ProjectAssetCaches->Materials.GetLoadedCount();
		Stats.GraphMaterialResources = m_GraphMaterialResources.size();
		Stats.GraphMaterialIdentityDescribeCalls = m_ProjectAssetCaches->GraphMaterialIdentityDescribeCalls;
		Stats.GraphMaterialIdentityCacheHits = m_ProjectAssetCaches->GraphMaterialIdentityCacheHits;
		Stats.GraphMaterialDxcProcessLaunches = m_GraphMaterialDxcProcessLaunches;
		Stats.GraphMaterialShaderCreations = m_GraphMaterialShaderCreations;
		Stats.GraphMaterialBindingLayoutCreations = m_GraphMaterialBindingLayoutCreations;
		Stats.GraphMaterialParameterBufferCreations = m_GraphMaterialParameterBufferCreations;
		Stats.GraphMaterialBindingSetCreations = m_GraphMaterialBindingSetCreations;
		Stats.GraphMaterialParameterUploads = m_GraphMaterialParameterUploads;
		Stats.GraphMaterialPipelines = m_PipelineCache->GetGraphScenePipelineCount();
		Stats.GraphMaterialPipelineCreations = m_PipelineCache->GetGraphScenePipelineCreations();
		Stats.GraphMaterialPipelineEvictions = m_PipelineCache->GetGraphScenePipelineEvictions();
		for (const auto& [Recipe, Resources] : m_GraphMaterialShaderResources)
		{
			(void)Recipe;
			Stats.GraphMaterialShaderResources += Resources.expired() ? 0u : 1u;
		}
		Stats.SkeletalMeshResources = m_SkeletalMeshResources.size();
		return Stats;
	}
#endif

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
	std::expected<void, SceneRendererError> SceneRenderer::SeedSkeletalProductForValidation(
		const AssetID& SourceAsset,
		std::string_view ProductKey,
		std::shared_ptr<const ImportedGltfSkeletalMesh> Product)
	{
		if (SourceAsset.IsNil() || !IsValidGltfSkeletalProductKey(ProductKey) || !Product || m_PreparedSnapshot)
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, std::nullopt, SourceAsset,
				"Validation skeletal product identity, payload, or renderer preparation state is invalid" });
		const auto EstimatedBytes = EstimateSkeletalProductBytes(*Product);
		if (!EstimatedBytes || *EstimatedBytes > MaximumRendererSkeletalProductBytes - m_SkeletalProductBytes)
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, std::nullopt, SourceAsset,
				"Validation skeletal product exceeds the renderer's 256 MiB immutable product budget" });
		const std::string CacheKey = MakeSkeletalProductCacheKey(SourceAsset, ProductKey);
		if (m_SkeletalProducts.contains(CacheKey))
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, std::nullopt, SourceAsset,
				"Validation skeletal product key was already seeded" });
		m_SkeletalProducts.emplace(CacheKey, std::pair{ std::move(Product), *EstimatedBytes });
		m_SkeletalProductBytes += *EstimatedBytes;
		return {};
	}
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

	const Texture* SceneRenderer::GetAmbientOcclusionTextureForValidation() const noexcept
	{
		return m_AmbientOcclusionPass->GetFinalOcclusionTextureForValidation();
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
		m_PreparedGraphMaterials.clear();
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
		std::erase_if(m_SkeletalMeshResources, [&Snapshot](const auto& Entry)
		{
			return std::none_of(Snapshot->Meshes.begin(), Snapshot->Meshes.end(), [&Entry](const SceneMeshInstance& Instance)
			{
				return Instance.Entity == Entry.first && Instance.SkeletalProductKey &&
					*Instance.SkeletalProductKey == Entry.second->ProductKey && Instance.MeshAsset == Entry.second->SourceAsset;
			});
		});
		std::vector<std::string> RequiredSkeletalProductKeys;
		RequiredSkeletalProductKeys.reserve(Snapshot->Meshes.size());
		for (const SceneMeshInstance& Instance : Snapshot->Meshes)
			if (Instance.SkeletalProductKey)
				RequiredSkeletalProductKeys.push_back(MakeSkeletalProductCacheKey(Instance.MeshAsset, *Instance.SkeletalProductKey));
		std::sort(RequiredSkeletalProductKeys.begin(), RequiredSkeletalProductKeys.end());
		RequiredSkeletalProductKeys.erase(
			std::unique(RequiredSkeletalProductKeys.begin(), RequiredSkeletalProductKeys.end()), RequiredSkeletalProductKeys.end());
		std::erase_if(m_SkeletalProducts, [this, &RequiredSkeletalProductKeys](const auto& Entry)
		{
			if (std::binary_search(RequiredSkeletalProductKeys.begin(), RequiredSkeletalProductKeys.end(), Entry.first))
				return false;
			m_SkeletalProductBytes -= Entry.second.second;
			return true;
		});
		const auto LoadSkeletalProduct = [this](
			const AssetID& Asset,
			const std::string& ProductKey) -> std::expected<std::shared_ptr<const ImportedGltfSkeletalMesh>, std::string>
		{
			const std::string CacheKey = MakeSkeletalProductCacheKey(Asset, ProductKey);
			if (const auto Found = m_SkeletalProducts.find(CacheKey); Found != m_SkeletalProducts.end())
				return Found->second.first;

			std::expected<ImportedGltfSkeletalMesh, std::string> Loaded =
				std::unexpected("Skeletal renderer has no source project or cooked provider");
			if (m_ProjectAssetCaches->CookedContent)
			{
				auto Product = m_ProjectAssetCaches->CookedContent->LoadSkeletalMeshByProductKey(Asset, ProductKey);
				if (!Product) return std::unexpected("Could not load cooked skeletal mesh: " + Product.error().Message);
				Loaded = std::move(*Product);
			}
#if !defined(PF_COOKED_RUNTIME_ONLY)
			else if (m_Project)
			{
				auto Product = GltfMeshImportCoordinator::LoadLastSuccessfulSkeletalProduct(
					Asset, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), ProductKey);
				if (!Product) return std::unexpected("Could not load source skeletal mesh: " + Product.error().Message);
				Loaded = std::move(Product->Mesh);
			}
#endif
			if (!Loaded) return std::unexpected(Loaded.error());
			auto EstimatedBytes = EstimateSkeletalProductBytes(*Loaded);
			if (!EstimatedBytes || *EstimatedBytes > MaximumRendererSkeletalProductBytes - m_SkeletalProductBytes)
				return std::unexpected("Immutable skeletal product cache exceeds its 256 MiB renderer budget");
			try
			{
				auto Retained = std::make_shared<const ImportedGltfSkeletalMesh>(std::move(*Loaded));
				auto Inserted = m_SkeletalProducts.emplace(CacheKey, std::pair{ std::move(Retained), *EstimatedBytes });
				m_SkeletalProductBytes += Inserted.first->second.second;
				return Inserted.first->second.first;
			}
			catch (const std::bad_alloc&)
			{
				return std::unexpected("Could not retain the immutable skeletal product within renderer memory");
			}
		};
		size_t TotalSkeletalVertices = 0;
		for (const SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			if (!Instance.SkeletalProductKey) continue;
			auto Product = LoadSkeletalProduct(Instance.MeshAsset, *Instance.SkeletalProductKey);
			if (!Product)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
					Instance.MeshAsset, Product.error() });
			const size_t VertexCount = (*Product)->Geometry.Vertices.size();
			if (VertexCount == 0 || VertexCount > MaximumGltfSkinnedVertices ||
				VertexCount > MaximumRendererSkeletalVerticesPerSnapshot - TotalSkeletalVertices)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, Instance.Entity,
					Instance.MeshAsset, "Visible skeletal instances exceed the renderer's 2,000,000 vertex memory budget" });
			TotalSkeletalVertices += VertexCount;
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
		for (SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			if (Instance.MaterialAsset && !m_ProjectAssetCaches->Materials.IsRegistered(*Instance.MaterialAsset))
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
					*Instance.MaterialAsset, "Referenced material asset is not present in the project registry" });

			const Mesh* MeshResource = nullptr;
			if (Instance.SkeletalProductKey)
			{
				auto Existing = m_SkeletalMeshResources.find(Instance.Entity);
				if (Existing != m_SkeletalMeshResources.end() &&
					(Existing->second->SourceAsset != Instance.MeshAsset || Existing->second->ProductKey != *Instance.SkeletalProductKey))
				{
					m_SkeletalMeshResources.erase(Existing);
					Existing = m_SkeletalMeshResources.end();
				}
				if (Existing == m_SkeletalMeshResources.end())
				{
					auto Product = LoadSkeletalProduct(Instance.MeshAsset, *Instance.SkeletalProductKey);
					if (!Product)
						return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
							Instance.MeshAsset, Product.error() });

					auto Prepared = std::make_unique<PreparedSkeletalMeshResource>();
					Prepared->SourceAsset = Instance.MeshAsset;
					Prepared->ProductKey = *Instance.SkeletalProductKey;
					Prepared->Product = std::move(*Product);
					Existing = m_SkeletalMeshResources.emplace(Instance.Entity, std::move(Prepared)).first;
				}

				PreparedSkeletalMeshResource& Skeletal = *Existing->second;
				std::vector<std::array<float, 16>> LocalTransforms = Instance.SkeletalLocalTransforms;
				if (LocalTransforms.empty())
				{
					LocalTransforms.reserve(Skeletal.Product->SkeletonNodes.size());
					for (const ImportedGltfSkeletonNode& Node : Skeletal.Product->SkeletonNodes)
						LocalTransforms.push_back(Node.LocalBindMatrix);
				}
				if (LocalTransforms != Skeletal.LastLocalTransforms || !Skeletal.Resource)
				{
					auto Palette = BuildGltfJointPalette(*Skeletal.Product, LocalTransforms);
					if (!Palette)
						return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
							Instance.MeshAsset, "Could not build skeletal joint palette: " + Palette.error().Message });
					auto SkinnedVertices = SkinGltfMeshVertices(*Skeletal.Product, *Palette);
					if (!SkinnedVertices)
						return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
							Instance.MeshAsset, "Could not prepare skinned vertices: " + SkinnedVertices.error().Message });
					Skeletal.CurrentPoseBounds = ComputeSkeletalPoseBounds(*SkinnedVertices);

					MeshDesc Description = Skeletal.Product->Geometry.GetMeshDescription();
					Description.VertexData = std::as_bytes(std::span(*SkinnedVertices));
					if (Skeletal.Resource)
					{
						Skeletal.PendingVertexData.assign(Description.VertexData.begin(), Description.VertexData.end());
					}
					else
					{
						auto Created = m_Runtime.CreateDynamicMesh(Description);
					if (!Created)
						return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, Instance.Entity,
							Instance.MeshAsset, "Could not create skeletal mesh buffers: " + Created.error().Message });
						Skeletal.Resource = std::move(*Created);
					}
					Skeletal.LastLocalTransforms = std::move(LocalTransforms);
				}
				MeshResource = Skeletal.Resource.get();
			}
			else
			{
				const bool HasPreviewMesh = m_TransientPreviewMesh && m_TransientPreviewMeshIdentity == Instance.MeshAsset;
				auto MeshResult = HasPreviewMesh
					? std::expected<std::reference_wrapper<const Mesh>, MeshAssetError>(std::cref(*m_TransientPreviewMesh))
					: m_ProjectAssetCaches->Meshes.GetOrLoad(Instance.MeshAsset);
				if (!MeshResult)
				{
					if (MeshResult.error().Code == MeshAssetErrorCode::Pending)
					{
						m_AssetPreparationPending = true;
						continue;
					}
					return std::unexpected(SceneRendererError{
						SceneRendererErrorCode::AssetLoadFailed,
						Instance.Entity,
						Instance.MeshAsset,
						"Could not load mesh asset: " + MeshResult.error().Message
					});
				}
				MeshResource = &MeshResult->get();
			}
			if (Snapshot->HasCameraFrustum)
			{
				const auto Skeletal = m_SkeletalMeshResources.find(Instance.Entity);
				if (Skeletal != m_SkeletalMeshResources.end() && Skeletal->second->CurrentPoseBounds)
					Instance.CameraVisible = IsSceneBoundsVisible(
						*Skeletal->second->CurrentPoseBounds, Instance.WorldTransform, Snapshot->ViewProjection);
				else if (const auto& Bounds = MeshResource->GetLocalBounds(); Bounds)
					Instance.CameraVisible = IsSceneBoundsVisible(
						{ Bounds->Minimum, Bounds->Maximum }, Instance.WorldTransform, Snapshot->ViewProjection);
			}
			const VertexLayoutDesc& Layout = MeshResource->GetVertexLayout();
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
			m_PreparedMeshResources.insert_or_assign(Instance.Entity, MeshResource);

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
			const bool HasTransientMaterial = Instance.MaterialAsset && m_TransientMaterialOverride &&
				m_TransientMaterialOverride->Material == *Instance.MaterialAsset;
			const bool HasTransientGraph = Instance.MaterialAsset && m_TransientMaterialOverride &&
				m_TransientMaterialOverride->Material == *Instance.MaterialAsset && m_TransientMaterialOverride->GraphProduct;
			const MaterialAssetDesc* SourceDescription = nullptr;
			bool HasSourceGraph = false;
#if !defined(PF_COOKED_RUNTIME_ONLY)
			if (Instance.MaterialAsset && !HasTransientMaterial && !m_ProjectAssetCaches->CookedContent)
			{
				auto Description = ResolveMaterialDescription(Instance);
				if (!Description) return std::unexpected(std::move(Description.error()));
				SourceDescription = *Description;
				HasSourceGraph = SourceDescription->Graph.has_value();
			}
#endif
			const bool HasCookedGraph = Instance.MaterialAsset && !HasTransientMaterial && m_ProjectAssetCaches->CookedContent &&
				m_ProjectAssetCaches->CookedContent->FindGraphMaterial(*Instance.MaterialAsset);
			if (Instance.MaterialAsset && (HasTransientGraph || HasCookedGraph || HasSourceGraph))
			{
				GraphMaterialRuntime* GraphMaterial = nullptr;
				auto Existing = m_GraphMaterialResources.find(*Instance.MaterialAsset);
				std::optional<MaterialGraphCompileIdentity> SourceIdentity;
				if (HasSourceGraph)
				{
#if !defined(PF_COOKED_RUNTIME_ONLY)
					auto Identity = m_ProjectAssetCaches->GraphMaterialIdentities.find(*Instance.MaterialAsset);
					if (Identity == m_ProjectAssetCaches->GraphMaterialIdentities.end())
					{
						++m_ProjectAssetCaches->GraphMaterialIdentityDescribeCalls;
						auto Described = MaterialGraphCompiler::Describe(
							*SourceDescription->Graph, GetMaterialGraphCompilerToolchainFingerprint());
						Identity = m_ProjectAssetCaches->GraphMaterialIdentities.emplace(
							*Instance.MaterialAsset, std::move(Described)).first;
					}
					else ++m_ProjectAssetCaches->GraphMaterialIdentityCacheHits;
					if (!Identity->second)
					{
						if (Existing == m_GraphMaterialResources.end())
							return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
								*Instance.MaterialAsset, "Saved graph material is invalid for source rendering: " +
								Identity->second.error().Message });
						PF_CORE_WARN("Saved graph material {} is invalid; retaining its last good renderer product: {}",
							Instance.MaterialAsset->ToString(), Identity->second.error().Message);
					}
					else SourceIdentity.emplace(Identity->second.value());
#endif
				}
			if (Existing != m_GraphMaterialResources.end() && (!SourceIdentity ||
				Existing->second->RecipeFingerprint == SourceIdentity->RecipeFingerprint))
			{
				GraphMaterial = Existing->second.get();
				if (SourceIdentity)
				{
					auto Parameters = SourceIdentity->Parameters;
					std::ranges::sort(Parameters, {}, &MaterialGraphCompiledArtifact::Parameter::Name);
					const size_t RegisterCount = (std::max)(size_t{ 1 }, (Parameters.size() + 3) / 4);
					std::vector<std::byte> ParameterBytes(RegisterCount * 16, std::byte{ 0 });
					for (size_t Index = 0; Index < Parameters.size(); ++Index)
						std::memcpy(ParameterBytes.data() + Index * sizeof(float), &Parameters[Index].DefaultValue, sizeof(float));
					if (ParameterBytes != GraphMaterial->ParameterBytes)
						GraphMaterial->PendingParameterBytes = std::move(ParameterBytes);
				}
			}
			else
				{
					std::optional<CookedGraphMaterialProduct> LoadedProduct;
					const CookedGraphMaterialProduct* Product = nullptr;
					if (HasTransientGraph)
						Product = &*m_TransientMaterialOverride->GraphProduct;
					else if (m_ProjectAssetCaches->CookedContent)
					{
						auto Loaded = m_ProjectAssetCaches->CookedContent->LoadGraphMaterialProduct(*Instance.MaterialAsset);
						if (!Loaded)
							return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
								*Instance.MaterialAsset, "Could not load cooked graph material: " + Loaded.error().Message });
						LoadedProduct.emplace(std::move(*Loaded));
						Product = &*LoadedProduct;
					}
					else
					{
#if !defined(PF_COOKED_RUNTIME_ONLY)
						if (!SourceDescription || !SourceDescription->Graph)
							return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
								*Instance.MaterialAsset, "Saved graph material source is unavailable" });
						MaterialGraphCompileRequest CompileRequest;
						CompileRequest.Source = *SourceDescription->Graph;
						CompileRequest.ToolchainFingerprint = GetMaterialGraphCompilerToolchainFingerprint();
						CompileRequest.ArtifactStore = &m_ProjectAssetCaches->Artifacts;
						CompileRequest.Generation = m_ProjectAssetCaches->Generation;
						CompileRequest.OnDxcProcessLaunched = [this] { ++m_GraphMaterialDxcProcessLaunches; };
						const ProjectAssetCaches* const Caches = m_ProjectAssetCaches.get();
						CompileRequest.IsGenerationCurrent = [Caches](uint64_t Generation)
						{
							return Caches->Generation == Generation;
						};
						auto Compiled = MaterialGraphCompiler::Compile(CompileRequest);
						if (!Compiled)
						{
							if (Existing == m_GraphMaterialResources.end())
								return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
										*Instance.MaterialAsset, "Could not compile saved graph material for source rendering: " +
										Compiled.error().Message });
							GraphMaterial = Existing->second.get();
							PF_CORE_WARN("Could not compile saved graph material {}; retaining its last good renderer product: {}",
								Instance.MaterialAsset->ToString(), Compiled.error().Message);
						}
						else
						{
							CookedGraphMaterialProduct SourceProduct;
							SourceProduct.RecipeFingerprint = Compiled->RecipeFingerprint;
							SourceProduct.Parameters.reserve(Compiled->Parameters.size());
							for (const auto& Parameter : Compiled->Parameters)
								SourceProduct.Parameters.push_back({ Parameter.ID, Parameter.Name, Parameter.DefaultValue });
							SourceProduct.SpirV = std::move(Compiled->SpirV);
							SourceProduct.Material = MakeCookedMaterialProduct(*SourceDescription);
							LoadedProduct.emplace(std::move(SourceProduct));
							Product = &*LoadedProduct;
						}
#else
						return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity,
							*Instance.MaterialAsset, "Cooked-only scene renderer cannot compile graph source" });
#endif
					}

					if (Product)
					{
					auto ShaderResources = m_GraphMaterialShaderResources[Product->RecipeFingerprint].lock();
					if (!ShaderResources)
					{
						ShaderDesc ShaderDescription;
						ShaderDescription.Stage = ShaderStage::Fragment;
						ShaderDescription.EntryPoint = "PSMain";
						ShaderDescription.DebugName = "PulseForge graph material " + Product->RecipeFingerprint;
						auto Shader = m_Runtime.CreateShader(ShaderDescription, Product->SpirV);
						if (!Shader)
							return std::unexpected(MakeResourceError("Could not create cooked graph material shader: " + Shader.error().Message));
						++m_GraphMaterialShaderCreations;

						BindingLayoutDesc GraphLayoutDescription;
						GraphLayoutDescription.Visibility = ShaderVisibility::Fragment;
						GraphLayoutDescription.ShaderRegisterSpace = 3;
						GraphLayoutDescription.Items = {
							{ BindingResourceType::TextureCube, 0 }, { BindingResourceType::TextureCube, 1 },
							{ BindingResourceType::Sampler, 0 }, { BindingResourceType::ConstantBuffer, 0 }
						};
						GraphLayoutDescription.DebugName = "PulseForge graph material and point-shadow resources (space3)";
						auto GraphLayout = m_Runtime.CreateBindingLayout(GraphLayoutDescription);
						if (!GraphLayout)
							return std::unexpected(MakeResourceError("Could not create graph-material binding layout: " +
							GraphLayout.error().Message));
						++m_GraphMaterialBindingLayoutCreations;

						ShaderResources = std::make_shared<GraphMaterialShaderRuntime>();
						ShaderResources->RecipeFingerprint = Product->RecipeFingerprint;
						ShaderResources->FragmentShader = std::move(*Shader);
						ShaderResources->MaterialLayout = std::move(*GraphLayout);
						std::erase_if(m_GraphMaterialShaderResources,
							[](const auto& Entry) { return Entry.second.expired(); });
						m_GraphMaterialShaderResources.insert_or_assign(Product->RecipeFingerprint, ShaderResources);
					}

					auto Parameters = Product->Parameters;
					std::ranges::sort(Parameters, {}, &CookedGraphMaterialProduct::Parameter::Name);
					const size_t RegisterCount = (std::max)(size_t{ 1 }, (Parameters.size() + 3) / 4);
					std::vector<std::byte> ParameterBytes(RegisterCount * 16, std::byte{ 0 });
					for (size_t Index = 0; Index < Parameters.size(); ++Index)
						std::memcpy(ParameterBytes.data() + Index * sizeof(float), &Parameters[Index].DefaultValue, sizeof(float));
					BufferDesc ParameterBufferDescription;
					ParameterBufferDescription.ByteSize = ParameterBytes.size();
					ParameterBufferDescription.Usage = BufferUsage::Constant;
					ParameterBufferDescription.DebugName = "PulseForge cooked graph parameter values";
					auto ParameterBuffer = m_Runtime.CreateBuffer(ParameterBufferDescription, ParameterBytes);
					if (!ParameterBuffer)
						return std::unexpected(MakeResourceError("Could not create graph-material parameter data: " +
							ParameterBuffer.error().Message));
					++m_GraphMaterialParameterBufferCreations;
					auto RuntimeMaterial = std::make_unique<GraphMaterialRuntime>();
					RuntimeMaterial->RecipeFingerprint = Product->RecipeFingerprint;
					RuntimeMaterial->ShaderResources = std::move(ShaderResources);
					RuntimeMaterial->ParameterBuffer = std::move(*ParameterBuffer);
					RuntimeMaterial->ParameterBytes = std::move(ParameterBytes);
					GraphMaterial = RuntimeMaterial.get();
					if (Existing != m_GraphMaterialResources.end()) m_GraphMaterialResources.erase(Existing);
					m_GraphMaterialResources.emplace(*Instance.MaterialAsset, std::move(RuntimeMaterial));
					}
				}
				m_PreparedGraphMaterials.insert_or_assign(Instance.Entity, GraphMaterial);
			}
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

	std::expected<void, SceneRendererError> SceneRenderer::ApplyPendingSkeletalMeshUpdates()
	{
		for (auto& [Entity, Skeletal] : m_SkeletalMeshResources)
		{
			if (Skeletal->PendingVertexData.empty()) continue;
			if (!Skeletal->Resource || !Skeletal->Product)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, Entity,
					Skeletal->SourceAsset, "A pending skinned vertex update has no retained mesh resource" });

			const MeshDesc Description = Skeletal->Product->Geometry.GetMeshDescription();
			auto Updated = m_Runtime.UpdateDynamicMeshVertices(
				*Skeletal->Resource, Description.VertexLayout, Skeletal->PendingVertexData);
			if (!Updated)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, Entity,
					Skeletal->SourceAsset, "Could not update skinned mesh vertices: " + Updated.error().Message });
			Skeletal->PendingVertexData.clear();
		}
		return {};
	}

	void SceneRenderer::SetTransientPreviewMesh(const AssetID& Identity, const Mesh* Geometry) noexcept
	{
		m_TransientPreviewMeshIdentity = Geometry ? std::optional<AssetID>(Identity) : std::nullopt;
		m_TransientPreviewMesh = Geometry;
		m_PreparedSnapshot.reset();
		m_PreparedMeshResources.clear();
		m_PreparedMaterialBindings.clear();
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
		const auto ApplyPendingSkeletalUpdates = [this] { return ApplyPendingSkeletalMeshUpdates(); };
		const ShadowPass::Resources Resources{ m_Runtime, m_PreparedSnapshot ? &*m_PreparedSnapshot : nullptr,
			m_PreparedCascades ? &*m_PreparedCascades : nullptr,
			*m_LocalLightingState, *m_ShadowTargetCache,
			*m_PipelineCache, *m_MaterialBindingCache, m_PreparedMeshResources, m_PreparedMaterialBindings,
			m_ShadowBindingLayout, m_PointShadowBindingLayout,
			*m_ShadowSampler, m_ShadowObjectConstantsBuffer, m_ShadowObjectBindingSet, m_PointShadowObjectConstantsBuffer,
			m_PointShadowObjectBindingSet, ApplyPendingSkeletalUpdates };
		return m_ShadowPass->Render(Resources);
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareGraphMaterialBindings()
	{
		const uint64_t PointShadowGeneration = m_ShadowTargetCache->PointLightResourceGeneration();
		for (auto& [Material, Graph] : m_GraphMaterialResources)
		{
			(void)Material;
			if (Graph->MaterialBindings && Graph->PointShadowResourceGeneration == PointShadowGeneration) continue;
			auto Bindings = m_ShadowTargetCache->CreateGraphMaterialBindings(Graph->ShaderResources->MaterialLayout,
				*Graph->ParameterBuffer);
			if (!Bindings) return std::unexpected(std::move(Bindings.error()));
			++m_GraphMaterialBindingSetCreations;
			Graph->MaterialBindings = std::move(*Bindings);
			Graph->PointShadowResourceGeneration = PointShadowGeneration;
		}
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::ApplyPendingGraphMaterialParameters()
	{
		for (auto& [Material, Graph] : m_GraphMaterialResources)
		{
			if (Graph->PendingParameterBytes.empty()) continue;
			if (!Graph->ParameterBuffer || Graph->PendingParameterBytes.size() != Graph->ParameterBuffer->GetDescription().ByteSize)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, {}, Material,
					"Prepared graph material parameter storage no longer matches its shader ABI" });
			auto Updated = m_Runtime.WriteBuffer(*Graph->ParameterBuffer, 0, Graph->PendingParameterBytes);
			if (!Updated)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::DrawFailed, {}, Material,
					"Could not update graph material parameter data: " + Updated.error().Message });
			++m_GraphMaterialParameterUploads;
			Graph->ParameterBytes = std::move(Graph->PendingParameterBytes);
			Graph->PendingParameterBytes.clear();
		}
		return {};
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
		const auto ApplyPendingSkeletalUpdates = [this] { return ApplyPendingSkeletalMeshUpdates(); };

		const AmbientOcclusionPass::RenderInputs Inputs{ *m_PreparedSnapshot, m_PipelineCache->GetVertexLayout(), m_BindingLayout,
			m_PreparedMeshResources, *m_BackgroundTriangleBuffer, ResolveMaterial, ResolveBindings, ApplyPendingSkeletalUpdates };
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
		m_PreparedGraphMaterials.clear();
		m_GraphMaterialResources.clear();
		m_GraphMaterialShaderResources.clear();
#if !defined(PF_COOKED_RUNTIME_ONLY)
		m_ProjectAssetCaches->GraphMaterialIdentities.clear();
#endif
		m_MaterialBindingCache->RefreshMaterialAssets();
	}

	void SceneRenderer::RefreshMaterialAsset(const AssetID& Material) noexcept
	{
		m_PreparedSnapshot.reset();
		m_PreparedMaterialBindings.clear();
		m_PreparedGraphMaterials.clear();
#if !defined(PF_COOKED_RUNTIME_ONLY)
		m_ProjectAssetCaches->GraphMaterialIdentities.erase(Material);
#endif
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
			auto Replacement = std::make_unique<TransientMaterialOverride>();
			Replacement->Material = Material;
			Replacement->Description = Description;
			Replacement->Revision = Revision;
			m_TransientMaterialOverride = std::move(Replacement);
			m_PreparedSnapshot.reset();
			m_PreparedMaterialBindings.clear();
			m_PreparedGraphMaterials.clear();
			m_GraphMaterialResources.erase(Material);
			m_MaterialBindingCache->RefreshMaterialAssets();
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, {}, Material,
				std::string("Could not retain the transient material draft: ") + Exception.what() });
		}
	}

	std::expected<void, SceneRendererError> SceneRenderer::SetTransientGraphMaterialOverride(
		const AssetID& Material, const MaterialAssetDesc& Description,
		const CookedGraphMaterialProduct& Product, uint64_t Revision)
	{
		if (Material.IsNil() || Revision == 0 || Product.ArtifactVersion != Detail::GraphMaterialProductSchemaVersion ||
			Product.RecipeFingerprint.size() != 32 || Product.SpirV.empty())
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, {}, Material,
				"A transient graph preview requires a valid material, artifact and nonzero draft revision" });
		try
		{
			auto Replacement = std::make_unique<TransientMaterialOverride>();
			Replacement->Material = Material;
			Replacement->Description = Description;
			Replacement->GraphProduct = Product;
			Replacement->Revision = Revision;
			const auto Existing = m_GraphMaterialResources.find(Material);
			if (Existing != m_GraphMaterialResources.end())
			{
				const std::vector<std::byte> ParameterBytes = BuildGraphParameterBytes(Product.Parameters);
				if (Existing->second->RecipeFingerprint == Product.RecipeFingerprint && Existing->second->ParameterBuffer &&
					Existing->second->ParameterBuffer->GetDescription().ByteSize == ParameterBytes.size())
					Existing->second->PendingParameterBytes = ParameterBytes;
				else
					m_GraphMaterialResources.erase(Existing);
			}
			m_TransientMaterialOverride = std::move(Replacement);
			m_PreparedSnapshot.reset();
			m_PreparedMaterialBindings.clear();
			m_PreparedGraphMaterials.clear();
			m_MaterialBindingCache->RefreshMaterialAsset(Material);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, {}, Material,
				std::string("Could not retain the transient graph material preview: ") + Exception.what() });
		}
	}

	void SceneRenderer::ClearTransientMaterialOverride() noexcept
	{
		if (!m_TransientMaterialOverride) return;
		const AssetID Material = m_TransientMaterialOverride->Material;
		m_TransientMaterialOverride.reset();
		m_PreparedSnapshot.reset();
		m_PreparedMaterialBindings.clear();
		m_PreparedGraphMaterials.clear();
		m_GraphMaterialResources.erase(Material);
		m_MaterialBindingCache->RefreshMaterialAssets();
	}

	void SceneRenderer::ClearTransientGraphMaterialOverride() noexcept
	{
		if (!m_TransientMaterialOverride || !m_TransientMaterialOverride->GraphProduct) return;
		const AssetID Material = m_TransientMaterialOverride->Material;
		m_TransientMaterialOverride.reset();
		m_PreparedSnapshot.reset();
		m_PreparedMaterialBindings.clear();
		m_PreparedGraphMaterials.clear();
		m_GraphMaterialResources.erase(Material);
		m_MaterialBindingCache->RefreshMaterialAssets();
	}

	void SceneRenderer::RefreshProjectAssets() noexcept
	{
		m_TransparentDrawOrder.clear();
		m_PreparedSnapshot.reset();
		m_PreparedMeshResources.clear();
		m_SkeletalMeshResources.clear();
		m_SkeletalProducts.clear();
		m_SkeletalProductBytes = 0;
		m_PreparedRuntimeUITextures.clear();
		m_PreparedMaterialBindings.clear();
		m_PreparedGraphMaterials.clear();
		m_GraphMaterialResources.clear();
		m_GraphMaterialShaderResources.clear();
#if !defined(PF_COOKED_RUNTIME_ONLY)
		m_ProjectAssetCaches->GraphMaterialIdentities.clear();
#endif
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
		if (auto Updated = ApplyPendingSkeletalMeshUpdates(); !Updated) return std::unexpected(std::move(Updated.error()));
		if (auto Updated = ApplyPendingGraphMaterialParameters(); !Updated) return std::unexpected(std::move(Updated.error()));
		if (auto Shadows = RenderShadowCascades(); !Shadows)
			return std::unexpected(std::move(Shadows.error()));
		const auto [Width, Height] = m_Runtime.GetOutputFramebufferSize();
		if (auto AmbientOcclusion = RenderAmbientOcclusion(Width, Height); !AmbientOcclusion)
			return std::unexpected(std::move(AmbientOcclusion.error()));
		if (auto GraphBindings = PrepareGraphMaterialBindings(); !GraphBindings)
			return std::unexpected(std::move(GraphBindings.error()));
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
		if (auto Updated = ApplyPendingGraphMaterialParameters(); !Updated) return std::unexpected(std::move(Updated.error()));
		if (auto Shadows = RenderShadowCascades(); !Shadows)
			return std::unexpected(std::move(Shadows.error()));
		if (auto AmbientOcclusion = RenderAmbientOcclusion(
			Target.GetDescription().Width, Target.GetDescription().Height); !AmbientOcclusion)
			return std::unexpected(std::move(AmbientOcclusion.error()));
		if (auto GraphBindings = PrepareGraphMaterialBindings(); !GraphBindings)
			return std::unexpected(std::move(GraphBindings.error()));

		const GraphicsResult BeginResult = m_Runtime.BeginRenderTarget(Target, ClearValue);
		if (!BeginResult)
			return std::unexpected(MakeDrawError("Could not begin scene render target: " + BeginResult.error().Message));

		RenderTargetFrameScope TargetScope(m_Runtime);
		if (auto Updated = ApplyPendingSkeletalMeshUpdates(); !Updated)
			return std::unexpected(std::move(Updated.error()));
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
			const auto GraphMaterial = m_PreparedGraphMaterials.find(Instance.Entity);
			const GraphMaterialRuntime* Graph = GraphMaterial != m_PreparedGraphMaterials.end() ? GraphMaterial->second : nullptr;
			if (Graph)
			{
				if (auto PipelineResult = m_PipelineCache->EnsureGraphScenePipeline(ColorFormat, Graph->RecipeFingerprint,
						Graph->ShaderResources->FragmentShader, Graph->ShaderResources->MaterialLayout, AlphaBlend,
						Material.DoubleSided, Instance.Orientation);
					!PipelineResult)
					return std::unexpected(std::move(PipelineResult.error()));
			}
			else if (auto PipelineResult =
					m_PipelineCache->EnsureScenePipeline(ColorFormat, AlphaBlend, Material.DoubleSided, Instance.Orientation);
				!PipelineResult)
				return std::unexpected(std::move(PipelineResult.error()));
			const GraphicsPipeline* Pipeline = Graph
				? m_PipelineCache->FindGraphScenePipeline(ColorFormat, Graph->RecipeFingerprint,
					AlphaBlend, Material.DoubleSided, Instance.Orientation)
				: m_PipelineCache->FindScenePipeline(ColorFormat, AlphaBlend, Material.DoubleSided, Instance.Orientation);
			if (!Pipeline) return std::unexpected(MakeResourceError("Prepared material graphics pipeline is unavailable"));
			auto ResolvedMaterialBinding = ResolveMaterialBinding(Instance);
			if (!ResolvedMaterialBinding) return std::unexpected(std::move(ResolvedMaterialBinding.error()));
			const BindingSet* const MaterialBinding = *ResolvedMaterialBinding;
			const Mesh* MeshResource = FindPreparedMesh(m_PreparedMeshResources, Instance.Entity);
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
			const std::array<const BindingSet*, 4> GraphBindingSets = { MaterialBinding, &m_ShadowTargetCache->ShadowBindings(),
				AmbientOcclusionBindings, Graph ? Graph->MaterialBindings.get() : nullptr };
			const std::array<const BindingSet*, 4> FixedBindingSets = { MaterialBinding, &m_ShadowTargetCache->ShadowBindings(),
				AmbientOcclusionBindings, &m_ShadowTargetCache->PointLightBindings() };
			const GraphicsResult Draw = Graph
				? m_Runtime.DrawIndexed(*Pipeline, *MeshResource, Arguments, GraphBindingSets)
				: m_Runtime.DrawIndexed(*Pipeline, *MeshResource, Arguments, FixedBindingSets);
			if (!Draw)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::DrawFailed,
					Instance.Entity,
					Instance.MeshAsset,
					"Could not draw scene mesh: " + Draw.error().Message
				});
			}
			if (Graph && !m_LoggedCookedGraphMaterialDraw)
			{
				const char* Acceptance = std::getenv("PULSEFORGE_COOKED_PLAYER_ACCEPTANCE");
				if (Acceptance && std::string_view(Acceptance) == "1")
				{
					m_LoggedCookedGraphMaterialDraw = true;
					PF_INFO("PULSEFORGE_COOKED_GRAPH_MATERIAL=PASS; graph material pipeline submitted a successful indexed draw");
				}
			}
			++SubmittedDraws;
			return {};
		};

		m_TransparentDrawOrder.clear();
		m_TransparentDrawOrder.reserve(std::max(m_TransparentDrawOrder.capacity(), m_PreparedSnapshot->Meshes.size()));
		for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
		{
			if (!Instance.CameraVisible) continue;
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
