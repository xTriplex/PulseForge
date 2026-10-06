#include "Core/PulseForgePCH.h"
#include "Renderer/SceneRenderer.h"

#include "Assets/MaterialAssetCache.h"
#include "Assets/MeshAssetCache.h"
#include "Assets/Project.h"
#include "Assets/TextureAssetCache.h"
#include "Core/Application.h"
#include "Core/Log.h"
#include "Renderer/Mesh.h"
#include "Scene/Scene.h"

#include <array>
#include <fstream>
#include <iterator>
#include <span>

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

		SceneRendererError MakeResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}
	}

	SceneRenderer::SceneRenderer(Application& Runtime, const Project& SourceProject)
		: m_Runtime(Runtime),
		  m_Project(SourceProject),
		  m_MeshAssetCache(std::make_unique<MeshAssetCache>(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry())),
		  m_TextureAssetCache(std::make_unique<TextureAssetCache>(Runtime, SourceProject.GetRootPath(), SourceProject.GetAssetRegistry())),
		  m_MaterialAssetCache(std::make_unique<MaterialAssetCache>(SourceProject.GetRootPath(), SourceProject.GetAssetRegistry()))
	{
	}

	SceneRenderer::~SceneRenderer() = default;

	std::expected<std::unique_ptr<SceneRenderer>, SceneRendererError> SceneRenderer::Create(
		Application& Runtime,
		const Project& SourceProject,
		const std::filesystem::path& CompiledShaderDirectory)
	{
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

		BindingLayoutDesc BindingLayoutDescription;
		BindingLayoutDescription.Visibility = ShaderVisibility::AllGraphics;
		BindingLayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 },
			{ BindingResourceType::Sampler, 0 },
			{ BindingResourceType::ConstantBuffer, 0 },
			{ BindingResourceType::ConstantBuffer, 1 }
		};
		BindingLayoutDescription.DebugName = "PulseForge scene resources";
		auto BindingLayout = m_Runtime.CreateBindingLayout(BindingLayoutDescription);
		if (!BindingLayout)
			return std::unexpected(MakeResourceError("Could not create the scene binding layout: " + BindingLayout.error().Message));
		m_BindingLayout = std::move(BindingLayout.value());

		const glm::mat4 InitialTransform(1.0f);
		BufferDesc TransformBufferDescription;
		TransformBufferDescription.ByteSize = sizeof(InitialTransform);
		TransformBufferDescription.Usage = BufferUsage::Constant;
		TransformBufferDescription.DebugName = "PulseForge scene per-object transform";
		auto TransformBuffer = m_Runtime.CreateBuffer(
			TransformBufferDescription,
			std::as_bytes(std::span(&InitialTransform, 1)));
		if (!TransformBuffer)
			return std::unexpected(MakeResourceError("Could not create the scene transform buffer: " + TransformBuffer.error().Message));
		m_TransformBuffer = std::move(TransformBuffer.value());
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::EnsureMaterialBindings(const AssetID& MaterialAsset)
	{
		if (m_MaterialBindings.contains(MaterialAsset))
			return {};

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

		const glm::vec4& BaseColorFactor = Material->get().BaseColorFactor;
		BufferDesc FactorBufferDescription;
		FactorBufferDescription.ByteSize = sizeof(BaseColorFactor);
		FactorBufferDescription.Usage = BufferUsage::Constant;
		FactorBufferDescription.DebugName = "PulseForge material base-color factor";
		auto FactorBuffer = m_Runtime.CreateBuffer(
			FactorBufferDescription,
			std::as_bytes(std::span(&BaseColorFactor, 1)));
		if (!FactorBuffer)
			return std::unexpected(MakeResourceError("Could not create material constants: " + FactorBuffer.error().Message));

		BindingSetDesc BindingSetDescription;
		BindingSetDescription.Layout = m_BindingLayout;
		BindingSetDescription.Textures.push_back({ 0, std::cref(Texture->get()) });
		BindingSetDescription.Samplers.push_back({ 0, std::cref(*m_Sampler) });
		BindingSetDescription.Buffers.push_back({ 0, std::cref(*FactorBuffer.value()) });
		BindingSetDescription.Buffers.push_back({ 1, std::cref(*m_TransformBuffer) });
		auto BindingSet = m_Runtime.CreateBindingSet(BindingSetDescription);
		if (!BindingSet)
			return std::unexpected(MakeResourceError("Could not create material bindings: " + BindingSet.error().Message));

		MaterialBindingResources Resources;
		Resources.BaseColorFactorBuffer = std::move(FactorBuffer.value());
		Resources.BindingSet = std::move(BindingSet.value());
		m_MaterialBindings.emplace(MaterialAsset, std::move(Resources));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::PrepareScene(
		const Scene& Source,
		UUID CameraEntity,
		float AspectRatio)
	{
		m_PreparedSnapshot.reset();
		auto Snapshot = SceneRenderSnapshotBuilder::Build(Source, CameraEntity, AspectRatio);
		if (!Snapshot)
		{
			return std::unexpected(SceneRendererError{
				SceneRendererErrorCode::SnapshotBuildFailed,
				Snapshot.error().Entity,
				{},
				Snapshot.error().Message
			});
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
			if (auto Bindings = EnsureMaterialBindings(*Instance.MaterialAsset); !Bindings)
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
			if (!m_Pipeline)
			{
				GraphicsPipelineDesc PipelineDescription;
				PipelineDescription.VertexShader = m_VertexShader;
				PipelineDescription.FragmentShader = m_FragmentShader;
				PipelineDescription.BindingLayouts = { m_BindingLayout };
				PipelineDescription.VertexLayout = *SceneVertexLayout;
				PipelineDescription.Rasterizer.Cull = CullMode::None;
				PipelineDescription.Depth.TestEnabled = true;
				PipelineDescription.Depth.WriteEnabled = true;
				PipelineDescription.Depth.Compare = DepthCompareOperation::Less;
				PipelineDescription.DebugName = "PulseForge scene pipeline";
				auto Pipeline = m_Runtime.CreateGraphicsPipeline(PipelineDescription);
				if (!Pipeline)
					return std::unexpected(MakeResourceError("Could not create scene graphics pipeline: " + Pipeline.error().Message));
				m_Pipeline = std::move(Pipeline.value());
				m_PipelineVertexLayout = std::move(SceneVertexLayout);
				PF_CORE_INFO("Created scene pipeline for vertex stride {0}", m_PipelineVertexLayout->Stride);
			}
		}

		m_PreparedSnapshot = std::move(*Snapshot);
		return {};
	}

	std::expected<size_t, SceneRendererError> SceneRenderer::RenderPreparedScene()
	{
		if (!m_PreparedSnapshot || m_PreparedSnapshot->Meshes.empty())
			return size_t{ 0 };
		if (!m_Pipeline)
			return std::unexpected(MakeResourceError("Scene has mesh instances but the graphics pipeline is unavailable"));

		size_t SubmittedDraws = 0;
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
			const auto Binding = m_MaterialBindings.find(*Instance.MaterialAsset);
			if (Binding == m_MaterialBindings.end())
			{
				return std::unexpected(SceneRendererError{
					SceneRendererErrorCode::AssetLoadFailed,
					Instance.Entity,
					*Instance.MaterialAsset,
					"Prepared material binding set is unavailable"
				});
			}

			const glm::mat4 ModelViewProjection = m_PreparedSnapshot->ViewProjection * Instance.WorldTransform;
			const auto Update = m_Runtime.WriteBuffer(
				*m_TransformBuffer,
				0,
				std::as_bytes(std::span(&ModelViewProjection, 1)));
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
			const std::array<const BindingSet*, 1> BindingSets = { Binding->second.BindingSet.get() };
			const GraphicsResult Draw = m_Runtime.DrawIndexed(*m_Pipeline, Mesh->get(), Arguments, BindingSets);
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
