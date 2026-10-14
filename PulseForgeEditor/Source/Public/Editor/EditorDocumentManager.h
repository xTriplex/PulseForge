#pragma once

#include "Assets/AssetRegistry.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace PulseForgeEditor
{
	enum class EditorDocumentKind : uint8_t
	{
		Level,
		Material,
		Texture
	};

	enum class EditorDocumentOpenPreference : uint8_t
	{
		NewNativeWindow,
		MainDocumentStrip,
		RememberedPlacement
	};

	enum class EditorDocumentHostRole : uint8_t
	{
		Main,
		Native
	};

	enum class EditorDocumentTransferCause : uint8_t
	{
		User,
		Recovery,
		WorkspaceRestore
	};

	enum class EditorDocumentCloseChoice : uint8_t
	{
		Save,
		Discard,
		Cancel
	};

	enum class EditorDocumentCloseStatus : uint8_t
	{
		Closed,
		Missing,
		Protected,
		Cancelled,
		SaveFailed,
		StillDirty,
		DiscardFailed
	};

	struct EditorDocumentIdentity
	{
		PulseForge::UUID Project;
		PulseForge::AssetID Asset;
		EditorDocumentKind Kind = EditorDocumentKind::Material;

		friend bool operator==(const EditorDocumentIdentity&, const EditorDocumentIdentity&) = default;
	};

	struct EditorDocumentIdentityHash
	{
		[[nodiscard]] size_t operator()(const EditorDocumentIdentity& Identity) const noexcept;
	};

	class EditorDocumentEditorState
	{
	public:
		virtual ~EditorDocumentEditorState() = default;
	};

	struct EditorDocumentSession
	{
		EditorDocumentIdentity Identity;
		std::filesystem::path ProjectRelativePath;
		std::string DisplayName;
		std::string HostID;
		uint64_t Revision = 0;
		uint64_t SavedRevision = 0;
		std::unique_ptr<EditorDocumentEditorState> EditorState;

		[[nodiscard]] bool IsDirty() const noexcept { return Revision != SavedRevision; }
	};

	struct EditorDocumentHost
	{
		std::string ID;
		EditorDocumentHostRole Role = EditorDocumentHostRole::Native;
		EditorDocumentKind PlacementKind = EditorDocumentKind::Material;
		struct WindowBounds
		{
			float X = 0.0f;
			float Y = 0.0f;
			float Width = 0.0f;
			float Height = 0.0f;
			float DpiScale = 1.0f;
			std::string MonitorName;
			bool Maximized = false;
			bool HasValue = false;
		};
		WindowBounds Bounds;
		std::optional<PulseForge::UUID> Project;
		std::vector<EditorDocumentIdentity> Documents;
		std::optional<EditorDocumentIdentity> ActiveDocument;

		[[nodiscard]] bool IsEmpty() const noexcept { return Documents.empty(); }
	};

	struct EditorDocumentPlacement
	{
		EditorDocumentHostRole Role = EditorDocumentHostRole::Native;
		EditorDocumentHost::WindowBounds Bounds;
		std::optional<std::string> LiveHostID;
	};

	struct EditorDocumentPlacementKey
	{
		PulseForge::UUID Project;
		EditorDocumentKind Kind = EditorDocumentKind::Material;

		friend bool operator==(const EditorDocumentPlacementKey&, const EditorDocumentPlacementKey&) = default;
	};

	struct EditorDocumentPlacementKeyHash
	{
		[[nodiscard]] size_t operator()(const EditorDocumentPlacementKey& Key) const noexcept;
	};

	struct EditorDocumentOpenResult
	{
		EditorDocumentIdentity Identity;
		std::string HostID;
		bool Created = false;
	};

	struct EditorDocumentTransferResult
	{
		bool Changed = false;
		std::optional<std::string> EmptiedSourceHost;
	};

	class EditorDocumentManager final
	{
	public:
		using TransferPreparation = std::function<bool(const EditorDocumentSession&, const EditorDocumentHost&)>;
		using SaveDocument = std::function<bool(const EditorDocumentSession&, uint64_t)>;
		using DiscardDocument = std::function<bool(const EditorDocumentSession&)>;

		static constexpr std::string_view MainHostID = "main";

		EditorDocumentManager();

		[[nodiscard]] std::expected<EditorDocumentOpenResult, std::string> Open(
			const EditorDocumentIdentity& Identity,
			const std::filesystem::path& ProjectRelativePath,
			std::string DisplayName,
			EditorDocumentOpenPreference Preference);
		[[nodiscard]] bool UpdateAssetLocation(
			const EditorDocumentIdentity& Identity,
			const std::filesystem::path& ProjectRelativePath,
			std::string DisplayName);
		[[nodiscard]] std::expected<std::string, std::string> CreateNativeHost(EditorDocumentKind PlacementKind);
		[[nodiscard]] std::expected<std::string, std::string> CreateNativeHost(
			std::string HostID,
			EditorDocumentKind PlacementKind,
			EditorDocumentHost::WindowBounds Bounds = {});
		[[nodiscard]] bool RemoveEmptyNativeHost(std::string_view HostID);
		[[nodiscard]] std::expected<EditorDocumentTransferResult, std::string> Transfer(
			const EditorDocumentIdentity& Identity,
			std::string_view DestinationHostID,
			size_t DestinationIndex,
			const TransferPreparation& Prepare = {},
			EditorDocumentTransferCause Cause = EditorDocumentTransferCause::User);
		[[nodiscard]] std::expected<size_t, std::string> RecoverAllToMain(std::string_view SourceHostID);
		[[nodiscard]] bool Activate(const EditorDocumentIdentity& Identity) noexcept;
		[[nodiscard]] bool FocusHost(std::string_view HostID) noexcept;
		[[nodiscard]] bool SetRememberedPlacement(
			const PulseForge::UUID& Project,
			EditorDocumentKind Kind,
			EditorDocumentHostRole Role,
			EditorDocumentHost::WindowBounds Bounds = {},
			std::optional<std::string> LiveHostID = std::nullopt);
		[[nodiscard]] bool SetHostBounds(std::string_view HostID, EditorDocumentHost::WindowBounds Bounds);
		[[nodiscard]] bool MarkEdited(const EditorDocumentIdentity& Identity) noexcept;
		[[nodiscard]] bool MarkSaved(const EditorDocumentIdentity& Identity, uint64_t Revision) noexcept;
		[[nodiscard]] std::vector<std::string> ClearProject(const PulseForge::UUID& Project);
		[[nodiscard]] EditorDocumentCloseStatus Close(
			const EditorDocumentIdentity& Identity,
			EditorDocumentCloseChoice Choice,
			const SaveDocument& Save,
			const DiscardDocument& Discard);

		[[nodiscard]] const EditorDocumentSession* Find(const EditorDocumentIdentity& Identity) const noexcept;
		[[nodiscard]] EditorDocumentSession* Find(const EditorDocumentIdentity& Identity) noexcept;
		[[nodiscard]] const EditorDocumentHost* FindHost(std::string_view HostID) const noexcept;
		[[nodiscard]] EditorDocumentHost* FindHost(std::string_view HostID) noexcept;
		[[nodiscard]] const EditorDocumentHost* GetFocusedHost() const noexcept;
		[[nodiscard]] const std::unordered_map<EditorDocumentIdentity, EditorDocumentSession,
			EditorDocumentIdentityHash>& GetSessions() const noexcept { return m_Sessions; }
		[[nodiscard]] const std::vector<EditorDocumentHost>& GetHosts() const noexcept { return m_Hosts; }
		[[nodiscard]] EditorDocumentOpenPreference GetOpeningPreference() const noexcept { return m_OpeningPreference; }
		void SetOpeningPreference(EditorDocumentOpenPreference Preference) noexcept { m_OpeningPreference = Preference; }
		[[nodiscard]] std::vector<std::pair<EditorDocumentKind, EditorDocumentPlacement>>
		GetRememberedPlacements(const PulseForge::UUID& Project) const;

	private:
		[[nodiscard]] EditorDocumentHost* CreateHost(std::string ID, EditorDocumentHostRole Role,
			EditorDocumentKind PlacementKind);
		void RemoveSessionFromHost(const EditorDocumentIdentity& Identity, const std::string& HostID) noexcept;

		std::unordered_map<EditorDocumentIdentity, EditorDocumentSession, EditorDocumentIdentityHash> m_Sessions;
		std::vector<EditorDocumentHost> m_Hosts;
		std::unordered_map<EditorDocumentPlacementKey, EditorDocumentPlacement,
			EditorDocumentPlacementKeyHash> m_RememberedPlacements;
		std::string m_FocusedHostID = std::string(MainHostID);
		EditorDocumentOpenPreference m_OpeningPreference = EditorDocumentOpenPreference::NewNativeWindow;
	};
}
