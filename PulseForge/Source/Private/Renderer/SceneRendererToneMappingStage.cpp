#include "Renderer/SceneRendererToneMappingStage.h"

namespace PulseForge
{
	namespace
	{
		SceneRendererError MakeToneMappingResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}

		SceneRendererError MakeToneMappingDrawError(std::string Message)
		{
			return { SceneRendererErrorCode::DrawFailed, {}, {}, std::move(Message) };
		}
	} // namespace

	SceneRenderer::ToneMappingStage::ToneMappingStage(RenderDevice& Runtime) noexcept : m_Runtime(Runtime)
	{
	}

	std::expected<void, SceneRendererError> SceneRenderer::ToneMappingStage::Initialize(
		std::span<const std::byte> VertexBytecode, std::span<const std::byte> FragmentBytecode)
	{
		ShaderDesc VertexDescription;
		VertexDescription.Stage = ShaderStage::Vertex;
		VertexDescription.EntryPoint = "VSMain";
		VertexDescription.DebugName = "PulseForge tone mapping vertex shader";
		auto Vertex = m_Runtime.CreateShader(VertexDescription, VertexBytecode);
		if (!Vertex)
			return std::unexpected(MakeToneMappingResourceError("Could not create tone mapping vertex shader: " + Vertex.error().Message));
		m_VertexShader = std::move(*Vertex);

		ShaderDesc FragmentDescription;
		FragmentDescription.Stage = ShaderStage::Fragment;
		FragmentDescription.EntryPoint = "PSMain";
		FragmentDescription.DebugName = "PulseForge tone mapping fragment shader";
		auto Fragment = m_Runtime.CreateShader(FragmentDescription, FragmentBytecode);
		if (!Fragment)
			return std::unexpected(
				MakeToneMappingResourceError("Could not create tone mapping fragment shader: " + Fragment.error().Message));
		m_FragmentShader = std::move(*Fragment);

		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = { { BindingResourceType::Texture2D, 0 }, { BindingResourceType::Sampler, 0 },
			{ BindingResourceType::ConstantBuffer, 0 } };
		LayoutDescription.DebugName = "PulseForge tone mapping input and parameters";
		auto Layout = m_Runtime.CreateBindingLayout(LayoutDescription);
		if (!Layout)
			return std::unexpected(MakeToneMappingResourceError("Could not create tone mapping binding layout: " + Layout.error().Message));
		m_BindingLayout = std::move(*Layout);

		SamplerDesc SamplerDescription;
		SamplerDescription.Minification = SamplerFilter::Linear;
		SamplerDescription.Magnification = SamplerFilter::Linear;
		SamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
		SamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
		SamplerDescription.DebugName = "PulseForge tone mapping HDR sampler";
		auto Sampler = m_Runtime.CreateSampler(SamplerDescription);
		if (!Sampler)
			return std::unexpected(MakeToneMappingResourceError("Could not create tone mapping sampler: " + Sampler.error().Message));
		m_Sampler = std::move(*Sampler);

		BufferDesc ConstantsDescription;
		ConstantsDescription.ByteSize = sizeof(Constants);
		ConstantsDescription.Usage = BufferUsage::Constant;
		ConstantsDescription.DebugName = "PulseForge tone mapping constants";
		auto ConstantsBuffer = m_Runtime.CreateBuffer(ConstantsDescription, std::as_bytes(std::span(&m_UploadedConstants, 1)));
		if (!ConstantsBuffer)
			return std::unexpected(
				MakeToneMappingResourceError("Could not create tone mapping constants: " + ConstantsBuffer.error().Message));
		m_ConstantsBuffer = std::move(*ConstantsBuffer);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::ToneMappingStage::Prepare(
		const Texture& HdrTexture, uint64_t Generation, ColorTargetFormat DestinationFormat, bool DepthAttachmentEnabled)
	{
		if (auto Pipeline = EnsurePipeline(DestinationFormat, DepthAttachmentEnabled); !Pipeline)
			return std::unexpected(std::move(Pipeline.error()));
		return EnsureBindings(HdrTexture, Generation);
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::ToneMappingStage::Draw(const Texture& HdrTexture, uint64_t Generation,
		ColorTargetFormat DestinationFormat, bool DepthAttachmentEnabled, const Buffer& FullscreenTriangle)
	{
		if (auto Ready = Prepare(HdrTexture, Generation, DestinationFormat, DepthAttachmentEnabled); !Ready)
			return std::unexpected(std::move(Ready.error()));

		const Constants ConstantsToUpload{ m_Settings.ExposureEV,
			ShouldShaderEncodeSrgb(DestinationFormat, m_Runtime.GetOutputColorEncoding()) ? 1u : 0u, {} };
		if (!m_HasUploadedConstants || ConstantsToUpload.ExposureEV != m_UploadedConstants.ExposureEV ||
			ConstantsToUpload.EncodeSrgbForUnorm != m_UploadedConstants.EncodeSrgbForUnorm)
		{
			if (const auto Update = m_Runtime.WriteBuffer(*m_ConstantsBuffer, 0, std::as_bytes(std::span(&ConstantsToUpload, 1))); !Update)
				return std::unexpected(MakeToneMappingDrawError("Could not update tone mapping constants: " + Update.error().Message));
			m_UploadedConstants = ConstantsToUpload;
			m_HasUploadedConstants = true;
		}

		const std::array<const BindingSet*, 1> BindingSets = { m_BindingSet.get() };
		const GraphicsResult DrawResult =
			m_Runtime.Draw(*m_Pipelines.at({ DestinationFormat, DepthAttachmentEnabled }), FullscreenTriangle, { 3, 1, 0, 0 }, BindingSets);
		if (!DrawResult)
			return std::unexpected(MakeToneMappingDrawError("Could not draw fullscreen tone mapping pass: " + DrawResult.error().Message));
		return size_t{ 1 };
	}

	void SceneRenderer::ToneMappingStage::InvalidateBindings() noexcept
	{
		m_BindingSet.reset();
		m_BindingGeneration = 0;
	}

	bool SceneRenderer::ToneMappingStage::SetSettings(const ToneMappingSettings& Settings) noexcept
	{
		return TryUpdateToneMappingSettings(m_Settings, Settings);
	}

	const ToneMappingSettings& SceneRenderer::ToneMappingStage::GetSettings() const noexcept
	{
		return m_Settings;
	}

	std::expected<void, SceneRendererError> SceneRenderer::ToneMappingStage::EnsurePipeline(
		ColorTargetFormat ColorFormat, bool DepthAttachmentEnabled)
	{
		const auto Key = std::pair{ ColorFormat, DepthAttachmentEnabled };
		if (m_Pipelines.contains(Key)) return {};

		GraphicsPipelineDesc Description;
		Description.VertexShader = m_VertexShader;
		Description.FragmentShader = m_FragmentShader;
		Description.BindingLayouts = { m_BindingLayout };
		Description.VertexLayout.Stride = sizeof(float) * 2;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
		Description.ColorFormat = ColorFormat;
		Description.DepthAttachmentEnabled = DepthAttachmentEnabled;
		Description.Depth.TestEnabled = false;
		Description.Depth.WriteEnabled = false;
		Description.DebugName = "PulseForge fullscreen SDR tone mapping pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline)
			return std::unexpected(MakeToneMappingResourceError("Could not create tone mapping pipeline: " + Pipeline.error().Message));
		m_Pipelines.emplace(Key, std::move(*Pipeline));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::ToneMappingStage::EnsureBindings(const Texture& HdrTexture, uint64_t Generation)
	{
		if (IsToneMappingBindingCurrent(m_BindingGeneration, Generation) && m_BindingSet) return {};
		InvalidateBindings();

		BindingSetDesc Description;
		Description.Layout = m_BindingLayout;
		Description.Textures = { { 0, std::cref(HdrTexture), BindingResourceType::Texture2D } };
		Description.Samplers = { { 0, std::cref(*m_Sampler) } };
		Description.Buffers = { { 0, std::cref(*m_ConstantsBuffer) } };
		auto Binding = m_Runtime.CreateBindingSet(Description);
		if (!Binding)
			return std::unexpected(
				MakeToneMappingResourceError("Could not bind HDR scene texture for tone mapping: " + Binding.error().Message));
		m_BindingSet = std::move(*Binding);
		m_BindingGeneration = Generation;
		return {};
	}
} // namespace PulseForge
