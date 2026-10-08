#include "Core/PulseForgePCH.h"
#include "Renderer/SceneRenderer.h"

#include "Assets/MaterialAssetCache.h"
#include "Assets/MeshAssetCache.h"
#include "Assets/Project.h"
#include "Assets/TextureAssetCache.h"
#include "Core/Application.h"
#include "Core/Log.h"
#include "Renderer/Mesh.h"
#include "Scene/Scene.h"
#include "Scene/Components/EnvironmentLightComponent.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <span>
#include <exception>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace PulseForge
{
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

		bool HasSameVertexLayout(const VertexLayoutDesc& Left, const VertexLayoutDesc& Right)
		{
			if (Left.Stride != Right.Stride || Left.Attributes.size() != Right.Attributes.size())
				return false;

			for (size_t Index = 0; Index < Left.Attributes.size(); ++Index)
			{
				const VertexAttributeDesc& LeftAttribute = Left.Attributes[Index];
				const VertexAttributeDesc& RightAttribute = Right.Attributes[Index];
				if (LeftAttribute.Semantic != RightAttribute.Semantic ||
					LeftAttribute.Format != RightAttribute.Format ||
					LeftAttribute.Offset != RightAttribute.Offset)
					return false;
			}
			return true;
		}

		bool IsFinite(const glm::mat4& Matrix)
		{
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row]))
						return false;
			return true;
		}

		SceneRendererError MakeResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}

		SceneRendererError MakeDrawError(std::string Message)
		{
			return { SceneRendererErrorCode::DrawFailed, {}, {}, std::move(Message) };
		}

		std::string MakeMaterialBindingKey(const AssetID& Material, const std::optional<AssetID>& Environment)
		{
			return Material.ToString() + ":" + (Environment ? Environment->ToString() : std::string("none"));
		}

		std::expected<TextureHandle, TextureError> CreateBlackCube(Application& Runtime, std::string DebugName)
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

		class RenderTargetFrameScope final
		{
		public:
			explicit RenderTargetFrameScope(Application& Runtime)
				: m_Runtime(Runtime)
			{
			}

			RenderTargetFrameScope(const RenderTargetFrameScope&) = delete;
			RenderTargetFrameScope& operator=(const RenderTargetFrameScope&) = delete;

			~RenderTargetFrameScope() noexcept
			{
				if (!m_Active)
					return;

				try
				{
					const GraphicsResult EndResult = m_Runtime.EndRenderTarget();
					if (!EndResult)
						PF_CORE_ERROR("Could not close render target during scene-renderer unwinding: {0}", EndResult.error().Message);
				}
				catch (const std::exception& Exception)
				{
					PF_CORE_ERROR("Exception while closing render target during scene-renderer unwinding: {0}", Exception.what());
				}
				catch (...)
				{
					PF_CORE_ERROR("Unknown exception while closing render target during scene-renderer unwinding");
				}
			}

			GraphicsResult End()
			{
				m_Active = false;
				return m_Runtime.EndRenderTarget();
			}

		private:
			Application& m_Runtime;
			bool m_Active = true;
		};
	}

	SceneRenderer::SceneRenderer(Application& Runtime, const Project& SourceProject)
		: m_Runtime(Runtime),
		  m_Project(SourceProject),
		  m_MeshAssetCache(std::make_unique<MeshAssetCache>(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry())),
		  m_TextureAssetCache(std::make_unique<TextureAssetCache>(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry())),
		  m_MaterialAssetCache(std::make_unique<MaterialAssetCache>(SourceProject.GetRootPath(), SourceProject.GetAssetRegistry())),
		  m_EnvironmentLightingCache(std::make_unique<EnvironmentLightingCache>(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry()))
	{
	}

	SceneRenderer::~SceneRenderer() = default;

	std::expected<std::unique_ptr<SceneRenderer>, SceneRendererError> SceneRenderer::Create(
		Application& Runtime,
		const Project& SourceProject,
		const std::filesystem::path& CompiledShaderDirectory)
	{
		auto Renderer = std::unique_ptr<SceneRenderer>(new SceneRenderer(Runtime, SourceProject));
		if (auto Result = Renderer->Initialize(CompiledShaderDirectory); !Result)
			return std::unexpected(std::move(Result.error()));
		return Renderer;
	}

	std::expected<void, SceneRendererError> SceneRenderer::Initialize(
		const std::filesystem::path& CompiledShaderDirectory)
	{
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
		m_VertexShader = std::move(VertexShader.value());

		ShaderDesc FragmentShaderDescription;
		FragmentShaderDescription.Stage = ShaderStage::Fragment;
		FragmentShaderDescription.EntryPoint = "PSMain";
		FragmentShaderDescription.DebugName = "PulseForge scene fragment shader";
		auto FragmentShader = m_Runtime.CreateShader(FragmentShaderDescription, *FragmentBytecode);
		if (!FragmentShader)
			return std::unexpected(MakeResourceError("Could not create the scene fragment shader: " + FragmentShader.error().Message));
		m_FragmentShader = std::move(FragmentShader.value());

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
			return std::unexpected(MakeResourceError("Could not create environment background vertex shader: " + BackgroundVertex.error().Message));
		m_BackgroundVertexShader = std::move(*BackgroundVertex);
		ShaderDesc BackgroundFragmentDescription;
		BackgroundFragmentDescription.Stage = ShaderStage::Fragment;
		BackgroundFragmentDescription.EntryPoint = "PSMain";
		BackgroundFragmentDescription.DebugName = "PulseForge environment background fragment shader";
		auto BackgroundFragment = m_Runtime.CreateShader(BackgroundFragmentDescription, *BackgroundFragmentBytecode);
		if (!BackgroundFragment)
			return std::unexpected(MakeResourceError("Could not create environment background fragment shader: " + BackgroundFragment.error().Message));
		m_BackgroundFragmentShader = std::move(*BackgroundFragment);

		SamplerDesc SamplerDescription;
		SamplerDescription.Minification = SamplerFilter::Nearest;
		SamplerDescription.Magnification = SamplerFilter::Nearest;
		SamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
		SamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
		SamplerDescription.DebugName = "PulseForge scene texture sampler";
		auto Sampler = m_Runtime.CreateSampler(SamplerDescription);
		if (!Sampler)
			return std::unexpected(MakeResourceError("Could not create the scene sampler: " + Sampler.error().Message));
		m_Sampler = std::move(Sampler.value());

		SamplerDesc EnvironmentSamplerDescription;
		EnvironmentSamplerDescription.Minification = SamplerFilter::Linear;
		EnvironmentSamplerDescription.Magnification = SamplerFilter::Linear;
		EnvironmentSamplerDescription.Mip = SamplerMipFilter::Linear;
		EnvironmentSamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
		EnvironmentSamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
		EnvironmentSamplerDescription.DebugName = "PulseForge environment linear mip sampler";
		auto EnvironmentSampler = m_Runtime.CreateSampler(EnvironmentSamplerDescription);
		if (!EnvironmentSampler)
			return std::unexpected(MakeResourceError("Could not create the environment sampler: " + EnvironmentSampler.error().Message));
		m_EnvironmentSampler = std::move(EnvironmentSampler.value());

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

		const std::array<uint8_t, 4> WhitePixel{ 255, 255, 255, 255 };
		TextureDesc WhiteDescription;
		WhiteDescription.Width = 1;
		WhiteDescription.Height = 1;
		WhiteDescription.Format = TextureFormat::RGBA8_Srgb;
		WhiteDescription.DebugName = "PulseForge fallback white base-color texture";
		auto WhiteTexture = m_Runtime.CreateTexture(WhiteDescription, std::as_bytes(std::span<const uint8_t>(WhitePixel)));
		if (!WhiteTexture)
			return std::unexpected(MakeResourceError("Could not create fallback base-color texture: " + WhiteTexture.error().Message));
		m_FallbackBaseColorTexture = std::move(*WhiteTexture);
		const MaterialConstants FallbackMaterial{ { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.0f } };
		BufferDesc FallbackMaterialDescription;
		FallbackMaterialDescription.ByteSize = sizeof(FallbackMaterial);
		FallbackMaterialDescription.Usage = BufferUsage::Constant;
		FallbackMaterialDescription.DebugName = "PulseForge fallback material constants";
		auto FallbackMaterialBuffer = m_Runtime.CreateBuffer(
			FallbackMaterialDescription, std::as_bytes(std::span(&FallbackMaterial, 1)));
		if (!FallbackMaterialBuffer)
			return std::unexpected(MakeResourceError("Could not create fallback material constants: " + FallbackMaterialBuffer.error().Message));
		m_FallbackMaterialConstantsBuffer = std::move(*FallbackMaterialBuffer);
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

		BindingLayoutDesc BindingLayoutDescription;
		BindingLayoutDescription.Visibility = ShaderVisibility::AllGraphics;
		BindingLayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 },
			{ BindingResourceType::TextureCube, 1 },
			{ BindingResourceType::TextureCube, 2 },
			{ BindingResourceType::TextureCube, 4 },
			{ BindingResourceType::Texture2D, 3 },
			{ BindingResourceType::Sampler, 0 },
			{ BindingResourceType::Sampler, 1 },
			{ BindingResourceType::ConstantBuffer, 0 },
			{ BindingResourceType::ConstantBuffer, 1 },
			{ BindingResourceType::ConstantBuffer, 2 }
		};
		BindingLayoutDescription.DebugName = "PulseForge scene resources";
		auto BindingLayout = m_Runtime.CreateBindingLayout(BindingLayoutDescription);
		if (!BindingLayout)
			return std::unexpected(MakeResourceError("Could not create the scene binding layout: " + BindingLayout.error().Message));
		m_BindingLayout = std::move(BindingLayout.value());

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
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureMaterialBindings(
		const AssetID& MaterialAsset,
		const std::optional<AssetID>& EnvironmentAsset,
		const EnvironmentLightingTextures& EnvironmentTextures)
	{
		const std::string Key = MakeMaterialBindingKey(MaterialAsset, EnvironmentAsset);
		if (m_MaterialBindings.contains(Key))
			return {};

		auto Material = m_MaterialAssetCache->GetOrLoad(MaterialAsset);
		if (!Material)
		{
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::AssetLoadFailed,
				{},
				MaterialAsset,
				"Could not load scene material " + MaterialAsset.ToString() + ": " + Material.error().Message
			});
		}

		auto Texture = m_TextureAssetCache->GetOrLoad(Material->get().BaseColorTexture, TextureFormat::RGBA8_Srgb);
		if (!Texture)
		{
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::AssetLoadFailed,
				{},
				Material->get().BaseColorTexture,
				"Could not load base-color texture for material " + MaterialAsset.ToString() + ": " + Texture.error().Message
			});
		}

		const MaterialConstants Constants{
			{ Material->get().BaseColorFactor.r, Material->get().BaseColorFactor.g,
				Material->get().BaseColorFactor.b, Material->get().BaseColorFactor.a },
			{ Material->get().MetallicFactor, Material->get().RoughnessFactor, 0.0f, 0.0f }
		};
		BufferDesc MaterialBufferDescription;
		MaterialBufferDescription.ByteSize = sizeof(Constants);
		MaterialBufferDescription.Usage = BufferUsage::Constant;
		MaterialBufferDescription.DebugName = "PulseForge PBR material constants";
		auto MaterialBuffer = m_Runtime.CreateBuffer(MaterialBufferDescription, std::as_bytes(std::span(&Constants, 1)));
		if (!MaterialBuffer)
			return std::unexpected(MakeResourceError("Could not create material constants: " + MaterialBuffer.error().Message));

		BindingSetDesc BindingSetDescription;
		BindingSetDescription.Layout = m_BindingLayout;
		BindingSetDescription.Textures.push_back({ 0, std::cref(Texture->get()) });
		BindingSetDescription.Textures.push_back({ 1, std::cref(*EnvironmentTextures.DiffuseIrradiance), BindingResourceType::TextureCube });
		BindingSetDescription.Textures.push_back({ 2, std::cref(*EnvironmentTextures.PrefilteredSpecular), BindingResourceType::TextureCube });
		BindingSetDescription.Textures.push_back({ 3, std::cref(*EnvironmentTextures.BrdfIntegrationLut) });
		BindingSetDescription.Textures.push_back({ 4, std::cref(*EnvironmentTextures.Environment), BindingResourceType::TextureCube });
		BindingSetDescription.Samplers.push_back({ 0, std::cref(*m_Sampler) });
		BindingSetDescription.Samplers.push_back({ 1, std::cref(*m_EnvironmentSampler) });
		BindingSetDescription.Buffers.push_back({ 0, std::cref(*MaterialBuffer.value()) });
		BindingSetDescription.Buffers.push_back({ 1, std::cref(*m_ObjectConstantsBuffer) });
		BindingSetDescription.Buffers.push_back({ 2, std::cref(*m_FrameConstantsBuffer) });
		auto BindingSet = m_Runtime.CreateBindingSet(BindingSetDescription);
		if (!BindingSet)
			return std::unexpected(MakeResourceError("Could not create material bindings: " + BindingSet.error().Message));

		MaterialBindingResources Resources;
		Resources.MaterialConstantsBuffer = std::move(MaterialBuffer.value());
		Resources.BindingSet = std::move(BindingSet.value());
		m_MaterialBindings.emplace(Key, std::move(Resources));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureEnvironmentBindings(
		const AssetID& EnvironmentAsset,
		const EnvironmentLightingTextures& EnvironmentTextures)
	{
		const std::string Key = EnvironmentAsset.ToString();
		if (m_EnvironmentBindingSets.contains(Key))
			return {};
		BindingSetDesc Description;
		Description.Layout = m_BindingLayout;
		Description.Textures = {
			{ 0, std::cref(*m_FallbackBaseColorTexture) },
			{ 1, std::cref(*EnvironmentTextures.DiffuseIrradiance), BindingResourceType::TextureCube },
			{ 2, std::cref(*EnvironmentTextures.PrefilteredSpecular), BindingResourceType::TextureCube },
			{ 3, std::cref(*EnvironmentTextures.BrdfIntegrationLut) },
			{ 4, std::cref(*EnvironmentTextures.Environment), BindingResourceType::TextureCube }
		};
		Description.Samplers = {
			{ 0, std::cref(*m_Sampler) },
			{ 1, std::cref(*m_EnvironmentSampler) }
		};
		Description.Buffers = {
			{ 0, std::cref(*m_FallbackMaterialConstantsBuffer) },
			{ 1, std::cref(*m_ObjectConstantsBuffer) },
			{ 2, std::cref(*m_FrameConstantsBuffer) }
		};
		auto BindingSet = m_Runtime.CreateBindingSet(Description);
		if (!BindingSet)
			return std::unexpected(MakeResourceError("Could not create environment binding set: " + BindingSet.error().Message));
		m_EnvironmentBindingSets.emplace(Key, std::move(*BindingSet));
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

	std::expected<void, SceneRendererError> SceneRenderer::PrepareSnapshot(
		std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> Snapshot)
	{
		m_PreparedSnapshot.reset();
		if (!Snapshot)
		{
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::SnapshotBuildFailed,
				Snapshot.error().Entity,
				{},
				Snapshot.error().Message
			});
		}
		const std::optional<AssetID> EnvironmentAsset = Snapshot->EnvironmentLight
			? std::optional<AssetID>{ Snapshot->EnvironmentLight->HdrImage }
			: std::nullopt;
		const EnvironmentLightingTextures* EnvironmentTextures = &m_FallbackEnvironmentTextures;
		if (EnvironmentAsset)
		{
			auto LoadedEnvironment = m_EnvironmentLightingCache->GetOrLoad(*EnvironmentAsset);
			if (!LoadedEnvironment)
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Snapshot->EnvironmentLight->Entity,
					*EnvironmentAsset,
					"Could not prepare HDR environment: " + LoadedEnvironment.error().Message
				});
			EnvironmentTextures = &LoadedEnvironment->get();
			if (auto Bindings = EnsureEnvironmentBindings(*EnvironmentAsset, *EnvironmentTextures); !Bindings)
				return std::unexpected(std::move(Bindings.error()));
		}

		std::optional<VertexLayoutDesc> SceneVertexLayout;
		for (const SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			auto Mesh = m_MeshAssetCache->GetOrLoad(Instance.MeshAsset);
			if (!Mesh)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Instance.Entity,
					Instance.MeshAsset,
					"Could not load mesh asset: " + Mesh.error().Message
				});
			}

			const VertexLayoutDesc& Layout = Mesh->get().GetVertexLayout();
			if (SceneVertexLayout && !HasSameVertexLayout(*SceneVertexLayout, Layout))
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::UnsupportedVertexLayout,
					Instance.Entity,
					Instance.MeshAsset,
					"This scene renderer instance requires all visible meshes to share one vertex layout"
				});
			}
			SceneVertexLayout = Layout;

			if (!Instance.MaterialAsset)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::MissingMaterial,
					Instance.Entity,
					Instance.MeshAsset,
					"Visible mesh entity has no material asset assigned"
				});
			}
			if (auto Bindings = EnsureMaterialBindings(*Instance.MaterialAsset, EnvironmentAsset, *EnvironmentTextures); !Bindings)
			{
				Bindings.error().Entity = Instance.Entity;
				return std::unexpected(std::move(Bindings.error()));
			}
		}

		if (SceneVertexLayout)
		{
			if (m_PipelineVertexLayout && !HasSameVertexLayout(*m_PipelineVertexLayout, *SceneVertexLayout))
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::UnsupportedVertexLayout,
					{},
					{},
					"Scene mesh vertex layout differs from the layout used to create the renderer pipeline"
				});
			}
			if (!m_PipelineVertexLayout)
				m_PipelineVertexLayout = *SceneVertexLayout;
			if (auto Pipeline = EnsurePipeline(ColorTargetFormat::Swapchain); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}

		m_PreparedSnapshot = std::move(*Snapshot);
		m_PreparedEnvironmentTextures = EnvironmentTextures;
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsurePipeline(ColorTargetFormat ColorFormat)
	{
		if (m_Pipelines.contains(ColorFormat))
			return {};
		if (!m_PipelineVertexLayout)
			return std::unexpected(MakeResourceError("Cannot create a scene pipeline before a mesh vertex layout is prepared"));

		GraphicsPipelineDesc PipelineDescription;
		PipelineDescription.VertexShader = m_VertexShader;
		PipelineDescription.FragmentShader = m_FragmentShader;
		PipelineDescription.BindingLayouts = { m_BindingLayout };
		PipelineDescription.VertexLayout = *m_PipelineVertexLayout;
		PipelineDescription.ColorFormat = ColorFormat;
		PipelineDescription.Rasterizer.Cull = CullMode::None;
		PipelineDescription.Depth.TestEnabled = true;
		PipelineDescription.Depth.WriteEnabled = true;
		PipelineDescription.Depth.Compare = DepthCompareOperation::Less;
		PipelineDescription.DebugName = "PulseForge scene pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(PipelineDescription);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create scene graphics pipeline: " + Pipeline.error().Message));

		m_Pipelines.emplace(ColorFormat, std::move(Pipeline.value()));
		PF_CORE_INFO("Created scene pipeline for vertex stride {0} and color format {1}",
			m_PipelineVertexLayout->Stride,
			static_cast<uint32_t>(ColorFormat));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureBackgroundPipeline(ColorTargetFormat ColorFormat)
	{
		if (m_BackgroundPipelines.contains(ColorFormat))
			return {};
		GraphicsPipelineDesc Description;
		Description.VertexShader = m_BackgroundVertexShader;
		Description.FragmentShader = m_BackgroundFragmentShader;
		Description.BindingLayouts = { m_BindingLayout };
		Description.VertexLayout.Stride = sizeof(float) * 2;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
		Description.ColorFormat = ColorFormat;
		Description.Depth.TestEnabled = false;
		Description.Depth.WriteEnabled = false;
		Description.DebugName = "PulseForge environment background pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create environment background pipeline: " + Pipeline.error().Message));
		m_BackgroundPipelines.emplace(ColorFormat, std::move(*Pipeline));
		return {};
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedScene()
	{
		return RenderPreparedSceneForFormat(ColorTargetFormat::Swapchain);
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedScene(
		const RenderTarget& Target,
		const RenderTargetClearValue& ClearValue)
	{
		const ColorTargetFormat ColorFormat = Target.GetDescription().ColorFormat;
		if (m_PreparedSnapshot && m_PreparedSnapshot->EnvironmentLight)
		{
			if (auto Pipeline = EnsureBackgroundPipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}
		if (m_PreparedSnapshot && !m_PreparedSnapshot->Meshes.empty())
		{
			if (auto Pipeline = EnsurePipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}

		const GraphicsResult BeginResult = m_Runtime.BeginRenderTarget(Target, ClearValue);
		if (!BeginResult)
			return std::unexpected(MakeDrawError("Could not begin scene render target: " + BeginResult.error().Message));

		RenderTargetFrameScope TargetScope(m_Runtime);
		auto Rendered = RenderPreparedSceneForFormat(ColorFormat);
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

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedSceneForFormat(ColorTargetFormat ColorFormat)
	{
		if (!m_PreparedSnapshot || (m_PreparedSnapshot->Meshes.empty() && !m_PreparedSnapshot->EnvironmentLight))
			return size_t{ 0 };
		if (m_PreparedSnapshot->EnvironmentLight)
			if (auto Pipeline = EnsureBackgroundPipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		if (!m_PreparedSnapshot->Meshes.empty())
			if (auto Pipeline = EnsurePipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		const GraphicsPipeline* ScenePipeline = m_PreparedSnapshot->Meshes.empty() ? nullptr : m_Pipelines.at(ColorFormat).get();

		size_t SubmittedDraws = 0;
		FrameConstants Frame{};
		Frame.CameraWorldPosition[0] = m_PreparedSnapshot->CameraWorldPosition.x;
		Frame.CameraWorldPosition[1] = m_PreparedSnapshot->CameraWorldPosition.y;
		Frame.CameraWorldPosition[2] = m_PreparedSnapshot->CameraWorldPosition.z;
		Frame.InverseViewProjection = glm::inverse(m_PreparedSnapshot->ViewProjection);
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
		}
		const auto FrameUpdate = m_Runtime.WriteBuffer(*m_FrameConstantsBuffer, 0, std::as_bytes(std::span(&Frame, 1)));
		if (!FrameUpdate)
			return std::unexpected(MakeDrawError("Could not update scene frame constants: " + FrameUpdate.error().Message));

		if (m_PreparedSnapshot->EnvironmentLight)
		{
			const std::string EnvironmentKey = m_PreparedSnapshot->EnvironmentLight->HdrImage.ToString();
			const auto BackgroundBindings = m_EnvironmentBindingSets.find(EnvironmentKey);
			if (BackgroundBindings == m_EnvironmentBindingSets.end())
				return std::unexpected(MakeDrawError("Prepared environment background bindings are unavailable"));
			const std::array<const BindingSet*, 1> BindingSets = { BackgroundBindings->second.get() };
			const DrawArguments BackgroundArguments{ 3, 1, 0, 0 };
			const GraphicsResult BackgroundDraw = m_Runtime.Draw(
				*m_BackgroundPipelines.at(ColorFormat), *m_BackgroundTriangleBuffer, BackgroundArguments, BindingSets);
			if (!BackgroundDraw)
				return std::unexpected(MakeDrawError("Could not draw environment background: " + BackgroundDraw.error().Message));
		}
		if (!ScenePipeline)
			return size_t{ 0 };

		for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
		{
			auto Mesh = m_MeshAssetCache->GetOrLoad(Instance.MeshAsset);
			if (!Mesh || !Instance.MaterialAsset)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Instance.Entity,
					Instance.MeshAsset,
					Mesh ? "Prepared mesh instance lost its material assignment" : "Prepared mesh resource is unavailable"
				});
			}
			const std::optional<AssetID> EnvironmentAsset = m_PreparedSnapshot->EnvironmentLight
				? std::optional<AssetID>{ m_PreparedSnapshot->EnvironmentLight->HdrImage }
				: std::nullopt;
			const auto Binding = m_MaterialBindings.find(MakeMaterialBindingKey(*Instance.MaterialAsset, EnvironmentAsset));
			if (Binding == m_MaterialBindings.end())
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Instance.Entity,
					*Instance.MaterialAsset,
					"Prepared material binding set is unavailable"
				});
			}

			const auto NormalTransform = BuildNormalTransform(Instance.WorldTransform);
			if (!NormalTransform)
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::DrawFailed,
					Instance.Entity,
					Instance.MeshAsset,
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

			const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
			const std::array<const BindingSet*, 1> BindingSets = { Binding->second.BindingSet.get() };
			const GraphicsResult Draw = m_Runtime.DrawIndexed(*ScenePipeline, Mesh->get(), Arguments, BindingSets);
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
		}
		return SubmittedDraws;
	}
}
