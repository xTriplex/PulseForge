#include "Core/PulseForgePCH.h"
#include "Renderer/SceneRenderer.h"

#include "Assets/MaterialAssetCache.h"
#include "Assets/MeshAssetCache.h"
#include "Assets/Project.h"
#include "Assets/TextureAssetCache.h"
#include "Core/Application.h"
#include "Core/Log.h"
#include "Core/ScopedProfileTimer.h"
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
		m_AmbientOcclusionPrepassVertexShader = std::move(*AoPrepassVs);
		m_AmbientOcclusionPrepassFragmentShader = std::move(*AoPrepassPs);
		m_AmbientOcclusionVertexShader = std::move(*AoVs);
		m_AmbientOcclusionFragmentShader = std::move(*AoPs);
		m_AmbientOcclusionBlurVertexShader = std::move(*AoBlurVs);
		m_AmbientOcclusionBlurFragmentShader = std::move(*AoBlurPs);

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
		SamplerDesc AmbientOcclusionPointDescription;
		AmbientOcclusionPointDescription.Minification = SamplerFilter::Nearest;
		AmbientOcclusionPointDescription.Magnification = SamplerFilter::Nearest;
		AmbientOcclusionPointDescription.AddressU = SamplerAddressMode::ClampToEdge;
		AmbientOcclusionPointDescription.AddressV = SamplerAddressMode::ClampToEdge;
		AmbientOcclusionPointDescription.DebugName = "PulseForge SSAO point clamp sampler";
		auto AmbientOcclusionPointSampler = m_Runtime.CreateSampler(AmbientOcclusionPointDescription);
		if (!AmbientOcclusionPointSampler)
			return std::unexpected(MakeResourceError("Could not create SSAO point sampler: " + AmbientOcclusionPointSampler.error().Message));
		m_AmbientOcclusionPointSampler = std::move(*AmbientOcclusionPointSampler);
		SamplerDesc AmbientOcclusionLinearDescription = AmbientOcclusionPointDescription;
		AmbientOcclusionLinearDescription.Minification = SamplerFilter::Linear;
		AmbientOcclusionLinearDescription.Magnification = SamplerFilter::Linear;
		AmbientOcclusionLinearDescription.DebugName = "PulseForge SSAO linear clamp sampler";
		auto AmbientOcclusionLinearSampler = m_Runtime.CreateSampler(AmbientOcclusionLinearDescription);
		if (!AmbientOcclusionLinearSampler)
			return std::unexpected(MakeResourceError("Could not create SSAO linear sampler: " + AmbientOcclusionLinearSampler.error().Message));
		m_AmbientOcclusionLinearSampler = std::move(*AmbientOcclusionLinearSampler);

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
		TextureDesc AmbientOcclusionFallbackDescription;
		AmbientOcclusionFallbackDescription.Width = 1;
		AmbientOcclusionFallbackDescription.Height = 1;
		AmbientOcclusionFallbackDescription.Format = TextureFormat::RGBA8_UNorm;
		AmbientOcclusionFallbackDescription.DebugName = "PulseForge unoccluded SSAO fallback";
		auto AmbientOcclusionFallback = m_Runtime.CreateTexture(
			AmbientOcclusionFallbackDescription, std::as_bytes(std::span<const uint8_t>(WhitePixel)));
		if (!AmbientOcclusionFallback)
			return std::unexpected(MakeResourceError("Could not create the unoccluded SSAO fallback: " + AmbientOcclusionFallback.error().Message));
		m_FallbackAmbientOcclusionTexture = std::move(*AmbientOcclusionFallback);
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

		auto ToneMappingVertexBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Tonemapping.vs.spv");
		if (!ToneMappingVertexBytecode)
			return std::unexpected(std::move(ToneMappingVertexBytecode.error()));
		auto ToneMappingFragmentBytecode = ReadShaderBytecode(CompiledShaderDirectory / "Tonemapping.ps.spv");
		if (!ToneMappingFragmentBytecode)
			return std::unexpected(std::move(ToneMappingFragmentBytecode.error()));
		ShaderDesc ToneMappingVertexDescription;
		ToneMappingVertexDescription.Stage = ShaderStage::Vertex;
		ToneMappingVertexDescription.EntryPoint = "VSMain";
		ToneMappingVertexDescription.DebugName = "PulseForge tone mapping vertex shader";
		auto ToneMappingVertex = m_Runtime.CreateShader(ToneMappingVertexDescription, *ToneMappingVertexBytecode);
		if (!ToneMappingVertex)
			return std::unexpected(MakeResourceError("Could not create tone mapping vertex shader: " + ToneMappingVertex.error().Message));
		m_ToneMappingVertexShader = std::move(*ToneMappingVertex);
		ShaderDesc ToneMappingFragmentDescription;
		ToneMappingFragmentDescription.Stage = ShaderStage::Fragment;
		ToneMappingFragmentDescription.EntryPoint = "PSMain";
		ToneMappingFragmentDescription.DebugName = "PulseForge tone mapping fragment shader";
		auto ToneMappingFragment = m_Runtime.CreateShader(ToneMappingFragmentDescription, *ToneMappingFragmentBytecode);
		if (!ToneMappingFragment)
			return std::unexpected(MakeResourceError("Could not create tone mapping fragment shader: " + ToneMappingFragment.error().Message));
		m_ToneMappingFragmentShader = std::move(*ToneMappingFragment);

		BindingLayoutDesc ToneMappingLayoutDescription;
		ToneMappingLayoutDescription.Visibility = ShaderVisibility::Fragment;
		ToneMappingLayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 },
			{ BindingResourceType::Sampler, 0 },
			{ BindingResourceType::ConstantBuffer, 0 }
		};
		ToneMappingLayoutDescription.DebugName = "PulseForge tone mapping input and parameters";
		auto ToneMappingLayout = m_Runtime.CreateBindingLayout(ToneMappingLayoutDescription);
		if (!ToneMappingLayout)
			return std::unexpected(MakeResourceError("Could not create tone mapping binding layout: " + ToneMappingLayout.error().Message));
		m_ToneMappingBindingLayout = std::move(*ToneMappingLayout);

		SamplerDesc ToneMappingSamplerDescription;
		ToneMappingSamplerDescription.Minification = SamplerFilter::Linear;
		ToneMappingSamplerDescription.Magnification = SamplerFilter::Linear;
		ToneMappingSamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
		ToneMappingSamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
		ToneMappingSamplerDescription.DebugName = "PulseForge tone mapping HDR sampler";
		auto ToneMappingSampler = m_Runtime.CreateSampler(ToneMappingSamplerDescription);
		if (!ToneMappingSampler)
			return std::unexpected(MakeResourceError("Could not create tone mapping sampler: " + ToneMappingSampler.error().Message));
		m_ToneMappingSampler = std::move(*ToneMappingSampler);

		BufferDesc ToneMappingConstantsDescription;
		ToneMappingConstantsDescription.ByteSize = sizeof(ToneMappingConstants);
		ToneMappingConstantsDescription.Usage = BufferUsage::Constant;
		ToneMappingConstantsDescription.DebugName = "PulseForge tone mapping constants";
		auto ToneMappingConstantsBuffer = m_Runtime.CreateBuffer(
			ToneMappingConstantsDescription, std::as_bytes(std::span(&m_UploadedToneMappingConstants, 1)));
		if (!ToneMappingConstantsBuffer)
			return std::unexpected(MakeResourceError("Could not create tone mapping constants: " + ToneMappingConstantsBuffer.error().Message));
		m_ToneMappingConstantsBuffer = std::move(*ToneMappingConstantsBuffer);

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

		BindingLayoutDesc AmbientOcclusionFinalDescription;
		AmbientOcclusionFinalDescription.Visibility = ShaderVisibility::Fragment;
		AmbientOcclusionFinalDescription.ShaderRegisterSpace = 2;
		AmbientOcclusionFinalDescription.Items = {
			{ BindingResourceType::Texture2D, 0 }, { BindingResourceType::Sampler, 0 }
		};
		AmbientOcclusionFinalDescription.DebugName = "PulseForge SSAO final result (space2)";
		auto AmbientOcclusionFinalLayout = m_Runtime.CreateBindingLayout(AmbientOcclusionFinalDescription);
		if (!AmbientOcclusionFinalLayout)
			return std::unexpected(MakeResourceError("Could not create SSAO final layout: " + AmbientOcclusionFinalLayout.error().Message));
		m_AmbientOcclusionFinalBindingLayout = std::move(*AmbientOcclusionFinalLayout);

		BindingLayoutDesc AmbientOcclusionEvaluationDescription;
		AmbientOcclusionEvaluationDescription.Visibility = ShaderVisibility::Fragment;
		AmbientOcclusionEvaluationDescription.ShaderRegisterSpace = 4;
		AmbientOcclusionEvaluationDescription.Items = {
			{ BindingResourceType::Texture2D, 0 }, { BindingResourceType::Texture2D, 1 },
			{ BindingResourceType::Sampler, 0 }, { BindingResourceType::ConstantBuffer, 0 }
		};
		AmbientOcclusionEvaluationDescription.DebugName = "PulseForge SSAO evaluation resources (space4)";
		auto AmbientOcclusionEvaluationLayout = m_Runtime.CreateBindingLayout(AmbientOcclusionEvaluationDescription);
		if (!AmbientOcclusionEvaluationLayout)
			return std::unexpected(MakeResourceError("Could not create SSAO evaluation layout: " + AmbientOcclusionEvaluationLayout.error().Message));
		m_AmbientOcclusionEvaluationBindingLayout = std::move(*AmbientOcclusionEvaluationLayout);

		BindingLayoutDesc AmbientOcclusionBlurDescription;
		AmbientOcclusionBlurDescription.Visibility = ShaderVisibility::Fragment;
		AmbientOcclusionBlurDescription.ShaderRegisterSpace = 4;
		AmbientOcclusionBlurDescription.Items = {
			{ BindingResourceType::Texture2D, 0 }, { BindingResourceType::Texture2D, 1 },
			{ BindingResourceType::Texture2D, 2 }, { BindingResourceType::Sampler, 0 },
			{ BindingResourceType::Sampler, 1 }, { BindingResourceType::ConstantBuffer, 0 }
		};
		AmbientOcclusionBlurDescription.DebugName = "PulseForge SSAO bilateral blur resources (space4)";
		auto AmbientOcclusionBlurLayout = m_Runtime.CreateBindingLayout(AmbientOcclusionBlurDescription);
		if (!AmbientOcclusionBlurLayout)
			return std::unexpected(MakeResourceError("Could not create SSAO blur layout: " + AmbientOcclusionBlurLayout.error().Message));
		m_AmbientOcclusionBlurBindingLayout = std::move(*AmbientOcclusionBlurLayout);

		BindingLayoutDesc AmbientOcclusionPrepassDescription;
		AmbientOcclusionPrepassDescription.Visibility = ShaderVisibility::Vertex;
		AmbientOcclusionPrepassDescription.ShaderRegisterSpace = 4;
		AmbientOcclusionPrepassDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		AmbientOcclusionPrepassDescription.DebugName = "PulseForge SSAO prepass constants (space4)";
		auto AmbientOcclusionPrepassLayout = m_Runtime.CreateBindingLayout(AmbientOcclusionPrepassDescription);
		if (!AmbientOcclusionPrepassLayout)
			return std::unexpected(MakeResourceError("Could not create SSAO prepass layout: " + AmbientOcclusionPrepassLayout.error().Message));
		m_AmbientOcclusionPrepassBindingLayout = std::move(*AmbientOcclusionPrepassLayout);

		BindingSetDesc AmbientOcclusionFinalSetDescription;
		AmbientOcclusionFinalSetDescription.Layout = m_AmbientOcclusionFinalBindingLayout;
		AmbientOcclusionFinalSetDescription.Textures.push_back({ 0, std::cref(*m_FallbackAmbientOcclusionTexture) });
		AmbientOcclusionFinalSetDescription.Samplers.push_back({ 0, std::cref(*m_AmbientOcclusionLinearSampler) });
		auto AmbientOcclusionFinalSet = m_Runtime.CreateBindingSet(AmbientOcclusionFinalSetDescription);
		if (!AmbientOcclusionFinalSet)
			return std::unexpected(MakeResourceError("Could not create fallback SSAO bindings: " + AmbientOcclusionFinalSet.error().Message));
		m_AmbientOcclusionFallbackBindingSet = std::move(*AmbientOcclusionFinalSet);
		m_AmbientOcclusionFinalBindingSet = nullptr;

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

		BindingLayoutDesc ShadowObjectLayoutDescription;
		ShadowObjectLayoutDescription.Visibility = ShaderVisibility::Vertex;
		ShadowObjectLayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		ShadowObjectLayoutDescription.DebugName = "PulseForge directional shadow object constants";
		auto ShadowObjectLayout = m_Runtime.CreateBindingLayout(ShadowObjectLayoutDescription);
		if (!ShadowObjectLayout)
			return std::unexpected(MakeResourceError("Could not create shadow object layout: " + ShadowObjectLayout.error().Message));
		m_ShadowObjectBindingLayout = std::move(*ShadowObjectLayout);

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

		AmbientOcclusionConstants InitialAmbientOcclusionConstants;
		InitialAmbientOcclusionConstants.Parameters = glm::vec4(
			m_AmbientOcclusionSettings.Radius,
			m_AmbientOcclusionSettings.Bias,
			m_AmbientOcclusionSettings.Strength,
			0.0f);
		m_AmbientOcclusionKernel = GenerateAmbientOcclusionKernel();
		InitialAmbientOcclusionConstants.Kernel = m_AmbientOcclusionKernel;
		BufferDesc AmbientOcclusionParametersDescription;
		AmbientOcclusionParametersDescription.ByteSize = sizeof(InitialAmbientOcclusionConstants);
		AmbientOcclusionParametersDescription.Usage = BufferUsage::Constant;
		AmbientOcclusionParametersDescription.DebugName = "PulseForge SSAO kernel and parameters";
		auto AmbientOcclusionParameters = m_Runtime.CreateBuffer(
			AmbientOcclusionParametersDescription,
			std::as_bytes(std::span(&InitialAmbientOcclusionConstants, 1)));
		if (!AmbientOcclusionParameters)
			return std::unexpected(MakeResourceError("Could not create SSAO parameters: " + AmbientOcclusionParameters.error().Message));
		m_AmbientOcclusionParametersBuffer = std::move(*AmbientOcclusionParameters);

		const AmbientOcclusionBlurConstants InitialBlurConstants{};
		BufferDesc AmbientOcclusionBlurParametersDescription;
		AmbientOcclusionBlurParametersDescription.ByteSize = sizeof(InitialBlurConstants);
		AmbientOcclusionBlurParametersDescription.Usage = BufferUsage::Constant;
		AmbientOcclusionBlurParametersDescription.DebugName = "PulseForge SSAO bilateral blur parameters";
		auto AmbientOcclusionBlurParameters = m_Runtime.CreateBuffer(
			AmbientOcclusionBlurParametersDescription,
			std::as_bytes(std::span(&InitialBlurConstants, 1)));
		if (!AmbientOcclusionBlurParameters)
			return std::unexpected(MakeResourceError("Could not create SSAO blur parameters: " + AmbientOcclusionBlurParameters.error().Message));
		m_AmbientOcclusionBlurParametersBuffer = std::move(*AmbientOcclusionBlurParameters);

		const AmbientOcclusionObjectConstants InitialAmbientOcclusionObject{};
		BufferDesc AmbientOcclusionObjectDescription;
		AmbientOcclusionObjectDescription.ByteSize = sizeof(InitialAmbientOcclusionObject);
		AmbientOcclusionObjectDescription.Usage = BufferUsage::Constant;
		AmbientOcclusionObjectDescription.DebugName = "PulseForge SSAO prepass object constants";
		auto AmbientOcclusionObjectBuffer = m_Runtime.CreateBuffer(
			AmbientOcclusionObjectDescription,
			std::as_bytes(std::span(&InitialAmbientOcclusionObject, 1)));
		if (!AmbientOcclusionObjectBuffer)
			return std::unexpected(MakeResourceError("Could not create SSAO prepass constants: " + AmbientOcclusionObjectBuffer.error().Message));
		m_AmbientOcclusionObjectConstantsBuffer = std::move(*AmbientOcclusionObjectBuffer);
		BindingSetDesc AmbientOcclusionPrepassSetDescription;
		AmbientOcclusionPrepassSetDescription.Layout = m_AmbientOcclusionPrepassBindingLayout;
		AmbientOcclusionPrepassSetDescription.Buffers.push_back({ 0, std::cref(*m_AmbientOcclusionObjectConstantsBuffer) });
		auto AmbientOcclusionPrepassSet = m_Runtime.CreateBindingSet(AmbientOcclusionPrepassSetDescription);
		if (!AmbientOcclusionPrepassSet)
			return std::unexpected(MakeResourceError("Could not create SSAO prepass object bindings: " + AmbientOcclusionPrepassSet.error().Message));
		m_AmbientOcclusionPrepassBindingSet = std::move(*AmbientOcclusionPrepassSet);

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
		if (auto Pipelines = EnsureAmbientOcclusionPipelines(); !Pipelines)
			return std::unexpected(std::move(Pipelines.error()));
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
		const EnvironmentLightingTextures& EnvironmentTextures,
		bool IsReady)
	{
		const std::string Key = EnvironmentAsset.ToString() + (IsReady ? ":ready" : ":loading");
		if (m_EnvironmentBindingSets.contains(Key))
			return {};
		Detail::ScopedProfileTimer Timer("Environment GPU binding creation");
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
			auto LoadedEnvironment = m_EnvironmentLightingCache->GetOrLoad(*EnvironmentAsset);
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
			if (auto AmbientOcclusionPipelines = EnsureAmbientOcclusionPipelines(); !AmbientOcclusionPipelines)
				return std::unexpected(std::move(AmbientOcclusionPipelines.error()));
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

	std::expected<void, SceneRendererError> SceneRenderer::EnsurePipeline(ColorTargetFormat ColorFormat)
	{
		if (m_Pipelines.contains(ColorFormat))
			return {};
		Detail::ScopedProfileTimer Timer("Scene graphics pipeline creation");
		if (!m_PipelineVertexLayout)
			return std::unexpected(MakeResourceError("Cannot create a scene pipeline before a mesh vertex layout is prepared"));

		GraphicsPipelineDesc PipelineDescription;
		PipelineDescription.VertexShader = m_VertexShader;
		PipelineDescription.FragmentShader = m_FragmentShader;
		PipelineDescription.BindingLayouts = {
			m_BindingLayout, m_ShadowBindingLayout, m_AmbientOcclusionFinalBindingLayout };
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

	std::expected<void, SceneRendererError> SceneRenderer::EnsureShadowPipeline(float DepthBias)
	{
		const int32_t RasterBias = static_cast<int32_t>(std::lround(DepthBias));
		if (m_ShadowPipelines.contains(RasterBias))
			return {};
		if (!m_PipelineVertexLayout)
			return std::unexpected(MakeResourceError("Cannot create a shadow pipeline before a mesh layout is prepared"));
		GraphicsPipelineDesc Description;
		Description.VertexShader = m_ShadowVertexShader;
		Description.BindingLayouts = { m_ShadowObjectBindingLayout };
		Description.VertexLayout.Stride = m_PipelineVertexLayout->Stride;
		const auto PositionAttribute = std::find_if(m_PipelineVertexLayout->Attributes.begin(),
			m_PipelineVertexLayout->Attributes.end(), [](const VertexAttributeDesc& Attribute)
			{
				return Attribute.Semantic == VertexSemantic::Position;
			});
		if (PositionAttribute == m_PipelineVertexLayout->Attributes.end())
			return std::unexpected(MakeResourceError("The mesh layout has no position attribute for shadow rendering"));
		Description.VertexLayout.Attributes.push_back(*PositionAttribute);
		Description.ColorFormat = ColorTargetFormat::None;
		Description.Rasterizer.Cull = CullMode::Back;
		Description.Rasterizer.DepthBias = static_cast<float>(RasterBias);
		Description.Rasterizer.SlopeScaledDepthBias = 1.5f;
		Description.Depth.TestEnabled = true;
		Description.Depth.WriteEnabled = true;
		Description.Depth.Compare = DepthCompareOperation::Less;
		Description.DebugName = "PulseForge directional shadow depth-only pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create directional shadow pipeline: " + Pipeline.error().Message));
		m_ShadowPipelines.emplace(RasterBias, std::move(*Pipeline));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::RenderShadowCascades()
	{
		if (!m_PreparedSnapshot || !m_PreparedSnapshot->DirectionalLight ||
			!m_PreparedSnapshot->DirectionalLight->CastShadows || m_PreparedSnapshot->Meshes.empty())
			return {};
		if (!m_PreparedCascades)
			return std::unexpected(MakeDrawError("Prepared directional shadow cascades are unavailable"));
		const int32_t RasterBias = static_cast<int32_t>(std::lround(m_PreparedSnapshot->DirectionalLight->ShadowBias));
		const auto Pipeline = m_ShadowPipelines.find(RasterBias);
		if (Pipeline == m_ShadowPipelines.end())
			return std::unexpected(MakeResourceError("Directional shadow pipeline was not prepared for the authored raster bias"));

		for (size_t CascadeIndex = 0; CascadeIndex < m_PreparedCascades->Cascades.size(); ++CascadeIndex)
		{
			const GraphicsResult Begin = m_Runtime.BeginRenderTarget(*m_ShadowTargets[CascadeIndex]);
			if (!Begin)
				return std::unexpected(MakeDrawError("Could not begin shadow cascade: " + Begin.error().Message));
			RenderTargetFrameScope TargetScope(m_Runtime);
			for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
			{
				auto Mesh = m_MeshAssetCache->GetOrLoad(Instance.MeshAsset);
				if (!Mesh)
					return std::unexpected(MakeDrawError("Shadow pass mesh became unavailable: " + Mesh.error().Message));
				const glm::mat4 ShadowTransform = m_PreparedCascades->Cascades[CascadeIndex].ViewProjection * Instance.WorldTransform;
				const auto Update = m_Runtime.WriteBuffer(
					*m_ShadowObjectConstantsBuffer, 0, std::as_bytes(std::span(&ShadowTransform, 1)));
				if (!Update)
					return std::unexpected(MakeDrawError("Could not update shadow object constants: " + Update.error().Message));
				const std::array<const BindingSet*, 1> Bindings = { m_ShadowObjectBindingSet.get() };
				const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
				const GraphicsResult Draw = m_Runtime.DrawIndexed(*Pipeline->second, Mesh->get(), Arguments, Bindings);
				if (!Draw)
					return std::unexpected(MakeDrawError("Could not draw shadow caster: " + Draw.error().Message));
			}
			const GraphicsResult End = TargetScope.End();
			if (!End)
				return std::unexpected(MakeDrawError("Could not end shadow cascade: " + End.error().Message));
		}
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureAmbientOcclusionPipelines()
	{
		if (!m_AmbientOcclusionPipeline)
		{
			GraphicsPipelineDesc Description;
			Description.VertexShader = m_AmbientOcclusionVertexShader;
			Description.FragmentShader = m_AmbientOcclusionFragmentShader;
			Description.BindingLayouts = { m_AmbientOcclusionEvaluationBindingLayout };
			Description.VertexLayout.Stride = sizeof(float) * 2;
			Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.DepthAttachmentEnabled = false;
			Description.DebugName = "PulseForge half-resolution SSAO evaluation pipeline";
			auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
			if (!Pipeline)
				return std::unexpected(MakeResourceError("Could not create SSAO evaluation pipeline: " + Pipeline.error().Message));
			m_AmbientOcclusionPipeline = std::move(*Pipeline);
		}

		if (!m_AmbientOcclusionBlurPipeline)
		{
			GraphicsPipelineDesc Description;
			Description.VertexShader = m_AmbientOcclusionBlurVertexShader;
			Description.FragmentShader = m_AmbientOcclusionBlurFragmentShader;
			Description.BindingLayouts = { m_AmbientOcclusionBlurBindingLayout };
			Description.VertexLayout.Stride = sizeof(float) * 2;
			Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.DepthAttachmentEnabled = false;
			Description.DebugName = "PulseForge bilateral SSAO blur pipeline";
			auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
			if (!Pipeline)
				return std::unexpected(MakeResourceError("Could not create SSAO blur pipeline: " + Pipeline.error().Message));
			m_AmbientOcclusionBlurPipeline = std::move(*Pipeline);
		}

		if (!m_AmbientOcclusionPrepassPipeline && m_PipelineVertexLayout)
		{
			GraphicsPipelineDesc Description;
			Description.VertexShader = m_AmbientOcclusionPrepassVertexShader;
			Description.FragmentShader = m_AmbientOcclusionPrepassFragmentShader;
			Description.BindingLayouts = { m_AmbientOcclusionPrepassBindingLayout };
			Description.VertexLayout.Stride = m_PipelineVertexLayout->Stride;
			for (const VertexSemantic Semantic : { VertexSemantic::Position, VertexSemantic::Normal })
			{
				const auto Attribute = std::find_if(m_PipelineVertexLayout->Attributes.begin(), m_PipelineVertexLayout->Attributes.end(),
					[Semantic](const VertexAttributeDesc& Candidate) { return Candidate.Semantic == Semantic; });
				if (Attribute == m_PipelineVertexLayout->Attributes.end())
					return std::unexpected(MakeResourceError("Mesh layout lacks position or normal attributes required by the SSAO prepass"));
				Description.VertexLayout.Attributes.push_back(*Attribute);
			}
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.Depth.TestEnabled = true;
			Description.Depth.WriteEnabled = true;
			Description.Depth.Compare = DepthCompareOperation::Less;
			Description.Rasterizer.Cull = CullMode::Back;
			Description.DebugName = "PulseForge camera normal-depth prepass pipeline";
			auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
			if (!Pipeline)
				return std::unexpected(MakeResourceError("Could not create SSAO normal-depth pipeline: " + Pipeline.error().Message));
			m_AmbientOcclusionPrepassPipeline = std::move(*Pipeline);
		}
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureAmbientOcclusionResources(uint32_t Width, uint32_t Height)
	{
		const auto [HalfWidth, HalfHeight] = CalculateAmbientOcclusionExtent(Width, Height);
		if (m_AmbientOcclusionPrepassTarget && m_AmbientOcclusionWidth == Width && m_AmbientOcclusionHeight == Height)
			return {};
		Detail::ScopedProfileTimer Timer("SSAO first/resize resource creation");

		const auto CreateTarget = [&](uint32_t TargetWidth, uint32_t TargetHeight, DepthAttachmentMode DepthMode, const char* Name)
			-> std::expected<RenderTargetHandle, SceneRendererError>
		{
			RenderTargetDesc Description;
			Description.Width = TargetWidth;
			Description.Height = TargetHeight;
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.DepthMode = DepthMode;
			Description.DebugName = Name;
			auto Target = m_Runtime.CreateRenderTarget(Description);
			if (!Target)
				return std::unexpected(MakeResourceError(std::string("Could not create ") + Name + ": " + Target.error().Message));
			return std::move(*Target);
		};
		auto Prepass = CreateTarget(Width, Height, DepthAttachmentMode::ShaderReadableAttachment,
			"PulseForge SSAO camera normal-depth target");
		auto Raw = CreateTarget(HalfWidth, HalfHeight, DepthAttachmentMode::None, "PulseForge raw half-resolution SSAO target");
		auto BlurHorizontal = CreateTarget(HalfWidth, HalfHeight, DepthAttachmentMode::None,
			"PulseForge horizontal SSAO blur target");
		auto BlurVertical = CreateTarget(HalfWidth, HalfHeight, DepthAttachmentMode::None,
			"PulseForge vertical SSAO blur target");
		if (!Prepass || !Raw || !BlurHorizontal || !BlurVertical)
			return std::unexpected(MakeResourceError("Could not allocate the persistent SSAO target set"));
		const Texture* PrepassNormal = (*Prepass)->GetColorTexture();
		const Texture* PrepassDepth = (*Prepass)->GetDepthTexture();
		const Texture* RawTexture = (*Raw)->GetColorTexture();
		const Texture* HorizontalTexture = (*BlurHorizontal)->GetColorTexture();
		const Texture* VerticalTexture = (*BlurVertical)->GetColorTexture();
		if (!PrepassNormal || !PrepassDepth || !RawTexture || !HorizontalTexture || !VerticalTexture)
			return std::unexpected(MakeResourceError("SSAO target allocation returned a missing required attachment"));

		BindingSetDesc EvaluationSetDescription;
		EvaluationSetDescription.Layout = m_AmbientOcclusionEvaluationBindingLayout;
		EvaluationSetDescription.Textures = { { 0, std::cref(*PrepassDepth) }, { 1, std::cref(*PrepassNormal) } };
		EvaluationSetDescription.Samplers = { { 0, std::cref(*m_AmbientOcclusionPointSampler) } };
		EvaluationSetDescription.Buffers = { { 0, std::cref(*m_AmbientOcclusionParametersBuffer) } };
		auto EvaluationSet = m_Runtime.CreateBindingSet(EvaluationSetDescription);
		if (!EvaluationSet)
			return std::unexpected(MakeResourceError("Could not create SSAO evaluation bindings: " + EvaluationSet.error().Message));

		const auto CreateBlurSet = [&](const Texture& AoTexture) -> std::expected<BindingSetHandle, SceneRendererError>
		{
			BindingSetDesc Description;
			Description.Layout = m_AmbientOcclusionBlurBindingLayout;
			Description.Textures = { { 0, std::cref(AoTexture) }, { 1, std::cref(*PrepassNormal) }, { 2, std::cref(*PrepassDepth) } };
			Description.Samplers = { { 0, std::cref(*m_AmbientOcclusionLinearSampler) },
				{ 1, std::cref(*m_AmbientOcclusionPointSampler) } };
			Description.Buffers = { { 0, std::cref(*m_AmbientOcclusionBlurParametersBuffer) } };
			auto Set = m_Runtime.CreateBindingSet(Description);
			if (!Set)
				return std::unexpected(MakeResourceError("Could not create SSAO bilateral bindings: " + Set.error().Message));
			return std::move(*Set);
		};
		auto HorizontalSet = CreateBlurSet(*RawTexture);
		auto VerticalSet = CreateBlurSet(*HorizontalTexture);
		if (!HorizontalSet || !VerticalSet)
			return std::unexpected(MakeResourceError("Could not create the SSAO blur binding sets"));
		BindingSetDesc FinalSetDescription;
		FinalSetDescription.Layout = m_AmbientOcclusionFinalBindingLayout;
		FinalSetDescription.Textures.push_back({ 0, std::cref(*VerticalTexture) });
		FinalSetDescription.Samplers.push_back({ 0, std::cref(*m_AmbientOcclusionLinearSampler) });
		auto FinalSet = m_Runtime.CreateBindingSet(FinalSetDescription);
		if (!FinalSet)
			return std::unexpected(MakeResourceError("Could not create final SSAO bindings: " + FinalSet.error().Message));

		m_AmbientOcclusionPrepassTarget = std::move(*Prepass);
		m_AmbientOcclusionRawTarget = std::move(*Raw);
		m_AmbientOcclusionBlurTargets[0] = std::move(*BlurHorizontal);
		m_AmbientOcclusionBlurTargets[1] = std::move(*BlurVertical);
		m_AmbientOcclusionEvaluationBindingSet = std::move(*EvaluationSet);
		m_AmbientOcclusionBlurBindingSets[0] = std::move(*HorizontalSet);
		m_AmbientOcclusionBlurBindingSets[1] = std::move(*VerticalSet);
		m_AmbientOcclusionFinalBindingSet = std::move(*FinalSet);
		m_AmbientOcclusionWidth = Width;
		m_AmbientOcclusionHeight = Height;
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::RenderAmbientOcclusion(uint32_t Width, uint32_t Height)
	{
		m_AmbientOcclusionFrameAvailable = false;
		if (!m_AmbientOcclusionSettings.Enabled || !m_PreparedSnapshot || m_PreparedSnapshot->Meshes.empty() ||
			!m_PreparedSnapshot->HasCameraFrustum || Width == 0 || Height == 0)
		{
			return {};
		}
		if (auto Resources = EnsureAmbientOcclusionResources(Width, Height); !Resources)
			return std::unexpected(std::move(Resources.error()));
		if (auto Pipelines = EnsureAmbientOcclusionPipelines(); !Pipelines)
			return std::unexpected(std::move(Pipelines.error()));
		if (!m_AmbientOcclusionPrepassPipeline)
			return std::unexpected(MakeResourceError("SSAO normal-depth pipeline is not available for the prepared mesh layout"));

		const glm::mat4 InverseProjection = glm::inverse(m_PreparedSnapshot->Projection);
		if (!IsFinite(InverseProjection))
			return std::unexpected(MakeDrawError("SSAO requires an invertible camera projection"));
		const auto [HalfWidth, HalfHeight] = CalculateAmbientOcclusionExtent(Width, Height);
		AmbientOcclusionConstants Parameters;
		Parameters.InverseProjection = InverseProjection;
		Parameters.Projection = m_PreparedSnapshot->Projection;
		Parameters.OutputSize = glm::vec4(static_cast<float>(Width), static_cast<float>(Height),
			1.0f / static_cast<float>(Width), 1.0f / static_cast<float>(Height));
		Parameters.HalfSize = glm::vec4(static_cast<float>(HalfWidth), static_cast<float>(HalfHeight),
			1.0f / static_cast<float>(HalfWidth), 1.0f / static_cast<float>(HalfHeight));
		Parameters.Parameters = glm::vec4(m_AmbientOcclusionSettings.Radius, m_AmbientOcclusionSettings.Bias,
			m_AmbientOcclusionSettings.Strength, 0.0f);
		Parameters.Kernel = m_AmbientOcclusionKernel;
		const auto ParameterUpdate = m_Runtime.WriteBuffer(*m_AmbientOcclusionParametersBuffer, 0,
			std::as_bytes(std::span(&Parameters, 1)));
		if (!ParameterUpdate)
			return std::unexpected(MakeDrawError("Could not update SSAO parameters: " + ParameterUpdate.error().Message));

		RenderTargetClearValue PrepassClear;
		PrepassClear.Color = { 0.5f, 0.5f, 1.0f, 1.0f };
		if (const GraphicsResult Begin = m_Runtime.BeginRenderTarget(*m_AmbientOcclusionPrepassTarget, PrepassClear); !Begin)
			return std::unexpected(MakeDrawError("Could not begin SSAO normal-depth prepass: " + Begin.error().Message));
		RenderTargetFrameScope PrepassScope(m_Runtime);
		for (const SceneMeshInstance& Instance : m_PreparedSnapshot->Meshes)
		{
			auto Mesh = m_MeshAssetCache->GetOrLoad(Instance.MeshAsset);
			if (!Mesh)
				return std::unexpected(MakeDrawError("SSAO prepass mesh became unavailable: " + Mesh.error().Message));
			const glm::mat4 ViewModel = m_PreparedSnapshot->View * Instance.WorldTransform;
			const glm::mat4 ViewNormalTransform = glm::transpose(glm::inverse(ViewModel));
			if (!IsFinite(ViewNormalTransform))
				return std::unexpected(MakeDrawError("SSAO prepass cannot transform normals for a singular mesh transform"));
			const AmbientOcclusionObjectConstants Object{
				m_PreparedSnapshot->ViewProjection * Instance.WorldTransform,
				ViewNormalTransform };
			const auto Update = m_Runtime.WriteBuffer(*m_AmbientOcclusionObjectConstantsBuffer, 0,
				std::as_bytes(std::span(&Object, 1)));
			if (!Update)
				return std::unexpected(MakeDrawError("Could not update SSAO object constants: " + Update.error().Message));
			const std::array<const BindingSet*, 1> Bindings = { m_AmbientOcclusionPrepassBindingSet.get() };
			const GraphicsResult Draw = m_Runtime.DrawIndexed(*m_AmbientOcclusionPrepassPipeline, Mesh->get(),
				{ Mesh->get().GetIndexCount(), 1, 0, 0 }, Bindings);
			if (!Draw)
				return std::unexpected(MakeDrawError("Could not draw SSAO normal-depth prepass: " + Draw.error().Message));
		}
		if (const GraphicsResult End = PrepassScope.End(); !End)
			return std::unexpected(MakeDrawError("Could not end SSAO normal-depth prepass: " + End.error().Message));

		const auto DrawFullscreen = [&](const RenderTarget& Target, const GraphicsPipeline& Pipeline,
			const BindingSet& Bindings, const char* PassName) -> std::expected<void, SceneRendererError>
		{
			RenderTargetClearValue Clear;
			Clear.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
			const GraphicsResult Begin = m_Runtime.BeginRenderTarget(Target, Clear);
			if (!Begin)
				return std::unexpected(MakeDrawError(std::string("Could not begin ") + PassName + ": " + Begin.error().Message));
			RenderTargetFrameScope Scope(m_Runtime);
			const std::array<const BindingSet*, 1> BindingSets = { &Bindings };
			const GraphicsResult Draw = m_Runtime.Draw(Pipeline, *m_BackgroundTriangleBuffer, { 3, 1, 0, 0 }, BindingSets);
			if (!Draw)
				return std::unexpected(MakeDrawError(std::string("Could not draw ") + PassName + ": " + Draw.error().Message));
			const GraphicsResult End = Scope.End();
			if (!End)
				return std::unexpected(MakeDrawError(std::string("Could not end ") + PassName + ": " + End.error().Message));
			return {};
		};
		if (auto RawPass = DrawFullscreen(*m_AmbientOcclusionRawTarget, *m_AmbientOcclusionPipeline,
			*m_AmbientOcclusionEvaluationBindingSet, "SSAO evaluation pass"); !RawPass)
			return std::unexpected(std::move(RawPass.error()));

		AmbientOcclusionBlurConstants BlurParameters;
		BlurParameters.InverseProjection = InverseProjection;
		BlurParameters.OutputSize = Parameters.OutputSize;
		BlurParameters.HalfSize = Parameters.HalfSize;
		BlurParameters.Direction = glm::vec4(1.0f / static_cast<float>(HalfWidth), 0.0f, 0.0f, 0.0f);
		if (const auto Update = m_Runtime.WriteBuffer(*m_AmbientOcclusionBlurParametersBuffer, 0,
			std::as_bytes(std::span(&BlurParameters, 1))); !Update)
			return std::unexpected(MakeDrawError("Could not update horizontal SSAO blur parameters: " + Update.error().Message));
		if (auto BlurPass = DrawFullscreen(*m_AmbientOcclusionBlurTargets[0], *m_AmbientOcclusionBlurPipeline,
			*m_AmbientOcclusionBlurBindingSets[0], "horizontal SSAO bilateral blur"); !BlurPass)
			return std::unexpected(std::move(BlurPass.error()));

		BlurParameters.Direction = glm::vec4(0.0f, 1.0f / static_cast<float>(HalfHeight), 0.0f, 0.0f);
		if (const auto Update = m_Runtime.WriteBuffer(*m_AmbientOcclusionBlurParametersBuffer, 0,
			std::as_bytes(std::span(&BlurParameters, 1))); !Update)
			return std::unexpected(MakeDrawError("Could not update vertical SSAO blur parameters: " + Update.error().Message));
		if (auto BlurPass = DrawFullscreen(*m_AmbientOcclusionBlurTargets[1], *m_AmbientOcclusionBlurPipeline,
			*m_AmbientOcclusionBlurBindingSets[1], "vertical SSAO bilateral blur"); !BlurPass)
			return std::unexpected(std::move(BlurPass.error()));
		m_AmbientOcclusionFrameAvailable = true;
		return {};
	}

	bool SceneRenderer::SetAmbientOcclusionSettings(const AmbientOcclusionSettings& Settings) noexcept
	{
		if (!std::isfinite(Settings.Radius) || Settings.Radius <= 0.0f || Settings.Radius > 10.0f ||
			!std::isfinite(Settings.Bias) || Settings.Bias < 0.0f || Settings.Bias > Settings.Radius ||
			!std::isfinite(Settings.Strength) || Settings.Strength < 0.0f || Settings.Strength > 4.0f)
			return false;
		m_AmbientOcclusionSettings = Settings;
		return true;
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

		m_ToneMappingBindingSet.reset();
		m_ToneMappingBindingGeneration = 0;
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
			m_PreparedSnapshot->EnvironmentLight.has_value(),
			m_HdrSceneTargetGeneration
		};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureToneMappingResources()
	{
		if (!m_ToneMappingVertexShader || !m_ToneMappingFragmentShader || !m_ToneMappingBindingLayout ||
			!m_ToneMappingSampler || !m_ToneMappingConstantsBuffer)
			return std::unexpected(MakeResourceError("Tone mapping resources are not initialized"));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureToneMappingPipeline(
		ColorTargetFormat ColorFormat,
		bool DepthAttachmentEnabled)
	{
		const auto Key = std::pair{ ColorFormat, DepthAttachmentEnabled };
		if (m_ToneMappingPipelines.contains(Key))
			return {};
		if (auto Resources = EnsureToneMappingResources(); !Resources)
			return std::unexpected(std::move(Resources.error()));

		GraphicsPipelineDesc Description;
		Description.VertexShader = m_ToneMappingVertexShader;
		Description.FragmentShader = m_ToneMappingFragmentShader;
		Description.BindingLayouts = { m_ToneMappingBindingLayout };
		Description.VertexLayout.Stride = sizeof(float) * 2;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
		Description.ColorFormat = ColorFormat;
		Description.DepthAttachmentEnabled = DepthAttachmentEnabled;
		Description.Depth.TestEnabled = false;
		Description.Depth.WriteEnabled = false;
		Description.DebugName = "PulseForge fullscreen SDR tone mapping pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create tone mapping pipeline: " + Pipeline.error().Message));
		m_ToneMappingPipelines.emplace(Key, std::move(*Pipeline));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureToneMappingBindings(
		const Texture& HdrTexture,
		uint64_t Generation)
	{
		if (IsToneMappingBindingCurrent(m_ToneMappingBindingGeneration, Generation) && m_ToneMappingBindingSet)
			return {};
		m_ToneMappingBindingSet.reset();
		m_ToneMappingBindingGeneration = 0;
		BindingSetDesc Description;
		Description.Layout = m_ToneMappingBindingLayout;
		Description.Textures = { { 0, std::cref(HdrTexture), BindingResourceType::Texture2D } };
		Description.Samplers = { { 0, std::cref(*m_ToneMappingSampler) } };
		Description.Buffers = { { 0, std::cref(*m_ToneMappingConstantsBuffer) } };
		auto BindingSet = m_Runtime.CreateBindingSet(Description);
		if (!BindingSet)
			return std::unexpected(MakeResourceError("Could not bind HDR scene texture for tone mapping: " + BindingSet.error().Message));
		m_ToneMappingBindingSet = std::move(*BindingSet);
		m_ToneMappingBindingGeneration = Generation;
		return {};
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::ToneMapToOutput(
		const Texture& HdrTexture,
		uint64_t Generation,
		ColorTargetFormat DestinationFormat,
		bool DepthAttachmentEnabled)
	{
		if (auto Pipeline = EnsureToneMappingPipeline(DestinationFormat, DepthAttachmentEnabled); !Pipeline)
			return std::unexpected(std::move(Pipeline.error()));
		if (auto Bindings = EnsureToneMappingBindings(HdrTexture, Generation); !Bindings)
			return std::unexpected(std::move(Bindings.error()));

		const ToneMappingConstants Constants{
			m_ToneMappingSettings.ExposureEV,
			ShouldShaderEncodeSrgb(DestinationFormat, m_Runtime.GetOutputColorEncoding()) ? 1u : 0u,
			{}
		};
		if (!m_HasUploadedToneMappingConstants || Constants.ExposureEV != m_UploadedToneMappingConstants.ExposureEV ||
			Constants.EncodeSrgbForUnorm != m_UploadedToneMappingConstants.EncodeSrgbForUnorm)
		{
			if (const auto Update = m_Runtime.WriteBuffer(*m_ToneMappingConstantsBuffer, 0,
				std::as_bytes(std::span(&Constants, 1))); !Update)
				return std::unexpected(MakeDrawError("Could not update tone mapping constants: " + Update.error().Message));
			m_UploadedToneMappingConstants = Constants;
			m_HasUploadedToneMappingConstants = true;
		}

		const std::array<const BindingSet*, 1> BindingSets = { m_ToneMappingBindingSet.get() };
		const GraphicsResult Draw = m_Runtime.Draw(
			*m_ToneMappingPipelines.at({ DestinationFormat, DepthAttachmentEnabled }),
			*m_BackgroundTriangleBuffer,
			{ 3, 1, 0, 0 },
			BindingSets);
		if (!Draw)
			return std::unexpected(MakeDrawError("Could not draw fullscreen tone mapping pass: " + Draw.error().Message));
		return size_t{ 1 };
	}

	std::expected<SceneRenderOutputResult, SceneRendererError> SceneRenderer::RenderPreparedSceneToOutput()
	{
		const auto [Width, Height] = m_Runtime.GetWindow().GetFramebufferSize();
		if (Width == 0 || Height == 0)
			return SceneRenderOutputResult{};
		auto Hdr = RenderPreparedSceneToHdr(Width, Height);
		if (!Hdr)
			return std::unexpected(std::move(Hdr.error()));
		if (auto Tonemapped = ToneMapToOutput(*Hdr->ColorTexture, Hdr->TargetGeneration,
			ColorTargetFormat::Swapchain, true); !Tonemapped)
			return std::unexpected(std::move(Tonemapped.error()));
	return SceneRenderOutputResult{
			Hdr->GeometryDrawCount,
			Hdr->TargetGeneration,
			ColorTargetFormat::Swapchain,
			ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, m_Runtime.GetOutputColorEncoding())
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
		if (auto Pipeline = EnsureToneMappingPipeline(
			Description.ColorFormat, Description.DepthMode != DepthAttachmentMode::None); !Pipeline)
			return std::unexpected(std::move(Pipeline.error()));
		if (auto Bindings = EnsureToneMappingBindings(*Hdr->ColorTexture, Hdr->TargetGeneration); !Bindings)
			return std::unexpected(std::move(Bindings.error()));

		RenderTargetClearValue Clear;
		Clear.Color = { 0.0f, 0.0f, 0.0f, 1.0f };
		Clear.Depth = 1.0f;
		if (const GraphicsResult Begin = m_Runtime.BeginRenderTarget(Target, Clear); !Begin)
			return std::unexpected(MakeDrawError("Could not begin tone-mapped output target: " + Begin.error().Message));
		RenderTargetFrameScope Scope(m_Runtime);
		auto Draw = ToneMapToOutput(*Hdr->ColorTexture, Hdr->TargetGeneration,
			Description.ColorFormat, Description.DepthMode != DepthAttachmentMode::None);
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
			ShouldShaderEncodeSrgb(Description.ColorFormat, m_Runtime.GetOutputColorEncoding())
		};
	}

	bool SceneRenderer::SetToneMappingSettings(const ToneMappingSettings& Settings) noexcept
	{
		return TryUpdateToneMappingSettings(m_ToneMappingSettings, Settings);
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedScene()
	{
		if (auto Shadows = RenderShadowCascades(); !Shadows)
			return std::unexpected(std::move(Shadows.error()));
		const auto [Width, Height] = m_Runtime.GetWindow().GetFramebufferSize();
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
			const std::optional<AssetID> BindingEnvironmentAsset = m_PreparedEnvironmentReady ? EnvironmentAsset : std::nullopt;
			const auto Binding = m_MaterialBindings.find(MakeMaterialBindingKey(*Instance.MaterialAsset, BindingEnvironmentAsset));
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
			const BindingSet* AmbientOcclusionBindings = m_AmbientOcclusionFrameAvailable && m_AmbientOcclusionFinalBindingSet
				? m_AmbientOcclusionFinalBindingSet.get()
				: m_AmbientOcclusionFallbackBindingSet.get();
			const std::array<const BindingSet*, 3> BindingSets = {
				Binding->second.BindingSet.get(), m_ShadowBindingSet.get(), AmbientOcclusionBindings };
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
