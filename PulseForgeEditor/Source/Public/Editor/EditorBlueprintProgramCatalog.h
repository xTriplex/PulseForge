#pragma once

#include "Assets/AssetID.h"
#include "Runtime/SceneRuntime.h"
#include "Scripting/BlueprintAsset.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class Project;
}

namespace PulseForgeEditor
{
	// Project-scoped, immutable Blueprint programs used by Editor Play.
	class EditorBlueprintProgramCatalog final
	{
	public:
		void SetProject(const PulseForge::Project* Project) noexcept;
		void ProgramPublished(const PulseForge::AssetID& Asset) noexcept;
		void BindRuntimeServices(PulseForge::SceneRuntimeServices& Services);

		[[nodiscard]] uint64_t GetGeneration() const noexcept { return m_Generation; }
		[[nodiscard]] uint64_t GetAssetRevision(const PulseForge::AssetID& Asset) const noexcept;
		[[nodiscard]] std::optional<PulseForge::BlueprintProgramBinding> Resolve(
			const PulseForge::Entity& Target, PulseForge::BlueprintEventKind Event);
		[[nodiscard]] std::optional<std::string> GetLoadFailure(const PulseForge::AssetID& Asset) const;

	private:
		[[nodiscard]] std::optional<float> ReadScalarProperty(
			const PulseForge::Entity& Target, std::string_view PropertyID) const;
		[[nodiscard]] bool ApplyPropertyWrites(
			const PulseForge::Entity& Target, std::span<const PulseForge::BlueprintScalarPropertyWrite> Writes) const;
		void AdvanceGeneration() noexcept;

		const PulseForge::Project* m_Project = nullptr;
		PulseForge::UUID m_ProjectIdentity{};
		uint64_t m_Generation = 1;
		std::unordered_map<PulseForge::AssetID,
			std::shared_ptr<const PulseForge::VerifiedBlueprintAsset>, PulseForge::UUIDHash> m_Programs;
		std::unordered_map<PulseForge::AssetID, std::string, PulseForge::UUIDHash> m_LoadFailures;
		std::unordered_map<PulseForge::AssetID, uint64_t, PulseForge::UUIDHash> m_AssetRevisions;
	};
}
