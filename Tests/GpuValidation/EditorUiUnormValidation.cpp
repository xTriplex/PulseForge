#include "GpuValidation/EditorUiUnormValidation.h"

#include "Core/Application.h"
#include "Core/Log.h"
#include "GpuValidation/ValidationShader.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>

namespace PulseForge::GpuValidation
{
	namespace
	{
		struct ValidationVertex
		{
			float Position[2];
			float TexCoord[2];
		};

		struct EditorValidationVertex
		{
			float Position[2];
			float TexCoord[2];
			float Color[4];
		};
	} // namespace

	void EditorUiUnormValidation::Initialize(Application& App, const BindingLayoutHandle& BindingLayout, const Sampler& Sampler)
	{
		m_BindingLayout = BindingLayout;
		m_Sampler = &Sampler;
		const std::filesystem::path ShaderDirectory(PF_GPU_VALIDATION_SHADER_DIRECTORY);
		const auto VertexBytecode = LoadValidationShader(ShaderDirectory / "EditorImGui.vs.spv");
		const auto FragmentBytecode = LoadValidationShader(ShaderDirectory / "EditorImGui.ps.spv");
		const auto PresentVertexBytecode = LoadValidationShader(ShaderDirectory / "EditorImGuiPresent.vs.spv");
		const auto PresentFragmentBytecode = LoadValidationShader(ShaderDirectory / "EditorImGuiPresent.ps.spv");

		PulseForge::ShaderDesc Description;
		Description.Stage = PulseForge::ShaderStage::Vertex;
		Description.EntryPoint = "VSMain";
		Description.DebugName = "Production editor ImGui vertex shader (GPU validation)";
		auto Vertex = App.CreateShader(Description, VertexBytecode);
		if (!Vertex) throw std::runtime_error("Could not create production editor ImGui vertex shader: " + Vertex.error().Message);
		m_EditorUiVertexShader = std::move(*Vertex);

		Description.Stage = PulseForge::ShaderStage::Fragment;
		Description.EntryPoint = "PSMain";
		Description.DebugName = "Production editor ImGui fragment shader (GPU validation)";
		auto Fragment = App.CreateShader(Description, FragmentBytecode);
		if (!Fragment) throw std::runtime_error("Could not create production editor ImGui fragment shader: " + Fragment.error().Message);
		m_EditorUiFragmentShader = std::move(*Fragment);

		Description.Stage = PulseForge::ShaderStage::Vertex;
		Description.EntryPoint = "VSMain";
		Description.DebugName = "Editor UI UNORM presentation vertex shader";
		auto PresentVertex = App.CreateShader(Description, PresentVertexBytecode);
		if (!PresentVertex) throw std::runtime_error("Could not create UI presentation vertex shader: " + PresentVertex.error().Message);
		m_EditorUiPresentationVertexShader = std::move(*PresentVertex);

		Description.Stage = PulseForge::ShaderStage::Fragment;
		Description.EntryPoint = "PSMain";
		Description.DebugName = "Editor UI UNORM presentation fragment shader";
		auto PresentFragment = App.CreateShader(Description, PresentFragmentBytecode);
		if (!PresentFragment)
			throw std::runtime_error("Could not create UI presentation fragment shader: " + PresentFragment.error().Message);
		m_EditorUiPresentationFragmentShader = std::move(*PresentFragment);

		constexpr std::array<std::byte, 4> WhitePixel = { std::byte{ 0xff }, std::byte{ 0xff }, std::byte{ 0xff }, std::byte{ 0xff } };
		PulseForge::TextureDesc FontTextureDescription;
		FontTextureDescription.Width = 1;
		FontTextureDescription.Height = 1;
		FontTextureDescription.Format = PulseForge::TextureFormat::RGBA8_UNorm;
		FontTextureDescription.Usage = PulseForge::TextureUsage::ShaderResource;
		FontTextureDescription.DebugName = "Editor UI validation white font texel";
		auto FontTexture = App.CreateTexture(FontTextureDescription, WhitePixel);
		if (!FontTexture) throw std::runtime_error("Could not create the editor UI validation texel: " + FontTexture.error().Message);
		m_EditorUiFontTexture = std::move(*FontTexture);

		PulseForge::BindingSetDesc UiBindingDescription;
		UiBindingDescription.Layout = m_BindingLayout;
		UiBindingDescription.Textures.push_back({ 0, *m_EditorUiFontTexture, PulseForge::BindingResourceType::Texture2D });
		UiBindingDescription.Samplers.push_back({ 0, *m_Sampler });
		auto UiBinding = App.CreateBindingSet(UiBindingDescription);
		if (!UiBinding) throw std::runtime_error("Could not bind the editor UI validation texel: " + UiBinding.error().Message);
		m_EditorUiDrawBindingSet = std::move(*UiBinding);

		PulseForge::GraphicsPipelineDesc UiPipeline;
		UiPipeline.VertexShader = m_EditorUiVertexShader;
		UiPipeline.FragmentShader = m_EditorUiFragmentShader;
		UiPipeline.BindingLayouts = { m_BindingLayout };
		UiPipeline.VertexLayout.Stride = sizeof(EditorValidationVertex);
		UiPipeline.VertexLayout.Attributes = { { PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2,
												   offsetof(EditorValidationVertex, Position) },
			{ PulseForge::VertexSemantic::Color, PulseForge::VertexFormat::Float4, offsetof(EditorValidationVertex, Color) },
			{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(EditorValidationVertex, TexCoord) } };
		UiPipeline.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		UiPipeline.DepthAttachmentEnabled = false;
		UiPipeline.Blend.Enabled = true;
		UiPipeline.DebugName = "Production editor ImGui linear-light sRGB composition validation";
		auto UiPipelineResult = App.CreateGraphicsPipeline(UiPipeline);
		if (!UiPipelineResult)
			throw std::runtime_error("Could not create production editor ImGui pipeline: " + UiPipelineResult.error().Message);
		m_EditorUiPipeline = std::move(*UiPipelineResult);
		constexpr std::array<EditorValidationVertex, 3> UiVertices = { { { { -1.0f, -1.0f }, { 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f, 0.5f } },
			{ { 3.0f, -1.0f }, { 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f, 0.5f } },
			{ { -1.0f, 3.0f }, { 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f, 0.5f } } } };
		PulseForge::BufferDesc UiVertexDescription;
		UiVertexDescription.ByteSize = sizeof(UiVertices);
		UiVertexDescription.Usage = PulseForge::BufferUsage::Vertex;
		UiVertexDescription.DebugName = "Production editor ImGui alpha-blend validation triangle";
		auto UiVertexBuffer = App.CreateBuffer(UiVertexDescription, std::as_bytes(std::span(UiVertices)));
		if (!UiVertexBuffer) throw std::runtime_error("Could not create editor UI validation draw data: " + UiVertexBuffer.error().Message);
		m_EditorUiVertexBuffer = std::move(*UiVertexBuffer);

		PulseForge::GraphicsPipelineDesc PresentationPipeline;
		PresentationPipeline.VertexShader = m_EditorUiPresentationVertexShader;
		PresentationPipeline.FragmentShader = m_EditorUiPresentationFragmentShader;
		PresentationPipeline.BindingLayouts = { m_BindingLayout };
		PresentationPipeline.VertexLayout.Stride = sizeof(ValidationVertex);
		PresentationPipeline.VertexLayout.Attributes = { { PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2,
			offsetof(ValidationVertex, Position) } };
		PresentationPipeline.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_UNorm;
		PresentationPipeline.DepthAttachmentEnabled = false;
		PresentationPipeline.DebugName = "Editor UI UNORM output transfer validation";
		auto PresentationPipelineResult = App.CreateGraphicsPipeline(PresentationPipeline);
		if (!PresentationPipelineResult)
			throw std::runtime_error("Could not create UI presentation pipeline: " + PresentationPipelineResult.error().Message);
		m_EditorUiPresentationPipeline = std::move(*PresentationPipelineResult);

		constexpr std::array<ValidationVertex, 3> Fullscreen = { ValidationVertex{ { -1.0f, -1.0f }, {} },
			ValidationVertex{ { 3.0f, -1.0f }, {} }, ValidationVertex{ { -1.0f, 3.0f }, {} } };
		PulseForge::BufferDesc BufferDescription;
		BufferDescription.ByteSize = sizeof(Fullscreen);
		BufferDescription.Usage = PulseForge::BufferUsage::Vertex;
		BufferDescription.DebugName = "Editor UI presentation fullscreen triangle validation";
		auto Buffer = App.CreateBuffer(BufferDescription, std::as_bytes(std::span(Fullscreen)));
		if (!Buffer) throw std::runtime_error("Could not create UI presentation vertex buffer: " + Buffer.error().Message);
		m_EditorUiFullscreenBuffer = std::move(*Buffer);
	}

	void EditorUiUnormValidation::EnsureTargets(Application& App, uint32_t Width, uint32_t Height)
	{
		if (m_EditorUiSrgbTarget && m_EditorUiUnormTarget && m_EditorUiBindingSet && m_EditorUiWidth == Width && m_EditorUiHeight == Height)
			return;

		PulseForge::RenderTargetDesc Description;
		Description.Width = Width;
		Description.Height = Height;
		Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		Description.DepthMode = PulseForge::DepthAttachmentMode::None;
		Description.DebugName = "Editor UI linear-light validation intermediate";
		auto SrgbTarget = App.CreateRenderTarget(Description);
		if (!SrgbTarget) throw std::runtime_error("Could not create UI sRGB intermediate: " + SrgbTarget.error().Message);
		const PulseForge::Texture* SrgbTexture = (*SrgbTarget)->GetColorTexture();
		if (!SrgbTexture) throw std::runtime_error("UI sRGB intermediate has no sampleable color texture");

		Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_UNorm;
		Description.DebugName = "Editor UI UNORM validation output";
		auto UnormTarget = App.CreateRenderTarget(Description);
		if (!UnormTarget) throw std::runtime_error("Could not create UI UNORM output target: " + UnormTarget.error().Message);

		PulseForge::BindingSetDesc BindingDescription;
		BindingDescription.Layout = m_BindingLayout;
		BindingDescription.Textures.push_back({ 0, *SrgbTexture, PulseForge::BindingResourceType::Texture2D });
		BindingDescription.Samplers.push_back({ 0, *m_Sampler });
		auto Binding = App.CreateBindingSet(BindingDescription);
		if (!Binding) throw std::runtime_error("Could not bind UI sRGB intermediate: " + Binding.error().Message);

		m_EditorUiBindingSet = std::move(*Binding);
		m_EditorUiSrgbTarget = std::move(*SrgbTarget);
		m_EditorUiUnormTarget = std::move(*UnormTarget);
		m_EditorUiWidth = Width;
		m_EditorUiHeight = Height;
		++m_EditorUiTargetCreations;
		PF_INFO("Created editor UI validation targets: {}x{} sRGB intermediate + UNORM output", Width, Height);
	}

	std::expected<void, std::string> EditorUiUnormValidation::RenderFrame(Application& App)
	{
		if (m_EditorUiFrames >= 3) return {};
		const uint32_t Width = m_EditorUiFrames < 2 ? 32u : 48u;
		const uint32_t Height = 32u;
		try
		{
			EnsureTargets(App, Width, Height);
			const PulseForge::Texture* Source = m_EditorUiSrgbTarget->GetColorTexture();
			const PulseForge::Texture* Destination = m_EditorUiUnormTarget->GetColorTexture();
			if (!Source || !Destination || Source->GetDescription().Format != PulseForge::TextureFormat::RGBA8_Srgb ||
				Destination->GetDescription().Format != PulseForge::TextureFormat::RGBA8_UNorm || Source->GetDescription().Width != Width ||
				Source->GetDescription().Height != Height || Destination->GetDescription().Width != Width ||
				Destination->GetDescription().Height != Height ||
				m_EditorUiSrgbTarget->GetDescription().DepthMode != PulseForge::DepthAttachmentMode::None ||
				m_EditorUiUnormTarget->GetDescription().DepthMode != PulseForge::DepthAttachmentMode::None)
				throw std::runtime_error("Editor UI validation target format or extent was incorrect");
			if (m_EditorUiFrames == 0) m_EditorUiFirstTexture = Source;
			else if (m_EditorUiFrames == 1 && Source != m_EditorUiFirstTexture)
				throw std::runtime_error("Same-sized editor UI target was not reused");
			if (m_EditorUiTargetCreations != (m_EditorUiFrames < 2 ? 1u : 2u))
				throw std::runtime_error("Editor UI target reuse or resize recreation count was incorrect");

			PulseForge::RenderTargetClearValue Clear;
			Clear.Color = { 0.0f, 0.0f, 0.0f, 1.0f };
			if (const auto Begin = App.BeginRenderTarget(*m_EditorUiSrgbTarget, Clear); !Begin)
				throw std::runtime_error("Could not begin editor UI sRGB target: " + Begin.error().Message);
			const std::array<const PulseForge::BindingSet*, 1> UiBindings = { m_EditorUiDrawBindingSet.get() };
			const auto BlendDraw = App.Draw(*m_EditorUiPipeline, *m_EditorUiVertexBuffer, { 3, 1, 0, 0 }, UiBindings);
			const auto BlendEnd = App.EndRenderTarget();
			if (!BlendDraw) throw std::runtime_error("Could not record alpha blend into sRGB UI target: " + BlendDraw.error().Message);
			if (!BlendEnd) throw std::runtime_error("Could not end editor UI sRGB target: " + BlendEnd.error().Message);

			if (const auto Begin = App.BeginRenderTarget(*m_EditorUiUnormTarget, Clear); !Begin)
				throw std::runtime_error("Could not begin editor UI UNORM output: " + Begin.error().Message);
			const std::array<const PulseForge::BindingSet*, 1> Bindings = { m_EditorUiBindingSet.get() };
			const auto PresentDraw = App.Draw(*m_EditorUiPresentationPipeline, *m_EditorUiFullscreenBuffer, { 3, 1, 0, 0 }, Bindings);
			const auto PresentEnd = App.EndRenderTarget();
			if (!PresentDraw)
				throw std::runtime_error("Could not record editor UI UNORM output-transfer draw: " + PresentDraw.error().Message);
			if (!PresentEnd) throw std::runtime_error("Could not end editor UI UNORM output: " + PresentEnd.error().Message);
			++m_EditorUiFrames;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(Exception.what());
		}
		return {};
	}

	void EditorUiUnormValidation::Reset() noexcept
	{
		m_EditorUiBindingSet.reset();
		m_EditorUiDrawBindingSet.reset();
		m_EditorUiUnormTarget.reset();
		m_EditorUiSrgbTarget.reset();
		m_EditorUiPresentationPipeline.reset();
		m_EditorUiPipeline.reset();
		m_EditorUiFullscreenBuffer.reset();
		m_EditorUiVertexBuffer.reset();
		m_EditorUiFontTexture.reset();
		m_EditorUiPresentationFragmentShader.reset();
		m_EditorUiPresentationVertexShader.reset();
		m_EditorUiFragmentShader.reset();
		m_EditorUiVertexShader.reset();
		m_EditorUiFirstTexture = nullptr;
		m_EditorUiFrames = 0;
		m_EditorUiTargetCreations = 0;
		m_EditorUiWidth = 0;
		m_EditorUiHeight = 0;
		m_Sampler = nullptr;
		m_BindingLayout.reset();
	}
} // namespace PulseForge::GpuValidation
