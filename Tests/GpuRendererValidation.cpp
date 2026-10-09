#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	struct ValidationVertex
	{
		float Position[2];
		float TexCoord[2];
	};

	std::vector<std::byte> LoadShader(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary | std::ios::ate);
		if (!Input)
			throw std::runtime_error("Could not open GPU-validation shader: " + Path.string());

		const std::streamoff FileSize = Input.tellg();
		if (FileSize < 20 || FileSize > 16 * 1024 * 1024 || FileSize % sizeof(uint32_t) != 0)
			throw std::runtime_error("GPU-validation shader has an invalid byte size: " + Path.string());

		std::vector<std::byte> Bytecode(static_cast<size_t>(FileSize));
		Input.seekg(0, std::ios::beg);
		Input.read(reinterpret_cast<char*>(Bytecode.data()), static_cast<std::streamsize>(FileSize));
		if (!Input)
			throw std::runtime_error("Could not read GPU-validation shader: " + Path.string());
		return Bytecode;
	}

	class GpuValidationLayer final : public PulseForge::Layer
	{
	public:
		GpuValidationLayer()
			: Layer("RGBA16F GPU validation")
		{
		}

		void OnAttach() override
		{
			m_StartedAt = std::chrono::steady_clock::now();
			try
			{
				InitializeResources();
				PF_INFO("RGBA16F GPU validation resources initialized");
			}
			catch (const std::exception& Exception)
			{
				Fail(Exception.what());
			}
		}

		void OnUpdate(PulseForge::Timestep) override
		{
			if (m_CompletedFrames >= 3)
			{
				PF_INFO("GPU_VALIDATION_RESULT=PASS; completed three RGBA16F render/sample frames");
				PulseForge::Application::Get().RequestClose();
			}
			else if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(10))
				Fail("Timed out before completing three RGBA16F render/sample frames");
		}

		void OnRender() override
		{
			if (!m_Succeeded || m_CompletedFrames >= 3)
				return;

			PulseForge::Application& App = PulseForge::Application::Get();
			const PulseForge::RenderTargetClearValue ClearValue{
				.Color = { 2.0f, 0.25f, 0.125f, 1.0f },
				.Depth = 1.0f
			};
			const PulseForge::GraphicsResult BeginResult = App.BeginRenderTarget(*m_Target, ClearValue);
			if (!BeginResult)
			{
				Fail("Could not begin the RGBA16F render target: " + BeginResult.error().Message);
				return;
			}

			const PulseForge::DrawArguments DrawArguments{ .VertexCount = 3 };
			const PulseForge::GraphicsResult ColorDrawResult = App.Draw(*m_ColorPipeline, *m_VertexBuffer, DrawArguments);
			const PulseForge::GraphicsResult EndResult = App.EndRenderTarget();
			if (!ColorDrawResult)
			{
				Fail("RGBA16F draw recording failed: " + ColorDrawResult.error().Message);
				return;
			}
			if (!EndResult)
			{
				Fail("Could not end the RGBA16F render target: " + EndResult.error().Message);
				return;
			}

			const std::array<const PulseForge::BindingSet*, 1> BindingSets = { m_SamplingBindingSet.get() };
			const PulseForge::GraphicsResult SamplingDrawResult = App.Draw(
				*m_SamplingPipeline,
				*m_VertexBuffer,
				DrawArguments,
				BindingSets);
			if (!SamplingDrawResult)
			{
				Fail("RGBA16F shader-resource sampling draw failed: " + SamplingDrawResult.error().Message);
				return;
			}

			++m_CompletedFrames;
			if (m_CompletedFrames == 1)
				PF_INFO("RGBA16F attachment draw, shader-resource transition, and sampling draw recorded");
		}

		void OnDetach() override
		{
			m_SamplingBindingSet.reset();
			m_SamplingPipeline.reset();
			m_ColorPipeline.reset();
			m_Sampler.reset();
			m_BindingLayout.reset();
			m_VertexBuffer.reset();
			m_Target.reset();
		}

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return m_Succeeded && m_CompletedFrames == 3;
		}

	private:
		void Fail(const std::string& Message)
		{
			m_Succeeded = false;
			PF_ERROR("GPU_VALIDATION_RESULT=FAIL: {0}", Message);
			PulseForge::Application::Get().RequestClose();
		}

		void InitializeResources()
		{
			PulseForge::Application& App = PulseForge::Application::Get();
			PulseForge::RenderTargetDesc TargetDescription;
			TargetDescription.Width = 32;
			TargetDescription.Height = 32;
			TargetDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA16_Float;
			TargetDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			TargetDescription.DebugName = "RGBA16F GPU validation target";
			auto TargetResult = App.CreateRenderTarget(TargetDescription);
			if (!TargetResult)
				throw std::runtime_error("RGBA16F render-target creation failed: " + TargetResult.error().Message);
			m_Target = std::move(TargetResult.value());

			const PulseForge::Texture* ColorTexture = m_Target->GetColorTexture();
			const PulseForge::Texture* DepthTexture = m_Target->GetDepthTexture();
			if (!ColorTexture || !DepthTexture)
				throw std::runtime_error("RGBA16F target did not provide its requested color and depth attachments");
			const PulseForge::TextureDesc& ColorDescription = ColorTexture->GetDescription();
			if (ColorDescription.Width != 32 || ColorDescription.Height != 32 ||
				ColorDescription.Format != PulseForge::TextureFormat::RGBA16_Float ||
				!PulseForge::HasTextureUsage(ColorDescription.Usage, PulseForge::TextureUsage::ColorAttachment) ||
				!PulseForge::HasTextureUsage(ColorDescription.Usage, PulseForge::TextureUsage::ShaderResource))
				throw std::runtime_error("RGBA16F target color texture has unexpected dimensions, format, or usage");
			if (DepthTexture->GetDescription().Format != PulseForge::TextureFormat::Depth32Float)
				throw std::runtime_error("RGBA16F target has an unexpected depth attachment format");
			PF_INFO("Created 32x32 RGBA16_Float color+shader-resource target with D32 attachment");

			const std::filesystem::path ShaderDirectory(PF_GPU_VALIDATION_SHADER_DIRECTORY);
			const std::vector<std::byte> VertexBytecode = LoadShader(ShaderDirectory / "GpuRendererValidation.vs.spv");
			const std::vector<std::byte> ColorBytecode = LoadShader(ShaderDirectory / "GpuRendererValidation.color.ps.spv");
			const std::vector<std::byte> SamplingBytecode = LoadShader(ShaderDirectory / "GpuRendererValidation.sample.ps.spv");

			PulseForge::ShaderDesc VertexShaderDescription;
			VertexShaderDescription.Stage = PulseForge::ShaderStage::Vertex;
			VertexShaderDescription.EntryPoint = "VSMain";
			VertexShaderDescription.DebugName = "RGBA16F validation vertex shader";
			auto VertexShaderResult = App.CreateShader(VertexShaderDescription, VertexBytecode);
			if (!VertexShaderResult)
				throw std::runtime_error("GPU-validation vertex shader creation failed: " + VertexShaderResult.error().Message);

			PulseForge::ShaderDesc ColorShaderDescription;
			ColorShaderDescription.Stage = PulseForge::ShaderStage::Fragment;
			ColorShaderDescription.EntryPoint = "PSColor";
			ColorShaderDescription.DebugName = "RGBA16F validation color shader";
			auto ColorShaderResult = App.CreateShader(ColorShaderDescription, ColorBytecode);
			if (!ColorShaderResult)
				throw std::runtime_error("GPU-validation color shader creation failed: " + ColorShaderResult.error().Message);

			PulseForge::ShaderDesc SamplingShaderDescription = ColorShaderDescription;
			SamplingShaderDescription.EntryPoint = "PSSample";
			SamplingShaderDescription.DebugName = "RGBA16F validation sampling shader";
			auto SamplingShaderResult = App.CreateShader(SamplingShaderDescription, SamplingBytecode);
			if (!SamplingShaderResult)
				throw std::runtime_error("GPU-validation sampling shader creation failed: " + SamplingShaderResult.error().Message);

			PulseForge::BindingLayoutDesc LayoutDescription;
			LayoutDescription.Visibility = PulseForge::ShaderVisibility::Fragment;
			LayoutDescription.Items = {
				{ PulseForge::BindingResourceType::Texture2D, 0 },
				{ PulseForge::BindingResourceType::Sampler, 0 }
			};
			LayoutDescription.DebugName = "RGBA16F validation sampling layout";
			auto LayoutResult = App.CreateBindingLayout(LayoutDescription);
			if (!LayoutResult)
				throw std::runtime_error("GPU-validation binding-layout creation failed: " + LayoutResult.error().Message);
			m_BindingLayout = std::move(LayoutResult.value());

			PulseForge::SamplerDesc SamplerDescription;
			SamplerDescription.Minification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.Magnification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.AddressU = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.AddressV = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.DebugName = "RGBA16F validation linear sampler";
			auto SamplerResult = App.CreateSampler(SamplerDescription);
			if (!SamplerResult)
				throw std::runtime_error("GPU-validation sampler creation failed: " + SamplerResult.error().Message);
			m_Sampler = std::move(SamplerResult.value());

			PulseForge::BindingSetDesc BindingDescription;
			BindingDescription.Layout = m_BindingLayout;
			BindingDescription.Textures.push_back({ 0, *ColorTexture, PulseForge::BindingResourceType::Texture2D });
			BindingDescription.Samplers.push_back({ 0, *m_Sampler });
			auto BindingResult = App.CreateBindingSet(BindingDescription);
			if (!BindingResult)
				throw std::runtime_error("GPU-validation sampling binding creation failed: " + BindingResult.error().Message);
			m_SamplingBindingSet = std::move(BindingResult.value());

			const std::array<ValidationVertex, 3> Vertices = {
				ValidationVertex{ { -1.0f, -1.0f }, { 0.0f, 1.0f } },
				ValidationVertex{ { 3.0f, -1.0f }, { 2.0f, 1.0f } },
				ValidationVertex{ { -1.0f, 3.0f }, { 0.0f, -1.0f } }
			};
			PulseForge::BufferDesc VertexBufferDescription;
			VertexBufferDescription.ByteSize = sizeof(Vertices);
			VertexBufferDescription.Usage = PulseForge::BufferUsage::Vertex;
			VertexBufferDescription.DebugName = "RGBA16F validation fullscreen triangle";
			auto VertexBufferResult = App.CreateBuffer(VertexBufferDescription, std::as_bytes(std::span(Vertices)));
			if (!VertexBufferResult)
				throw std::runtime_error("GPU-validation vertex-buffer creation failed: " + VertexBufferResult.error().Message);
			m_VertexBuffer = std::move(VertexBufferResult.value());

			PulseForge::GraphicsPipelineDesc ColorPipelineDescription;
			ColorPipelineDescription.VertexShader = std::move(VertexShaderResult.value());
			ColorPipelineDescription.FragmentShader = std::move(ColorShaderResult.value());
			ColorPipelineDescription.VertexLayout.Stride = sizeof(ValidationVertex);
			ColorPipelineDescription.VertexLayout.Attributes = {
				{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2, offsetof(ValidationVertex, Position) },
				{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(ValidationVertex, TexCoord) }
			};
			ColorPipelineDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA16_Float;
			ColorPipelineDescription.DepthAttachmentEnabled = true;
			ColorPipelineDescription.Depth.TestEnabled = true;
			ColorPipelineDescription.Depth.WriteEnabled = true;
			ColorPipelineDescription.DebugName = "RGBA16F validation target pipeline";
			auto ColorPipelineResult = App.CreateGraphicsPipeline(ColorPipelineDescription);
			if (!ColorPipelineResult)
				throw std::runtime_error("RGBA16F graphics-pipeline creation failed: " + ColorPipelineResult.error().Message);
			m_ColorPipeline = std::move(ColorPipelineResult.value());

			PulseForge::GraphicsPipelineDesc SamplingPipelineDescription = ColorPipelineDescription;
			SamplingPipelineDescription.FragmentShader = std::move(SamplingShaderResult.value());
			SamplingPipelineDescription.BindingLayouts = { m_BindingLayout };
			SamplingPipelineDescription.ColorFormat = PulseForge::ColorTargetFormat::Swapchain;
			SamplingPipelineDescription.Depth.TestEnabled = false;
			SamplingPipelineDescription.Depth.WriteEnabled = false;
			SamplingPipelineDescription.DebugName = "RGBA16F validation sampling pipeline";
			auto SamplingPipelineResult = App.CreateGraphicsPipeline(SamplingPipelineDescription);
			if (!SamplingPipelineResult)
				throw std::runtime_error("RGBA16F sampling-pipeline creation failed: " + SamplingPipelineResult.error().Message);
			m_SamplingPipeline = std::move(SamplingPipelineResult.value());
		}

		PulseForge::RenderTargetHandle m_Target;
		PulseForge::BufferHandle m_VertexBuffer;
		PulseForge::BindingLayoutHandle m_BindingLayout;
		PulseForge::SamplerHandle m_Sampler;
		PulseForge::BindingSetHandle m_SamplingBindingSet;
		PulseForge::GraphicsPipelineHandle m_ColorPipeline;
		PulseForge::GraphicsPipelineHandle m_SamplingPipeline;
		std::chrono::steady_clock::time_point m_StartedAt{};
		uint32_t m_CompletedFrames = 0;
		bool m_Succeeded = true;
	};

	class GpuValidationApplication final : public PulseForge::Application
	{
	public:
		GpuValidationApplication()
			: Application(PulseForge::RendererAPI::Vulkan)
		{
			auto Layer = std::make_unique<GpuValidationLayer>();
			m_Layer = Layer.get();
			PushLayer(std::move(Layer));
		}

		int RunValidation()
		{
			Run();
			return m_Layer && m_Layer->Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
		}

	private:
		GpuValidationLayer* m_Layer = nullptr;
	};
}

int main()
{
	PulseForge::Log::Init();
	try
	{
		GpuValidationApplication App;
		return App.RunValidation();
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("GPU validation startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
