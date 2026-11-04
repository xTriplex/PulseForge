#include "Editor/EditorMaterialPreviewService.h"

#include "Assets/AssetTypes.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Project.h"
#include "Editor/EditorTextureRegistry.h"
#include "Renderer/RenderDevice.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/TransformComponent.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <span>

namespace PulseForgeEditor
{
	namespace
	{
		constexpr uint32_t PreviewWidth = 320;
		constexpr uint32_t PreviewHeight = 240;
		constexpr size_t MaximumPreviewSessions = 4;

		uint64_t HashDraft(std::string_view Bytes) noexcept
		{
			uint64_t Hash = 14695981039346656037ull;
			for (const unsigned char Byte : Bytes)
			{
				Hash ^= Byte;
				Hash *= 1099511628211ull;
			}
			return Hash == 0 ? 1 : Hash;
		}
	}

	struct EditorMaterialPreviewService::Entry final
	{
		PulseForge::UUID Project;
		PulseForge::UUID Material;
		PulseForge::RenderDevice* Runtime = nullptr;
		EditorTextureRegistry* Textures = nullptr;
		std::unique_ptr<PulseForge::Scene> Scene;
		std::unique_ptr<PulseForge::SceneRenderer> Renderer;
		PulseForge::RenderTargetHandle Target;
		uint64_t TextureID = 0;
		uint64_t DraftFingerprint = 0;
		uint64_t LastTouchedFrame = 0;
		bool Prepared = false;
		bool HasLastGoodFrame = false;
		std::string Error;

		~Entry()
		{
			if (Textures && TextureID != 0) Textures->UnregisterTexture(TextureID);
		}
	};

	EditorMaterialPreviewService::EditorMaterialPreviewService() = default;
	EditorMaterialPreviewService::~EditorMaterialPreviewService() = default;

	void EditorMaterialPreviewService::BeginFrame() noexcept
	{
		++m_Frame;
	}

	void EditorMaterialPreviewService::Clear() noexcept
	{
		m_Entries.clear();
		m_ProjectID = {};
	}

	void EditorMaterialPreviewService::RefreshProjectAssets() noexcept
	{
		for (auto& [Generation, Preview] : m_Entries)
		{
			(void)Generation;
			if (!Preview->Renderer) continue;
			Preview->Renderer->RefreshProjectAssets();
			Preview->Prepared = false;
		}
	}

	void EditorMaterialPreviewService::RetainSessions(std::span<const uint64_t> SessionGenerations) noexcept
	{
		std::erase_if(m_Entries, [SessionGenerations](const auto& Entry)
		{
			return std::find(SessionGenerations.begin(), SessionGenerations.end(), Entry.first) == SessionGenerations.end();
		});
	}

	EditorMaterialPreviewService::Entry* EditorMaterialPreviewService::FindOrCreate(
		PulseForge::RenderDevice& Runtime, EditorTextureRegistry& Textures, PulseForge::Project& Project,
		const PulseForge::UUID& Material, uint64_t SessionGeneration, const std::filesystem::path& CompiledShaderDirectory)
	{
		if (m_ProjectID != Project.GetDescription().ID)
		{
			Clear();
			m_ProjectID = Project.GetDescription().ID;
		}
		if (auto Existing = m_Entries.find(SessionGeneration); Existing != m_Entries.end())
		{
			if (Existing->second->Project == m_ProjectID && Existing->second->Material == Material)
				return Existing->second.get();
			m_Entries.erase(Existing);
		}
		if (m_Entries.size() >= MaximumPreviewSessions)
		{
			auto Oldest = std::min_element(m_Entries.begin(), m_Entries.end(), [](const auto& First, const auto& Second)
			{
				return First.second->LastTouchedFrame < Second.second->LastTouchedFrame;
			});
			if (Oldest != m_Entries.end()) m_Entries.erase(Oldest);
		}

		auto New = std::make_unique<Entry>();
		New->Project = m_ProjectID;
		New->Material = Material;
		New->Runtime = &Runtime;
		New->Textures = &Textures;
		auto Renderer = PulseForge::SceneRenderer::Create(Runtime, Project, CompiledShaderDirectory);
		if (!Renderer)
		{
			New->Error = "Could not create the material preview renderer: " + Renderer.error().Message;
			return m_Entries.emplace(SessionGeneration, std::move(New)).first->second.get();
		}
		New->Renderer = std::move(*Renderer);
		auto Scene = std::make_unique<PulseForge::Scene>();
		std::vector<PulseForge::AssetRecord> MeshAssets;
		for (const PulseForge::AssetRecord& Asset : Project.GetAssetRegistry().GetAssets())
			if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Mesh)) MeshAssets.push_back(Asset);
		std::sort(MeshAssets.begin(), MeshAssets.end(), [](const auto& First, const auto& Second)
		{
			return First.ProjectRelativePath < Second.ProjectRelativePath;
		});
		if (MeshAssets.empty())
			New->Error = "Material preview needs a managed mesh asset in this project.";
		else
		{
			auto MeshEntity = Scene->CreateEntity("Material preview mesh");
			auto LightEntity = Scene->CreateEntity("Material preview light");
			if (!MeshEntity || !LightEntity)
				New->Error = "Could not create the isolated material preview scene.";
			else
			{
				const auto MeshSet = MeshEntity->SetMeshRenderer({ MeshAssets.front().ID, Material });
				PulseForge::DirectionalLightComponent Light;
				Light.Intensity = 3.0f;
				Light.CastShadows = false;
				const auto LightSet = LightEntity->SetDirectionalLight(Light);
				PulseForge::TransformComponent LightTransform;
				LightTransform.Rotation = glm::quat(glm::vec3(glm::radians(-35.0f), glm::radians(25.0f), 0.0f));
				const auto TransformSet = LightEntity->SetTransform(LightTransform);
				if (!MeshSet || !LightSet || !TransformSet)
					New->Error = "Could not configure the isolated material preview scene.";
				else
					New->Scene = std::move(Scene);
			}
		}

		PulseForge::RenderTargetDesc TargetDescription;
		TargetDescription.Width = PreviewWidth;
		TargetDescription.Height = PreviewHeight;
		TargetDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		TargetDescription.DebugName = "PulseForge material draft preview";
		if (auto Target = Runtime.CreateRenderTarget(TargetDescription); Target)
		{
			if (const PulseForge::Texture* Color = (*Target)->GetColorTexture())
			{
				if (auto TextureID = Textures.RegisterTexture(*Color); TextureID)
				{
					New->TextureID = *TextureID;
					New->Target = std::move(*Target);
				}
				else New->Error = "Could not register the material preview image: " + TextureID.error();
			}
			else New->Error = "The material preview target has no color texture.";
		}
		else New->Error = "Could not create the material preview target: " + Target.error().Message;

		Entry* Result = New.get();
		m_Entries.emplace(SessionGeneration, std::move(New));
		return Result;
	}

	void EditorMaterialPreviewService::Prepare(PulseForge::RenderDevice& Runtime, EditorTextureRegistry& Textures,
		PulseForge::Project& Project, const PulseForge::UUID& Material, uint64_t SessionGeneration, uint64_t DraftRevision,
		const PulseForge::MaterialAssetDesc& Draft, const std::filesystem::path& CompiledShaderDirectory)
	{
		Entry* const Preview = FindOrCreate(Runtime, Textures, Project, Material, SessionGeneration, CompiledShaderDirectory);
		if (!Preview) return;
		Preview->LastTouchedFrame = m_Frame;
		Preview->Prepared = false;
		if (!Preview->Renderer || !Preview->Scene || !Preview->Target) return;

		auto Serialized = PulseForge::MaterialAssetSerializer::Serialize(Draft);
		if (!Serialized)
		{
			Preview->Error = "Draft preview is invalid: " + Serialized.error().Message;
			return;
		}
		const uint64_t Fingerprint = HashDraft(*Serialized);
		if (Fingerprint != Preview->DraftFingerprint)
		{
			if (auto Updated = Preview->Renderer->SetTransientMaterialOverride(Material, Draft,
				(DraftRevision ^ Fingerprint) ? (DraftRevision ^ Fingerprint) : 1); !Updated)
			{
				Preview->Error = Updated.error().Message;
				return;
			}
			Preview->DraftFingerprint = Fingerprint;
		}

		const glm::vec3 CameraPosition(3.0f, 2.25f, 4.25f);
		const glm::mat4 View = glm::lookAt(CameraPosition, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 Projection = glm::perspective(glm::radians(42.0f),
			static_cast<float>(PreviewWidth) / static_cast<float>(PreviewHeight), 0.1f, 100.0f);
		auto Prepared = Preview->Renderer->PrepareScene(*Preview->Scene, View, Projection, CameraPosition, 0.1f, 100.0f);
		if (!Prepared)
		{
			Preview->Error = Prepared.error().Message;
			return;
		}
		Preview->Prepared = true;
		Preview->Error.clear();
	}

	bool EditorMaterialPreviewService::Draw(uint64_t SessionGeneration, float Width, float Height)
	{
		const auto Found = m_Entries.find(SessionGeneration);
		if (Found == m_Entries.end()) return false;
		Entry& Preview = *Found->second;
		Preview.LastTouchedFrame = m_Frame;
		if (Preview.Prepared && Preview.Renderer && Preview.Target)
		{
			auto Rendered = Preview.Renderer->RenderPreparedSceneToOutput(*Preview.Target);
			if (Rendered)
			{
				Preview.HasLastGoodFrame = true;
				Preview.Error.clear();
			}
			else Preview.Error = Rendered.error().Message;
		}
		if (Preview.HasLastGoodFrame && Preview.TextureID != 0)
		{
			const float DrawWidth = (std::max)(1.0f, Width);
			const float DrawHeight = (std::max)(1.0f, Height);
			ImGui::Image(ImTextureRef(static_cast<ImTextureID>(Preview.TextureID)), ImVec2(DrawWidth, DrawHeight));
			return true;
		}
		return false;
	}

	const std::string& EditorMaterialPreviewService::GetError(uint64_t SessionGeneration) const noexcept
	{
		static const std::string Empty;
		const auto Found = m_Entries.find(SessionGeneration);
		return Found == m_Entries.end() ? Empty : Found->second->Error;
	}
}
