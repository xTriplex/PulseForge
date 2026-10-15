#include "Editor/EditorMaterialDraftController.h"

#include "Assets/MaterialAssetService.h"
#include "Editor/EditorMaterialEditorPolicy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
		}

		std::string NormalizedExtension(const std::filesystem::path& Path)
		{
			std::string Extension = Path.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](unsigned char Character)
			{
				return static_cast<char>(std::tolower(Character));
			});
			return Extension;
		}
	}

	std::string EditorMaterialDraftController::MakeKey(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset)
	{
		return MakeMaterialDraftKey(PathToUtf8(Project.GetRootPath().lexically_normal()), Asset.ToString());
	}

	PulseForge::MaterialAssetDesc* EditorMaterialDraftController::FindDraft(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset)
	{
		return m_Drafts.FindDraft(MakeKey(Project, Asset));
	}

	bool EditorMaterialDraftController::ContainsDraft(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset) const
	{
		return m_Drafts.ContainsDraft(MakeKey(Project, Asset));
	}

	EditorMaterialDraftController::DraftResult EditorMaterialDraftController::LoadDraft(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset)
	{
		const std::string Key = MakeKey(Project, Asset);
		if (auto* Draft = m_Drafts.FindDraft(Key))
			return std::ref(*Draft);
		if (const std::string* Error = m_Drafts.FindError(Key))
			return std::unexpected(*Error);

		const auto Record = Project.GetAssetRegistry().Find(Asset);
		if (!Record || NormalizedExtension(Record->ProjectRelativePath) != ".material")
		{
			const std::string Error = "The selected material is no longer registered.";
			m_Drafts.SetError(Key, Error);
			return std::unexpected(Error);
		}

		auto Loaded = PulseForge::MaterialAssetService::Load(
			Asset, Project.GetRootPath(), Project.GetAssetRegistry());
		if (!Loaded)
		{
			m_Drafts.SetError(Key, Loaded.error().Message);
			return std::unexpected(Loaded.error().Message);
		}
		return std::ref(m_Drafts.PutDraft(Key, std::move(*Loaded)));
	}

	void EditorMaterialDraftController::ReplaceDraft(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset,
		PulseForge::MaterialAssetDesc Draft)
	{
		m_Drafts.ReplaceDraft(MakeKey(Project, Asset), std::move(Draft));
	}

	const std::string* EditorMaterialDraftController::FindError(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset) const
	{
		return m_Drafts.FindError(MakeKey(Project, Asset));
	}

	void EditorMaterialDraftController::ClearLoadError(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset)
	{
		m_Drafts.ClearError(MakeKey(Project, Asset));
	}

	bool EditorMaterialDraftController::IsDirty(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset) const
	{
		return m_Drafts.IsDirty(MakeKey(Project, Asset));
	}

	bool EditorMaterialDraftController::CanUndo(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset) const
	{
		return m_Drafts.CanUndo(MakeKey(Project, Asset));
	}

	bool EditorMaterialDraftController::CanRedo(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset) const
	{
		return m_Drafts.CanRedo(MakeKey(Project, Asset));
	}

	bool EditorMaterialDraftController::BeginEdit(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset,
		const PulseForge::MaterialAssetDesc& InitialState)
	{
		return m_Drafts.BeginEdit(MakeKey(Project, Asset), InitialState);
	}

	void EditorMaterialDraftController::FinalizePendingEdits(bool HasActiveImGuiItem)
	{
		m_Drafts.FinalizePendingEdits(HasActiveImGuiItem);
	}

	std::optional<bool> EditorMaterialDraftController::ApplyHistoryStep(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset,
		bool Redo,
		EditorDocumentManager& Documents)
	{
		const std::string Key = MakeKey(Project, Asset);
		const auto Changed = m_Drafts.ApplyHistoryStep(Key, Redo);
		if (!Changed || !*Changed)
			return Changed;

		const PulseForgeEditor::EditorDocumentIdentity Identity{
			Project.GetDescription().ID, Asset, PulseForgeEditor::EditorDocumentKind::Material
		};
		if (auto* Session = Documents.Find(Identity))
		{
			if (!Documents.MarkEdited(Identity))
				return std::nullopt;
			if (!m_Drafts.IsDirty(Key))
				Session->SavedRevision = Session->Revision;
		}
		return Changed;
	}

	std::expected<std::filesystem::path, std::string> EditorMaterialDraftController::Save(
		PulseForge::Project& Project,
		const PulseForge::AssetID& Asset,
		EditorDocumentManager& Documents)
	{
		const auto Record = Project.GetAssetRegistry().Find(Asset);
		if (!Record || NormalizedExtension(Record->ProjectRelativePath) != ".material")
			return std::unexpected("The material asset is no longer registered.");

		const std::string Key = MakeKey(Project, Asset);
		PulseForge::MaterialAssetDesc* const Draft = m_Drafts.FindDraft(Key);
		if (Draft && m_Drafts.IsDirty(Key))
		{
			const auto Saved = PulseForge::MaterialAssetService::Save(
				Asset, Project.GetRootPath(), Project.GetAssetRegistry(), *Draft);
			if (!Saved)
				return std::unexpected(Saved.error().Message);
			m_Drafts.MarkSaved(Key);

			const PulseForgeEditor::EditorDocumentIdentity Identity{
				Project.GetDescription().ID, Asset, PulseForgeEditor::EditorDocumentKind::Material
			};
			if (const auto* Session = Documents.Find(Identity))
				(void)Documents.MarkSaved(Identity, Session->Revision);
		}
		return Project.GetRootPath() / Record->ProjectRelativePath;
	}

	std::expected<void, std::string> EditorMaterialDraftController::Reload(
		PulseForge::Project& Project,
		const PulseForge::AssetID& Asset,
		EditorDocumentManager& Documents)
	{
		auto Loaded = PulseForge::MaterialAssetService::Load(
			Asset, Project.GetRootPath(), Project.GetAssetRegistry());
		if (!Loaded)
			return std::unexpected(Loaded.error().Message);

		m_Drafts.ReplaceDraft(MakeKey(Project, Asset), std::move(*Loaded));
		const PulseForgeEditor::EditorDocumentIdentity Identity{
			Project.GetDescription().ID, Asset, PulseForgeEditor::EditorDocumentKind::Material
		};
		if (auto* Session = Documents.Find(Identity))
			Session->Revision = Session->SavedRevision;
		return {};
	}

	std::vector<PulseForge::AssetID> EditorMaterialDraftController::GetDirtyAssets(
		const PulseForge::Project& Project) const
	{
		const std::string Prefix = PathToUtf8(Project.GetRootPath().lexically_normal()) + ":";
		std::vector<PulseForge::AssetID> Assets;
		for (const std::string& Key : m_Drafts.GetDirtyKeys())
		{
			if (!Key.starts_with(Prefix))
				continue;
			const size_t Separator = Key.rfind(':');
			if (Separator == std::string::npos)
				continue;
			if (auto Asset = PulseForge::UUID::Parse(std::string_view(Key).substr(Separator + 1)))
				Assets.push_back(*Asset);
		}
		return Assets;
	}

	void EditorMaterialDraftController::Forget(
		const PulseForge::Project& Project,
		const PulseForge::AssetID& Asset)
	{
		m_Drafts.Forget(MakeKey(Project, Asset));
	}

	void EditorMaterialDraftController::ClearProject(const PulseForge::Project& Project)
	{
		m_Drafts.ClearProject(PathToUtf8(Project.GetRootPath().lexically_normal()) + ":");
	}
}
