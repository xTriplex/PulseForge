#include "Client/PulseForgeGame.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "ImGui/UI.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace
{
	std::vector<std::byte> ReadShaderBytecode(const std::filesystem::path& Path)
	{
		std::ifstream File(Path, std::ios::binary | std::ios::ate);
		if (!File)
			throw std::runtime_error("Could not open compiled shader bytecode: " + Path.string());

		const std::streampos End = File.tellg();
		if (End <= 0)
			throw std::runtime_error("Compiled shader bytecode is empty or unreadable: " + Path.string());

		std::vector<std::byte> Bytecode(static_cast<size_t>(End));
		File.seekg(0, std::ios::beg);
		File.read(reinterpret_cast<char*>(Bytecode.data()), static_cast<std::streamsize>(Bytecode.size()));
		if (!File)
			throw std::runtime_error("Could not read compiled shader bytecode: " + Path.string());
		return Bytecode;
	}
}

class ExampleLayer : public PulseForge::Layer
{
public:
	ExampleLayer()
		: Layer("Example")
	{
		struct VertexPositionColorTexCoord
		{
			float Position[3];
			float Color[3];
			float TexCoord[2];
		};
		const std::array<VertexPositionColorTexCoord, 6> InitialVertices = {{
			// Submit the smaller, nearer red triangle first.
			{ { 0.0f, 0.32f, 0.2f }, { 1.0f, 0.08f, 0.08f }, { 0.5f, 0.0f } },
			{ { 0.32f, -0.25f, 0.2f }, { 1.0f, 0.08f, 0.08f }, { 1.0f, 1.0f } },
			{ { -0.32f, -0.25f, 0.2f }, { 1.0f, 0.08f, 0.08f }, { 0.0f, 1.0f } },
			// Submit a larger, farther blue triangle afterward. Depth testing must preserve
			// the near triangle where the two overlap.
			{ { 0.0f, 0.72f, 0.8f }, { 0.08f, 0.2f, 1.0f }, { 0.5f, 0.0f } },
			{ { 0.72f, -0.64f, 0.8f }, { 0.08f, 0.2f, 1.0f }, { 1.0f, 1.0f } },
			{ { -0.72f, -0.64f, 0.8f }, { 0.08f, 0.2f, 1.0f }, { 0.0f, 1.0f } }
		}};
		PulseForge::BufferDesc BufferDescription;
		BufferDescription.ByteSize = sizeof(InitialVertices);
		BufferDescription.Usage = PulseForge::BufferUsage::Vertex;
		BufferDescription.DebugName = "PulseForge sample vertex buffer";

		auto CreatedBuffer = PulseForge::Application::Get().CreateBuffer(
			BufferDescription,
			std::as_bytes(std::span(InitialVertices)));
		if (!CreatedBuffer)
			throw std::runtime_error(CreatedBuffer.error().Message);

		m_VertexBuffer = std::move(CreatedBuffer.value());
		PF_INFO("Created sample renderer buffer ({0} bytes)", BufferDescription.ByteSize);

		if (PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan)
		{
			const std::filesystem::path ShaderDirectory =
				std::filesystem::current_path() / PF_SAMPLE_SHADER_DIRECTORY;
			auto VertexBytecode = ReadShaderBytecode(ShaderDirectory / "Triangle.vs.spv");
			auto FragmentBytecode = ReadShaderBytecode(ShaderDirectory / "Triangle.ps.spv");

			PulseForge::ShaderDesc VertexShaderDescription;
			VertexShaderDescription.Stage = PulseForge::ShaderStage::Vertex;
			VertexShaderDescription.EntryPoint = "VSMain";
			VertexShaderDescription.DebugName = "PulseForge triangle vertex shader";
			auto CreatedVertexShader = PulseForge::Application::Get().CreateShader(
				VertexShaderDescription,
				VertexBytecode);
			if (!CreatedVertexShader)
				throw std::runtime_error(CreatedVertexShader.error().Message);
			m_VertexShader = std::move(CreatedVertexShader.value());

			PulseForge::ShaderDesc FragmentShaderDescription;
			FragmentShaderDescription.Stage = PulseForge::ShaderStage::Fragment;
			FragmentShaderDescription.EntryPoint = "PSMain";
			FragmentShaderDescription.DebugName = "PulseForge triangle fragment shader";
			auto CreatedFragmentShader = PulseForge::Application::Get().CreateShader(
				FragmentShaderDescription,
				FragmentBytecode);
			if (!CreatedFragmentShader)
				throw std::runtime_error(CreatedFragmentShader.error().Message);
			m_FragmentShader = std::move(CreatedFragmentShader.value());

			const std::array<uint8_t, 16> TexturePixels = {{
				255, 255, 255, 255, 255, 150, 40, 255,
				40, 210, 255, 255, 230, 60, 180, 255
			}};
			PulseForge::TextureDesc TextureDescription;
			TextureDescription.Width = 2;
			TextureDescription.Height = 2;
			TextureDescription.Format = PulseForge::TextureFormat::RGBA8_Srgb;
			TextureDescription.DebugName = "PulseForge sample 2x2 texture";
			auto CreatedTexture = PulseForge::Application::Get().CreateTexture(
				TextureDescription,
				std::as_bytes(std::span(TexturePixels)));
			if (!CreatedTexture)
				throw std::runtime_error(CreatedTexture.error().Message);
			m_Texture = std::move(CreatedTexture.value());

			PulseForge::SamplerDesc SamplerDescription;
			SamplerDescription.Minification = PulseForge::SamplerFilter::Nearest;
			SamplerDescription.Magnification = PulseForge::SamplerFilter::Nearest;
			SamplerDescription.AddressU = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.AddressV = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.DebugName = "PulseForge sample texture sampler";
			auto CreatedSampler = PulseForge::Application::Get().CreateSampler(SamplerDescription);
			if (!CreatedSampler)
				throw std::runtime_error(CreatedSampler.error().Message);
			m_Sampler = std::move(CreatedSampler.value());

			const std::array<float, 4> Tint = { 1.0f, 0.82f, 0.9f, 1.0f };
			PulseForge::BufferDesc ConstantBufferDescription;
			ConstantBufferDescription.ByteSize = sizeof(Tint);
			ConstantBufferDescription.Usage = PulseForge::BufferUsage::Constant;
			ConstantBufferDescription.DebugName = "PulseForge sample tint constants";
			auto CreatedConstantBuffer = PulseForge::Application::Get().CreateBuffer(
				ConstantBufferDescription,
				std::as_bytes(std::span(Tint)));
			if (!CreatedConstantBuffer)
				throw std::runtime_error(CreatedConstantBuffer.error().Message);
			m_ConstantBuffer = std::move(CreatedConstantBuffer.value());

			PulseForge::BindingLayoutDesc BindingLayoutDescription;
			BindingLayoutDescription.Visibility = PulseForge::ShaderStage::Fragment;
			BindingLayoutDescription.Items = {
				{ PulseForge::BindingResourceType::Texture2D, 0 },
				{ PulseForge::BindingResourceType::Sampler, 0 },
				{ PulseForge::BindingResourceType::ConstantBuffer, 0 }
			};
			BindingLayoutDescription.DebugName = "PulseForge sample fragment resources";
			auto CreatedBindingLayout = PulseForge::Application::Get().CreateBindingLayout(BindingLayoutDescription);
			if (!CreatedBindingLayout)
				throw std::runtime_error(CreatedBindingLayout.error().Message);
			m_BindingLayout = std::move(CreatedBindingLayout.value());

			PulseForge::BindingSetDesc BindingSetDescription;
			BindingSetDescription.Layout = m_BindingLayout;
			BindingSetDescription.Textures.push_back({ 0, std::cref(*m_Texture) });
			BindingSetDescription.Samplers.push_back({ 0, std::cref(*m_Sampler) });
			BindingSetDescription.Buffers.push_back({ 0, std::cref(*m_ConstantBuffer) });
			auto CreatedBindingSet = PulseForge::Application::Get().CreateBindingSet(BindingSetDescription);
			if (!CreatedBindingSet)
				throw std::runtime_error(CreatedBindingSet.error().Message);
			m_BindingSet = std::move(CreatedBindingSet.value());

			PulseForge::GraphicsPipelineDesc PipelineDescription;
			PipelineDescription.VertexShader = m_VertexShader;
			PipelineDescription.FragmentShader = m_FragmentShader;
			PipelineDescription.BindingLayouts = { m_BindingLayout };
			PipelineDescription.VertexLayout.Stride = sizeof(VertexPositionColorTexCoord);
			PipelineDescription.VertexLayout.Attributes = {
				{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float3, offsetof(VertexPositionColorTexCoord, Position) },
				{ PulseForge::VertexSemantic::Color, PulseForge::VertexFormat::Float3, offsetof(VertexPositionColorTexCoord, Color) },
				{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(VertexPositionColorTexCoord, TexCoord) }
			};
			PipelineDescription.Rasterizer.Cull = PulseForge::CullMode::None;
			PipelineDescription.Depth.TestEnabled = true;
			PipelineDescription.Depth.WriteEnabled = true;
			PipelineDescription.Depth.Compare = PulseForge::DepthCompareOperation::Less;
			PipelineDescription.DebugName = "PulseForge triangle pipeline";
			auto CreatedPipeline = PulseForge::Application::Get().CreateGraphicsPipeline(PipelineDescription);
			if (!CreatedPipeline)
				throw std::runtime_error(CreatedPipeline.error().Message);
			m_Pipeline = std::move(CreatedPipeline.value());
		}
	}

	void OnUpdate(PulseForge::Timestep DeltaTime) override
	{
		(void)DeltaTime;
	}

	void OnRender() override
	{
		if (!m_Pipeline || m_DrawFailed)
			return;

		const PulseForge::DrawArguments NearTriangle{ 3, 1, 0, 0 };
		const PulseForge::DrawArguments FarTriangle{ 3, 1, 3, 0 };
		const std::array<const PulseForge::BindingSet*, 1> BindingSets = { m_BindingSet.get() };
		auto NearDraw = PulseForge::Application::Get().Draw(
			*m_Pipeline,
			*m_VertexBuffer,
			NearTriangle,
			BindingSets);
		if (!NearDraw)
		{
			PF_ERROR("Near depth-test sample draw failed: {0}", NearDraw.error().Message);
			m_DrawFailed = true;
			return;
		}

		auto FarDraw = PulseForge::Application::Get().Draw(
			*m_Pipeline,
			*m_VertexBuffer,
			FarTriangle,
			BindingSets);
		if (!FarDraw)
		{
			PF_ERROR("Far depth-test sample draw failed: {0}", FarDraw.error().Message);
			m_DrawFailed = true;
			return;
		}

		if (!m_LoggedDepthTestDraws)
		{
			m_LoggedDepthTestDraws = true;
			PF_INFO("Submitted overlapping depth-tested triangles: near first, far second");
		}
	}

	void OnImGuiRender() override
	{
		PulseForge::UI::BeginWindow("Pulseforge Debug");
		PulseForge::UI::Text("Welcome to the PulseForge UI System!");
		PulseForge::UI::Separator();

		if (PulseForge::UI::Button("Click Me!"))
		{
			PF_TRACE("Button was clicked from the UI!");
		}

		PulseForge::UI::EndWindow();

		PulseForge::UI::ShowDemoWindow();
	}

	void OnEvent(PulseForge::Event& Event) override
	{
		PF_TRACE("{0}", Event.ToString());
	}

private:
	PulseForge::BufferHandle m_VertexBuffer;
	PulseForge::TextureHandle m_Texture;
	PulseForge::SamplerHandle m_Sampler;
	PulseForge::BufferHandle m_ConstantBuffer;
	PulseForge::BindingLayoutHandle m_BindingLayout;
	PulseForge::BindingSetHandle m_BindingSet;
	PulseForge::ShaderHandle m_VertexShader;
	PulseForge::ShaderHandle m_FragmentShader;
	PulseForge::GraphicsPipelineHandle m_Pipeline;
	bool m_DrawFailed = false;
	bool m_LoggedDepthTestDraws = false;
};

PulseForgeGameApp::PulseForgeGameApp()
#if defined(PF_SAMPLE_RENDERER_OPENGL)
	: Application(PulseForge::RendererAPI::OpenGL)
#else
	: Application()
#endif

{
	PF_INFO("Hello, PulseForge!");

	PushLayer(std::make_unique<ExampleLayer>());
}

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication()
{
	return std::make_unique<PulseForgeGameApp>();
}
