#include "GpuValidation/MirroredWindingValidation.h"

#include "Assets/AssetMetadata.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Project.h"
#include "Core/Application.h"
#include "Core/Log.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"
#include "Scene/Components/TransformComponent.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace PulseForge::GpuValidation
{
	namespace
	{
		constexpr uint32_t OutputExtent = 160;
		constexpr size_t MinimumVisiblePixels = 300;

		struct TransformCase
		{
			const char* Name;
			glm::vec3 ParentScale;
			glm::vec3 MeshScale;
		};

		const std::array<TransformCase, 5> TransformCases = { {
			{ "positive scale", { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } },
			{ "one reflected axis", { 1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f } },
			{ "two reflected axes", { 1.0f, 1.0f, 1.0f }, { -1.0f, -1.0f, 1.0f } },
			{ "parent-inherited reflection", { -1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } },
			{ "parent and child reflections cancel", { -1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f } },
		} };

		void AppendLittleEndianFloat(std::vector<std::byte>& Data, float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (uint32_t Shift = 0; Shift < 32; Shift += 8)
				Data.push_back(static_cast<std::byte>((Bits >> Shift) & 0xff));
		}

		std::string EncodeBase64(std::span<const std::byte> Data)
		{
			constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string Encoded;
			Encoded.reserve(((Data.size() + 2) / 3) * 4);
			for (size_t Offset = 0; Offset < Data.size(); Offset += 3)
			{
				const uint32_t First = std::to_integer<uint8_t>(Data[Offset]);
				const uint32_t Second = Offset + 1 < Data.size() ? std::to_integer<uint8_t>(Data[Offset + 1]) : 0;
				const uint32_t Third = Offset + 2 < Data.size() ? std::to_integer<uint8_t>(Data[Offset + 2]) : 0;
				const uint32_t Block = (First << 16) | (Second << 8) | Third;
				Encoded.push_back(Alphabet[(Block >> 18) & 0x3f]);
				Encoded.push_back(Alphabet[(Block >> 12) & 0x3f]);
				Encoded.push_back(Offset + 1 < Data.size() ? Alphabet[(Block >> 6) & 0x3f] : '=');
				Encoded.push_back(Offset + 2 < Data.size() ? Alphabet[Block & 0x3f] : '=');
			}
			return Encoded;
		}

		std::vector<std::byte> MakeTriangleBuffer()
		{
			std::vector<std::byte> Data;
			Data.reserve(78);
			for (const glm::vec3 Position : { glm::vec3(-0.8f, -0.7f, 0.0f), glm::vec3(0.8f, -0.7f, 0.0f), glm::vec3(-0.1f, 0.8f, 0.0f) })
			{
				AppendLittleEndianFloat(Data, Position.x);
				AppendLittleEndianFloat(Data, Position.y);
				AppendLittleEndianFloat(Data, Position.z);
			}
			for (size_t Vertex = 0; Vertex < 3; ++Vertex)
			{
				AppendLittleEndianFloat(Data, 0.0f);
				AppendLittleEndianFloat(Data, 0.0f);
				AppendLittleEndianFloat(Data, 1.0f);
			}
			for (const uint16_t Index : { uint16_t{ 0 }, uint16_t{ 1 }, uint16_t{ 2 } })
			{
				Data.push_back(static_cast<std::byte>(Index & 0xff));
				Data.push_back(static_cast<std::byte>(Index >> 8));
			}
			return Data;
		}

		std::string MakeTriangleGltf()
		{
			const std::vector<std::byte> Buffer = MakeTriangleBuffer();
			return R"({
"asset":{"version":"2.0","generator":"PulseForge mirrored-winding validation"},
"buffers":[{"byteLength":78,"uri":"data:application/octet-stream;base64,)" +
				   EncodeBase64(Buffer) + R"("}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
{"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},
{"buffer":0,"byteOffset":72,"byteLength":6,"target":34963}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.8,-0.7,0],"max":[0.8,0.8,0]},
{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2,"mode":4}]}]
})";
		}

		void WriteFile(const std::filesystem::path& Path, std::string_view Contents)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output.is_open()) throw std::runtime_error("Could not create mirrored-winding fixture file: " + Path.string());
			Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			Output.close();
			if (!Output) throw std::runtime_error("Could not finish mirrored-winding fixture file: " + Path.string());
		}

		void Require(bool Condition, std::string Message)
		{
			if (!Condition) throw std::runtime_error(std::move(Message));
		}

		size_t CountWrittenDepthPixels(const TextureReadback& Readback)
		{
			if (Readback.Format != TextureFormat::Depth32Float ||
				Readback.Pixels.size() != static_cast<size_t>(Readback.Width) * Readback.Height * sizeof(float))
				return 0;

			size_t WrittenPixels = 0;
			for (size_t Offset = 0; Offset < Readback.Pixels.size(); Offset += sizeof(float))
			{
				float Depth = 1.0f;
				std::memcpy(&Depth, Readback.Pixels.data() + Offset, sizeof(Depth));
				if (std::isfinite(Depth) && Depth >= 0.0f && Depth < 0.99999f) ++WrittenPixels;
			}
			return WrittenPixels;
		}
	} // namespace

	MirroredWindingValidation::MirroredWindingValidation(
		Application& Runtime, std::filesystem::path ShaderDirectory, std::chrono::steady_clock::time_point StartedAt) noexcept
		: m_Runtime(Runtime), m_ShaderDirectory(std::move(ShaderDirectory)), m_StartedAt(StartedAt)
	{
	}

	MirroredWindingValidation::~MirroredWindingValidation()
	{
		Reset();
	}

	void MirroredWindingValidation::Initialize()
	{
		const auto FixtureID = UUID::Generate();
		Require(FixtureID.has_value(), "Could not generate a unique mirrored-winding GPU fixture directory");
		m_FixtureRoot = std::filesystem::temp_directory_path() / "PulseForgeGpuValidation" / ("MirroredWinding-" + FixtureID->ToString());
		std::filesystem::create_directories(m_FixtureRoot);

		auto CreatedProject = Project::Create(m_FixtureRoot / "MirroredWinding.pfproj", "Mirrored Winding Validation");
		Require(CreatedProject.has_value(), CreatedProject ? "Unexpected project creation state" : CreatedProject.error().Message);
		m_Project.emplace(std::move(*CreatedProject));
		const std::filesystem::path MeshPath = m_Project->GetRootPath() / "Assets" / "MirroredTriangle.gltf";
		WriteFile(MeshPath, MakeTriangleGltf());
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(MeshPath);
		Require(Metadata.has_value(), Metadata ? "Unexpected metadata creation state" : Metadata.error().Message);
		m_MeshAssetID = Metadata->ID;
		const std::filesystem::path DoubleSidedMaterialPath = m_Project->GetRootPath() / "Assets" / "DoubleSided.material";
		MaterialAssetDesc DoubleSidedMaterial;
		DoubleSidedMaterial.BaseColorFactor = { 0.05f, 1.0f, 0.05f, 1.0f };
		DoubleSidedMaterial.EmissiveFactor = { 0.0f, 0.15f, 0.0f };
		DoubleSidedMaterial.DoubleSided = true;
		const auto SerializedMaterial = MaterialAssetSerializer::Serialize(DoubleSidedMaterial);
		Require(SerializedMaterial.has_value(),
			SerializedMaterial ? "Unexpected double-sided material serialization state" : SerializedMaterial.error().Message);
		WriteFile(DoubleSidedMaterialPath, *SerializedMaterial);
		const auto DoubleSidedMaterialMetadata = AssetMetadataSerializer::CreateForNewAsset(DoubleSidedMaterialPath);
		Require(DoubleSidedMaterialMetadata.has_value(),
			DoubleSidedMaterialMetadata ? "Unexpected material metadata creation state" : DoubleSidedMaterialMetadata.error().Message);
		m_DoubleSidedMaterialAssetID = DoubleSidedMaterialMetadata->ID;
		const auto Registry = m_Project->GetAssetRegistry().Rebuild(m_Project->GetRootPath());
		Require(Registry.has_value(), "Could not register mirrored-winding glTF fixture");

		m_Scene = std::make_unique<Scene>();
		auto Camera = m_Scene->CreateEntity("Mirrored-winding validation camera");
		Require(Camera.has_value(), "Could not create mirrored-winding validation camera");
		CameraComponent CameraDescription;
		CameraDescription.IsPrimary = true;
		TransformComponent CameraTransform;
		CameraTransform.Translation = { 0.0f, 0.0f, 3.0f };
		Require(Camera->SetTransform(CameraTransform).has_value(), "Could not position mirrored-winding camera");
		Require(Camera->SetCamera(CameraDescription).has_value(), "Could not configure mirrored-winding camera");
		m_Camera = *Camera;

		auto Light = m_Scene->CreateEntity("Mirrored-winding validation light");
		Require(Light.has_value(), "Could not create mirrored-winding validation light");
		DirectionalLightComponent LightDescription;
		LightDescription.Intensity = 3.0f;
		LightDescription.CastShadows = true;
		Require(Light->SetDirectionalLight(LightDescription).has_value(), "Could not configure mirrored-winding directional light");
		m_Light = *Light;
		auto SpotLight = m_Scene->CreateEntity("Mirrored-winding spotlight");
		Require(SpotLight.has_value(), "Could not create mirrored-winding spotlight");
		SpotLightComponent SpotLightDescription;
		SpotLightDescription.Intensity = 20.0f;
		SpotLightDescription.Range = 8.0f;
		SpotLightDescription.OuterConeAngleDegrees = 35.0f;
		SpotLightDescription.CastShadows = true;
		TransformComponent SpotLightTransform;
		SpotLightTransform.Translation = { 0.0f, 0.0f, 3.0f };
		Require(SpotLight->SetTransform(SpotLightTransform).has_value(), "Could not position mirrored-winding spotlight");
		Require(SpotLight->SetSpotLight(SpotLightDescription).has_value(), "Could not configure mirrored-winding spotlight");
		m_SpotLight = *SpotLight;

		auto PointLight = m_Scene->CreateEntity("Mirrored-winding point shadow light");
		Require(PointLight.has_value(), "Could not create mirrored-winding point light");
		PointLightComponent PointLightDescription;
		PointLightDescription.Intensity = 40.0f;
		PointLightDescription.Range = 6.0f;
		PointLightDescription.CastShadows = true;
		TransformComponent PointLightTransform;
		PointLightTransform.Translation = { 0.0f, 0.0f, 2.0f };
		Require(PointLight->SetTransform(PointLightTransform).has_value(), "Could not position mirrored-winding point light");
		Require(PointLight->SetPointLight(PointLightDescription).has_value(), "Could not configure mirrored-winding point light");
		m_PointLight = *PointLight;

		auto Parent = m_Scene->CreateEntity("Mirrored-winding parent");
		Require(Parent.has_value(), "Could not create mirrored-winding transform parent");
		m_Parent = *Parent;
		auto Mesh = m_Scene->CreateEntity("Mirrored-winding open triangle");
		Require(Mesh.has_value(), "Could not create mirrored-winding mesh entity");
		const auto Record = m_Project->GetAssetRegistry().Find(Metadata->ID);
		Require(Record.has_value(), "Mirrored-winding mesh was not present in the rebuilt asset registry");
		Require(Mesh->SetMeshRenderer({ Metadata->ID, {} }).has_value(), "Could not assign mirrored-winding mesh asset");
		Require(Mesh->SetParent(*Parent).has_value(), "Could not parent mirrored-winding mesh");
		TransformComponent MeshTransform;
		MeshTransform.Translation = { -0.55f, 0.0f, 0.0f };
		Require(Mesh->SetTransform(MeshTransform).has_value(), "Could not position single-sided mirrored-winding mesh");
		m_Mesh = *Mesh;
		auto DoubleSidedMesh = m_Scene->CreateEntity("Mirrored-winding double-sided triangle");
		Require(DoubleSidedMesh.has_value(), "Could not create mirrored-winding double-sided mesh entity");
		Require(m_Project->GetAssetRegistry().Find(DoubleSidedMaterialMetadata->ID).has_value(),
			"Double-sided material was not present in the rebuilt asset registry");
		Require(DoubleSidedMesh->SetMeshRenderer({ Metadata->ID, DoubleSidedMaterialMetadata->ID }).has_value(),
			"Could not assign mirrored-winding double-sided material");
		Require(DoubleSidedMesh->SetParent(*Parent).has_value(), "Could not parent mirrored-winding double-sided mesh");
		TransformComponent DoubleSidedMeshTransform;
		DoubleSidedMeshTransform.Translation = { 0.55f, 0.0f, 0.0f };
		Require(
			DoubleSidedMesh->SetTransform(DoubleSidedMeshTransform).has_value(), "Could not position mirrored-winding double-sided mesh");
		m_DoubleSidedMesh = *DoubleSidedMesh;

		auto Renderer = SceneRenderer::Create(m_Runtime, *m_Project, m_ShaderDirectory);
		Require(Renderer.has_value(), Renderer ? "Unexpected SceneRenderer creation state" : Renderer.error().Message);
		m_Renderer = std::move(*Renderer);

		RenderTargetDesc TargetDescription;
		TargetDescription.Width = OutputExtent;
		TargetDescription.Height = OutputExtent;
		TargetDescription.ColorFormat = ColorTargetFormat::RGBA8_Srgb;
		TargetDescription.DepthMode = DepthAttachmentMode::Attachment;
		TargetDescription.DebugName = "Mirrored-winding image validation";
		auto Target = m_Runtime.CreateRenderTarget(TargetDescription);
		Require(Target.has_value(), Target ? "Unexpected render-target creation state" : Target.error().Message);
		m_OutputTarget = std::move(*Target);
		PF_INFO("Initialized isolated single-sided open-triangle mirrored-winding image validation");
	}

	std::expected<bool, std::string> MirroredWindingValidation::Update()
	{
		if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(30))
			return std::unexpected("Timed out during mirrored-winding GPU validation");
		if (!m_Scene || !m_Renderer || !m_Parent || !m_Mesh || !m_DoubleSidedMesh || !m_Camera || !m_OutputTarget)
			return std::unexpected("Mirrored-winding GPU validation was not initialized");
		if (m_ReadbackPending)
		{
			const TransformCase& PreviousCase = TransformCases[m_ScenarioIndex];
			if (!m_ShadowProbe)
			{
				const Texture* Color = m_OutputTarget->GetColorTexture();
				if (!Color) return std::unexpected("Mirrored-winding output target has no color attachment");
				const auto Readback = m_Runtime.ReadTextureForValidation(*Color);
				if (!Readback) return std::unexpected("Could not read mirrored-winding output: " + Readback.error().Message);
				if (Readback->Format != TextureFormat::RGBA8_Srgb || Readback->Width != OutputExtent || Readback->Height != OutputExtent ||
					Readback->Pixels.size() != static_cast<size_t>(OutputExtent) * OutputExtent * 4)
					return std::unexpected("Mirrored-winding output readback has an unexpected format or extent");

				size_t DefaultMaterialPixels = 0;
				size_t DoubleSidedMaterialPixels = 0;
				for (size_t Pixel = 0; Pixel < static_cast<size_t>(OutputExtent) * OutputExtent; ++Pixel)
				{
					const uint8_t Red = std::to_integer<uint8_t>(Readback->Pixels[Pixel * 4]);
					const uint8_t Green = std::to_integer<uint8_t>(Readback->Pixels[Pixel * 4 + 1]);
					const uint8_t Blue = std::to_integer<uint8_t>(Readback->Pixels[Pixel * 4 + 2]);
					if (std::max({ Red, Green, Blue }) <= 12) continue;
					if (Green > Red + 20 && Green > Blue + 20) ++DoubleSidedMaterialPixels;
					else ++DefaultMaterialPixels;
				}
				if (DefaultMaterialPixels < MinimumVisiblePixels || DoubleSidedMaterialPixels < MinimumVisiblePixels)
					return std::unexpected(std::string(PreviousCase.Name) +
										   " material visibility was below threshold (default=" + std::to_string(DefaultMaterialPixels) +
										   ", double-sided=" + std::to_string(DoubleSidedMaterialPixels) + ")");

				PF_INFO("MIRRORED_WINDING_CASE={} default_pixels={} double_sided_pixels={} geometry_draws={}", PreviousCase.Name,
					DefaultMaterialPixels, DoubleSidedMaterialPixels, 2);

				if (const auto Removed = m_DoubleSidedMesh->RemoveMeshRenderer(); !Removed)
					return std::unexpected("Could not isolate the single-sided caster for shadow validation: " + Removed.error().Message);
				m_ShadowProbe = true;
				auto Prepared = m_Renderer->PrepareScene(*m_Scene, m_Camera->GetUUID(), 1.0f);
				if (!Prepared)
					return std::unexpected(
						std::string("Could not prepare ") + PreviousCase.Name + " single-sided shadow probe: " + Prepared.error().Message);
				m_ScenePrepared = true;
				m_ReadbackPending = false;
				return false;
			}

			const auto ReadDepth = [this](const Texture* TextureToRead, uint32_t ArraySlice,
									   std::string_view Name) -> std::expected<size_t, std::string>
			{
				if (!TextureToRead) return std::unexpected(std::string(Name) + " texture was not created");
				auto Readback = m_Runtime.ReadTextureForValidation(*TextureToRead, ArraySlice);
				if (!Readback)
					return std::unexpected(std::string("Could not read ") + std::string(Name) + " depth: " + Readback.error().Message);
				return CountWrittenDepthPixels(*Readback);
			};
			const Texture* DirectionalDepth = m_Renderer->GetDirectionalShadowTextureForValidation(0);
			auto DirectionalPixels = ReadDepth(DirectionalDepth, 0, "directional shadow");
			if (!DirectionalPixels || *DirectionalPixels == 0)
				return std::unexpected(
					std::string(PreviousCase.Name) + ": " +
					(DirectionalPixels ? "directional shadow map contains no rasterized geometry" : DirectionalPixels.error()));
			auto SpotlightPixels = ReadDepth(m_Renderer->GetSpotlightShadowTextureForValidation(0), 0, "spotlight shadow");
			if (!SpotlightPixels || *SpotlightPixels == 0)
				return std::unexpected(
					std::string(PreviousCase.Name) + ": " +
					(SpotlightPixels ? "spotlight shadow map contains no rasterized geometry" : SpotlightPixels.error()));
			auto AoPixels = ReadDepth(m_Renderer->GetAmbientOcclusionDepthTextureForValidation(), 0, "AO prepass");
			if (!AoPixels || *AoPixels == 0)
				return std::unexpected(
					std::string(PreviousCase.Name) + ": " + (AoPixels ? "AO prepass contains no rasterized geometry" : AoPixels.error()));

			size_t PointShadowPixels = 0;
			for (uint32_t Face = 0; Face < 6 && PointShadowPixels == 0; ++Face)
			{
				auto FacePixels = ReadDepth(m_Renderer->GetPointShadowFaceTextureForValidation(0, Face), Face, "point shadow");
				if (!FacePixels) return std::unexpected(std::string(PreviousCase.Name) + ": " + FacePixels.error());
				PointShadowPixels = *FacePixels;
			}
			if (PointShadowPixels == 0)
				return std::unexpected(std::string(PreviousCase.Name) + " did not rasterize the single-sided point-shadow caster");
			PF_INFO("MIRRORED_WINDING_SHADOW_CASE={} directional_pixels={} spotlight_pixels={} point_pixels={} ao_prepass_pixels={}",
				PreviousCase.Name, *DirectionalPixels, *SpotlightPixels, PointShadowPixels, *AoPixels);

			if (const auto Restored = m_DoubleSidedMesh->SetMeshRenderer({ m_MeshAssetID, m_DoubleSidedMaterialAssetID }); !Restored)
				return std::unexpected("Could not restore the double-sided validation mesh: " + Restored.error().Message);
			m_ShadowProbe = false;
			++m_ScenarioIndex;
			m_ReadbackPending = false;
		}
		if (IsComplete()) return true;

		const TransformCase& Case = TransformCases[m_ScenarioIndex];
		TransformComponent ParentTransform;
		ParentTransform.Scale = Case.ParentScale;
		TransformComponent MeshTransform;
		MeshTransform.Scale = Case.MeshScale;
		MeshTransform.Translation = { -0.55f, 0.0f, 0.0f };
		TransformComponent DoubleSidedMeshTransform;
		DoubleSidedMeshTransform.Scale = Case.MeshScale;
		DoubleSidedMeshTransform.Translation = { 0.55f, 0.0f, 0.0f };
		if (auto Result = m_Parent->SetTransform(ParentTransform); !Result)
			return std::unexpected("Could not update mirrored-winding parent: " + Result.error().Message);
		if (auto Result = m_Mesh->SetTransform(MeshTransform); !Result)
			return std::unexpected("Could not update mirrored-winding mesh: " + Result.error().Message);
		if (auto Result = m_DoubleSidedMesh->SetTransform(DoubleSidedMeshTransform); !Result)
			return std::unexpected("Could not update mirrored-winding double-sided mesh: " + Result.error().Message);

		const auto Prepared = m_Renderer->PrepareScene(*m_Scene, m_Camera->GetUUID(), 1.0f);
		if (!Prepared) return std::unexpected(std::string("Could not prepare ") + Case.Name + " scene: " + Prepared.error().Message);
		m_ScenePrepared = true;
		return false;
	}

	std::expected<void, std::string> MirroredWindingValidation::Render()
	{
		if (!m_ScenePrepared || IsComplete()) return {};
		const TransformCase& Case = TransformCases[m_ScenarioIndex];
		auto Rendered = m_Renderer->RenderPreparedSceneToOutput(*m_OutputTarget);
		if (!Rendered) return std::unexpected(std::string("Could not render ") + Case.Name + ": " + Rendered.error().Message);
		if (!m_ShadowProbe)
		{
			if (Rendered->GeometryDrawCount != 2)
				return std::unexpected(std::string(Case.Name) + " did not submit both single- and double-sided triangle draws");
			if (Rendered->SpotlightShadowPassCount != 1 || Rendered->SpotlightShadowCasterDrawCount != 2)
				return std::unexpected(std::string(Case.Name) + " did not submit both materials to the spotlight shadow pass");
			if (Rendered->PointShadowPassCount != 6 || Rendered->PointShadowCasterDrawCount != 12)
				return std::unexpected(std::string(Case.Name) + " did not submit both materials to all point-shadow faces");
		}
		else
		{
			if (Rendered->GeometryDrawCount != 1)
				return std::unexpected(std::string(Case.Name) + " single-sided shadow probe rendered an unexpected geometry count");
			if (Rendered->SpotlightShadowPassCount != 1 || Rendered->SpotlightShadowCasterDrawCount != 1)
				return std::unexpected(std::string(Case.Name) + " did not submit the single-sided caster to the spotlight pass");
			if (Rendered->PointShadowPassCount != 6 || Rendered->PointShadowCasterDrawCount != 6)
				return std::unexpected(std::string(Case.Name) + " did not submit the single-sided caster to all point-shadow faces");
		}

		m_ScenePrepared = false;
		m_ReadbackPending = true;
		return {};
	}

	bool MirroredWindingValidation::IsComplete() const noexcept
	{
		return m_ScenarioIndex >= TransformCases.size();
	}

	void MirroredWindingValidation::Reset() noexcept
	{
		m_OutputTarget.reset();
		m_Renderer.reset();
		m_Mesh.reset();
		m_DoubleSidedMesh.reset();
		m_Parent.reset();
		m_Light.reset();
		m_SpotLight.reset();
		m_PointLight.reset();
		m_Camera.reset();
		m_Scene.reset();
		m_Project.reset();
		if (!m_FixtureRoot.empty())
		{
			std::error_code Error;
			std::filesystem::remove_all(m_FixtureRoot, Error);
			if (Error) PF_WARN("Could not remove mirrored-winding fixture {}: {}", m_FixtureRoot.string(), Error.message());
			m_FixtureRoot.clear();
		}
	}
} // namespace PulseForge::GpuValidation
