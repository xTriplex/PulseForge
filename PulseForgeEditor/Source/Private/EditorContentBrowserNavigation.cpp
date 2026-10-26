#include "Editor/EditorContentBrowserNavigation.h"

#include "Editor/EditorStyle.h"

#include <misc/cpp/imgui_stdlib.h>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}

		std::string FitFolderLabel(std::string_view Label, float AvailableWidth)
		{
			constexpr std::string_view Ellipsis = "\xe2\x80\xa6";
			if (ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x <= AvailableWidth) return std::string(Label);

			size_t PrefixLength = Utf8SafePrefixLength(Label, 480);
			const float EllipsisWidth = ImGui::CalcTextSize(Ellipsis.data(), Ellipsis.data() + Ellipsis.size()).x;
			while (PrefixLength > 0)
			{
				const std::string_view Prefix = Label.substr(0, PrefixLength);
				if (ImGui::CalcTextSize(Prefix.data(), Prefix.data() + Prefix.size()).x + EllipsisWidth <= AvailableWidth)
					return std::string(Prefix) + std::string(Ellipsis);
				PrefixLength = Utf8SafePrefixLength(Label, PrefixLength - 1);
			}
			return EllipsisWidth <= AvailableWidth ? std::string(Ellipsis) : std::string{};
		}
	} // namespace

	void EditorContentBrowserNavigation::DrawFolderTree(EditorContentBrowserState& State, EditorStyle& Style,
		EditorContentBrowserNavigationActions& Actions, std::function<void()>& DeferredAction)
	{
		DrawFolderNode(State, State.FolderRoot(), Style, Actions, DeferredAction, true);
	}

	void EditorContentBrowserNavigation::DrawFolderNode(EditorContentBrowserState& State, const EditorContentFolderNode& Node,
		EditorStyle& Style, EditorContentBrowserNavigationActions& Actions, std::function<void()>& DeferredAction, bool IsRoot)
	{
		if (IsRoot) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (Node.RelativePath == State.CurrentFolder()) Flags |= ImGuiTreeNodeFlags_Selected;
		if (Node.Children.empty()) Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		const std::string StableID = PathToUtf8(Node.RelativePath);
		const bool IsOpen = Style.TreeNode(
			Node.RelativePath == State.CurrentFolder() ? EditorIcon::FolderOpen : EditorIcon::Folder, Node.Name, StableID, Flags);
		if (ImGui::IsItemClicked()) State.SetCurrentFolder(Node.RelativePath);
		BeginFolderDragSource(Node.RelativePath, Node.Name);
		HandleFolderDropTarget(State, Node.RelativePath, Actions, DeferredAction);
		if (IsOpen && !Node.Children.empty())
		{
			for (const EditorContentFolderNode& Child : Node.Children)
				DrawFolderNode(State, Child, Style, Actions, DeferredAction);
			ImGui::TreePop();
		}
	}

	void EditorContentBrowserNavigation::BeginFolderDragSource(const std::filesystem::path& Folder, std::string_view Name)
	{
		if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) return;
		const std::string Payload = PathToUtf8(Folder);
		ImGui::SetDragDropPayload("PF_MANAGED_FOLDER_MOVE", Payload.c_str(), Payload.size() + 1);
		ImGui::Text("Move folder: %.*s", static_cast<int>(Name.size()), Name.data());
		ImGui::EndDragDropSource();
	}

	void EditorContentBrowserNavigation::DrawFolderContextMenu(
		const std::filesystem::path& Folder, EditorContentBrowserNavigationActions& Actions, std::function<void()>& DeferredAction)
	{
		if (DeferredAction || !ImGui::BeginPopupContextItem("ManagedFolderContext")) return;
		if (ImGui::MenuItem("Rename folder"))
		{
			m_RenameFolder = Folder;
			m_FolderRenameText = PathToUtf8(Folder.filename());
			m_OpenFolderRenamePopup = true;
		}
		if (ImGui::MenuItem("Duplicate folder")) DeferredAction = [&Actions, Folder] { Actions.QueueFolderDuplicateAfterSave(Folder); };
		ImGui::EndPopup();
	}

	void EditorContentBrowserNavigation::DrawFolderRenamePopup(
		EditorContentBrowserNavigationActions& Actions, std::function<void()>& DeferredAction)
	{
		constexpr const char* PopupName = "Rename Managed Folder";
		if (m_OpenFolderRenamePopup)
		{
			ImGui::OpenPopup(PopupName);
			m_OpenFolderRenamePopup = false;
		}
		if (!ImGui::BeginPopupModal(PopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
		ImGui::TextWrapped("Rename %s", PathToUtf8(m_RenameFolder).c_str());
		ImGui::SetNextItemWidth(300.0f);
		const bool Commit = ImGui::InputText("New folder name", &m_FolderRenameText, ImGuiInputTextFlags_EnterReturnsTrue);
		const std::filesystem::path NewName = std::filesystem::u8path(m_FolderRenameText);
		const bool ValidName =
			!m_FolderRenameText.empty() && NewName.filename() == NewName && m_FolderRenameText != "." && m_FolderRenameText != "..";
		ImGui::BeginDisabled(!ValidName);
		const bool Clicked = ImGui::Button("Rename");
		if (ValidName && (Commit || Clicked))
		{
			const std::filesystem::path Destination = m_RenameFolder.parent_path() / NewName;
			DeferredAction = [&Actions, Source = m_RenameFolder, Destination] { Actions.QueueFolderMoveAfterSave(Source, Destination); };
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	void EditorContentBrowserNavigation::HandleFolderDropTarget(const EditorContentBrowserState& State,
		const std::filesystem::path& Destination, EditorContentBrowserNavigationActions& Actions, std::function<void()>& DeferredAction)
	{
		if (DeferredAction || !ImGui::BeginDragDropTarget()) return;

		if (const ImGuiPayload* BatchPayload = ImGui::AcceptDragDropPayload("PF_MANAGED_ASSET_SET_MOVE"))
		{
			if (BatchPayload->IsDelivery() && BatchPayload->Data && BatchPayload->DataSize > 0)
			{
				const std::string_view PayloadText(static_cast<const char*>(BatchPayload->Data),
					static_cast<size_t>(BatchPayload->DataSize));
				std::vector<PulseForge::AssetID> Identifiers;
				size_t Begin = 0;
				bool Valid = true;
				while (Begin < PayloadText.size())
				{
					const size_t End = PayloadText.find('\n', Begin);
					const std::string_view Item = PayloadText.substr(
						Begin, End == std::string_view::npos ? PayloadText.size() - Begin : End - Begin);
					const auto Identifier = PulseForge::AssetID::Parse(Item);
					if (!Identifier)
					{
						Valid = false;
						break;
					}
					Identifiers.push_back(*Identifier);
					if (End == std::string_view::npos) break;
					Begin = End + 1;
				}
				if (Valid && Identifiers.size() > 1)
					DeferredAction = [&Actions, Identifiers = std::move(Identifiers), Destination]
					{ Actions.QueueAssetSetMoveAfterSave(Identifiers, Destination); };
			}
		}
		else if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("PF_MANAGED_ASSET_MOVE"))
		{
			if (Payload->IsDelivery() && Payload->Data && Payload->DataSize > 1)
			{
				std::string_view IdentifierText(static_cast<const char*>(Payload->Data), static_cast<size_t>(Payload->DataSize));
				if (!IdentifierText.empty() && IdentifierText.back() == '\0') IdentifierText.remove_suffix(1);
				if (const auto Identifier = PulseForge::AssetID::Parse(IdentifierText))
				{
					const auto& Assets = State.Assets();
					const auto Record = std::find_if(
						Assets.begin(), Assets.end(), [&](const PulseForge::AssetRecord& Asset) { return Asset.ID == *Identifier; });
					if (Record != Assets.end())
					{
						const std::filesystem::path AssetDestination = Destination / Record->ProjectRelativePath.filename();
						DeferredAction = [&Actions, Identifier = *Identifier, AssetDestination]
						{ Actions.QueueAssetMoveAfterSave(Identifier, AssetDestination); };
					}
				}
			}
		}
		else if (const ImGuiPayload* FolderPayload = ImGui::AcceptDragDropPayload("PF_MANAGED_FOLDER_MOVE"))
		{
			if (FolderPayload->IsDelivery() && FolderPayload->Data && FolderPayload->DataSize > 1)
			{
				std::string_view SourceText(static_cast<const char*>(FolderPayload->Data), static_cast<size_t>(FolderPayload->DataSize));
				if (!SourceText.empty() && SourceText.back() == '\0') SourceText.remove_suffix(1);
				const std::string SourceString(SourceText);
				const std::filesystem::path Source = std::filesystem::u8path(SourceString);
				DeferredAction = [&Actions, Source, Target = Destination / Source.filename()]
				{ Actions.QueueFolderMoveAfterSave(Source, Target); };
			}
		}
		ImGui::EndDragDropTarget();
	}

	void EditorContentBrowserNavigation::DrawBreadcrumbs(EditorContentBrowserState& State, EditorStyle& Style)
	{
		std::vector<std::pair<std::filesystem::path, std::string>> Breadcrumbs;
		std::filesystem::path Breadcrumb = "Assets";
		for (auto Component = State.CurrentFolder().begin(); Component != State.CurrentFolder().end(); ++Component)
		{
			if (*Component == "Assets") continue;
			Breadcrumb /= *Component;
			Breadcrumbs.emplace_back(Breadcrumb, PathToUtf8(*Component));
		}

		if (ImGui::SmallButton("Assets##BreadcrumbRoot")) State.SetCurrentFolder("Assets");
		if (Breadcrumbs.empty()) return;

		ImGui::SameLine();
		Style.TextMuted("/");
		if (Breadcrumbs.size() > 1)
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("...##BreadcrumbAncestors")) ImGui::OpenPopup("ContentBreadcrumbAncestors");
			if (ImGui::BeginPopup("ContentBreadcrumbAncestors"))
			{
				for (const auto& BreadcrumbEntry : Breadcrumbs)
				{
					const std::filesystem::path& Path = BreadcrumbEntry.first;
					const std::string FullPath = PathToUtf8(Path);
					if (ImGui::Selectable(FullPath.c_str(), Path == State.CurrentFolder())) State.SetCurrentFolder(Path);
				}
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			Style.TextMuted("/");
		}

		const std::string& CurrentFolderName = Breadcrumbs.back().second;
		const float LabelWidth = (std::max)(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2.0f, 0.0f);
		const std::string VisibleLabel = FitFolderLabel(CurrentFolderName, LabelWidth);
		const std::string CurrentLabel = (VisibleLabel.empty() ? "..." : VisibleLabel) + "##BreadcrumbCurrent";
		if (ImGui::SmallButton(CurrentLabel.c_str())) State.SetCurrentFolder(Breadcrumbs.back().first);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PathToUtf8(State.CurrentFolder()).c_str());
	}

	void EditorContentBrowserNavigation::DrawCompactFolderPicker(EditorContentBrowserState& State)
	{
		const std::string CurrentFolder = PathToUtf8(State.CurrentFolder());
		if (ImGui::BeginCombo("##ContentFolder", CurrentFolder.c_str()))
		{
			const auto DrawFolderOptions = [&State](auto&& Self, const EditorContentFolderNode& Node) -> void
			{
				const std::string Path = PathToUtf8(Node.RelativePath);
				if (ImGui::Selectable(Path.c_str(), Node.RelativePath == State.CurrentFolder())) State.SetCurrentFolder(Node.RelativePath);
				for (const EditorContentFolderNode& Child : Node.Children)
					Self(Self, Child);
			};
			DrawFolderOptions(DrawFolderOptions, State.FolderRoot());
			ImGui::EndCombo();
		}
	}

	void EditorContentBrowserNavigation::DrawFolderItems(EditorContentBrowserState& State,
		const std::vector<EditorContentFolderNode>& Folders, EditorStyle& Style, EditorContentBrowserNavigationActions& Actions,
		std::function<void()>& DeferredAction)
	{
		if (State.GridView()) DrawFolderGrid(State, Folders, Style, Actions, DeferredAction);
		else DrawFolderList(State, Folders, Style, Actions, DeferredAction);
		DrawFolderRenamePopup(Actions, DeferredAction);
	}

	void EditorContentBrowserNavigation::DrawFolderList(EditorContentBrowserState& State,
		const std::vector<EditorContentFolderNode>& Folders, EditorStyle& Style, EditorContentBrowserNavigationActions& Actions,
		std::function<void()>& DeferredAction)
	{
		if (Folders.empty()) return;

		if (!ImGui::BeginTable("ManagedFolderList", 1,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
					ImGuiTableFlags_NoSavedSettings))
			return;
		ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		for (const EditorContentFolderNode& Folder : Folders)
		{
			const std::string StableID = PathToUtf8(Folder.RelativePath);
			ImGui::PushID(StableID.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			const bool IsSelected =
				State.SelectedFolder() && State.SelectedFolder()->lexically_normal() == Folder.RelativePath.lexically_normal();
			const bool Activated = Style.Selectable(
				EditorIcon::Folder, Folder.Name, IsSelected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns);
			if (Activated)
			{
				State.SelectedFolder() = Folder.RelativePath;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) State.SetCurrentFolder(Folder.RelativePath);
			}
			DrawFolderContextMenu(Folder.RelativePath, Actions, DeferredAction);
			BeginFolderDragSource(Folder.RelativePath, Folder.Name);
			HandleFolderDropTarget(State, Folder.RelativePath, Actions, DeferredAction);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", StableID.c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	void EditorContentBrowserNavigation::DrawFolderGrid(EditorContentBrowserState& State,
		const std::vector<EditorContentFolderNode>& Folders, EditorStyle& Style, EditorContentBrowserNavigationActions& Actions,
		std::function<void()>& DeferredAction)
	{
		if (Folders.empty()) return;

		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const float DesiredTileWidth = 122.0f;
		const float Spacing = ImGui::GetStyle().ItemSpacing.x;
		const int ColumnCount = (std::max)(1, static_cast<int>((AvailableWidth + Spacing) / (DesiredTileWidth + Spacing)));
		if (!ImGui::BeginTable("ManagedFolderGrid", ColumnCount,
				ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoBordersInBody))
			return;

		constexpr float TileHeight = 96.0f;
		const int RowCount = static_cast<int>((Folders.size() + static_cast<size_t>(ColumnCount) - 1) / static_cast<size_t>(ColumnCount));
		ImGuiListClipper Clipper;
		Clipper.Begin(RowCount, TileHeight + Spacing);
		while (Clipper.Step())
		{
			for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				ImGui::TableNextRow(ImGuiTableRowFlags_None, TileHeight);
				for (int Column = 0; Column < ColumnCount; ++Column)
				{
					const size_t Index = static_cast<size_t>(Row) * static_cast<size_t>(ColumnCount) + static_cast<size_t>(Column);
					if (Index >= Folders.size()) break;
					ImGui::TableSetColumnIndex(Column);
					const EditorContentFolderNode& Folder = Folders[Index];
					const std::string StableID = PathToUtf8(Folder.RelativePath);
					ImGui::PushID(StableID.c_str());
					const bool IsSelected =
						State.SelectedFolder() && State.SelectedFolder()->lexically_normal() == Folder.RelativePath.lexically_normal();
					const bool Activated = Style.AssetTile(EditorIcon::Folder, Folder.Name, IsSelected,
						ImVec2((std::max)(ImGui::GetContentRegionAvail().x - 2.0f, 1.0f), TileHeight),
						ImGuiSelectableFlags_AllowDoubleClick);
					if (Activated)
					{
						State.SelectedFolder() = Folder.RelativePath;
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) State.SetCurrentFolder(Folder.RelativePath);
					}
					DrawFolderContextMenu(Folder.RelativePath, Actions, DeferredAction);
					BeginFolderDragSource(Folder.RelativePath, Folder.Name);
					HandleFolderDropTarget(State, Folder.RelativePath, Actions, DeferredAction);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", StableID.c_str());
					ImGui::PopID();
				}
			}
		}
		ImGui::EndTable();
	}
} // namespace PulseForgeEditor
