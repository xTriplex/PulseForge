#include "Editor/EditorBlueprintProgramCatalog.h"

#include "Assets/AssetTypes.h"
#include "Assets/Project.h"
#include "Core/BoundedInput.h"
#include "Scene/Components/BlueprintComponent.h"

#include <algorithm>
#include <fstream>

namespace PulseForgeEditor
{
	void EditorBlueprintProgramCatalog::AdvanceGeneration() noexcept
	{
		++m_Generation;
		if (m_Generation == 0) ++m_Generation;
	}

	void EditorBlueprintProgramCatalog::SetProject(const PulseForge::Project* Project) noexcept
	{
		const PulseForge::UUID Identity = Project ? Project->GetDescription().ID : PulseForge::UUID{};
		if (Project == m_Project && Identity == m_ProjectIdentity) return;
		m_Project = Project;
		m_ProjectIdentity = Identity;
		m_Programs.clear();
		m_LoadFailures.clear();
		m_AssetRevisions.clear();
		AdvanceGeneration();
	}

	void EditorBlueprintProgramCatalog::ProgramPublished(const PulseForge::AssetID& Asset) noexcept
	{
		m_Programs.erase(Asset);
		m_LoadFailures.erase(Asset);
		uint64_t& Revision = m_AssetRevisions[Asset];
		if (Revision == 0) Revision = 1;
		++Revision;
		if (Revision == 0) ++Revision;
		AdvanceGeneration();
	}

	uint64_t EditorBlueprintProgramCatalog::GetAssetRevision(const PulseForge::AssetID& Asset) const noexcept
	{
		const auto Found = m_AssetRevisions.find(Asset);
		return Found == m_AssetRevisions.end() || Found->second == 0 ? 1 : Found->second;
	}

	void EditorBlueprintProgramCatalog::BindRuntimeServices(PulseForge::SceneRuntimeServices& Services)
	{
		Services.ResolveBlueprintProgram = [this](const PulseForge::Entity& Target, PulseForge::BlueprintEventKind Event)
			{ return Resolve(Target, Event); };
		Services.GetBlueprintProgramGeneration = [this] { return GetGeneration(); };
		Services.GetBlueprintProgramRevision = [this](const PulseForge::AssetID& Asset) { return GetAssetRevision(Asset); };
		Services.ReadBlueprintScalarProperty = [this](const PulseForge::Entity& Target, std::string_view PropertyID)
			{ return ReadScalarProperty(Target, PropertyID); };
		Services.ApplyBlueprintPropertyWrites = [this](const PulseForge::Entity& Target,
			std::span<const PulseForge::BlueprintScalarPropertyWrite> Writes)
			{ return ApplyPropertyWrites(Target, Writes); };
	}

	std::optional<PulseForge::BlueprintProgramBinding> EditorBlueprintProgramCatalog::Resolve(
		const PulseForge::Entity& Target, PulseForge::BlueprintEventKind Event)
	{
		if (!m_Project || !Target.IsValid()) return std::nullopt;
		const auto Component = Target.GetBlueprint();
		if (!Component || !Component->has_value() || !(**Component).Enabled) return std::nullopt;
		const PulseForge::AssetID Asset = (**Component).BlueprintAsset;
		if (m_LoadFailures.contains(Asset)) return std::nullopt;
		auto Found = m_Programs.find(Asset);
		if (Found == m_Programs.end())
		{
			auto Fail = [this, &Asset](std::string Message) -> std::optional<PulseForge::BlueprintProgramBinding>
			{
				m_LoadFailures.insert_or_assign(Asset, std::move(Message));
				return std::nullopt;
			};
			const auto Record = m_Project->GetAssetRegistry().Find(Asset);
			if (!Record || !PulseForge::HasAssetTypeExtension(Record->ProjectRelativePath, PulseForge::AssetFileType::Blueprint))
				return Fail("Blueprint asset is not registered as a .pfblueprint in the open project.");

			std::ifstream Input(m_Project->GetRootPath() / Record->ProjectRelativePath, std::ios::binary);
			if (!Input) return Fail("Blueprint program could not be opened.");
			const auto Bytes = PulseForge::Detail::ReadBoundedInput(Input, PulseForge::BlueprintMaximumAssetBytes);
			if (!Bytes) return Fail("Blueprint program could not be read within its supported size.");
			std::vector<std::byte> Buffer(Bytes->size());
			std::transform(Bytes->begin(), Bytes->end(), Buffer.begin(), [](char Value)
				{ return static_cast<std::byte>(static_cast<unsigned char>(Value)); });
			const auto Decoded = PulseForge::DeserializeBlueprintAsset(Buffer);
			if (!Decoded) return Fail("Blueprint program is invalid: " + Decoded.error().Message);
			if (Decoded->GetSourceRevision() != 0)
			{
				const std::filesystem::path GraphPath = m_Project->GetRootPath() / ".pulseforge" / "blueprint-graphs" /
					(Asset.ToString() + ".pfg");
				std::ifstream GraphInput(GraphPath, std::ios::binary);
				if (!GraphInput) return Fail("Blueprint graph source is missing; reopen and save the Blueprint to rebuild its program.");
				const auto GraphBytes = PulseForge::Detail::ReadBoundedInput(GraphInput, PulseForge::BlueprintMaximumAssetBytes);
				if (!GraphBytes)
					return Fail("Blueprint graph source could not be read within its supported size; program publication was rejected.");
				const auto GraphSource = std::as_bytes(std::span<const char>(GraphBytes->data(), GraphBytes->size()));
				if (!PulseForge::DoesBlueprintSourceRevisionMatch(*Decoded, GraphSource))
					return Fail("Compiled Blueprint program does not match the current graph source; save the Blueprint to rebuild it.");
			}
			Found = m_Programs.emplace(Asset,
				std::make_shared<const PulseForge::VerifiedBlueprintAsset>(std::move(*Decoded))).first;
			m_LoadFailures.erase(Asset);
		}
		uint64_t& Revision = m_AssetRevisions[Asset];
		if (Revision == 0) Revision = 1;

		for (const PulseForge::BlueprintVerifiedEventProgram& Candidate : Found->second->GetEvents())
		{
			if (Candidate.Event != Event) continue;
			auto Program = std::shared_ptr<const PulseForge::VerifiedBlueprintProgram>(Found->second, &Candidate.Program);
			return PulseForge::BlueprintProgramBinding{ Asset, Revision, std::move(Program) };
		}
		return std::nullopt;
	}

	std::optional<std::string> EditorBlueprintProgramCatalog::GetLoadFailure(const PulseForge::AssetID& Asset) const
	{
		const auto Found = m_LoadFailures.find(Asset);
		return Found == m_LoadFailures.end() ? std::nullopt : std::optional<std::string>{ Found->second };
	}

	std::optional<float> EditorBlueprintProgramCatalog::ReadScalarProperty(
		const PulseForge::Entity& Target, std::string_view PropertyID) const
	{
		const auto Blueprint = Target.GetBlueprint();
		return Blueprint && Blueprint->has_value() ? (**Blueprint).FindFloatProperty(PropertyID) : std::nullopt;
	}

	bool EditorBlueprintProgramCatalog::ApplyPropertyWrites(
		const PulseForge::Entity& Target, std::span<const PulseForge::BlueprintScalarPropertyWrite> Writes) const
	{
		auto Existing = Target.GetBlueprint();
		if (!Existing || !Existing->has_value()) return false;
		PulseForge::BlueprintComponent Candidate = **Existing;
		for (const PulseForge::BlueprintScalarPropertyWrite& Write : Writes)
			if (!Candidate.SetFloatProperty(Write.PropertyId, Write.Value)) return false;
		return Target.SetBlueprint(Candidate).has_value();
	}
}
