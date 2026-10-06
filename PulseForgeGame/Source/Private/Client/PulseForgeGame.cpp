#include "Client/PulseForgeGame.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MeshAssetCache.h"
#include "Assets/SceneAssetService.h"
#include "Assets/TextureAssetCache.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "Events/ApplicationEvent.h"
#include "Events/Event.h"
#include "ImGui/UI.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Scene.h"
#include "Scene/SceneRenderSnapshot.h"

#include <algorithm>
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
		if (PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan)
		{
			const std::filesystem::path ProjectRoot = std::filesystem::current_path();
			if (auto RegistryResult = m_AssetRegistry.Rebuild(ProjectRoot); !RegistryResult)
			{
				const std::string Error = RegistryResult.error().Issues.empty()
					? "unknown registry error"
					: RegistryResult.error().Issues.front().Message;
				throw std::runtime_error("Could not build sample asset registry: " + Error);
			}
			LoadValidationScene(ProjectRoot);
			m_MeshAssetCache = std::make_unique<PulseForge::MeshAssetCache>(
				PulseForge::Application::Get(),
				ProjectRoot,
				m_AssetRegistry);
			m_TextureAssetCache = std::make_unique<PulseForge::TextureAssetCache>(
				PulseForge::Application::Get(),
				ProjectRoot,
				m_AssetRegistry);

			const auto MeshRenderer = m_CubeEntity.GetMeshRenderer();
			if (!MeshRenderer || !MeshRenderer->has_value())
				throw std::runtime_error("Could not read the validation mesh renderer component");

			m_MeshAssetID = MeshRenderer->value().MeshAsset;
			auto LoadedMesh = m_MeshAssetCache->GetOrLoad(m_MeshAssetID);
			if (!LoadedMesh)
				throw std::runtime_error(LoadedMesh.error().Message);
			const PulseForge::Mesh& Mesh = LoadedMesh->get();
			PF_INFO("Resolved mesh asset {0} as indexed geometry ({1} vertices, {2} indices)",
				m_MeshAssetID.ToString(),
				Mesh.GetVertexCount(),
				Mesh.GetIndexCount());

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

			const auto TextureAssetID = PulseForge::UUID::Parse("4c9b0a26-5ca1-4d37-b451-f23231002f92");
			if (!TextureAssetID)
				throw std::runtime_error(TextureAssetID.error().Message);
			m_TextureAssetID = *TextureAssetID;
			auto CreatedTexture = m_TextureAssetCache->GetOrLoad(
				m_TextureAssetID,
				PulseForge::TextureFormat::RGBA8_Srgb);
			if (!CreatedTexture)
				throw std::runtime_error(CreatedTexture.error().Message);
			PF_INFO("Resolved image asset {0} as {1}x{2} sRGB texture",
				m_TextureAssetID.ToString(),
				CreatedTexture->get().GetDescription().Width,
				CreatedTexture->get().GetDescription().Height);

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

			if (auto BindingResult = CreateTransformBindings(); !BindingResult)
				throw std::runtime_error(BindingResult.error());

			PulseForge::GraphicsPipelineDesc PipelineDescription;
			PipelineDescription.VertexShader = m_VertexShader;
			PipelineDescription.FragmentShader = m_FragmentShader;
			PipelineDescription.BindingLayouts = { m_BindingLayout };
			PipelineDescription.VertexLayout = Mesh.GetVertexLayout();
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
		if (!m_Pipeline || !m_MeshAssetCache || m_DrawFailed)
			return;

		if (m_TransformBindingsDirty)
		{
			if (auto UpdateResult = UpdateTransformBuffer(); !UpdateResult)
			{
				if (!m_LoggedTransformUpdateFailure)
				{
					PF_ERROR("Could not update sample camera constants after resize: {0}", UpdateResult.error());
					m_LoggedTransformUpdateFailure = true;
				}
				return;
			}

			m_TransformBindingsDirty = false;
			m_LoggedTransformUpdateFailure = false;
		}

		const auto Mesh = m_MeshAssetCache->GetOrLoad(m_MeshAssetID);
		if (!Mesh)
		{
			PF_ERROR("Sample mesh asset resolution failed: {0}", Mesh.error().Message);
			m_DrawFailed = true;
			return;
		}
		const PulseForge::DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
		const std::array<const PulseForge::BindingSet*, 1> BindingSets = { m_BindingSet.get() };
		auto DrawResult = PulseForge::Application::Get().DrawIndexed(
			*m_Pipeline,
			Mesh->get(),
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

			m_TransformBindingsDirty = true;
			return false;
		});
		PF_TRACE("{0}", Event.ToString());
	}

private:
	void LoadValidationScene(const std::filesystem::path& ProjectRoot)
	{
		const auto SceneAsset = PulseForge::UUID::Parse("b101a2e7-582d-4dfb-ae1e-8ce41fb375ce");
		if (!SceneAsset)
			throw std::runtime_error(SceneAsset.error().Message);
		if (auto LoadResult = PulseForge::SceneAssetService::Load(*SceneAsset, ProjectRoot, m_AssetRegistry, m_Scene);
			!LoadResult)
			throw std::runtime_error("Could not load validation scene: " + LoadResult.error().Message);

		const auto CubeID = PulseForge::UUID::Parse("2e5f5605-08d3-4f7f-84d9-39ca49047701");
		const auto CameraID = PulseForge::UUID::Parse("c312582b-32cb-4811-9b93-4917d7bb6096");
		if (!CubeID || !CameraID)
			throw std::runtime_error("Validation scene entity UUID is invalid");
		const auto Cube = m_Scene.FindEntity(*CubeID);
		const auto Camera = m_Scene.FindEntity(*CameraID);
		if (!Cube || !Camera)
			throw std::runtime_error("Validation scene is missing its expected cube or camera entity");
		m_CubeEntity = *Cube;
		m_CameraEntity = *Camera;
	}

	[[nodiscard]] std::expected<glm::mat4, std::string> CreateModelViewProjection() const
	{
		const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
		if (Width == 0 || Height == 0)
			return std::unexpected("Cannot build the camera projection for a zero-sized framebuffer");

		const auto Snapshot = PulseForge::SceneRenderSnapshotBuilder::Build(
			m_Scene,
			m_CameraEntity.GetUUID(),
			static_cast<float>(Width) / static_cast<float>(Height));
		if (!Snapshot)
			return std::unexpected(Snapshot.error().Message);

		const auto Mesh = std::find_if(Snapshot->Meshes.begin(), Snapshot->Meshes.end(), [this](const auto& Instance)
		{
			return Instance.Entity == m_CubeEntity.GetUUID();
		});
		if (Mesh == Snapshot->Meshes.end())
			return std::unexpected("Sample scene has no mesh-renderer instance for its validation cube");
		return Snapshot->ViewProjection * Mesh->WorldTransform;
	}

	[[nodiscard]] std::expected<void, std::string> CreateTransformBindings()
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
		const auto Texture = m_TextureAssetCache->GetOrLoad(m_TextureAssetID, PulseForge::TextureFormat::RGBA8_Srgb);
		if (!Texture)
			return std::unexpected(Texture.error().Message);
		BindingSetDescription.Textures.push_back({ 0, std::cref(Texture->get()) });
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

	[[nodiscard]] std::expected<void, std::string> UpdateTransformBuffer()
	{
		const auto ModelViewProjection = CreateModelViewProjection();
		if (!ModelViewProjection)
			return std::unexpected(ModelViewProjection.error());

		const auto UpdateResult = PulseForge::Application::Get().WriteBuffer(
			*m_TransformBuffer,
			0,
			std::as_bytes(std::span(&ModelViewProjection.value(), 1)));
		if (!UpdateResult)
			return std::unexpected(UpdateResult.error().Message);

		return {};
	}

	PulseForge::Scene m_Scene;
	PulseForge::Entity m_CubeEntity;
	PulseForge::Entity m_CameraEntity;
	PulseForge::AssetRegistry m_AssetRegistry;
	PulseForge::AssetID m_MeshAssetID;
	PulseForge::AssetID m_TextureAssetID;
	std::unique_ptr<PulseForge::MeshAssetCache> m_MeshAssetCache;
	std::unique_ptr<PulseForge::TextureAssetCache> m_TextureAssetCache;
	PulseForge::SamplerHandle m_Sampler;
	PulseForge::BufferHandle m_ConstantBuffer;
	PulseForge::BufferHandle m_TransformBuffer;
	PulseForge::BindingLayoutHandle m_BindingLayout;
	PulseForge::BindingSetHandle m_BindingSet;
	PulseForge::ShaderHandle m_VertexShader;
	PulseForge::ShaderHandle m_FragmentShader;
	PulseForge::GraphicsPipelineHandle m_Pipeline;
	bool m_TransformBindingsDirty = false;
	bool m_LoggedTransformUpdateFailure = false;
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
