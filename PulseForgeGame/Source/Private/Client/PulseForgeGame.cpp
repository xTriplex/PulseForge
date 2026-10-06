#include "Client/PulseForgeGame.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "Events/ApplicationEvent.h"
#include "Events/Event.h"
#include "ImGui/UI.h"
#include "Scene/Scene.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <expected>
#include <string>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

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
		const std::array<VertexPositionColorTexCoord, 24> CubeVertices = {{
			{ { -0.5f, -0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f } },
			{ {  0.5f, -0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f } },
			{ {  0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { -0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } },

			{ {  0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f } },
			{ { -0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f } },
			{ { -0.5f,  0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
			{ {  0.5f,  0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } },

			{ {  0.5f, -0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f } },
			{ {  0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f } },
			{ {  0.5f,  0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
			{ {  0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } },

			{ { -0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f } },
			{ { -0.5f, -0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f } },
			{ { -0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { -0.5f,  0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } },

			{ { -0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f } },
			{ {  0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f } },
			{ {  0.5f,  0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { -0.5f,  0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } },

			{ { -0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f } },
			{ {  0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f } },
			{ {  0.5f, -0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { -0.5f, -0.5f,  0.5f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } }
		}};
		const std::array<uint32_t, 36> CubeIndices = {{
			0, 1, 2, 0, 2, 3,
			4, 5, 6, 4, 6, 7,
			8, 9, 10, 8, 10, 11,
			12, 13, 14, 12, 14, 15,
			16, 17, 18, 16, 18, 19,
			20, 21, 22, 20, 22, 23
		}};

		PulseForge::MeshDesc MeshDescription;
		MeshDescription.VertexLayout.Stride = sizeof(VertexPositionColorTexCoord);
		MeshDescription.VertexLayout.Attributes = {
			{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float3, offsetof(VertexPositionColorTexCoord, Position) },
			{ PulseForge::VertexSemantic::Color, PulseForge::VertexFormat::Float3, offsetof(VertexPositionColorTexCoord, Color) },
			{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(VertexPositionColorTexCoord, TexCoord) }
		};
		MeshDescription.VertexData = std::as_bytes(std::span(CubeVertices));
		MeshDescription.Indices = CubeIndices;
		MeshDescription.DebugName = "PulseForge sample textured cube";

		if (PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan)
		{
			InitializeValidationScene();

			auto CreatedMesh = PulseForge::Application::Get().CreateMesh(MeshDescription);
			if (!CreatedMesh)
				throw std::runtime_error(CreatedMesh.error().Message);
			m_Mesh = std::move(CreatedMesh.value());
			PF_INFO("Created indexed sample mesh ({0} vertices, {1} indices)",
				m_Mesh->GetVertexCount(),
				m_Mesh->GetIndexCount());

			const std::filesystem::path ShaderDirectory =
				std::filesystem::current_path() / PF_SAMPLE_SHADER_DIRECTORY;
			auto VertexBytecode = ReadShaderBytecode(ShaderDirectory / "Triangle.vs.spv");
			auto FragmentBytecode = ReadShaderBytecode(ShaderDirectory / "Triangle.ps.spv");

			PulseForge::ShaderDesc VertexShaderDescription;
			VertexShaderDescription.Stage = PulseForge::ShaderStage::Vertex;
			VertexShaderDescription.EntryPoint = "VSMain";
			VertexShaderDescription.DebugName = "PulseForge cube vertex shader";
			auto CreatedVertexShader = PulseForge::Application::Get().CreateShader(
				VertexShaderDescription,
				VertexBytecode);
			if (!CreatedVertexShader)
				throw std::runtime_error(CreatedVertexShader.error().Message);
			m_VertexShader = std::move(CreatedVertexShader.value());

			PulseForge::ShaderDesc FragmentShaderDescription;
			FragmentShaderDescription.Stage = PulseForge::ShaderStage::Fragment;
			FragmentShaderDescription.EntryPoint = "PSMain";
			FragmentShaderDescription.DebugName = "PulseForge cube fragment shader";
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
			BindingLayoutDescription.Visibility = PulseForge::ShaderVisibility::AllGraphics;
			BindingLayoutDescription.Items = {
				{ PulseForge::BindingResourceType::Texture2D, 0 },
				{ PulseForge::BindingResourceType::Sampler, 0 },
				{ PulseForge::BindingResourceType::ConstantBuffer, 0 },
				{ PulseForge::BindingResourceType::ConstantBuffer, 1 }
			};
			BindingLayoutDescription.DebugName = "PulseForge sample shader resources";
			auto CreatedBindingLayout = PulseForge::Application::Get().CreateBindingLayout(BindingLayoutDescription);
			if (!CreatedBindingLayout)
				throw std::runtime_error(CreatedBindingLayout.error().Message);
			m_BindingLayout = std::move(CreatedBindingLayout.value());

			if (auto BindingResult = RebuildTransformBindings(); !BindingResult)
				throw std::runtime_error(BindingResult.error());

			PulseForge::GraphicsPipelineDesc PipelineDescription;
			PipelineDescription.VertexShader = m_VertexShader;
			PipelineDescription.FragmentShader = m_FragmentShader;
			PipelineDescription.BindingLayouts = { m_BindingLayout };
			PipelineDescription.VertexLayout = MeshDescription.VertexLayout;
			PipelineDescription.Rasterizer.Cull = PulseForge::CullMode::None;
			PipelineDescription.Depth.TestEnabled = true;
			PipelineDescription.Depth.WriteEnabled = true;
			PipelineDescription.Depth.Compare = PulseForge::DepthCompareOperation::Less;
			PipelineDescription.DebugName = "PulseForge textured cube pipeline";
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
		if (!m_Pipeline || !m_Mesh || m_DrawFailed)
			return;

		const PulseForge::DrawIndexedArguments Arguments{ m_Mesh->GetIndexCount(), 1, 0, 0 };
		const std::array<const PulseForge::BindingSet*, 1> BindingSets = { m_BindingSet.get() };
		auto DrawResult = PulseForge::Application::Get().DrawIndexed(
			*m_Pipeline,
			*m_Mesh,
			Arguments,
			BindingSets);
		if (!DrawResult)
		{
			PF_ERROR("Sample indexed mesh draw failed: {0}", DrawResult.error().Message);
			m_DrawFailed = true;
			return;
		}

		if (!m_LoggedDepthTestDraws)
		{
			m_LoggedDepthTestDraws = true;
			PF_INFO("Submitted indexed textured cube mesh with depth testing");
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
		PulseForge::EventDispatcher Dispatcher(Event);
		Dispatcher.Dispatch<PulseForge::WindowResizeEvent>([this](PulseForge::WindowResizeEvent&)
		{
			if (!m_BindingLayout)
				return false;

			const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
			if (Width == 0 || Height == 0)
				return false;

			if (auto BindingResult = RebuildTransformBindings(); !BindingResult)
				PF_ERROR("Could not update sample camera for resized framebuffer: {0}", BindingResult.error());
			return false;
		});
		PF_TRACE("{0}", Event.ToString());
	}

private:
	void InitializeValidationScene()
	{
		auto Cube = m_Scene.CreateEntity("Sample Cube");
		if (!Cube)
			throw std::runtime_error(Cube.error().Message);
		m_CubeEntity = *Cube;

		const glm::mat4 Model =
			glm::rotate(glm::mat4(1.0f), 0.48f, glm::vec3(0.0f, 1.0f, 0.0f)) *
			glm::rotate(glm::mat4(1.0f), -0.31f, glm::vec3(1.0f, 0.0f, 0.0f));
		PulseForge::TransformComponent CubeTransform;
		CubeTransform.Rotation = glm::quat_cast(Model);
		if (auto TransformResult = m_CubeEntity.SetTransform(CubeTransform); !TransformResult)
			throw std::runtime_error(TransformResult.error().Message);

		auto Camera = m_Scene.CreateEntity("Sample Camera");
		if (!Camera)
			throw std::runtime_error(Camera.error().Message);
		m_CameraEntity = *Camera;

		PulseForge::TransformComponent CameraTransform;
		CameraTransform.Translation = { 2.2f, 1.7f, 3.1f };
		CameraTransform.Rotation = glm::quatLookAtRH(
			glm::normalize(-CameraTransform.Translation),
			glm::vec3(0.0f, 1.0f, 0.0f));
		if (auto TransformResult = m_CameraEntity.SetTransform(CameraTransform); !TransformResult)
			throw std::runtime_error(TransformResult.error().Message);
		if (auto CameraResult = m_CameraEntity.SetCamera(PulseForge::CameraComponent{}); !CameraResult)
			throw std::runtime_error(CameraResult.error().Message);
	}

	[[nodiscard]] std::expected<glm::mat4, std::string> CreateModelViewProjection() const
	{
		const auto Model = m_CubeEntity.GetWorldMatrix();
		const auto CameraWorld = m_CameraEntity.GetWorldMatrix();
		const auto Camera = m_CameraEntity.GetCamera();
		if (!Model || !CameraWorld || !Camera || !Camera->has_value())
			return std::unexpected("Could not read the sample scene camera or model transform");

		const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
		if (Width == 0 || Height == 0)
			return std::unexpected("Cannot build the camera projection for a zero-sized framebuffer");

		const auto Projection = Camera->value().GetProjectionMatrix(static_cast<float>(Width) / static_cast<float>(Height));
		if (!Projection)
			return std::unexpected(Projection.error().Message);
		return *Projection * glm::inverse(*CameraWorld) * *Model;
	}

	[[nodiscard]] std::expected<void, std::string> RebuildTransformBindings()
	{
		const auto ModelViewProjection = CreateModelViewProjection();
		if (!ModelViewProjection)
			return std::unexpected(ModelViewProjection.error());

		PulseForge::BufferDesc TransformBufferDescription;
		TransformBufferDescription.ByteSize = sizeof(glm::mat4);
		TransformBufferDescription.Usage = PulseForge::BufferUsage::Constant;
		TransformBufferDescription.DebugName = "PulseForge sample camera and model constants";
		auto CreatedTransformBuffer = PulseForge::Application::Get().CreateBuffer(
			TransformBufferDescription,
			std::as_bytes(std::span(&ModelViewProjection.value(), 1)));
		if (!CreatedTransformBuffer)
			return std::unexpected(CreatedTransformBuffer.error().Message);

		PulseForge::BindingSetDesc BindingSetDescription;
		BindingSetDescription.Layout = m_BindingLayout;
		BindingSetDescription.Textures.push_back({ 0, std::cref(*m_Texture) });
		BindingSetDescription.Samplers.push_back({ 0, std::cref(*m_Sampler) });
		BindingSetDescription.Buffers.push_back({ 0, std::cref(*m_ConstantBuffer) });
		BindingSetDescription.Buffers.push_back({ 1, std::cref(*CreatedTransformBuffer.value()) });
		auto CreatedBindingSet = PulseForge::Application::Get().CreateBindingSet(BindingSetDescription);
		if (!CreatedBindingSet)
			return std::unexpected(CreatedBindingSet.error().Message);

		m_BindingSet = std::move(CreatedBindingSet.value());
		m_TransformBuffer = std::move(CreatedTransformBuffer.value());
		return {};
	}

	PulseForge::Scene m_Scene;
	PulseForge::Entity m_CubeEntity;
	PulseForge::Entity m_CameraEntity;
	PulseForge::MeshHandle m_Mesh;
	PulseForge::TextureHandle m_Texture;
	PulseForge::SamplerHandle m_Sampler;
	PulseForge::BufferHandle m_ConstantBuffer;
	PulseForge::BufferHandle m_TransformBuffer;
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
