#include "Core/PulseForgePCH.h"
#include "Editor/EditorAssetDestinationDialog.h"

#include "Assets/AssetMetadata.h"
#include "Assets/Project.h"
#include "Editor/EditorAssetDestinationPolicy.h"
#include "Editor/EditorAssetWorkflowState.h"
#include "Editor/EditorContentBrowserState.h"
#include "Editor/EditorStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string UTF8Path = Path.generic_u8string();
			return { reinterpret_cast<const char*>(UTF8Path.data()), UTF8Path.size() };
		}

		std::string FitContextLabel(std::string_view Label, float AvailableWidth)
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

		void CollectFolders(const EditorContentFolderNode& Node, std::vector<std::filesystem::path>& Folders)
		{
			Folders.push_back(Node.RelativePath);
			for (const EditorContentFolderNode& Child : Node.Children)
				CollectFolders(Child, Folders);
		}
	} // namespace

	std::optional<EditorAssetDestinationOperation> EditorAssetDestinationDialog::Draw(EditorStyle& Style,
		EditorAssetWorkflowState& Workflow, const PulseForge::Project* Project, const EditorContentBrowserState& ContentBrowser)
	{
		if (Workflow.TakeDestinationDialogRequest()) ImGui::OpenPopup("Project Asset Destination");
		bool PopupOpen = true;
		if (!ImGui::BeginPopupModal("Project Asset Destination", &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize)) return {};

		std::optional<EditorAssetDestinationOperation> Operation;
		const auto Asset = Project && Workflow.DestinationAsset() ? Project->GetAssetRegistry().Find(*Workflow.DestinationAsset())
																  : std::optional<PulseForge::AssetRecord>{};
		const bool IsProjectAssetCreation = Workflow.CreateKind() != ProjectAssetDestinationCreateKind::None;
		if (!Project || (!IsProjectAssetCreation && !Asset))
			ImGui::TextUnformatted(Project ? "The asset is no longer registered." : "The project is no longer open.");
		else
		{
			const std::filesystem::path SourcePath = Asset ? Asset->ProjectRelativePath
													 : Workflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab
														 ? std::filesystem::path("Assets/NewPrefab.prefab")
														 : std::filesystem::path("Assets/NewScene.scene");
			std::string OperationLabel;
			if (Asset) OperationLabel = Workflow.IsDuplicate() ? "Duplicate" : "Move / Rename";
			else if (Workflow.CreateKind() == ProjectAssetDestinationCreateKind::SaveSceneAs) OperationLabel = "Save Scene As";
			else if (Workflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab) OperationLabel = "Create Prefab";
			else OperationLabel = "Create Scene";
			ImGui::Text(
				"%s%s%s", OperationLabel.c_str(), Asset ? " " : "", Asset ? PathToUtf8(Asset->ProjectRelativePath.filename()).c_str() : "");
			ImGui::TextDisabled("Choose a project folder and use the required asset extension.");

			const std::filesystem::path CurrentFolder = std::filesystem::u8path(Workflow.DestinationFolder()).lexically_normal();
			const std::vector<std::filesystem::path> Breadcrumbs = BuildAssetDestinationBreadcrumbs(CurrentFolder);
			if (!Breadcrumbs.empty())
			{
				if (ImGui::SmallButton("Assets##AssetDestinationBreadcrumbRoot"))
				{
					Workflow.DestinationFolder() = "Assets";
					Workflow.ValidationError().clear();
				}
				if (Breadcrumbs.size() > 1)
				{
					ImGui::SameLine();
					Style.TextMuted("/");
					ImGui::SameLine();
					if (ImGui::SmallButton("...##AssetDestinationBreadcrumbAncestors"))
						ImGui::OpenPopup("AssetDestinationBreadcrumbAncestors");
					if (ImGui::BeginPopup("AssetDestinationBreadcrumbAncestors"))
					{
						for (size_t Index = 0; Index + 1 < Breadcrumbs.size(); ++Index)
						{
							const std::filesystem::path& Path = Breadcrumbs[Index];
							const std::string PathText = PathToUtf8(Path);
							if (ImGui::Selectable(PathText.c_str(), Path == CurrentFolder))
							{
								Workflow.DestinationFolder() = PathText;
								Workflow.ValidationError().clear();
							}
						}
						ImGui::EndPopup();
					}
					ImGui::SameLine();
					Style.TextMuted("/");
					ImGui::SameLine();
				}

				const std::string CurrentLabel = PathToUtf8(Breadcrumbs.back().filename());
				const float LabelWidth = (std::max)(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2.0f, 0.0f);
				ImGui::TextUnformatted(FitContextLabel(CurrentLabel, LabelWidth).c_str());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PathToUtf8(CurrentFolder).c_str());
			}

			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputTextWithHint(
				"##AssetDestinationSearch", "Search project folders...", Workflow.SearchBuffer().data(), Workflow.SearchBuffer().size());

			std::vector<std::filesystem::path> Folders;
			CollectFolders(ContentBrowser.FolderRoot(), Folders);
			std::string Search = Workflow.SearchBuffer().data();
			std::transform(Search.begin(), Search.end(), Search.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (ImGui::BeginChild("AssetDestinationFolderResults", ImVec2(420.0f, 180.0f), ImGuiChildFlags_Borders))
			{
				bool AnyFolder = false;
				for (const std::filesystem::path& Folder : Folders)
				{
					const std::string FolderText = PathToUtf8(Folder);
					std::string Searchable = FolderText;
					std::transform(Searchable.begin(), Searchable.end(), Searchable.begin(),
						[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
					if (!Search.empty() && Searchable.find(Search) == std::string::npos) continue;
					AnyFolder = true;
					if (ImGui::Selectable(FolderText.c_str(), Workflow.DestinationFolder() == FolderText))
					{
						Workflow.DestinationFolder() = FolderText;
						Workflow.ValidationError().clear();
					}
				}
				if (!AnyFolder) ImGui::TextDisabled("No matching project folders.");
			}
			ImGui::EndChild();
			ImGui::TextWrapped("Destination: %s", Workflow.DestinationFolder().c_str());
			ImGui::SetNextItemWidth(420.0f);
			const bool EnterPressed = ImGui::InputText(
				"Asset filename", Workflow.FilenameBuffer().data(), Workflow.FilenameBuffer().size(), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::TextDisabled("Extension: %s", SourcePath.extension().string().c_str());
			if (!Workflow.ValidationError().empty())
				ImGui::TextColored(Style.GetColor(EditorColorToken::Error), "%s", Workflow.ValidationError().c_str());

			const char* SubmitLabel = Workflow.IsDuplicate()													? "Duplicate"
									  : Workflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab		? "Create Prefab"
									  : Workflow.CreateKind() == ProjectAssetDestinationCreateKind::SaveSceneAs ? "Save Scene As"
									  : Workflow.CreateKind() == ProjectAssetDestinationCreateKind::NewScene	? "Create Scene"
																												: "Move";
			if (ImGui::Button(SubmitLabel, ImVec2(132.0f, 0.0f)) || EnterPressed)
			{
				const std::string FilenameText = Workflow.FilenameBuffer().data();
				const auto FinalFilename = ResolveAssetDestinationFilename(FilenameText, SourcePath);
				if (!FinalFilename)
				{
					switch (FinalFilename.error())
					{
					case AssetDestinationNameError::InvalidFilename:
						Workflow.ValidationError() = "Enter a single valid filename without a folder path.";
						break;
					case AssetDestinationNameError::ExtensionMismatch:
						Workflow.ValidationError() = "The destination must keep the original asset extension.";
						break;
					case AssetDestinationNameError::ReservedSidecarExtension:
						Workflow.ValidationError() = "The .meta extension is reserved for asset sidecars.";
						break;
					}
				}
				else if (!Project || (!IsProjectAssetCreation && !Workflow.DestinationAsset()) ||
						 !ContentBrowser.IsKnownFolder(std::filesystem::u8path(Workflow.DestinationFolder())))
					Workflow.ValidationError() = "Choose a folder that belongs to the open project.";
				else
				{
					const std::filesystem::path Target = std::filesystem::u8path(Workflow.DestinationFolder()) / *FinalFilename;
					const std::filesystem::path AbsoluteTarget = Project->GetRootPath() / Target;
					const std::filesystem::path AbsoluteSidecar = PulseForge::AssetMetadataSerializer::GetSidecarPath(AbsoluteTarget);
					const std::filesystem::path AbsoluteSource =
						Asset ? Project->GetRootPath() / Asset->ProjectRelativePath : std::filesystem::path{};
					const auto Inspect = [](const std::filesystem::path& Path, bool& Exists, std::error_code& Error)
					{
						const auto Status = std::filesystem::symlink_status(Path, Error);
						if (Error == std::errc::no_such_file_or_directory)
						{
							Error.clear();
							Exists = false;
						}
						else Exists = !Error && Status.type() != std::filesystem::file_type::not_found;
					};
					std::error_code DestinationError;
					std::error_code SidecarError;
					bool DestinationExists = false;
					bool SidecarExists = false;
					Inspect(AbsoluteTarget, DestinationExists, DestinationError);
					Inspect(AbsoluteSidecar, SidecarExists, SidecarError);
					bool SameAsset = false;
					bool SameSidecar = false;
					std::error_code EquivalentError;
					if (!Workflow.IsDuplicate() && Asset && DestinationExists)
						SameAsset = std::filesystem::equivalent(AbsoluteSource, AbsoluteTarget, EquivalentError);
					EquivalentError.clear();
					if (!Workflow.IsDuplicate() && Asset && SidecarExists)
						SameSidecar = std::filesystem::equivalent(
							PulseForge::AssetMetadataSerializer::GetSidecarPath(AbsoluteSource), AbsoluteSidecar, EquivalentError);
					const bool InspectionFailed =
						static_cast<bool>(DestinationError) || static_cast<bool>(SidecarError) || static_cast<bool>(EquivalentError);
					const std::string SourceSpelling = Asset ? PathToUtf8(Asset->ProjectRelativePath.lexically_normal()) : std::string{};
					const std::string TargetSpelling = PathToUtf8(Target.lexically_normal());
					const AssetDestinationConflict Conflict =
						ResolveAssetDestinationConflict(Workflow.IsDuplicate() || IsProjectAssetCreation, DestinationExists, SidecarExists,
							SameAsset, Asset && SourceSpelling == TargetSpelling, InspectionFailed, SameSidecar);
					switch (Conflict)
					{
					case AssetDestinationConflict::SamePath:
						Workflow.ValidationError() = "The selected folder and filename are already the source asset location.";
						break;
					case AssetDestinationConflict::ExistingDestination:
						Workflow.ValidationError() = "An asset or metadata sidecar already exists at that destination.";
						break;
					case AssetDestinationConflict::CaseOnlyRename:
						// The asset operation stages the asset and its sidecar to support Windows case-only renames.
						[[fallthrough]];
					case AssetDestinationConflict::None:
					{
						EditorAssetDestinationOperationKind Kind = EditorAssetDestinationOperationKind::CreateScene;
						if (Asset)
							Kind = Workflow.IsDuplicate() ? EditorAssetDestinationOperationKind::DuplicateAsset
														  : EditorAssetDestinationOperationKind::MoveAsset;
						else if (Workflow.CreateKind() == ProjectAssetDestinationCreateKind::SaveSceneAs)
							Kind = EditorAssetDestinationOperationKind::SaveSceneAs;
						else if (Workflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab)
							Kind = EditorAssetDestinationOperationKind::CreatePrefab;
						Operation = EditorAssetDestinationOperation{ Kind, Workflow.DestinationAsset(), Target, Workflow.PrefabRoot() };
						Workflow.DismissDestination();
						ImGui::CloseCurrentPopup();
						break;
					}
					case AssetDestinationConflict::InspectionFailed:
						Workflow.ValidationError() = "Could not inspect the destination. Check project folder permissions and try again.";
						break;
					}
				}
			}
		}

		ImGui::SameLine();
		const bool EscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		if (ImGui::Button("Cancel", ImVec2(112.0f, 0.0f)) || EscapePressed)
		{
			Workflow.DismissDestination();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		if (!PopupOpen) Workflow.DismissDestination();
		return Operation;
	}
} // namespace PulseForgeEditor
