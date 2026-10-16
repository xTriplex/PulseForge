#include "GpuValidation/ActiveFrameUploadValidation.h"
#include "GpuValidation/ValidationShader.h"

#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace PulseForge::GpuValidation
{
	namespace
	{
		struct ValidationVertex
		{
			float Position[2];
			float TexCoord[2];
		};

		class ActiveFrameUploadValidationLayer final : public Layer
		{
		public:
			ActiveFrameUploadValidationLayer() : Layer("Active-frame resource upload validation") {}

			void OnAttach() override
			{
				m_StartedAt = std::chrono::steady_clock::now();
				try
				{
					InitializeResources();
					PF_INFO("Active-frame resource upload validation resources initialized");
				}
				catch (const std::exception& Exception)
				{
					Fail(Exception.what());
				}
			}

			void OnUpdate(Timestep) override
			{
				if (m_CompletedFrames == 3)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; initialized a vertex buffer and shader-resource texture on each of three active "
							"frame command lists, sampled and drew them, and submitted all frames");
					Application::Get().RequestClose();
				}
				else if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(15))
					Fail("Timed out during active-frame resource upload validation");
			}

			void OnRender() override
			{
				if (!m_Succeeded || m_CompletedFrames >= 3) return;

				Application& App = Application::Get();
				std::array<std::byte, 4 * 4 * 4> Pixels{};
				for (size_t Pixel = 0; Pixel < Pixels.size() / 4; ++Pixel)
				{
					Pixels[Pixel * 4 + 0] = static_cast<std::byte>(48 + m_CompletedFrames * 64);
					Pixels[Pixel * 4 + 1] = static_cast<std::byte>(192 - m_CompletedFrames * 32);
					Pixels[Pixel * 4 + 2] = static_cast<std::byte>(96 + m_CompletedFrames * 24);
					Pixels[Pixel * 4 + 3] = std::byte{ 0xff };
				}

				TextureDesc TextureDescription;
				TextureDescription.Width = 4;
				TextureDescription.Height = 4;
				TextureDescription.Format = TextureFormat::RGBA8_UNorm;
				TextureDescription.Usage = TextureUsage::ShaderResource;
				TextureDescription.DebugName = "Active-frame upload validation texture " + std::to_string(m_CompletedFrames);
				const TextureSubresourceData Subresource{ 0, 0, Pixels, 0 };
				auto TextureResult = App.CreateTexture(TextureDescription, std::span(&Subresource, 1));
				if (!TextureResult)
				{
					Fail("Could not upload a texture while the frame command list was open: " + TextureResult.error().Message);
					return;
				}
				TextureHandle Texture = std::move(*TextureResult);

				const std::array<ValidationVertex, 3> Vertices = { ValidationVertex{ { -1.0f, -1.0f }, { 0.0f, 1.0f } },
					ValidationVertex{ { 3.0f, -1.0f }, { 2.0f, 1.0f } }, ValidationVertex{ { -1.0f, 3.0f }, { 0.0f, -1.0f } } };
				BufferDesc BufferDescription;
				BufferDescription.ByteSize = sizeof(Vertices);
				BufferDescription.Usage = BufferUsage::Vertex;
				BufferDescription.DebugName = "Active-frame upload validation vertices " + std::to_string(m_CompletedFrames);
				auto BufferResult = App.CreateBuffer(BufferDescription, std::as_bytes(std::span(Vertices)));
				if (!BufferResult)
				{
					Fail("Could not upload a vertex buffer while the frame command list was open: " + BufferResult.error().Message);
					return;
				}
				BufferHandle Buffer = std::move(*BufferResult);

				BindingSetDesc BindingDescription;
				BindingDescription.Layout = m_BindingLayout;
				BindingDescription.Textures.push_back({ 0, *Texture, BindingResourceType::Texture2D });
				BindingDescription.Samplers.push_back({ 0, *m_Sampler });
				auto BindingResult = App.CreateBindingSet(BindingDescription);
				if (!BindingResult)
				{
					Fail("Could not bind the active-frame uploaded texture: " + BindingResult.error().Message);
					return;
				}
				BindingSetHandle Binding = std::move(*BindingResult);
				const std::array<const BindingSet*, 1> BindingSets = { Binding.get() };
				if (const GraphicsResult Draw = App.Draw(*m_Pipeline, *Buffer, { 3, 1, 0, 0 }, BindingSets); !Draw)
				{
					Fail("Could not draw resources uploaded in the active frame: " + Draw.error().Message);
					return;
				}

				m_Textures.push_back(std::move(Texture));
				m_Buffers.push_back(std::move(Buffer));
				m_BindingSets.push_back(std::move(Binding));
				++m_CompletedFrames;
			}

			void OnDetach() override
			{
				m_BindingSets.clear();
				m_Buffers.clear();
				m_Textures.clear();
				m_Pipeline.reset();
				m_Sampler.reset();
				m_BindingLayout.reset();
			}

			[[nodiscard]] bool Succeeded() const noexcept { return m_Succeeded && m_CompletedFrames == 3; }

		private:
			void Fail(const std::string& Message)
			{
				m_Succeeded = false;
				PF_ERROR("GPU_VALIDATION_RESULT=FAIL: {0}", Message);
				Application::Get().RequestClose();
			}

			void InitializeResources()
			{
				Application& App = Application::Get();
				const std::filesystem::path ShaderDirectory(PF_GPU_VALIDATION_SHADER_DIRECTORY);
				const std::vector<std::byte> VertexBytecode = LoadValidationShader(ShaderDirectory / "GpuRendererValidation.vs.spv");
				const std::vector<std::byte> SamplingBytecode =
					LoadValidationShader(ShaderDirectory / "GpuRendererValidation.sample.ps.spv");

				ShaderDesc VertexDescription;
				VertexDescription.Stage = ShaderStage::Vertex;
				VertexDescription.EntryPoint = "VSMain";
				VertexDescription.DebugName = "Active-frame upload validation vertex shader";
				auto VertexShader = App.CreateShader(VertexDescription, VertexBytecode);
				if (!VertexShader)
					throw std::runtime_error("GPU-validation vertex shader creation failed: " + VertexShader.error().Message);

				ShaderDesc SamplingDescription;
				SamplingDescription.Stage = ShaderStage::Fragment;
				SamplingDescription.EntryPoint = "PSSample";
				SamplingDescription.DebugName = "Active-frame upload validation sampling shader";
				auto SamplingShader = App.CreateShader(SamplingDescription, SamplingBytecode);
				if (!SamplingShader)
					throw std::runtime_error("GPU-validation sampling shader creation failed: " + SamplingShader.error().Message);

				BindingLayoutDesc LayoutDescription;
				LayoutDescription.Visibility = ShaderVisibility::Fragment;
				LayoutDescription.Items = { { BindingResourceType::Texture2D, 0 }, { BindingResourceType::Sampler, 0 } };
				LayoutDescription.DebugName = "Active-frame upload validation sampling layout";
				auto Layout = App.CreateBindingLayout(LayoutDescription);
				if (!Layout) throw std::runtime_error("GPU-validation binding-layout creation failed: " + Layout.error().Message);
				m_BindingLayout = std::move(*Layout);

				SamplerDesc SamplerDescription;
				SamplerDescription.Minification = SamplerFilter::Linear;
				SamplerDescription.Magnification = SamplerFilter::Linear;
				SamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
				SamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
				SamplerDescription.DebugName = "Active-frame upload validation linear sampler";
				auto Sampler = App.CreateSampler(SamplerDescription);
				if (!Sampler) throw std::runtime_error("GPU-validation sampler creation failed: " + Sampler.error().Message);
				m_Sampler = std::move(*Sampler);

				GraphicsPipelineDesc PipelineDescription;
				PipelineDescription.VertexShader = std::move(*VertexShader);
				PipelineDescription.FragmentShader = std::move(*SamplingShader);
				PipelineDescription.BindingLayouts = { m_BindingLayout };
				PipelineDescription.VertexLayout.Stride = sizeof(ValidationVertex);
				PipelineDescription.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2,
																	offsetof(ValidationVertex, Position) },
					{ VertexSemantic::TexCoord, VertexFormat::Float2, offsetof(ValidationVertex, TexCoord) } };
				PipelineDescription.ColorFormat = ColorTargetFormat::Swapchain;
				PipelineDescription.DepthAttachmentEnabled = true;
				PipelineDescription.DebugName = "Active-frame upload validation sampling pipeline";
				auto Pipeline = App.CreateGraphicsPipeline(PipelineDescription);
				if (!Pipeline) throw std::runtime_error("GPU-validation sampling-pipeline creation failed: " + Pipeline.error().Message);
				m_Pipeline = std::move(*Pipeline);
			}

			std::chrono::steady_clock::time_point m_StartedAt;
			BindingLayoutHandle m_BindingLayout;
			SamplerHandle m_Sampler;
			GraphicsPipelineHandle m_Pipeline;
			std::vector<TextureHandle> m_Textures;
			std::vector<BufferHandle> m_Buffers;
			std::vector<BindingSetHandle> m_BindingSets;
			uint32_t m_CompletedFrames = 0;
			bool m_Succeeded = true;
		};

		class ActiveFrameUploadValidationApplication final : public Application
		{
		public:
			ActiveFrameUploadValidationApplication() : Application(RendererAPI::Vulkan)
			{
				auto Layer = std::make_unique<ActiveFrameUploadValidationLayer>();
				m_Layer = Layer.get();
				PushLayer(std::move(Layer));
			}

			int RunValidation()
			{
				Run();
				return m_Layer && m_Layer->Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
			}

		private:
			ActiveFrameUploadValidationLayer* m_Layer = nullptr;
		};
	} // namespace

	int RunActiveFrameUploadValidation()
	{
		ActiveFrameUploadValidationApplication App;
		return App.RunValidation();
	}
} // namespace PulseForge::GpuValidation
