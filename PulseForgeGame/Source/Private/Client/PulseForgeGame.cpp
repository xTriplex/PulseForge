#include "Client/PulseForgeGame.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MeshAssetCache.h"
#include "Assets/SceneAssetService.h"
#include "Assets/TextureAssetCache.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "Events/Event.h"
#include "ImGui/UI.h"
#include "Scene/Scene.h"
#include "Scene/SceneRenderSnapshot.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <expected>
#include <string>
#include <unordered_set>
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

			const auto [FramebufferWidth, FramebufferHeight] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
			if (FramebufferWidth == 0 || FramebufferHeight == 0)
				throw std::runtime_error("The sample window has no drawable framebuffer during renderer setup");
			const auto InitialSnapshot = PulseForge::SceneRenderSnapshotBuilder::Build(
				m_Scene,
				m_CameraEntity,
				static_cast<float>(FramebufferWidth) / static_cast<float>(FramebufferHeight));
			if (!InitialSnapshot)
				throw std::runtime_error("Could not build the initial scene render snapshot: " + InitialSnapshot.error().Message);
			if (InitialSnapshot->Meshes.empty())
				throw std::runtime_error("The validation scene does not contain a mesh-renderer entity");

			auto InitialMesh = m_MeshAssetCache->GetOrLoad(InitialSnapshot->Meshes.front().MeshAsset);
			if (!InitialMesh)
				throw std::runtime_error(InitialMesh.error().Message);
			const PulseForge::Mesh& Mesh = InitialMesh->get();
			PF_INFO("Resolved the validation scene's initial mesh asset as indexed geometry ({0} vertices, {1} indices)",
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
		if (!m_Pipeline || !m_MeshAssetCache)
			return;

		const auto [FramebufferWidth, FramebufferHeight] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
		if (FramebufferWidth == 0 || FramebufferHeight == 0)
			return;

		const auto Snapshot = PulseForge::SceneRenderSnapshotBuilder::Build(
			m_Scene,
			m_CameraEntity,
			static_cast<float>(FramebufferWidth) / static_cast<float>(FramebufferHeight));
		if (!Snapshot)
		{
			if (!m_LoggedSnapshotFailure)
			{
				PF_ERROR("Could not build the sample scene render snapshot: {0}", Snapshot.error().Message);
				m_LoggedSnapshotFailure = true;
			}
			return;
		}
		m_LoggedSnapshotFailure = false;

		const std::array<const PulseForge::BindingSet*, 1> BindingSets = { m_BindingSet.get() };
		size_t SubmittedDraws = 0;
		for (const PulseForge::SceneMeshInstance& Instance : Snapshot->Meshes)
		{
			auto Mesh = m_MeshAssetCache->GetOrLoad(Instance.MeshAsset);
			if (!Mesh)
			{
				if (m_ReportedMeshFailures.insert(Instance.MeshAsset).second)
					PF_ERROR("Could not resolve mesh asset {0}: {1}", Instance.MeshAsset.ToString(), Mesh.error().Message);
				continue;
			}

			const glm::mat4 ModelViewProjection = Snapshot->ViewProjection * Instance.WorldTransform;
			const auto UpdateResult = PulseForge::Application::Get().WriteBuffer(
				*m_TransformBuffer,
				0,
				std::as_bytes(std::span(&ModelViewProjection, 1)));
			if (!UpdateResult)
			{
				if (!m_LoggedTransformUpdateFailure)
				{
					PF_ERROR("Could not update sample per-object camera constants: {0}", UpdateResult.error().Message);
					m_LoggedTransformUpdateFailure = true;
				}
				return;
			}
			m_LoggedTransformUpdateFailure = false;

			const PulseForge::DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
			const auto DrawResult = PulseForge::Application::Get().DrawIndexed(
				*m_Pipeline,
				Mesh->get(),
				Arguments,
				BindingSets);
			if (!DrawResult)
			{
				if (m_ReportedMeshFailures.insert(Instance.MeshAsset).second)
					PF_ERROR("Could not draw mesh asset {0}: {1}", Instance.MeshAsset.ToString(), DrawResult.error().Message);
				continue;
			}
			++SubmittedDraws;
		}

		if (SubmittedDraws > 0 && !m_LoggedSceneDraw)
		{
			m_LoggedSceneDraw = true;
			PF_INFO("Submitted {0} indexed mesh instance(s) from the scene snapshot using {1} cached mesh asset(s)",
				SubmittedDraws,
				m_MeshAssetCache->GetLoadedCount());
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
	void LoadValidationScene(const std::filesystem::path& ProjectRoot)
	{
		const auto SceneAsset = PulseForge::UUID::Parse("b101a2e7-582d-4dfb-ae1e-8ce41fb375ce");
		if (!SceneAsset)
			throw std::runtime_error(SceneAsset.error().Message);
		if (auto LoadResult = PulseForge::SceneAssetService::Load(*SceneAsset, ProjectRoot, m_AssetRegistry, m_Scene);
			!LoadResult)
			throw std::runtime_error("Could not load validation scene: " + LoadResult.error().Message);

		const auto CameraID = PulseForge::UUID::Parse("c312582b-32cb-4811-9b93-4917d7bb6096");
		if (!CameraID)
			throw std::runtime_error("Validation scene camera UUID is invalid");
		const auto Camera = m_Scene.FindEntity(*CameraID);
		if (!Camera)
			throw std::runtime_error("Validation scene is missing its expected camera entity");
		m_CameraEntity = *CameraID;
	}

	[[nodiscard]] std::expected<void, std::string> CreateTransformBindings()
	{
		const glm::mat4 InitialTransform(1.0f);
		PulseForge::BufferDesc TransformBufferDescription;
		TransformBufferDescription.ByteSize = sizeof(InitialTransform);
		TransformBufferDescription.Usage = PulseForge::BufferUsage::Constant;
		TransformBufferDescription.DebugName = "PulseForge sample per-object camera constants";
		auto CreatedTransformBuffer = PulseForge::Application::Get().CreateBuffer(
			TransformBufferDescription,
			std::as_bytes(std::span(&InitialTransform, 1)));
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

	PulseForge::Scene m_Scene;
	PulseForge::UUID m_CameraEntity;
	PulseForge::AssetRegistry m_AssetRegistry;
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
	bool m_LoggedTransformUpdateFailure = false;
	bool m_LoggedSnapshotFailure = false;
	bool m_LoggedSceneDraw = false;
	std::unordered_set<PulseForge::AssetID, PulseForge::UUIDHash> m_ReportedMeshFailures;
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
