#pragma once

#include "Assets/AssetRegistry.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	enum class ProjectAssetDestinationCreateKind : uint8_t
	{
		None,
		NewScene,
		SaveSceneAs,
		Prefab
	};

	class EditorAssetWorkflowState final
	{
	public:
		void BeginAssetDestination(const PulseForge::AssetRecord& Asset, bool Duplicate);
		void BeginSceneDestination(bool SaveAs, const std::optional<PulseForge::AssetRecord>& Source);
		void BeginPrefabDestination(const PulseForge::UUID& Root);
		void DismissDestination() noexcept;
		[[nodiscard]] bool TakeDestinationDialogRequest() noexcept;

		void BeginDelete(const PulseForge::AssetID& Asset);
		void DismissDelete() noexcept;
		[[nodiscard]] bool TakeDeleteDialogRequest() noexcept;

		[[nodiscard]] const std::optional<PulseForge::AssetID>& DestinationAsset() const noexcept { return m_DestinationAsset; }
		[[nodiscard]] bool IsDuplicate() const noexcept { return m_IsDuplicate; }
		[[nodiscard]] ProjectAssetDestinationCreateKind CreateKind() const noexcept { return m_CreateKind; }
		[[nodiscard]] const std::optional<PulseForge::UUID>& PrefabRoot() const noexcept { return m_PrefabRoot; }
		[[nodiscard]] std::string& DestinationFolder() noexcept { return m_DestinationFolder; }
		[[nodiscard]] std::array<char, 1024>& FilenameBuffer() noexcept { return m_Filename; }
		[[nodiscard]] std::array<char, 256>& SearchBuffer() noexcept { return m_Search; }
		[[nodiscard]] std::string& ValidationError() noexcept { return m_ValidationError; }
		[[nodiscard]] const std::optional<PulseForge::AssetID>& PendingDeleteAsset() const noexcept { return m_PendingDeleteAsset; }

	private:
		void InitializeDestination(std::string_view Folder, std::string_view Filename);
		static void CopyText(std::span<char> Destination, std::string_view Source) noexcept;

		std::optional<PulseForge::AssetID> m_DestinationAsset;
		std::optional<PulseForge::UUID> m_PrefabRoot;
		std::optional<PulseForge::AssetID> m_PendingDeleteAsset;
		std::array<char, 1024> m_Filename{};
		std::array<char, 256> m_Search{};
		std::string m_DestinationFolder = "Assets";
		std::string m_ValidationError;
		ProjectAssetDestinationCreateKind m_CreateKind = ProjectAssetDestinationCreateKind::None;
		bool m_IsDuplicate = false;
		bool m_DestinationDialogRequested = false;
		bool m_DeleteDialogRequested = false;
	};
} // namespace PulseForgeEditor
