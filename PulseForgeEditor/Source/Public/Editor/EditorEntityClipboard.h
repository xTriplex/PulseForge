#pragma once

#include "Assets/AssetID.h"
#include "Scene/UUID.h"

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	class EditorEntityClipboard final
	{
	public:
		struct PreparedPaste final
		{
			std::string SerializedScene;
			std::vector<PulseForge::UUID> RootIdentifiers;
			size_t EntityCount = 0;
		};

		using AssetAvailable = std::function<bool(const PulseForge::UUID&)>;
		using IdentifierGenerator = std::function<std::expected<PulseForge::UUID, std::string>()>;

		[[nodiscard]] bool HasData() const noexcept { return !m_Records.empty(); }
		[[nodiscard]] bool CanPaste(const PulseForge::UUID& Project) const noexcept;

		[[nodiscard]] static std::expected<EditorEntityClipboard, std::string> Capture(
			PulseForge::UUID Project,
			std::optional<PulseForge::AssetID> Scene,
			std::span<const PulseForge::UUID> Roots,
			std::span<const PulseForge::UUID> IncludedEntities,
			std::string_view SerializedScene);

		[[nodiscard]] std::expected<PreparedPaste, std::string> PreparePaste(
			PulseForge::UUID TargetProject,
			std::optional<PulseForge::AssetID> TargetScene,
			std::string_view SerializedTargetScene,
			const AssetAvailable& IsAssetAvailable,
			IdentifierGenerator GenerateIdentifier = {}) const;

	private:
		PulseForge::UUID m_Project;
		std::optional<PulseForge::AssetID> m_Scene;
		std::vector<PulseForge::UUID> m_Roots;
		std::vector<std::string> m_Records;
	};
}
