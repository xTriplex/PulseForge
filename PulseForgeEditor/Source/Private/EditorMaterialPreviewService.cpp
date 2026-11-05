#include "Editor/EditorMaterialPreviewService.h"
#include "Editor/EditorMaterialPreviewPublication.h"

#include "Assets/AssetTypes.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Project.h"
#include "Editor/EditorTextureRegistry.h"
#include "Renderer/RenderDevice.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/MeshData.h"
#include "Renderer/Mesh.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/TransformComponent.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <limits>
#include <span>
#include <vector>

namespace PulseForgeEditor
{
	namespace
	{
		constexpr uint32_t PreviewWidth = 320;
		constexpr uint32_t PreviewHeight = 240;
		constexpr size_t MaximumPreviewSessions = 512;
		constexpr uint32_t SphereSegments = 32;
		constexpr uint32_t SphereRings = 20;

		struct PreviewVertex final
		{
			float Position[3];
			float Color[4];
			float UV[2];
			float Normal[3];
		};

		PulseForge::MeshCreateResult CreatePreviewSphere(PulseForge::RenderDevice& Runtime)
		{
			std::vector<PreviewVertex> Vertices;
			std::vector<uint32_t> Indices;
			Vertices.reserve((SphereSegments + 1) * (SphereRings + 1));
			Indices.reserve(SphereSegments * SphereRings * 6);
			for (uint32_t Ring = 0; Ring <= SphereRings; ++Ring)
			{
				const float V = static_cast<float>(Ring) / SphereRings;
				const float Latitude = glm::pi<float>() * V;
				for (uint32_t Segment = 0; Segment <= SphereSegments; ++Segment)
				{
					const float U = static_cast<float>(Segment) / SphereSegments;
					const float Longitude = glm::two_pi<float>() * U;
					const float X = std::sin(Latitude) * std::cos(Longitude);
					const float Y = std::cos(Latitude);
					const float Z = std::sin(Latitude) * std::sin(Longitude);
					Vertices.push_back({ { X, Y, Z }, { 1.0f, 1.0f, 1.0f, 1.0f }, { U, V }, { X, Y, Z } });
				}
			}
			for (uint32_t Ring = 0; Ring < SphereRings; ++Ring)
				for (uint32_t Segment = 0; Segment < SphereSegments; ++Segment)
				{
					const uint32_t First = Ring * (SphereSegments + 1) + Segment;
					const uint32_t Next = First + SphereSegments + 1;
					Indices.insert(Indices.end(), { First, First + 1, Next, First + 1, Next + 1, Next });
				}

			PulseForge::MeshDesc Description;
			Description.VertexLayout.Stride = sizeof(PreviewVertex);
			Description.VertexLayout.Attributes = {
				{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float3, offsetof(PreviewVertex, Position) },
				{ PulseForge::VertexSemantic::Color, PulseForge::VertexFormat::Float4, offsetof(PreviewVertex, Color) },
				{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(PreviewVertex, UV) },
				{ PulseForge::VertexSemantic::Normal, PulseForge::VertexFormat::Float3, offsetof(PreviewVertex, Normal) } };
			Description.VertexData = std::as_bytes(std::span(Vertices));
			Description.Indices = Indices;
			Description.DebugName = "PulseForge editor fixed-PBR material preview sphere";
			return Runtime.CreateMesh(Description);
		}

	}

	struct EditorMaterialPreviewService::Entry final
	{
		PulseForge::UUID Project;
		PulseForge::UUID Material;
		PulseForge::RenderDevice* Runtime = nullptr;
		EditorTextureRegistry* Textures = nullptr;
		std::unique_ptr<PulseForge::Scene> Scene;
		PulseForge::MeshHandle PreviewMesh;
		std::unique_ptr<PulseForge::SceneRenderer> Renderer;
		PulseForge::AssetID PreviewMeshIdentity;
		std::array<PulseForge::RenderTargetHandle, 2> Targets;
		std::array<uint64_t, 2> TextureIDs{};
		uint64_t DraftFingerprint = 0;
		uint64_t RequestedFingerprint = 0;
		uint64_t AssetRevision = 1;
		uint64_t PreparedDraftRevision = 0;
		EditorMaterialPreviewPublication Publication;
		uint64_t LastTouchedFrame = 0;
		bool Prepared = false;
		bool RetryRequested = false;
		std::string Error;

		~Entry()
		{
			if (Textures)
				for (const uint64_t TextureID : TextureIDs)
					if (TextureID != 0) Textures->UnregisterTexture(TextureID);
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
		if (m_AssetRevision != (std::numeric_limits<uint64_t>::max)()) ++m_AssetRevision;
		for (auto& [Generation, Preview] : m_Entries)
		{
			(void)Generation;
			if (Preview->Renderer) Preview->Renderer->RefreshProjectAssets();
			Preview->AssetRevision = m_AssetRevision;
			Preview->Prepared = false;
			Preview->Publication.Invalidate(Preview->AssetRevision);
			Preview->Error = "Material preview assets were refreshed; preparing a replacement while retaining the last complete preview.";
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
			return nullptr;
		}

		auto New = std::make_unique<Entry>();
		New->Project = m_ProjectID;
		New->Material = Material;
		New->Runtime = &Runtime;
		New->Textures = &Textures;
		New->AssetRevision = m_AssetRevision;
		auto Renderer = PulseForge::SceneRenderer::Create(Runtime, Project, CompiledShaderDirectory);
		if (!Renderer)
		{
			New->Error = "Could not create the material preview renderer: " + Renderer.error().Message;
			return m_Entries.emplace(SessionGeneration, std::move(New)).first->second.get();
		}
		New->Renderer = std::move(*Renderer);
		auto Scene = std::make_unique<PulseForge::Scene>();
		auto CreatedPreviewMesh = CreatePreviewSphere(Runtime);
		if (!CreatedPreviewMesh)
			New->Error = "Could not create the built-in material preview sphere: " + CreatedPreviewMesh.error().Message;
		else
		{
			New->PreviewMesh = std::move(*CreatedPreviewMesh);
			New->PreviewMeshIdentity = PulseForge::UUID::Generate().value_or(PulseForge::UUID{});
			auto MeshEntity = Scene->CreateEntity("Material preview mesh");
			auto LightEntity = Scene->CreateEntity("Material preview light");
			if (!MeshEntity || !LightEntity)
				New->Error = "Could not create the isolated material preview scene.";
			else
			{
				const auto MeshSet = MeshEntity->SetMeshRenderer({ New->PreviewMeshIdentity, Material });
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
				{
					New->Scene = std::move(Scene);
					New->Renderer->SetTransientPreviewMesh(New->PreviewMeshIdentity, New->PreviewMesh.get());
				}
			}
		}

		for (size_t Index = 0; Index < New->Targets.size(); ++Index)
		{
			PulseForge::RenderTargetDesc TargetDescription;
			TargetDescription.Width = PreviewWidth;
			TargetDescription.Height = PreviewHeight;
			TargetDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			TargetDescription.DebugName = "PulseForge material draft preview candidate";
			if (auto Target = Runtime.CreateRenderTarget(TargetDescription); Target)
			{
				if (const PulseForge::Texture* Color = (*Target)->GetColorTexture())
				{
					if (auto TextureID = Textures.RegisterTexture(*Color); TextureID)
					{
						New->TextureIDs[Index] = *TextureID;
						New->Targets[Index] = std::move(*Target);
					}
					else New->Error = "Could not register the material preview image: " + TextureID.error();
				}
				else New->Error = "The material preview target has no color texture.";
			}
			else New->Error = "Could not create the material preview target: " + Target.error().Message;
		}

		Entry* Result = New.get();
		m_Entries.emplace(SessionGeneration, std::move(New));
		return Result;
	}

	void EditorMaterialPreviewService::Prepare(PulseForge::RenderDevice& Runtime, EditorTextureRegistry& Textures,
		PulseForge::Project& Project, const PulseForge::UUID& Material, uint64_t SessionGeneration, uint64_t DraftRevision,
		const PulseForge::MaterialAssetDesc& Draft, const std::filesystem::path& CompiledShaderDirectory)
	{
		if (const auto Existing = m_Entries.find(SessionGeneration); Existing != m_Entries.end() &&
			Existing->second->RetryRequested && !Existing->second->Publication.HasPublishedFrame())
			m_Entries.erase(Existing);
		Entry* const Preview = FindOrCreate(Runtime, Textures, Project, Material, SessionGeneration, CompiledShaderDirectory);
		if (!Preview) return;
		Preview->LastTouchedFrame = m_Frame;
		Preview->Prepared = false;
		if (!Preview->Renderer || !Preview->Scene || !Preview->Targets[0] || !Preview->Targets[1]) return;

		auto Serialized = PulseForge::MaterialAssetSerializer::Serialize(Draft);
		if (!Serialized)
		{
			Preview->Error = "Draft preview is invalid: " + Serialized.error().Message;
			return;
		}
		const uint64_t Fingerprint = ComputeEditorMaterialPreviewFingerprint(*Serialized);
		Preview->RequestedFingerprint = Fingerprint;
		if (Preview->Publication.IsCurrent(Fingerprint, Preview->AssetRevision))
		{
			Preview->Prepared = false;
			Preview->Error.clear();
			return;
		}
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
		if (Preview->Renderer->IsAssetPreparationPending())
		{
			Preview->Error = "Preview resources are still preparing; the last complete preview is retained.";
			return;
		}
		if (Preview->Renderer->GetPreparedMeshCount() != 1)
		{
			Preview->Error = "The preview scene did not prepare its required material geometry.";
			return;
		}
		Preview->Prepared = true;
		Preview->PreparedDraftRevision = DraftRevision;
		Preview->Error = "A complete material preview candidate is ready.";
	}

	bool EditorMaterialPreviewService::RequestRetry(uint64_t SessionGeneration) noexcept
	{
		const auto Found = m_Entries.find(SessionGeneration);
		if (Found == m_Entries.end()) return false;
		Entry& Preview = *Found->second;
		if (Preview.RequestedFingerprint != 0 &&
			Preview.Publication.IsCurrent(Preview.RequestedFingerprint, Preview.AssetRevision)) return false;
		if (!Preview.Publication.HasPublishedFrame())
		{
			Preview.RetryRequested = true;
			return true;
		}
		if (!Preview.Renderer) return false;
		Preview.Renderer->RefreshProjectAssets();
		if (m_AssetRevision != (std::numeric_limits<uint64_t>::max)()) ++m_AssetRevision;
		Preview.AssetRevision = m_AssetRevision;
		Preview.Publication.Invalidate(Preview.AssetRevision);
		Preview.Prepared = false;
		Preview.Error = "Retrying material preview preparation; the last complete preview remains available.";
		return true;
	}

	bool EditorMaterialPreviewService::Draw(
		uint64_t SessionGeneration, uint64_t CurrentDraftRevision, float Width, float Height)
	{
		const auto Found = m_Entries.find(SessionGeneration);
		if (Found == m_Entries.end()) return false;
		Entry& Preview = *Found->second;
		Preview.LastTouchedFrame = m_Frame;
		if (Preview.Prepared && Preview.Renderer && Preview.PreparedDraftRevision == CurrentDraftRevision)
		{
			const size_t Candidate = Preview.Publication.GetCandidateSlot();
			auto Rendered = Preview.Renderer->RenderPreparedSceneToOutput(*Preview.Targets[Candidate]);
			const size_t GeometryDrawCount = Rendered ? Rendered->GeometryDrawCount : 0;
			if (Preview.Publication.PublishCandidate(Preview.RequestedFingerprint, Preview.AssetRevision,
				Preview.PreparedDraftRevision, CurrentDraftRevision, true, Rendered.has_value(), GeometryDrawCount))
			{
				Preview.Error.clear();
			}
			else Preview.Error = Rendered
				? "The candidate rendered without the required material geometry; the last complete preview is retained."
				: Rendered.error().Message;
			Preview.Prepared = false;
		}
		else if (Preview.Prepared)
		{
			Preview.Prepared = false;
			Preview.Error = "The material draft changed after preview preparation; retaining the last complete preview.";
		}
		if (Preview.Publication.HasPublishedFrame() && Preview.TextureIDs[Preview.Publication.GetDisplayedSlot()] != 0)
		{
			const float DrawWidth = (std::max)(1.0f, Width);
			const float DrawHeight = (std::max)(1.0f, Height);
			ImGui::Image(ImTextureRef(static_cast<ImTextureID>(Preview.TextureIDs[Preview.Publication.GetDisplayedSlot()])), ImVec2(DrawWidth, DrawHeight));
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
