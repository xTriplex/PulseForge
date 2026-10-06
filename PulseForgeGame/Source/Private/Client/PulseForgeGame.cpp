#include "Client/PulseForgeGame.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAssetCache.h"
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
#include <unordered_map>
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
			m_MaterialAssetCache = std::make_unique<PulseForge::MaterialAssetCache>(ProjectRoot, m_AssetRegistry);

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

			if (auto TransformResult = CreateTransformBuffer(); !TransformResult)
				throw std::runtime_error(TransformResult.error());

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

			for (const PulseForge::SceneMeshInstance& Instance : InitialSnapshot->Meshes)
			{
				if (!Instance.MaterialAsset)
					throw std::runtime_error("Every mesh in the validation scene must reference a material asset");
				auto MaterialBindings = GetOrCreateMaterialBindings(*Instance.MaterialAsset);
				if (!MaterialBindings)
					throw std::runtime_error("Could not prepare validation-scene material: " + MaterialBindings.error());
			}
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
			if (!Instance.MaterialAsset)
			{
				if (m_ReportedMaterialFailures.insert(Instance.MeshAsset).second)
					PF_ERROR("Mesh asset {0} has no material assigned in the scene", Instance.MeshAsset.ToString());
				continue;
			}
			const auto MaterialBindings = m_MaterialBindings.find(*Instance.MaterialAsset);
			if (MaterialBindings == m_MaterialBindings.end())
			{
				if (m_ReportedMaterialFailures.insert(*Instance.MaterialAsset).second)
					PF_ERROR("Material asset {0} was not prepared before rendering began", Instance.MaterialAsset->ToString());
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
			const std::array<const PulseForge::BindingSet*, 1> BindingSets = {
				MaterialBindings->second->BindingSet.get()
			};
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
			PF_INFO("Submitted {0} indexed mesh instance(s) from the scene snapshot using {1} cached mesh asset(s) and {2} material(s)",
				SubmittedDraws,
				m_MeshAssetCache->GetLoadedCount(),
				m_MaterialBindings.size());
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

	struct MaterialBindingResources
	{
		PulseForge::BufferHandle BaseColorFactorBuffer;
		PulseForge::BindingSetHandle BindingSet;
	};

	[[nodiscard]] std::expected<void, std::string> CreateTransformBuffer()
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

		m_TransformBuffer = std::move(CreatedTransformBuffer.value());
		return {};
	}

	[[nodiscard]] std::expected<MaterialBindingResources*, std::string> GetOrCreateMaterialBindings(
		const PulseForge::AssetID& MaterialAsset)
	{
		if (const auto Existing = m_MaterialBindings.find(MaterialAsset); Existing != m_MaterialBindings.end())
			return Existing->second.get();

		auto Material = m_MaterialAssetCache->GetOrLoad(MaterialAsset);
		if (!Material)
			return std::unexpected(Material.error().Message);

		auto Texture = m_TextureAssetCache->GetOrLoad(
			Material->get().BaseColorTexture,
			PulseForge::TextureFormat::RGBA8_Srgb);
		if (!Texture)
			return std::unexpected(Texture.error().Message);

		const glm::vec4& BaseColorFactor = Material->get().BaseColorFactor;
		PulseForge::BufferDesc FactorBufferDescription;
		FactorBufferDescription.ByteSize = sizeof(BaseColorFactor);
		FactorBufferDescription.Usage = PulseForge::BufferUsage::Constant;
		FactorBufferDescription.DebugName = "PulseForge material base-color factor";
		auto FactorBuffer = PulseForge::Application::Get().CreateBuffer(
			FactorBufferDescription,
			std::as_bytes(std::span(&BaseColorFactor, 1)));
		if (!FactorBuffer)
			return std::unexpected(FactorBuffer.error().Message);

		PulseForge::BindingSetDesc BindingSetDescription;
		BindingSetDescription.Layout = m_BindingLayout;
		BindingSetDescription.Textures.push_back({ 0, std::cref(Texture->get()) });
		BindingSetDescription.Samplers.push_back({ 0, std::cref(*m_Sampler) });
		BindingSetDescription.Buffers.push_back({ 0, std::cref(*FactorBuffer.value()) });
		BindingSetDescription.Buffers.push_back({ 1, std::cref(*m_TransformBuffer) });
		auto BindingSet = PulseForge::Application::Get().CreateBindingSet(BindingSetDescription);
		if (!BindingSet)
			return std::unexpected(BindingSet.error().Message);

		auto Resources = std::make_unique<MaterialBindingResources>();
		Resources->BaseColorFactorBuffer = std::move(FactorBuffer.value());
		Resources->BindingSet = std::move(BindingSet.value());
		auto [Inserted, WasInserted] = m_MaterialBindings.emplace(MaterialAsset, std::move(Resources));
		if (!WasInserted)
			return std::unexpected("Material binding set was already present during cache insertion");
		PF_INFO("Resolved material {0} to sRGB texture {1} ({2}x{3})",
			MaterialAsset.ToString(),
			Material->get().BaseColorTexture.ToString(),
			Texture->get().GetDescription().Width,
			Texture->get().GetDescription().Height);
		return Inserted->second.get();
	}

	PulseForge::Scene m_Scene;
	PulseForge::UUID m_CameraEntity;
	PulseForge::AssetRegistry m_AssetRegistry;
	std::unique_ptr<PulseForge::MeshAssetCache> m_MeshAssetCache;
	std::unique_ptr<PulseForge::TextureAssetCache> m_TextureAssetCache;
	std::unique_ptr<PulseForge::MaterialAssetCache> m_MaterialAssetCache;
	PulseForge::SamplerHandle m_Sampler;
	PulseForge::BufferHandle m_TransformBuffer;
	PulseForge::BindingLayoutHandle m_BindingLayout;
	std::unordered_map<PulseForge::AssetID, std::unique_ptr<MaterialBindingResources>, PulseForge::UUIDHash> m_MaterialBindings;
	PulseForge::ShaderHandle m_VertexShader;
	PulseForge::ShaderHandle m_FragmentShader;
	PulseForge::GraphicsPipelineHandle m_Pipeline;
	bool m_LoggedTransformUpdateFailure = false;
	bool m_LoggedSnapshotFailure = false;
	bool m_LoggedSceneDraw = false;
	std::unordered_set<PulseForge::AssetID, PulseForge::UUIDHash> m_ReportedMeshFailures;
	std::unordered_set<PulseForge::AssetID, PulseForge::UUIDHash> m_ReportedMaterialFailures;
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
