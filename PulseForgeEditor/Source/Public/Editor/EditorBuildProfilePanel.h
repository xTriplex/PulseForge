#pragma once

#include "Assets/AssetExportService.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace PulseForgeEditor
{
	class EditorStyle;

	enum class EditorBuildProfileActionKind : uint8_t
	{
		None,
		NewProfile,
		LoadProfile,
		SaveProfile,
		SaveProfileAs,
		SelectTemplate,
		StartExport,
		CancelExport
	};

	struct EditorBuildProfileAction final
	{
		EditorBuildProfileActionKind Kind = EditorBuildProfileActionKind::None;
	};

	struct EditorBuildProfileDrawResult final
	{
		EditorBuildProfileAction Action;
		std::optional<PulseForge::ExportBuildProfile> UpdatedProfile;
	};

	class EditorBuildProfilePanel final
	{
	public:
		[[nodiscard]] EditorBuildProfileDrawResult DrawContents(
			const PulseForge::ExportBuildProfile* Profile,
			std::string_view ProfilePath,
			std::string_view TemplateDescriptorPath,
			bool ProjectOpen,
			bool SavedContentReady,
			const PulseForge::AssetExportSnapshot& Export,
			std::string_view StatusMessage,
			EditorStyle& Style);

	private:
		std::array<char, 40> m_StartupSceneBuffer{};
		std::array<char, 40> m_DynamicRootBuffer{};
		PulseForge::UUID m_StartupSceneProfileID;
		PulseForge::UUID m_StartupSceneValue;
		bool m_StartupSceneBufferInitialized = false;
	};
} // namespace PulseForgeEditor
