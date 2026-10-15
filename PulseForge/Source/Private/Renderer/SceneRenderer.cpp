#include "Renderer/SceneRenderer.h"
#include "Renderer/SceneRendererAmbientOcclusionPass.h"
#include "Renderer/SceneRendererToneMappingStage.h"

#include "Assets/MaterialAssetCache.h"
#include "Renderer/AssetCache/MeshAssetCache.h"
#include "Assets/Project.h"
#include "Renderer/AssetCache/TextureAssetCache.h"
#include "Renderer/EnvironmentLightingCache.h"
#include "Core/Log.h"
#include "Core/ScopedProfileTimer.h"
#include "Renderer/Mesh.h"
#include "Scene/Scene.h"
#include "Scene/Components/EnvironmentLightComponent.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <span>
#include <exception>
#include <limits>
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

		class RenderTargetFrameScope final
		{
		public:
			explicit RenderTargetFrameScope(RenderDevice& Runtime) : m_Runtime(Runtime) {}

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
			RenderDevice& m_Runtime;
			bool m_Active = true;
		};
	}

	struct SceneRenderer::ProjectAssetCaches final
	{
		MeshAssetCache Meshes;
		TextureAssetCache Textures;
		MaterialAssetCache Materials;
		EnvironmentLightingCache EnvironmentLighting;

		ProjectAssetCaches(RenderDevice& Runtime, const Project& SourceProject)
			: Meshes(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry()),
			  Textures(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry()),
			  Materials(SourceProject.GetRootPath(), SourceProject.GetAssetRegistry()),
			  EnvironmentLighting(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry())
		{
		}
	};

	SceneRenderer::SceneRenderer(RenderDevice& Runtime, const Project& SourceProject)
		: m_Runtime(Runtime), m_Project(SourceProject), m_ProjectAssetCaches(std::make_unique<ProjectAssetCaches>(Runtime, SourceProject)),
		  m_ToneMappingStage(std::make_unique<ToneMappingStage>(Runtime)),
		  m_AmbientOcclusionPass(std::make_unique<AmbientOcclusionPass>(Runtime))
	{
	}

	SceneRenderer::~SceneRenderer() = default;

	const Texture* SceneRenderer::GetPointShadowFaceTextureForValidation(size_t Slot, uint32_t Face) const noexcept
	{
		if (Slot >= m_PointShadowCubemaps.size() || Face >= 6 || !m_PointShadowCubemaps[Slot])
			return nullptr;
		return m_PointShadowCubemaps[Slot]->GetFaceTarget(Face).GetDepthTexture();
	}

	std::expected<std::unique_ptr<SceneRenderer>, SceneRendererError> SceneRenderer::Create(
		RenderDevice& Runtime, const Project& SourceProject, const std::filesystem::path& CompiledShaderDirectory)
	{
		Detail::ScopedProfileTimer Timer("SceneRenderer::Create");
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
		m_ShadowVertexShader = std::move(*ShadowVertex);

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
		auto ShadowFragment = LoadAuxiliaryShader("DirectionalShadow.ps.spv", ShaderStage::Fragment,
			"PulseForge alpha-tested shadow fragment shader");
		if (!ShadowFragment)
			return std::unexpected(std::move(ShadowFragment.error()));
		m_ShadowFragmentShader = std::move(*ShadowFragment);
		auto AoPrepassVs = LoadAuxiliaryShader("SsaoNormalDepth.vs.spv", ShaderStage::Vertex,
			"PulseForge SSAO normal-depth vertex shader");
		auto AoPrepassPs = LoadAuxiliaryShader("SsaoNormalDepth.ps.spv", ShaderStage::Fragment,
			"PulseForge SSAO normal-depth fragment shader");
		auto AoVs = LoadAuxiliaryShader("Ssao.vs.spv", ShaderStage::Vertex, "PulseForge SSAO fullscreen vertex shader");
		auto AoPs = LoadAuxiliaryShader("Ssao.ps.spv", ShaderStage::Fragment, "PulseForge SSAO evaluation fragment shader");
		auto AoBlurVs = LoadAuxiliaryShader("SsaoBlur.vs.spv", ShaderStage::Vertex, "PulseForge SSAO blur vertex shader");
		auto AoBlurPs = LoadAuxiliaryShader("SsaoBlur.ps.spv", ShaderStage::Fragment, "PulseForge SSAO blur fragment shader");
		if (!AoPrepassVs || !AoPrepassPs || !AoVs || !AoPs || !AoBlurVs || !AoBlurPs)
			return std::unexpected(MakeResourceError("Could not load the SSAO shader set"));
		auto PointShadowVs = LoadAuxiliaryShader("PointShadow.vs.spv", ShaderStage::Vertex,
			"PulseForge point-shadow vertex shader");
		auto PointShadowPs = LoadAuxiliaryShader("PointShadow.ps.spv", ShaderStage::Fragment,
			"PulseForge point-shadow radial-depth fragment shader");
		if (!PointShadowVs || !PointShadowPs)
			return std::unexpected(MakeResourceError("Could not load the point-shadow shader set"));
		m_PointShadowVertexShader = std::move(*PointShadowVs);
		m_PointShadowFragmentShader = std::move(*PointShadowPs);

		SamplerDesc SamplerDescription;
		SamplerDescription.Minification = SamplerFilter::Linear;
		SamplerDescription.Magnification = SamplerFilter::Linear;
		SamplerDescription.Mip = SamplerMipFilter::Linear;
		SamplerDescription.AddressU = SamplerAddressMode::Repeat;
		SamplerDescription.AddressV = SamplerAddressMode::Repeat;
		SamplerDescription.DebugName = "PulseForge material linear mip sampler";
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
		auto CreateMaterialFallback = [this](
			const std::array<uint8_t, 4>& Pixel,
			TextureFormat Format,
			std::string DebugName)
		{
			TextureDesc Description;
			Description.Width = 1;
			Description.Height = 1;
			Description.Format = Format;
			Description.DebugName = std::move(DebugName);
			return m_Runtime.CreateTexture(Description, std::as_bytes(std::span<const uint8_t>(Pixel)));
		};
		auto MetallicRoughnessFallback = CreateMaterialFallback(
			WhitePixel, TextureFormat::RGBA8_UNorm, "PulseForge default metallic-roughness map");
		if (!MetallicRoughnessFallback)
			return std::unexpected(MakeResourceError(
				"Could not create metallic-roughness fallback: " + MetallicRoughnessFallback.error().Message));
		m_FallbackMetallicRoughnessTexture = std::move(*MetallicRoughnessFallback);
		const std::array<uint8_t, 4> FlatNormalPixel{ 128, 128, 255, 255 };
		auto NormalFallback = CreateMaterialFallback(
			FlatNormalPixel, TextureFormat::RGBA8_UNorm, "PulseForge flat normal map fallback");
		if (!NormalFallback)
			return std::unexpected(MakeResourceError("Could not create normal-map fallback: " + NormalFallback.error().Message));
		m_FallbackNormalTexture = std::move(*NormalFallback);
		const std::array<uint8_t, 4> BlackPixel{ 0, 0, 0, 255 };
		auto EmissiveFallback = CreateMaterialFallback(
			BlackPixel, TextureFormat::RGBA8_Srgb, "PulseForge black emissive map fallback");
		if (!EmissiveFallback)
			return std::unexpected(MakeResourceError("Could not create emissive-map fallback: " + EmissiveFallback.error().Message));
		m_FallbackEmissiveTexture = std::move(*EmissiveFallback);
		auto FallbackPointShadow = CreateWhiteCube(m_Runtime, "PulseForge fully-lit point-shadow fallback cube");
		if (!FallbackPointShadow)
			return std::unexpected(MakeResourceError("Could not create fallback point-shadow cubemap: " + FallbackPointShadow.error().Message));
		m_FallbackPointShadowTexture = std::move(*FallbackPointShadow);
		const MaterialConstants FallbackMaterial{
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 0.0f, 1.0f, 1.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 0.0f }
		};
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

		auto ToneMappingVertexBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Tonemapping.vs.spv");
		if (!ToneMappingVertexBytecode)
			return std::unexpected(std::move(ToneMappingVertexBytecode.error()));
		auto ToneMappingFragmentBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Tonemapping.ps.spv");
		if (!ToneMappingFragmentBytecode)
			return std::unexpected(std::move(ToneMappingFragmentBytecode.error()));
		if (auto ToneMappingInitialized = m_ToneMappingStage->Initialize(*ToneMappingVertexBytecode, *ToneMappingFragmentBytecode);
			!ToneMappingInitialized)
			return std::unexpected(std::move(ToneMappingInitialized.error()));

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
			{ BindingResourceType::Sampler, 0 }
		};
		ShadowBindingLayoutDescription.DebugName = "PulseForge directional shadow maps (space1)";
		auto ShadowBindingLayout = m_Runtime.CreateBindingLayout(ShadowBindingLayoutDescription);
		if (!ShadowBindingLayout)
			return std::unexpected(MakeResourceError("Could not create directional shadow binding layout: " + ShadowBindingLayout.error().Message));
		m_ShadowBindingLayout = std::move(*ShadowBindingLayout);

		BindingLayoutDesc SpotShadowBindingLayoutDescription;
		SpotShadowBindingLayoutDescription.Visibility = ShaderVisibility::Fragment;
		SpotShadowBindingLayoutDescription.ShaderRegisterSpace = 3;
		SpotShadowBindingLayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 }, { BindingResourceType::Texture2D, 1 },
			{ BindingResourceType::Texture2D, 2 }, { BindingResourceType::Texture2D, 3 },
			{ BindingResourceType::Sampler, 0 }
		};
		SpotShadowBindingLayoutDescription.DebugName = "PulseForge spotlight shadow maps (space3)";
		auto SpotShadowBindingLayout = m_Runtime.CreateBindingLayout(SpotShadowBindingLayoutDescription);
		if (!SpotShadowBindingLayout)
			return std::unexpected(MakeResourceError("Could not create spotlight shadow binding layout: " + SpotShadowBindingLayout.error().Message));
		m_SpotShadowBindingLayout = std::move(*SpotShadowBindingLayout);

		BindingLayoutDesc PointShadowBindingLayoutDescription;
		PointShadowBindingLayoutDescription.Visibility = ShaderVisibility::Fragment;
		PointShadowBindingLayoutDescription.ShaderRegisterSpace = 5;
		PointShadowBindingLayoutDescription.Items = {
			{ BindingResourceType::TextureCube, 0 }, { BindingResourceType::TextureCube, 1 },
			{ BindingResourceType::Sampler, 0 }
		};
		PointShadowBindingLayoutDescription.DebugName = "PulseForge point-shadow cubemaps (space5)";
		auto PointShadowBindingLayout = m_Runtime.CreateBindingLayout(PointShadowBindingLayoutDescription);
		if (!PointShadowBindingLayout)
			return std::unexpected(MakeResourceError("Could not create point-shadow binding layout: " + PointShadowBindingLayout.error().Message));
		m_PointShadowBindingLayout = std::move(*PointShadowBindingLayout);

		BindingLayoutDesc ShadowObjectLayoutDescription;
		ShadowObjectLayoutDescription.Visibility = ShaderVisibility::Vertex;
		ShadowObjectLayoutDescription.ShaderRegisterSpace = 4;
		ShadowObjectLayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		ShadowObjectLayoutDescription.DebugName = "PulseForge directional shadow object constants";
		auto ShadowObjectLayout = m_Runtime.CreateBindingLayout(ShadowObjectLayoutDescription);
		if (!ShadowObjectLayout)
			return std::unexpected(MakeResourceError("Could not create shadow object layout: " + ShadowObjectLayout.error().Message));
		m_ShadowObjectBindingLayout = std::move(*ShadowObjectLayout);

		BindingLayoutDesc PointShadowObjectLayoutDescription;
		PointShadowObjectLayoutDescription.Visibility = ShaderVisibility::AllGraphics;
		PointShadowObjectLayoutDescription.ShaderRegisterSpace = 4;
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

		constexpr uint32_t ShadowResolution = 1024;
		for (size_t CascadeIndex = 0; CascadeIndex < m_ShadowTargets.size(); ++CascadeIndex)
		{
			RenderTargetDesc ShadowTargetDescription;
			ShadowTargetDescription.Width = ShadowResolution;
			ShadowTargetDescription.Height = ShadowResolution;
			ShadowTargetDescription.ColorFormat = ColorTargetFormat::None;
			ShadowTargetDescription.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
			ShadowTargetDescription.DebugName = "PulseForge directional shadow cascade " + std::to_string(CascadeIndex);
			auto ShadowTarget = m_Runtime.CreateRenderTarget(ShadowTargetDescription);
			if (!ShadowTarget)
				return std::unexpected(MakeResourceError("Could not create directional shadow target: " + ShadowTarget.error().Message));
			m_ShadowTargets[CascadeIndex] = std::move(*ShadowTarget);
		}

		BindingSetDesc ShadowSetDescription;
		ShadowSetDescription.Layout = m_ShadowBindingLayout;
		for (uint32_t CascadeIndex = 0; CascadeIndex < m_ShadowTargets.size(); ++CascadeIndex)
			ShadowSetDescription.Textures.push_back({ CascadeIndex,
				std::cref(*m_ShadowTargets[CascadeIndex]->GetDepthTexture()) });
		ShadowSetDescription.Samplers.push_back({ 0, std::cref(*m_ShadowSampler) });
		auto ShadowSet = m_Runtime.CreateBindingSet(ShadowSetDescription);
		if (!ShadowSet)
			return std::unexpected(MakeResourceError("Could not create directional shadow bindings: " + ShadowSet.error().Message));
		m_ShadowBindingSet = std::move(*ShadowSet);

		BindingSetDesc SpotShadowSetDescription;
		SpotShadowSetDescription.Layout = m_SpotShadowBindingLayout;
		for (uint32_t ShadowIndex = 0; ShadowIndex < m_SpotShadowTargets.size(); ++ShadowIndex)
			SpotShadowSetDescription.Textures.push_back({ ShadowIndex,
				std::cref(*m_ShadowTargets[ShadowIndex]->GetDepthTexture()) });
		SpotShadowSetDescription.Samplers.push_back({ 0, std::cref(*m_ShadowSampler) });
		auto SpotShadowSet = m_Runtime.CreateBindingSet(SpotShadowSetDescription);
		if (!SpotShadowSet)
			return std::unexpected(MakeResourceError("Could not create safe initial spotlight shadow bindings: " + SpotShadowSet.error().Message));
		m_SpotShadowBindingSet = std::move(*SpotShadowSet);
		BindingSetDesc PointShadowSetDescription;
		PointShadowSetDescription.Layout = m_PointShadowBindingLayout;
		PointShadowSetDescription.Textures = {
			{ 0, std::cref(*m_FallbackPointShadowTexture), BindingResourceType::TextureCube },
			{ 1, std::cref(*m_FallbackPointShadowTexture), BindingResourceType::TextureCube }
		};
		PointShadowSetDescription.Samplers.push_back({ 0, std::cref(*m_ShadowSampler) });
		auto PointShadowSet = m_Runtime.CreateBindingSet(PointShadowSetDescription);
		if (!PointShadowSet)
			return std::unexpected(MakeResourceError("Could not create fallback point-shadow bindings: " + PointShadowSet.error().Message));
		m_PointShadowBindingSet = std::move(*PointShadowSet);
		if (auto Initialized = m_AmbientOcclusionPass->Initialize(std::move(*AoPrepassVs), std::move(*AoPrepassPs), std::move(*AoVs),
				std::move(*AoPs), std::move(*AoBlurVs), std::move(*AoBlurPs));
			!Initialized)
			return std::unexpected(std::move(Initialized.error()));
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
		Detail::ScopedProfileTimer Timer("PBR material/texture binding first creation");

		auto Material = m_ProjectAssetCaches->Materials.GetOrLoad(MaterialAsset);
		if (!Material)
		{
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::AssetLoadFailed,
				{},
				MaterialAsset,
				"Could not load scene material " + MaterialAsset.ToString() + ": " + Material.error().Message
			});
		}

		const auto LoadMaterialTexture = [this, &MaterialAsset](
			const AssetID& TextureAsset,
			TextureFormat Format,
			const Texture& Fallback,
			std::string_view Name) -> std::expected<std::reference_wrapper<const Texture>, SceneRendererError>
		{
			if (TextureAsset.IsNil())
				return std::cref(Fallback);
			auto Texture = m_ProjectAssetCaches->Textures.GetOrLoad(TextureAsset, Format);
			if (!Texture)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					{},
					TextureAsset,
					"Could not load " + std::string(Name) + " texture for material " +
						MaterialAsset.ToString() + ": " + Texture.error().Message
				});
			}
			return std::cref(Texture->get());
		};

		auto BaseColorTexture = LoadMaterialTexture(
			Material->get().BaseColorTexture, TextureFormat::RGBA8_Srgb, *m_FallbackBaseColorTexture, "base-color");
		if (!BaseColorTexture)
			return std::unexpected(std::move(BaseColorTexture.error()));
		auto MetallicRoughnessTexture = LoadMaterialTexture(
			Material->get().MetallicRoughnessTexture,
			TextureFormat::RGBA8_UNorm,
			*m_FallbackMetallicRoughnessTexture,
			"metallic-roughness");
		if (!MetallicRoughnessTexture)
			return std::unexpected(std::move(MetallicRoughnessTexture.error()));
		auto NormalTexture = LoadMaterialTexture(
			Material->get().NormalTexture, TextureFormat::RGBA8_UNorm, *m_FallbackNormalTexture, "normal");
		if (!NormalTexture)
			return std::unexpected(std::move(NormalTexture.error()));
		auto OcclusionTexture = LoadMaterialTexture(Material->get().OcclusionTexture, TextureFormat::RGBA8_UNorm,
			*m_AmbientOcclusionPass->GetFallbackOcclusionTexture(), "occlusion");
		if (!OcclusionTexture)
			return std::unexpected(std::move(OcclusionTexture.error()));
		auto EmissiveTexture = LoadMaterialTexture(
			Material->get().EmissiveTexture, TextureFormat::RGBA8_Srgb, *m_FallbackEmissiveTexture, "emissive");
		if (!EmissiveTexture)
			return std::unexpected(std::move(EmissiveTexture.error()));

		const MaterialConstants Constants{
			{ Material->get().BaseColorFactor.r, Material->get().BaseColorFactor.g,
				Material->get().BaseColorFactor.b, Material->get().BaseColorFactor.a },
			{ Material->get().MetallicFactor, Material->get().RoughnessFactor,
				Material->get().NormalScale, Material->get().OcclusionStrength },
			{ Material->get().EmissiveFactor.r, Material->get().EmissiveFactor.g,
				Material->get().EmissiveFactor.b, 0.0f },
			{ static_cast<float>(Material->get().AlphaMode), Material->get().AlphaCutoff,
				Material->get().DoubleSided ? 1.0f : 0.0f, 0.0f }
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
		BindingSetDescription.Textures.push_back({ 0, *BaseColorTexture });
		BindingSetDescription.Textures.push_back({ 1, std::cref(*EnvironmentTextures.DiffuseIrradiance), BindingResourceType::TextureCube });
		BindingSetDescription.Textures.push_back({ 2, std::cref(*EnvironmentTextures.PrefilteredSpecular), BindingResourceType::TextureCube });
		BindingSetDescription.Textures.push_back({ 3, std::cref(*EnvironmentTextures.BrdfIntegrationLut) });
		BindingSetDescription.Textures.push_back({ 4, std::cref(*EnvironmentTextures.Environment), BindingResourceType::TextureCube });
		BindingSetDescription.Textures.push_back({ 5, *MetallicRoughnessTexture });
		BindingSetDescription.Textures.push_back({ 6, *NormalTexture });
		BindingSetDescription.Textures.push_back({ 7, *OcclusionTexture });
		BindingSetDescription.Textures.push_back({ 8, *EmissiveTexture });
		BindingSetDescription.Samplers.push_back({ 0, std::cref(*m_Sampler) });
		BindingSetDescription.Samplers.push_back({ 1, std::cref(*m_EnvironmentSampler) });
		BindingSetDescription.Buffers.push_back({ 0, std::cref(*MaterialBuffer.value()) });
		BindingSetDescription.Buffers.push_back({ 1, std::cref(*m_ObjectConstantsBuffer) });
		BindingSetDescription.Buffers.push_back({ 2, std::cref(*m_FrameConstantsBuffer) });
		BindingSetDescription.Buffers.push_back({ 3, std::cref(*m_LocalLightingConstantsBuffer) });
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
		const EnvironmentLightingTextures& EnvironmentTextures,
		bool IsReady)
	{
		const std::string Key = EnvironmentAsset.ToString() + (IsReady ? ":ready" : ":loading");
		if (m_EnvironmentBindingSets.contains(Key))
			return {};
		Detail::ScopedProfileTimer Timer("Environment GPU binding creation");
		BindingSetDesc Description;
		Description.Layout = m_BindingLayout;
		Description.Textures = { { 0, std::cref(*m_FallbackBaseColorTexture) },
			{ 1, std::cref(*EnvironmentTextures.DiffuseIrradiance), BindingResourceType::TextureCube },
			{ 2, std::cref(*EnvironmentTextures.PrefilteredSpecular), BindingResourceType::TextureCube },
			{ 3, std::cref(*EnvironmentTextures.BrdfIntegrationLut) },
			{ 4, std::cref(*EnvironmentTextures.Environment), BindingResourceType::TextureCube },
			{ 5, std::cref(*m_FallbackMetallicRoughnessTexture) }, { 6, std::cref(*m_FallbackNormalTexture) },
			{ 7, std::cref(*m_AmbientOcclusionPass->GetFallbackOcclusionTexture()) }, { 8, std::cref(*m_FallbackEmissiveTexture) } };
		Description.Samplers = {
			{ 0, std::cref(*m_Sampler) },
			{ 1, std::cref(*m_EnvironmentSampler) }
		};
		Description.Buffers = {
			{ 0, std::cref(*m_FallbackMaterialConstantsBuffer) },
			{ 1, std::cref(*m_ObjectConstantsBuffer) },
			{ 2, std::cref(*m_FrameConstantsBuffer) },
			{ 3, std::cref(*m_LocalLightingConstantsBuffer) }
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
		auto Snapshot = SceneRenderSnapshotBuilder::BuildForView(
			Source, View, Projection, CameraWorldPosition, NearClipPlane, FarClipPlane);
		if (!Snapshot)
			return PrepareSnapshot(std::move(Snapshot));
		const std::unordered_set<UUID, UUIDHash> Excluded(ExcludedEntities.begin(), ExcludedEntities.end());
		const auto IsExcluded = [&Excluded](const UUID& Identifier) { return Excluded.contains(Identifier); };
		std::erase_if(Snapshot->Meshes, [&IsExcluded](const SceneMeshInstance& Instance)
		{
			return IsExcluded(Instance.Entity);
		});
		std::erase_if(Snapshot->PointLights, [&IsExcluded](const ScenePointLight& Light)
		{
			return IsExcluded(Light.Entity);
		});
		std::erase_if(Snapshot->SpotLights, [&IsExcluded](const SceneSpotLight& Light)
		{
			return IsExcluded(Light.Entity);
		});
		if (Snapshot->DirectionalLight && IsExcluded(Snapshot->DirectionalLight->Entity))
			Snapshot->DirectionalLight.reset();
		if (Snapshot->EnvironmentLight && IsExcluded(Snapshot->EnvironmentLight->Entity))
			Snapshot->EnvironmentLight.reset();
		return PrepareSnapshot(std::move(Snapshot));
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareSnapshot(
		std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> Snapshot)
	{
		std::optional<Detail::ScopedProfileTimer> FirstPrepareTimer;
		if (!m_LoggedFirstSnapshotPreparation)
		{
			m_LoggedFirstSnapshotPreparation = true;
			FirstPrepareTimer.emplace("SceneRenderer first PrepareSnapshot");
		}
		m_PreparedSnapshot.reset();
		m_EnvironmentLightingPending = false;
		m_PreparedEnvironmentReady = false;
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
		std::optional<AssetID> BindingEnvironmentAsset = EnvironmentAsset;
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
				BindingEnvironmentAsset.reset();
			}
			else
			{
				EnvironmentTextures = &LoadedEnvironment->value().get();
				m_PreparedEnvironmentReady = true;
			}
			if (auto Bindings = EnsureEnvironmentBindings(*EnvironmentAsset, *EnvironmentTextures, m_PreparedEnvironmentReady); !Bindings)
				return std::unexpected(std::move(Bindings.error()));
		}

		std::optional<VertexLayoutDesc> SceneVertexLayout;
		for (const SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			auto Mesh = m_ProjectAssetCaches->Meshes.GetOrLoad(Instance.MeshAsset);
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
			if (auto Bindings = EnsureMaterialBindings(*Instance.MaterialAsset, BindingEnvironmentAsset, *EnvironmentTextures); !Bindings)
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
			if (Snapshot->DirectionalLight && Snapshot->DirectionalLight->CastShadows)
				if (auto ShadowPipeline = EnsureShadowPipeline(Snapshot->DirectionalLight->ShadowBias); !ShadowPipeline)
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

	std::expected<const MaterialAssetDesc*, SceneRendererError> SceneRenderer::ResolveMaterialDescription(
		const SceneMeshInstance& Instance)
	{
		if (!Instance.MaterialAsset)
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::MissingMaterial,
				Instance.Entity,
				Instance.MeshAsset,
				"Visible mesh entity has no material asset assigned" });
		auto Material = m_ProjectAssetCaches->Materials.GetOrLoad(*Instance.MaterialAsset);
		if (!Material)
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::AssetLoadFailed,
				Instance.Entity,
				*Instance.MaterialAsset,
				"Could not resolve material for mesh instance: " + Material.error().Message });
		return &Material->get();
	}

	std::expected<const SceneRenderer::MaterialBindingResources*, SceneRendererError> SceneRenderer::ResolveMaterialBinding(
		const SceneMeshInstance& Instance)
	{
		if (!Instance.MaterialAsset)
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::MissingMaterial,
				Instance.Entity,
				Instance.MeshAsset,
				"Visible mesh entity has no material asset assigned" });
		const std::optional<AssetID> EnvironmentAsset = m_PreparedSnapshot && m_PreparedSnapshot->EnvironmentLight
			? std::optional<AssetID>{ m_PreparedSnapshot->EnvironmentLight->HdrImage }
			: std::nullopt;
		const std::optional<AssetID> BindingEnvironmentAsset = m_PreparedEnvironmentReady ? EnvironmentAsset : std::nullopt;
		const auto Binding = m_MaterialBindings.find(MakeMaterialBindingKey(*Instance.MaterialAsset, BindingEnvironmentAsset));
		if (Binding == m_MaterialBindings.end())
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::AssetLoadFailed,
				Instance.Entity,
				*Instance.MaterialAsset,
				"Prepared material binding set is unavailable" });
		return &Binding->second;
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsurePipeline(
		ColorTargetFormat ColorFormat,
		bool AlphaBlend,
		bool DoubleSided)
	{
		const auto Key = std::tuple{ ColorFormat, AlphaBlend, DoubleSided };
		if (m_Pipelines.contains(Key))
			return {};
		Detail::ScopedProfileTimer Timer("Scene graphics pipeline creation");
		if (!m_PipelineVertexLayout)
			return std::unexpected(MakeResourceError("Cannot create a scene pipeline before a mesh vertex layout is prepared"));

		GraphicsPipelineDesc PipelineDescription;
		PipelineDescription.VertexShader = m_VertexShader;
		PipelineDescription.FragmentShader = m_FragmentShader;
		PipelineDescription.BindingLayouts = { m_BindingLayout, m_ShadowBindingLayout, m_AmbientOcclusionPass->GetFinalLayout(),
			m_SpotShadowBindingLayout, m_PointShadowBindingLayout };
		PipelineDescription.VertexLayout = *m_PipelineVertexLayout;
		PipelineDescription.ColorFormat = ColorFormat;
		PipelineDescription.Rasterizer.Cull = DoubleSided ? CullMode::None : CullMode::Back;
		PipelineDescription.Blend.Enabled = AlphaBlend;
		PipelineDescription.Depth.TestEnabled = true;
		PipelineDescription.Depth.WriteEnabled = !AlphaBlend;
		PipelineDescription.Depth.Compare = DepthCompareOperation::Less;
		PipelineDescription.DebugName = AlphaBlend ? "PulseForge transparent scene pipeline" :
			DoubleSided ? "PulseForge double-sided scene pipeline" : "PulseForge opaque/masked scene pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(PipelineDescription);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create scene graphics pipeline: " + Pipeline.error().Message));

		m_Pipelines.emplace(Key, std::move(Pipeline.value()));
		PF_CORE_INFO("Created scene pipeline for vertex stride {0}, color format {1}, blend {2}, double-sided {3}",
			m_PipelineVertexLayout->Stride,
			static_cast<uint32_t>(ColorFormat), AlphaBlend, DoubleSided);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureShadowPipeline(float DepthBias, bool DoubleSided)
	{
		const int32_t RasterBias = static_cast<int32_t>(std::lround(DepthBias));
		const auto Key = std::pair{ RasterBias, DoubleSided };
		if (m_ShadowPipelines.contains(Key))
			return {};
		if (!m_PipelineVertexLayout)
			return std::unexpected(MakeResourceError("Cannot create a shadow pipeline before a mesh layout is prepared"));
		GraphicsPipelineDesc Description;
		Description.VertexShader = m_ShadowVertexShader;
		Description.FragmentShader = m_ShadowFragmentShader;
		Description.BindingLayouts = { m_BindingLayout, m_ShadowObjectBindingLayout };
		Description.VertexLayout.Stride = m_PipelineVertexLayout->Stride;
		const auto PositionAttribute = std::find_if(m_PipelineVertexLayout->Attributes.begin(),
			m_PipelineVertexLayout->Attributes.end(), [](const VertexAttributeDesc& Attribute)
			{
				return Attribute.Semantic == VertexSemantic::Position;
			});
		if (PositionAttribute == m_PipelineVertexLayout->Attributes.end())
			return std::unexpected(MakeResourceError("The mesh layout has no position attribute for shadow rendering"));
		Description.VertexLayout.Attributes.push_back(*PositionAttribute);
		const auto ColorAttribute = std::find_if(m_PipelineVertexLayout->Attributes.begin(),
			m_PipelineVertexLayout->Attributes.end(), [](const VertexAttributeDesc& Attribute)
			{
				return Attribute.Semantic == VertexSemantic::Color;
			});
		if (ColorAttribute == m_PipelineVertexLayout->Attributes.end())
			return std::unexpected(MakeResourceError("The mesh layout has no color attribute for alpha-tested shadows"));
		Description.VertexLayout.Attributes.push_back(*ColorAttribute);
		const auto TexCoordAttribute = std::find_if(m_PipelineVertexLayout->Attributes.begin(),
			m_PipelineVertexLayout->Attributes.end(), [](const VertexAttributeDesc& Attribute)
			{
				return Attribute.Semantic == VertexSemantic::TexCoord;
			});
		if (TexCoordAttribute == m_PipelineVertexLayout->Attributes.end())
			return std::unexpected(MakeResourceError("The mesh layout has no texture coordinates for alpha-tested shadows"));
		Description.VertexLayout.Attributes.push_back(*TexCoordAttribute);
		Description.ColorFormat = ColorTargetFormat::None;
		Description.Rasterizer.Cull = DoubleSided ? CullMode::None : CullMode::Back;
		Description.Rasterizer.DepthBias = static_cast<float>(RasterBias);
		Description.Rasterizer.SlopeScaledDepthBias = 1.5f;
		Description.Depth.TestEnabled = true;
		Description.Depth.WriteEnabled = true;
		Description.Depth.Compare = DepthCompareOperation::Less;
		Description.DebugName = DoubleSided
			? "PulseForge double-sided alpha-tested shadow depth pipeline"
			: "PulseForge alpha-tested shadow depth pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create directional shadow pipeline: " + Pipeline.error().Message));
		m_ShadowPipelines.emplace(Key, std::move(*Pipeline));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureSpotShadowResources()
	{
		if (m_SpotShadowTargets.front())
			return {};

		std::array<RenderTargetHandle, MaxSpotlightShadowMapCount> Targets;
		for (size_t ShadowIndex = 0; ShadowIndex < Targets.size(); ++ShadowIndex)
		{
			RenderTargetDesc Description;
			Description.Width = SpotlightShadowMapResolution;
			Description.Height = SpotlightShadowMapResolution;
			Description.ColorFormat = ColorTargetFormat::None;
			Description.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
			Description.DebugName = "PulseForge spotlight shadow map " + std::to_string(ShadowIndex);
			auto Target = m_Runtime.CreateRenderTarget(Description);
			if (!Target)
				return std::unexpected(MakeResourceError("Could not create spotlight shadow map: " + Target.error().Message));
			if (!(*Target)->GetDepthTexture() ||
				(*Target)->GetDepthTexture()->GetDescription().Format != TextureFormat::Depth32Float)
				return std::unexpected(MakeResourceError("Spotlight shadow target did not provide a shader-readable D32 depth texture"));
			Targets[ShadowIndex] = std::move(*Target);
		}

		BindingSetDesc Description;
		Description.Layout = m_SpotShadowBindingLayout;
		for (uint32_t ShadowIndex = 0; ShadowIndex < Targets.size(); ++ShadowIndex)
			Description.Textures.push_back({ ShadowIndex, std::cref(*Targets[ShadowIndex]->GetDepthTexture()) });
		Description.Samplers.push_back({ 0, std::cref(*m_ShadowSampler) });
		auto BindingSet = m_Runtime.CreateBindingSet(Description);
		if (!BindingSet)
			return std::unexpected(MakeResourceError("Could not create spotlight shadow bindings: " + BindingSet.error().Message));

		m_SpotShadowTargets = std::move(Targets);
		m_SpotShadowBindingSet = std::move(*BindingSet);
		if (m_SpotShadowResourceGeneration == (std::numeric_limits<uint64_t>::max)())
			m_SpotShadowResourceGeneration = 1;
		else
			++m_SpotShadowResourceGeneration;
		PF_CORE_INFO("Created {0} persistent {1}x{1} D32 spotlight shadow maps ({2} MiB depth storage)",
			MaxSpotlightShadowMapCount, SpotlightShadowMapResolution,
			(MaxSpotlightShadowMapCount * SpotlightShadowMapResolution * SpotlightShadowMapResolution * sizeof(float)) / (1024 * 1024));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsurePointShadowResources()
	{
		if (m_PointShadowCubemaps.front())
			return {};

		std::array<DepthCubemapHandle, MaxPointLightShadowCount> Cubemaps;
		for (size_t Slot = 0; Slot < Cubemaps.size(); ++Slot)
		{
			auto Cubemap = m_Runtime.CreateDepthCubemap(PointLightShadowMapResolution,
				"PulseForge point-shadow depth cubemap " + std::to_string(Slot));
			if (!Cubemap)
				return std::unexpected(MakeResourceError("Could not create point-shadow depth cubemap: " + Cubemap.error().Message));
			if ((*Cubemap)->GetTexture().GetDescription().Format != TextureFormat::Depth32Float ||
				(*Cubemap)->GetResolution() != PointLightShadowMapResolution)
				return std::unexpected(MakeResourceError("Point-shadow cubemap has an incompatible depth format or resolution"));
			Cubemaps[Slot] = std::move(*Cubemap);
		}

		BindingSetDesc Description;
		Description.Layout = m_PointShadowBindingLayout;
		for (uint32_t Slot = 0; Slot < Cubemaps.size(); ++Slot)
			Description.Textures.push_back({ Slot, std::cref(Cubemaps[Slot]->GetTexture()), BindingResourceType::TextureCube });
		Description.Samplers.push_back({ 0, std::cref(*m_ShadowSampler) });
		auto BindingSet = m_Runtime.CreateBindingSet(Description);
		if (!BindingSet)
			return std::unexpected(MakeResourceError("Could not create point-shadow cubemap bindings: " + BindingSet.error().Message));

		m_PointShadowCubemaps = std::move(Cubemaps);
		m_PointShadowBindingSet = std::move(*BindingSet);
		if (m_PointShadowResourceGeneration == (std::numeric_limits<uint64_t>::max)())
			m_PointShadowResourceGeneration = 1;
		else
			++m_PointShadowResourceGeneration;
		PF_CORE_INFO("Created {0} persistent {1}x{1} D32 point-shadow cubemaps ({2} MiB depth storage)",
			MaxPointLightShadowCount, PointLightShadowMapResolution,
			(MaxPointLightShadowCount * 6 * PointLightShadowMapResolution * PointLightShadowMapResolution * sizeof(float)) / (1024 * 1024));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareLocalLighting()
	{
		m_PreparedLocalLighting = {};
		m_SelectedLocalLightCount = 0;
		m_PreparedSpotShadowMatrices.fill(glm::mat4(1.0f));
		m_PreparedSpotShadowParameters.fill(glm::vec4(0.0f));
		for (auto& FaceMatrices : m_PreparedPointShadowMatrices)
			FaceMatrices.fill(glm::mat4(1.0f));
		m_PreparedPointShadowParameters.fill(glm::vec4(0.0f));
		if (!m_PreparedSnapshot)
		{
			m_SpotShadowSlotOwners.fill(std::nullopt);
			m_PointShadowSlotOwners.fill(std::nullopt);
			return {};
		}

		size_t ActiveLightCount = 0;
		const auto ConsiderLocalLight = [&](const LocalLightRelevance& Relevance, const LocalLightGpuData& Data,
			UUID Entity, const SceneSpotLight* Spot, const ScenePointLight* Point)
		{
			if (Relevance.Intensity <= 0.0f)
				return;
			++ActiveLightCount;
			const SelectedLocalLight Candidate{ Relevance, Data, Entity, Spot, Point };
			size_t InsertIndex = 0;
			while (InsertIndex < m_SelectedLocalLightCount && !IsLocalLightMoreRelevant(
				Candidate.Relevance, m_SelectedLocalLights[InsertIndex].Relevance, m_PreparedSnapshot->CameraWorldPosition))
				++InsertIndex;
			if (InsertIndex >= MaxLocalLightCount)
				return;
			const size_t NewCount = (std::min)(m_SelectedLocalLightCount + 1, MaxLocalLightCount);
			for (size_t Index = NewCount - 1; Index > InsertIndex; --Index)
				m_SelectedLocalLights[Index] = m_SelectedLocalLights[Index - 1];
			m_SelectedLocalLights[InsertIndex] = Candidate;
			m_SelectedLocalLightCount = NewCount;
		};

		for (const ScenePointLight& Light : m_PreparedSnapshot->PointLights)
		{
			LocalLightGpuData Data{};
			Data.PositionRange = glm::vec4(Light.WorldPosition, Light.Range);
			Data.ColorIntensity = glm::vec4(Light.Color, Light.Intensity);
			Data.OuterCosAndType = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
			ConsiderLocalLight({ Light.Entity, Light.WorldPosition, Light.Color, Light.Intensity, Light.Range, PointLocalLightTypeOrder },
				Data, Light.Entity, nullptr, &Light);
		}
		for (const SceneSpotLight& Light : m_PreparedSnapshot->SpotLights)
		{
			LocalLightGpuData Data{};
			Data.PositionRange = glm::vec4(Light.WorldPosition, Light.Range);
			Data.ColorIntensity = glm::vec4(Light.Color, Light.Intensity);
			Data.DirectionInnerCos = glm::vec4(Light.WorldDirection, std::cos(glm::radians(Light.InnerConeAngleDegrees)));
			Data.OuterCosAndType = glm::vec4(std::cos(glm::radians(Light.OuterConeAngleDegrees)), -1.0f, 0.0f, 1.0f);
			ConsiderLocalLight({ Light.Entity, Light.WorldPosition, Light.Color, Light.Intensity, Light.Range, SpotLocalLightTypeOrder },
				Data, Light.Entity, &Light, nullptr);
		}

		if (ActiveLightCount > MaxLocalLightCount && !m_LoggedLocalLightOverflow)
		{
			PF_CORE_WARN("Scene has {0} active local lights; rendering the {1} most camera-relevant lights (UUID and type break ties)",
				ActiveLightCount, MaxLocalLightCount);
			m_LoggedLocalLightOverflow = true;
		}

		std::array<SpotlightShadowCandidate, MaxLocalLightCount> ShadowCandidates{};
		size_t ShadowCandidateCount = 0;
		for (size_t Index = 0; Index < m_SelectedLocalLightCount; ++Index)
		{
			const SceneSpotLight* Spot = m_SelectedLocalLights[Index].Spot;
			if (!Spot || !Spot->CastShadows || m_PreparedSnapshot->Meshes.empty())
				continue;
			auto Projection = BuildSpotlightShadowProjection(
				Spot->WorldPosition, Spot->WorldDirection, Spot->Range, Spot->OuterConeAngleDegrees);
			if (!Projection)
			{
				if (Spot->OuterConeAngleDegrees > MaxSpotlightShadowOuterHalfAngleDegrees && !m_LoggedUnsupportedSpotShadowCone)
				{
					PF_CORE_WARN("Spotlight shadow projection supports outer half-angles up to {0} degrees; wider spotlights remain illuminated without shadows",
						MaxSpotlightShadowOuterHalfAngleDegrees);
					m_LoggedUnsupportedSpotShadowCone = true;
				}
				continue;
			}
			ShadowCandidates[ShadowCandidateCount++] = { m_SelectedLocalLights[Index].Entity,
				static_cast<uint32_t>(Index) };
		}

		if (ShadowCandidateCount > 0)
		{
			if (auto Resources = EnsureSpotShadowResources(); !Resources)
				return std::unexpected(std::move(Resources.error()));
			if (auto Pipeline = EnsureShadowPipeline(1.0f); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}
		const SpotlightShadowSlotAssignments SlotAssignments = AssignSpotlightShadowSlots(
			std::span(ShadowCandidates.data(), ShadowCandidateCount), m_SpotShadowSlotOwners);
		for (size_t Index = 0; Index < m_SelectedLocalLightCount; ++Index)
			if (SlotAssignments[Index] >= 0)
				m_SelectedLocalLights[Index].Data.OuterCosAndType.y = static_cast<float>(SlotAssignments[Index]);
		if (ShadowCandidateCount > MaxSpotlightShadowMapCount && !m_LoggedSpotShadowOverflow)
		{
			PF_CORE_WARN("More than {0} selected spotlights request shadows; remaining lights render unshadowed",
				MaxSpotlightShadowMapCount);
			m_LoggedSpotShadowOverflow = true;
		}

		std::array<PointShadowCandidate, MaxLocalLightCount> PointCandidates{};
		size_t PointCandidateCount = 0;
		for (size_t Index = 0; Index < m_SelectedLocalLightCount; ++Index)
		{
			const ScenePointLight* Point = m_SelectedLocalLights[Index].Point;
			if (!Point || !Point->CastShadows || m_PreparedSnapshot->Meshes.empty())
				continue;
			auto Projections = BuildPointShadowProjections(Point->WorldPosition, Point->Range);
			if (!Projections)
				return std::unexpected(MakeDrawError("Could not build point-shadow projections for entity " +
					Point->Entity.ToString() + ": " + Projections.error().Message));
			PointCandidates[PointCandidateCount++] = { Point->Entity, static_cast<uint32_t>(Index) };
		}
		if (PointCandidateCount > 0)
		{
			if (auto Resources = EnsurePointShadowResources(); !Resources)
				return std::unexpected(std::move(Resources.error()));
			for (size_t DoubleSidedIndex = 0; DoubleSidedIndex < m_PointShadowPipelines.size(); ++DoubleSidedIndex)
			{
				if (m_PointShadowPipelines[DoubleSidedIndex])
					continue;
				if (!m_PipelineVertexLayout)
					return std::unexpected(MakeResourceError("Cannot create point-shadow pipeline before a mesh layout is prepared"));
				GraphicsPipelineDesc Description;
				Description.VertexShader = m_PointShadowVertexShader;
				Description.FragmentShader = m_PointShadowFragmentShader;
				Description.BindingLayouts = { m_BindingLayout, m_PointShadowObjectBindingLayout };
				Description.VertexLayout.Stride = m_PipelineVertexLayout->Stride;
				const auto Position = std::find_if(m_PipelineVertexLayout->Attributes.begin(), m_PipelineVertexLayout->Attributes.end(),
					[](const VertexAttributeDesc& Attribute) { return Attribute.Semantic == VertexSemantic::Position; });
				if (Position == m_PipelineVertexLayout->Attributes.end())
					return std::unexpected(MakeResourceError("The mesh layout has no position attribute for point-shadow rendering"));
				Description.VertexLayout.Attributes.push_back(*Position);
				const auto Color = std::find_if(m_PipelineVertexLayout->Attributes.begin(), m_PipelineVertexLayout->Attributes.end(),
					[](const VertexAttributeDesc& Attribute) { return Attribute.Semantic == VertexSemantic::Color; });
				if (Color == m_PipelineVertexLayout->Attributes.end())
					return std::unexpected(MakeResourceError("The mesh layout has no color attribute for point-shadow alpha tests"));
				Description.VertexLayout.Attributes.push_back(*Color);
				const auto TexCoord = std::find_if(m_PipelineVertexLayout->Attributes.begin(), m_PipelineVertexLayout->Attributes.end(),
					[](const VertexAttributeDesc& Attribute) { return Attribute.Semantic == VertexSemantic::TexCoord; });
				if (TexCoord == m_PipelineVertexLayout->Attributes.end())
					return std::unexpected(MakeResourceError("The mesh layout has no texture coordinates for point-shadow alpha tests"));
				Description.VertexLayout.Attributes.push_back(*TexCoord);
				Description.ColorFormat = ColorTargetFormat::None;
				Description.Rasterizer.Cull = DoubleSidedIndex == 1 ? CullMode::None : CullMode::Back;
				Description.Depth.TestEnabled = true;
				Description.Depth.WriteEnabled = true;
				Description.Depth.Compare = DepthCompareOperation::Less;
				Description.DebugName = DoubleSidedIndex == 1
					? "PulseForge double-sided alpha-tested point-shadow pipeline"
					: "PulseForge alpha-tested point-shadow pipeline";
				auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
				if (!Pipeline)
					return std::unexpected(MakeResourceError("Could not create point-shadow pipeline: " + Pipeline.error().Message));
				m_PointShadowPipelines[DoubleSidedIndex] = std::move(*Pipeline);
			}
		}
		const PointShadowSlotAssignments PointAssignments = AssignPointShadowSlots(
			std::span(PointCandidates.data(), PointCandidateCount), m_PointShadowSlotOwners);
		for (size_t Index = 0; Index < m_SelectedLocalLightCount; ++Index)
			if (m_SelectedLocalLights[Index].Point && PointAssignments[Index] >= 0)
				m_SelectedLocalLights[Index].Data.OuterCosAndType.y = static_cast<float>(PointAssignments[Index]);
		if (PointCandidateCount > MaxPointLightShadowCount && !m_LoggedPointShadowOverflow)
		{
			PF_CORE_WARN("More than {0} selected point lights request shadows; remaining lights continue unshadowed",
				MaxPointLightShadowCount);
			m_LoggedPointShadowOverflow = true;
		}

		m_PreparedLocalLighting.Counts.x = static_cast<uint32_t>(m_SelectedLocalLightCount);
		for (size_t Index = 0; Index < m_SelectedLocalLightCount; ++Index)
			m_PreparedLocalLighting.Lights[Index] = m_SelectedLocalLights[Index].Data;

		for (size_t Slot = 0; Slot < m_SpotShadowSlotOwners.size(); ++Slot)
		{
			if (!m_SpotShadowSlotOwners[Slot])
				continue;
			const auto Selected = std::find_if(m_SelectedLocalLights.begin(),
				m_SelectedLocalLights.begin() + static_cast<std::ptrdiff_t>(m_SelectedLocalLightCount),
				[&](const SelectedLocalLight& Light)
				{
					return Light.Spot && MatchesLocalLightIdentity(
						Light.Relevance, *m_SpotShadowSlotOwners[Slot], SpotLocalLightTypeOrder);
				});
			if (Selected == m_SelectedLocalLights.begin() + static_cast<std::ptrdiff_t>(m_SelectedLocalLightCount) || !Selected->Spot)
				return std::unexpected(MakeDrawError("Spotlight shadow slot no longer identifies a selected spotlight"));
			auto Projection = BuildSpotlightShadowProjection(
				Selected->Spot->WorldPosition, Selected->Spot->WorldDirection,
				Selected->Spot->Range, Selected->Spot->OuterConeAngleDegrees);
			if (!Projection)
				return std::unexpected(MakeDrawError("Could not build assigned spotlight shadow projection: " + Projection.error().Message));
			m_PreparedSpotShadowMatrices[Slot] = Projection->ViewProjection;
			m_PreparedSpotShadowParameters[Slot] = glm::vec4(
				Selected->Spot->ShadowBias, Selected->Spot->ShadowNormalBias,
				Selected->Spot->ShadowSoftness, 1.0f);
		}
		for (size_t Slot = 0; Slot < m_PointShadowSlotOwners.size(); ++Slot)
		{
			if (!m_PointShadowSlotOwners[Slot])
				continue;
			const auto Selected = std::find_if(m_SelectedLocalLights.begin(),
				m_SelectedLocalLights.begin() + static_cast<std::ptrdiff_t>(m_SelectedLocalLightCount),
				[&](const SelectedLocalLight& Light)
				{
					return Light.Point && MatchesLocalLightIdentity(
						Light.Relevance, *m_PointShadowSlotOwners[Slot], PointLocalLightTypeOrder);
				});
			if (Selected == m_SelectedLocalLights.begin() + static_cast<std::ptrdiff_t>(m_SelectedLocalLightCount) || !Selected->Point)
				return std::unexpected(MakeDrawError("Point-shadow slot no longer identifies a selected point light"));
			auto Projections = BuildPointShadowProjections(Selected->Point->WorldPosition, Selected->Point->Range);
			if (!Projections)
				return std::unexpected(MakeDrawError("Could not build assigned point-shadow projections: " + Projections.error().Message));
			for (size_t Face = 0; Face < Projections->Faces.size(); ++Face)
				m_PreparedPointShadowMatrices[Slot][Face] = Projections->Faces[Face].ViewProjection;
			m_PreparedPointShadowParameters[Slot] = glm::vec4(
				Selected->Point->ShadowBias, Selected->Point->ShadowNormalBias,
				Selected->Point->ShadowSoftness, 1.0f);
		}
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::RenderSpotlightShadows()
	{
		if (!m_PreparedSnapshot || m_PreparedSnapshot->Meshes.empty() || !m_SpotShadowTargets.front())
			return RenderPointLightShadows();
		for (size_t Slot = 0; Slot < m_SpotShadowSlotOwners.size(); ++Slot)
		{
			if (!m_SpotShadowSlotOwners[Slot])
				continue;
			const GraphicsResult Begin = m_Runtime.BeginRenderTarget(*m_SpotShadowTargets[Slot]);
			if (!Begin)
				return std::unexpected(MakeDrawError("Could not begin spotlight shadow target: " + Begin.error().Message));
			RenderTargetFrameScope TargetScope(m_Runtime);
			for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
			{
				auto Material = ResolveMaterialDescription(Instance);
				if (!Material)
					return std::unexpected(std::move(Material.error()));
				if ((*Material)->AlphaMode == MaterialAlphaMode::Blend)
					continue;
				auto MaterialBinding = ResolveMaterialBinding(Instance);
				if (!MaterialBinding)
					return std::unexpected(std::move(MaterialBinding.error()));
				if (auto PipelineResult = EnsureShadowPipeline(1.0f, (*Material)->DoubleSided); !PipelineResult)
					return std::unexpected(std::move(PipelineResult.error()));
				const auto Pipeline = m_ShadowPipelines.find({ 1, (*Material)->DoubleSided });
				if (Pipeline == m_ShadowPipelines.end())
					return std::unexpected(MakeResourceError("Spotlight alpha-tested shadow pipeline was not prepared"));
				auto Mesh = m_ProjectAssetCaches->Meshes.GetOrLoad(Instance.MeshAsset);
				if (!Mesh)
					return std::unexpected(MakeDrawError("Spotlight shadow caster mesh became unavailable: " + Mesh.error().Message));
				const glm::mat4 ShadowTransform = m_PreparedSpotShadowMatrices[Slot] * Instance.WorldTransform;
				const auto Update = m_Runtime.WriteBuffer(
					*m_ShadowObjectConstantsBuffer, 0, std::as_bytes(std::span(&ShadowTransform, 1)));
				if (!Update)
					return std::unexpected(MakeDrawError("Could not update spotlight shadow object constants: " + Update.error().Message));
				const std::array<const BindingSet*, 2> Bindings = {
					(*MaterialBinding)->BindingSet.get(), m_ShadowObjectBindingSet.get() };
				const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
				const GraphicsResult Draw = m_Runtime.DrawIndexed(*Pipeline->second, Mesh->get(), Arguments, Bindings);
				if (!Draw)
					return std::unexpected(MakeDrawError("Could not draw spotlight shadow caster: " + Draw.error().Message));
				++m_LastSpotShadowCasterDrawCount;
			}
			const GraphicsResult End = TargetScope.End();
			if (!End)
				return std::unexpected(MakeDrawError("Could not end spotlight shadow target: " + End.error().Message));
			++m_LastSpotShadowPassCount;
		}
		return RenderPointLightShadows();
	}

	std::expected<void, SceneRendererError> SceneRenderer::RenderPointLightShadows()
	{
		if (!m_PreparedSnapshot || m_PreparedSnapshot->Meshes.empty() || !m_PointShadowCubemaps.front())
			return {};
		if (std::none_of(m_PointShadowPipelines.begin(), m_PointShadowPipelines.end(),
			[](const GraphicsPipelineHandle& Pipeline) { return static_cast<bool>(Pipeline); }))
			return std::unexpected(MakeResourceError("Point-shadow pipeline was not prepared for active shadow owners"));

		for (size_t Slot = 0; Slot < m_PointShadowSlotOwners.size(); ++Slot)
		{
			if (!m_PointShadowSlotOwners[Slot])
				continue;
			const auto Selected = std::find_if(m_SelectedLocalLights.begin(),
				m_SelectedLocalLights.begin() + static_cast<std::ptrdiff_t>(m_SelectedLocalLightCount),
				[&](const SelectedLocalLight& Light)
				{
					return Light.Point && MatchesLocalLightIdentity(
						Light.Relevance, *m_PointShadowSlotOwners[Slot], PointLocalLightTypeOrder);
				});
			if (Selected == m_SelectedLocalLights.begin() + static_cast<std::ptrdiff_t>(m_SelectedLocalLightCount) || !Selected->Point)
				return std::unexpected(MakeDrawError("Point-shadow rendering found a stale slot owner"));

			for (size_t Face = 0; Face < 6; ++Face)
			{
				const RenderTarget& Target = m_PointShadowCubemaps[Slot]->GetFaceTarget(static_cast<uint32_t>(Face));
				RenderTargetClearValue ClearValue;
				ClearValue.Depth = 1.0f;
				const GraphicsResult Begin = m_Runtime.BeginRenderTarget(Target, ClearValue);
				if (!Begin)
					return std::unexpected(MakeDrawError("Could not begin point-shadow cubemap face: " + Begin.error().Message));
				RenderTargetFrameScope TargetScope(m_Runtime);
				for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
				{
					auto Material = ResolveMaterialDescription(Instance);
					if (!Material)
						return std::unexpected(std::move(Material.error()));
					if ((*Material)->AlphaMode == MaterialAlphaMode::Blend)
						continue;
					auto MaterialBinding = ResolveMaterialBinding(Instance);
					if (!MaterialBinding)
						return std::unexpected(std::move(MaterialBinding.error()));
					const size_t PipelineIndex = (*Material)->DoubleSided ? 1u : 0u;
					if (!m_PointShadowPipelines[PipelineIndex])
						return std::unexpected(MakeResourceError("Point-shadow alpha-tested pipeline was not prepared"));
					auto Mesh = m_ProjectAssetCaches->Meshes.GetOrLoad(Instance.MeshAsset);
					if (!Mesh)
						return std::unexpected(MakeDrawError("Point-shadow caster mesh became unavailable: " + Mesh.error().Message));
					const PointShadowObjectConstants Object{
						m_PreparedPointShadowMatrices[Slot][Face],
						glm::vec4(Selected->Point->WorldPosition, Selected->Point->Range),
						Instance.WorldTransform };
					const auto Update = m_Runtime.WriteBuffer(
						*m_PointShadowObjectConstantsBuffer, 0, std::as_bytes(std::span(&Object, 1)));
					if (!Update)
						return std::unexpected(MakeDrawError("Could not update point-shadow constants: " + Update.error().Message));
					const std::array<const BindingSet*, 2> Bindings = {
						(*MaterialBinding)->BindingSet.get(), m_PointShadowObjectBindingSet.get() };
					const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
					const GraphicsResult Draw = m_Runtime.DrawIndexed(
						*m_PointShadowPipelines[PipelineIndex], Mesh->get(), Arguments, Bindings);
					if (!Draw)
						return std::unexpected(MakeDrawError("Could not draw point-shadow caster: " + Draw.error().Message));
					++m_LastPointShadowCasterDrawCount;
				}
				const GraphicsResult End = TargetScope.End();
				if (!End)
					return std::unexpected(MakeDrawError("Could not end point-shadow cubemap face: " + End.error().Message));
				++m_LastPointShadowPassCount;
			}
		}
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::RenderShadowCascades()
	{
		m_LastSpotShadowPassCount = 0;
		m_LastSpotShadowCasterDrawCount = 0;
		m_LastPointShadowPassCount = 0;
		m_LastPointShadowCasterDrawCount = 0;
		if (auto LocalLighting = PrepareLocalLighting(); !LocalLighting)
			return std::unexpected(std::move(LocalLighting.error()));
		if (!m_PreparedSnapshot || !m_PreparedSnapshot->DirectionalLight ||
			!m_PreparedSnapshot->DirectionalLight->CastShadows || m_PreparedSnapshot->Meshes.empty())
			return RenderSpotlightShadows();
		if (!m_PreparedCascades)
			return std::unexpected(MakeDrawError("Prepared directional shadow cascades are unavailable"));
		const int32_t RasterBias = static_cast<int32_t>(std::lround(m_PreparedSnapshot->DirectionalLight->ShadowBias));

		for (size_t CascadeIndex = 0; CascadeIndex < m_PreparedCascades->Cascades.size(); ++CascadeIndex)
		{
			const GraphicsResult Begin = m_Runtime.BeginRenderTarget(*m_ShadowTargets[CascadeIndex]);
			if (!Begin)
				return std::unexpected(MakeDrawError("Could not begin shadow cascade: " + Begin.error().Message));
			RenderTargetFrameScope TargetScope(m_Runtime);
			for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
			{
				auto Material = ResolveMaterialDescription(Instance);
				if (!Material)
					return std::unexpected(std::move(Material.error()));
				if ((*Material)->AlphaMode == MaterialAlphaMode::Blend)
					continue;
				auto MaterialBinding = ResolveMaterialBinding(Instance);
				if (!MaterialBinding)
					return std::unexpected(std::move(MaterialBinding.error()));
				if (auto PipelineResult = EnsureShadowPipeline(
						m_PreparedSnapshot->DirectionalLight->ShadowBias, (*Material)->DoubleSided); !PipelineResult)
					return std::unexpected(std::move(PipelineResult.error()));
				const auto Pipeline = m_ShadowPipelines.find({ RasterBias, (*Material)->DoubleSided });
				if (Pipeline == m_ShadowPipelines.end())
					return std::unexpected(MakeResourceError("Directional alpha-tested shadow pipeline was not prepared"));
				auto Mesh = m_ProjectAssetCaches->Meshes.GetOrLoad(Instance.MeshAsset);
				if (!Mesh)
					return std::unexpected(MakeDrawError("Shadow pass mesh became unavailable: " + Mesh.error().Message));
				const glm::mat4 ShadowTransform = m_PreparedCascades->Cascades[CascadeIndex].ViewProjection * Instance.WorldTransform;
				const auto Update = m_Runtime.WriteBuffer(
					*m_ShadowObjectConstantsBuffer, 0, std::as_bytes(std::span(&ShadowTransform, 1)));
				if (!Update)
					return std::unexpected(MakeDrawError("Could not update shadow object constants: " + Update.error().Message));
				const std::array<const BindingSet*, 2> Bindings = {
					(*MaterialBinding)->BindingSet.get(), m_ShadowObjectBindingSet.get() };
				const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
				const GraphicsResult Draw = m_Runtime.DrawIndexed(*Pipeline->second, Mesh->get(), Arguments, Bindings);
				if (!Draw)
					return std::unexpected(MakeDrawError("Could not draw shadow caster: " + Draw.error().Message));
			}
			const GraphicsResult End = TargetScope.End();
			if (!End)
				return std::unexpected(MakeDrawError("Could not end shadow cascade: " + End.error().Message));
		}
		return RenderSpotlightShadows();
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
			return (*Bindings)->BindingSet.get();
		};

		const AmbientOcclusionPass::RenderInputs Inputs{ *m_PreparedSnapshot, m_PipelineVertexLayout, m_BindingLayout,
			m_ProjectAssetCaches->Meshes, *m_BackgroundTriangleBuffer, ResolveMaterial, ResolveBindings };
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
		m_MaterialBindings.clear();
		m_ProjectAssetCaches->Materials.Clear();
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

	std::expected<void, SceneRendererError> SceneRenderer::EnsureHdrSceneTarget(uint32_t Width, uint32_t Height)
	{
		if (Width == 0 || Height == 0)
			return std::unexpected(MakeResourceError("HDR scene target dimensions must be non-zero"));

		if (m_HdrSceneTarget && MatchesRenderTargetConfiguration(
			m_HdrSceneTarget->GetDescription(),
			Width,
			Height,
			ColorTargetFormat::RGBA16_Float,
			DepthAttachmentMode::Attachment))
		{
			const Texture* Color = m_HdrSceneTarget->GetColorTexture();
			const Texture* Depth = m_HdrSceneTarget->GetDepthTexture();
			if (Color && Depth && Color->GetDescription().Format == TextureFormat::RGBA16_Float &&
				Depth->GetDescription().Format == TextureFormat::Depth32Float)
				return {};
		}

		RenderTargetDesc Description;
		Description.Width = Width;
		Description.Height = Height;
		Description.ColorFormat = ColorTargetFormat::RGBA16_Float;
		Description.DepthMode = DepthAttachmentMode::Attachment;
		Description.DebugName = "PulseForge persistent HDR scene target";
		auto Replacement = m_Runtime.CreateRenderTarget(Description);
		if (!Replacement)
			return std::unexpected(MakeResourceError("Could not create HDR scene target: " + Replacement.error().Message));

		const Texture* Color = (*Replacement)->GetColorTexture();
		const Texture* Depth = (*Replacement)->GetDepthTexture();
		if (!Color || !Depth || Color->GetDescription().Width != Width || Color->GetDescription().Height != Height ||
			Color->GetDescription().Format != TextureFormat::RGBA16_Float ||
			!HasTextureUsage(Color->GetDescription().Usage, TextureUsage::ColorAttachment) ||
			!HasTextureUsage(Color->GetDescription().Usage, TextureUsage::ShaderResource) ||
			Depth->GetDescription().Width != Width || Depth->GetDescription().Height != Height ||
			Depth->GetDescription().Format != TextureFormat::Depth32Float ||
			!HasTextureUsage(Depth->GetDescription().Usage, TextureUsage::DepthStencilAttachment))
		{
			return std::unexpected(MakeResourceError("HDR scene target returned incompatible color or depth attachments"));
		}

		m_ToneMappingStage->InvalidateBindings();
		m_HdrSceneTarget = std::move(*Replacement);
		if (m_HdrSceneTargetGeneration == (std::numeric_limits<uint64_t>::max)())
			m_HdrSceneTargetGeneration = 1;
		else
			++m_HdrSceneTargetGeneration;
		PF_CORE_INFO("Created/resized persistent RGBA16F HDR scene target to {0}x{1}", Width, Height);
		return {};
	}

	std::expected<HdrSceneRenderResult, SceneRendererError> SceneRenderer::RenderPreparedSceneToHdr(
		uint32_t Width,
		uint32_t Height)
	{
		if (!m_PreparedSnapshot)
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::SnapshotBuildFailed,
				{},
				{},
				"HDR scene rendering requires a successfully prepared scene snapshot" });

		if (auto Target = EnsureHdrSceneTarget(Width, Height); !Target)
			return std::unexpected(std::move(Target.error()));

		RenderTargetClearValue ClearValue;
		ClearValue.Color = { 0.0f, 0.0f, 0.0f, 1.0f };
		ClearValue.Depth = 1.0f;
		auto Rendered = RenderPreparedScene(*m_HdrSceneTarget, ClearValue);
		if (!Rendered)
			return std::unexpected(std::move(Rendered.error()));

		const Texture* Color = m_HdrSceneTarget->GetColorTexture();
		if (!Color)
			return std::unexpected(MakeResourceError("HDR scene target color texture is unavailable after rendering"));
		return HdrSceneRenderResult{
			Color,
			m_HdrSceneTarget->GetDepthTexture(),
			Width,
			Height,
			*Rendered,
			m_LastSpotShadowPassCount,
			m_LastSpotShadowCasterDrawCount,
			m_PreparedSnapshot->EnvironmentLight.has_value(),
			m_HdrSceneTargetGeneration,
			m_SpotShadowResourceGeneration,
			m_LastPointShadowPassCount,
			m_LastPointShadowCasterDrawCount,
			m_PointShadowResourceGeneration,
			m_PointShadowSlotOwners
		};
	}

	std::expected<SceneRenderOutputResult, SceneRendererError> SceneRenderer::RenderPreparedSceneToOutput()
	{
		const auto [Width, Height] = m_Runtime.GetOutputFramebufferSize();
		if (Width == 0 || Height == 0)
			return SceneRenderOutputResult{};
		auto Hdr = RenderPreparedSceneToHdr(Width, Height);
		if (!Hdr)
			return std::unexpected(std::move(Hdr.error()));
		if (auto Tonemapped = m_ToneMappingStage->Draw(
				*Hdr->ColorTexture, Hdr->TargetGeneration, ColorTargetFormat::Swapchain, true, *m_BackgroundTriangleBuffer);
			!Tonemapped)
			return std::unexpected(std::move(Tonemapped.error()));
		return SceneRenderOutputResult{
			Hdr->GeometryDrawCount,
			Hdr->TargetGeneration,
			ColorTargetFormat::Swapchain,
			ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, m_Runtime.GetOutputColorEncoding()),
			Hdr->SpotlightShadowPassCount,
			Hdr->SpotlightShadowCasterDrawCount,
			Hdr->SpotlightShadowResourceGeneration,
			m_SpotShadowSlotOwners,
			Hdr->PointShadowPassCount,
			Hdr->PointShadowCasterDrawCount,
			Hdr->PointShadowResourceGeneration,
			Hdr->PointShadowSlotOwners
		};
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
		if (auto Prepared = m_ToneMappingStage->Prepare(
				*Hdr->ColorTexture, Hdr->TargetGeneration, Description.ColorFormat, Description.DepthMode != DepthAttachmentMode::None);
			!Prepared)
			return std::unexpected(std::move(Prepared.error()));

		RenderTargetClearValue Clear;
		Clear.Color = { 0.0f, 0.0f, 0.0f, 1.0f };
		Clear.Depth = 1.0f;
		if (const GraphicsResult Begin = m_Runtime.BeginRenderTarget(Target, Clear); !Begin)
			return std::unexpected(MakeDrawError("Could not begin tone-mapped output target: " + Begin.error().Message));
		RenderTargetFrameScope Scope(m_Runtime);
		auto Draw = m_ToneMappingStage->Draw(*Hdr->ColorTexture, Hdr->TargetGeneration, Description.ColorFormat,
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
		return SceneRenderOutputResult{
			Hdr->GeometryDrawCount,
			Hdr->TargetGeneration,
			Description.ColorFormat,
			ShouldShaderEncodeSrgb(Description.ColorFormat, m_Runtime.GetOutputColorEncoding()),
			Hdr->SpotlightShadowPassCount,
			Hdr->SpotlightShadowCasterDrawCount,
			Hdr->SpotlightShadowResourceGeneration,
			m_SpotShadowSlotOwners,
			Hdr->PointShadowPassCount,
			Hdr->PointShadowCasterDrawCount,
			Hdr->PointShadowResourceGeneration,
			Hdr->PointShadowSlotOwners
		};
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
			if (auto Pipeline = EnsureBackgroundPipeline(ColorFormat); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}
		if (m_PreparedSnapshot && !m_PreparedSnapshot->Meshes.empty())
		{
			if (auto Pipeline = EnsurePipeline(ColorFormat); !Pipeline)
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
			if (auto Pipeline = EnsureBackgroundPipeline(ColorFormat); !Pipeline)
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
		Frame.SpotShadowViewProjection = m_PreparedSpotShadowMatrices;
		Frame.SpotShadowParameters = m_PreparedSpotShadowParameters;
		Frame.PointShadowParameters = m_PreparedPointShadowParameters;
		const auto LocalLightingUpdate = m_Runtime.WriteBuffer(
			*m_LocalLightingConstantsBuffer, 0, std::as_bytes(std::span(&m_PreparedLocalLighting, 1)));
		if (!LocalLightingUpdate)
			return std::unexpected(MakeDrawError("Could not update local-light constants: " + LocalLightingUpdate.error().Message));

		const auto FrameUpdate = m_Runtime.WriteBuffer(*m_FrameConstantsBuffer, 0, std::as_bytes(std::span(&Frame, 1)));
		if (!FrameUpdate)
			return std::unexpected(MakeDrawError("Could not update scene frame constants: " + FrameUpdate.error().Message));

		if (m_PreparedSnapshot->EnvironmentLight)
		{
			const std::string EnvironmentKey = m_PreparedSnapshot->EnvironmentLight->HdrImage.ToString() +
				(m_PreparedEnvironmentReady ? ":ready" : ":loading");
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
		if (m_PreparedSnapshot->Meshes.empty())
			return size_t{ 0 };

		const auto DrawMaterialInstance = [this, ColorFormat, &SubmittedDraws](
			const SceneMeshInstance& Instance,
			const MaterialAssetDesc& Material) -> std::expected<void, SceneRendererError>
		{
			const bool AlphaBlend = Material.AlphaMode == MaterialAlphaMode::Blend;
			if (auto PipelineResult = EnsurePipeline(ColorFormat, AlphaBlend, Material.DoubleSided); !PipelineResult)
				return std::unexpected(std::move(PipelineResult.error()));
			const auto Pipeline = m_Pipelines.find(std::tuple{ ColorFormat, AlphaBlend, Material.DoubleSided });
			if (Pipeline == m_Pipelines.end())
				return std::unexpected(MakeResourceError("Prepared material graphics pipeline is unavailable"));
			auto MaterialBinding = ResolveMaterialBinding(Instance);
			if (!MaterialBinding)
				return std::unexpected(std::move(MaterialBinding.error()));
			auto Mesh = m_ProjectAssetCaches->Meshes.GetOrLoad(Instance.MeshAsset);
			if (!Mesh)
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Instance.Entity,
					Instance.MeshAsset,
					"Prepared mesh resource is unavailable"
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
			const BindingSet* AmbientOcclusionBindings = m_AmbientOcclusionPass->GetSceneBindingSet();
			const std::array<const BindingSet*, 5> BindingSets = {
				(*MaterialBinding)->BindingSet.get(), m_ShadowBindingSet.get(), AmbientOcclusionBindings,
				m_SpotShadowBindingSet.get(), m_PointShadowBindingSet.get() };
			const GraphicsResult Draw = m_Runtime.DrawIndexed(*Pipeline->second, Mesh->get(), Arguments, BindingSets);
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
}
