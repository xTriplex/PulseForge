#include "Editor/EditorRuntimeUIDocumentEditor.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Core/PlatformServices.h"
#include "Editor/EditorSnapshotHistory.h"
#include "Editor/EditorStyle.h"
#include "UI/RuntimeUI.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace PulseForgeEditor
{
	struct EditorRuntimeUIDocumentEditor::State final : EditorDocumentEditorState
	{
		PulseForge::RuntimeUIDocument Document;
		std::string SavedBytes;
		std::string Error;
		std::filesystem::path ProjectRoot;
		const PulseForge::AssetRegistry* Registry = nullptr;
		EditorSnapshotHistory<PulseForge::RuntimeUIDocument> History;
		size_t Selected = 0;
	};

	namespace
	{
		EditorRuntimeUIDocumentEditor::State* GetState(EditorDocumentSession& Session)
		{
			return dynamic_cast<EditorRuntimeUIDocumentEditor::State*>(Session.EditorState.get());
		}

		const EditorRuntimeUIDocumentEditor::State* GetState(const EditorDocumentSession& Session)
		{
			return dynamic_cast<const EditorRuntimeUIDocumentEditor::State*>(Session.EditorState.get());
		}

		std::expected<std::pair<std::filesystem::path, std::string>, std::string> ReadDocument(
			const EditorDocumentEditorContext& Context)
		{
			if (!Context.Registry || Context.Identity.Kind != EditorDocumentKind::RuntimeUI || Context.Identity.Asset.IsNil())
				return std::unexpected("Runtime UI document identity or project registry is unavailable");
			const auto Record = Context.Registry->Find(Context.Identity.Asset);
			if (!Record || !PulseForge::HasAssetTypeExtension(Record->ProjectRelativePath, PulseForge::AssetFileType::RuntimeUI))
				return std::unexpected("The selected asset is no longer a managed .pfui document");
			auto Path = PulseForge::AssetPathResolver::ResolveManagedSourcePath(Context.ProjectRoot, Record->ProjectRelativePath);
			if (!Path) return std::unexpected(Path.error().Message);
			std::error_code Error;
			const uint64_t Size = std::filesystem::file_size(*Path, Error);
			if (Error || Size > 4ull * 1024ull * 1024ull)
				return std::unexpected("Runtime UI source is unreadable or exceeds the 4 MiB editor limit");
			std::ifstream File(*Path, std::ios::binary);
			if (!File) return std::unexpected("Could not open the managed Runtime UI document");
			std::string Bytes(static_cast<size_t>(Size), '\0');
			File.read(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
			if (!File) return std::unexpected("Could not read the complete Runtime UI document");
			return std::pair{ *Path, std::move(Bytes) };
		}
	}

	EditorRuntimeUIDocumentEditor::EditorRuntimeUIDocumentEditor(
		EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors, EditorStyle& Style)
		: m_Documents(Documents), m_Editors(Editors), m_Style(Style)
	{
	}

	std::expected<void, std::string> EditorRuntimeUIDocumentEditor::Register()
	{
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::RuntimeUI;
		Adapter.IsEligible = [this](const auto& Context) { return IsEligible(Context); };
		Adapter.LoadSession = [this](const auto& Context) { return LoadSession(Context); };
		Adapter.DrawWorkspace = [this](auto& Session) { (void)DrawWorkspace(Session); };
		Adapter.Save = [this](auto& Session, uint64_t Revision) { return SaveSession(Session, Revision); };
		Adapter.Discard = [this](auto& Session) { return DiscardSession(Session); };
		Adapter.FocusedCommand = [this](auto& Session, auto Command) { return DispatchFocusedCommand(Session, Command); };
		Adapter.AssetLocationChanged = [this](auto& Session, const auto& Path, std::string_view Name)
		{ return UpdateAssetLocation(Session, Path, Name); };
		Adapter.PrepareClose = [this](const auto& Context) { return PrepareClose(Context); };
		return m_Editors.Register(std::move(Adapter));
	}

	bool EditorRuntimeUIDocumentEditor::Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const
	{
		return Session.Identity.Kind == EditorDocumentKind::RuntimeUI && m_Editors.Load(Session, Context);
	}

	bool EditorRuntimeUIDocumentEditor::IsEligible(const EditorDocumentEditorContext& Context) const
	{
		if (Context.Identity.Kind != EditorDocumentKind::RuntimeUI || !Context.Registry) return false;
		const auto Record = Context.Registry->Find(Context.Identity.Asset);
		return Record && PulseForge::HasAssetTypeExtension(Record->ProjectRelativePath, PulseForge::AssetFileType::RuntimeUI);
	}

	std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> EditorRuntimeUIDocumentEditor::LoadSession(
		const EditorDocumentEditorContext& Context) const
	{
		auto Source = ReadDocument(Context);
		if (!Source) return std::unexpected(Source.error());
		auto Parsed = PulseForge::ParseRuntimeUIDocument(Source->second);
		if (!Parsed) return std::unexpected("Runtime UI document is invalid: " + Parsed.error().Message);
		auto State = std::make_unique<EditorRuntimeUIDocumentEditor::State>();
		State->Document = *Parsed;
		State->History.Reset(State->Document);
		State->SavedBytes = std::move(Source->second);
		State->ProjectRoot = Context.ProjectRoot;
		State->Registry = Context.Registry;
		return std::unique_ptr<EditorDocumentEditorState>(std::move(State));
	}

	bool EditorRuntimeUIDocumentEditor::DrawWorkspace(EditorDocumentSession& Session)
	{
		State* Current = GetState(Session);
		if (!Current) return false;
		ImGui::BeginDisabled(!Current->History.CanUndo());
		if (ImGui::Button("Undo")) (void)DispatchFocusedCommand(Session, EditorDocumentCommand::Undo);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!Current->History.CanRedo());
		if (ImGui::Button("Redo")) (void)DispatchFocusedCommand(Session, EditorDocumentCommand::Redo);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Save") && !Save(Session)) Current->Error = "Could not save Runtime UI document";
		ImGui::SameLine();
		ImGui::BeginDisabled(!Session.IsDirty());
		if (ImGui::Button("Discard") && !Discard(Session)) Current->Error = "Could not discard Runtime UI changes";
		ImGui::EndDisabled();

		PulseForge::RuntimeUIDocument Before = Current->Document;
		bool Changed = false;
		Changed |= ImGui::DragFloat2("Design size", &Current->Document.DesignSize.x, 1.0f, 1.0f, 8192.0f);
		ImGui::Text("Version %u · %zu / %zu elements", Current->Document.Version, Current->Document.Elements.size(),
			PulseForge::RuntimeUIDocument::MaximumElements);
		ImGui::Separator();
		if (ImGui::Button("Add Image"))
		{
			auto ID = PulseForge::UUID::Generate();
			if (ID && Current->Document.Elements.size() < PulseForge::RuntimeUIDocument::MaximumElements)
			{
				PulseForge::RuntimeUIElement Element;
				Element.ID = *ID;
				Element.Kind = PulseForge::RuntimeUIElementKind::Image;
				for (const auto& Asset : Current->Registry->GetAssets())
					if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Image))
					{ Element.Image = Asset.ID; break; }
				Current->Document.Elements.push_back(Element);
				Current->Selected = Current->Document.Elements.size() - 1;
				Changed = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Add Text"))
		{
			auto ID = PulseForge::UUID::Generate();
			if (ID && Current->Document.Elements.size() < PulseForge::RuntimeUIDocument::MaximumElements)
			{
				PulseForge::RuntimeUIElement Element;
				Element.ID = *ID;
				Element.Kind = PulseForge::RuntimeUIElementKind::Text;
				for (const auto& Asset : Current->Registry->GetAssets())
					if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Font))
					{ Element.Font = Asset.ID; break; }
				Current->Document.Elements.push_back(Element);
				Current->Selected = Current->Document.Elements.size() - 1;
				Changed = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Add Button"))
		{
			auto ID = PulseForge::UUID::Generate();
			if (ID && Current->Document.Elements.size() < PulseForge::RuntimeUIDocument::MaximumElements)
			{
				PulseForge::RuntimeUIElement Element;
				Element.ID = *ID;
				Element.Kind = PulseForge::RuntimeUIElementKind::Button;
				for (const auto& Asset : Current->Registry->GetAssets())
					if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Font))
					{ Element.Font = Asset.ID; break; }
				Element.Action = "activate";
				Element.Text = "Button";
				Current->Document.Elements.push_back(Element);
				Current->Selected = Current->Document.Elements.size() - 1;
				Changed = true;
			}
		}
		if (ImGui::BeginChild("RuntimeUIElements", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders))
			for (size_t Index = 0; Index < Current->Document.Elements.size(); ++Index)
			{
				const auto& Element = Current->Document.Elements[Index];
				const char* Kind = Element.Kind == PulseForge::RuntimeUIElementKind::Image ? "Image" :
					Element.Kind == PulseForge::RuntimeUIElementKind::Text ? "Text" : "Button";
				const std::string Label = std::string(Kind) + "  " + Element.ID.ToString();
				if (ImGui::Selectable(Label.c_str(), Current->Selected == Index)) Current->Selected = Index;
			}
		ImGui::EndChild();
		if (Current->Selected < Current->Document.Elements.size())
		{
			auto& Element = Current->Document.Elements[Current->Selected];
			ImGui::Text("Stable ID  %s", Element.ID.ToString().c_str());
			std::string ParentID = Element.Parent.IsNil() ? std::string{} : Element.Parent.ToString();
			if (ImGui::InputText("Parent ID", &ParentID))
			{
				if (ParentID.empty()) { Element.Parent = {}; Changed = true; }
				else if (const auto Parsed = PulseForge::UUID::Parse(ParentID); Parsed) { Element.Parent = *Parsed; Changed = true; }
				else Current->Error = "Parent must be an existing element UUID or empty for a root element";
			}
			Changed |= ImGui::DragInt("Order", &Element.Order);
			Changed |= ImGui::DragFloat2("Anchor minimum", &Element.AnchorMinimum.x, 0.005f, 0.0f, 1.0f);
			Changed |= ImGui::DragFloat2("Anchor maximum", &Element.AnchorMaximum.x, 0.005f, 0.0f, 1.0f);
			Changed |= ImGui::DragFloat2("Offset minimum", &Element.OffsetMinimum.x, 1.0f);
			Changed |= ImGui::DragFloat2("Offset maximum", &Element.OffsetMaximum.x, 1.0f);
			Changed |= ImGui::DragFloat2("Pivot", &Element.Pivot.x, 0.005f, 0.0f, 1.0f);
			Changed |= ImGui::ColorEdit4("Tint", &Element.Color.x);
			Changed |= ImGui::ColorEdit4("Text color", &Element.TextColor.x);
			Changed |= ImGui::Checkbox("Visible", &Element.Visible);
			Changed |= ImGui::Checkbox("Enabled", &Element.Enabled);
			Changed |= ImGui::Checkbox("Hit test", &Element.HitTestVisible);
			Changed |= ImGui::Checkbox("Clip children", &Element.ClipChildren);
			Changed |= ImGui::InputFloat("Font size", &Element.FontSize, 1.0f, 4.0f, "%.1f");
			Changed |= ImGui::InputTextMultiline("Text", &Element.Text, ImVec2(-1.0f, 70.0f));
			Changed |= ImGui::InputText("Action", &Element.Action);
			std::string ImageID = Element.Image.IsNil() ? std::string{} : Element.Image.ToString();
			if (ImGui::InputText("Image Asset ID", &ImageID))
			{
				const auto Parsed = PulseForge::UUID::Parse(ImageID);
				if (Parsed) { Element.Image = *Parsed; Changed = true; }
				else Current->Error = "Image reference must be a valid managed asset UUID";
			}
			std::string FontID = Element.Font.IsNil() ? std::string{} : Element.Font.ToString();
			if (ImGui::InputText("Font Asset ID", &FontID))
			{
				const auto Parsed = PulseForge::UUID::Parse(FontID);
				if (Parsed) { Element.Font = *Parsed; Changed = true; }
				else Current->Error = "Font reference must be a valid managed asset UUID";
			}
			if (ImGui::Button("Remove element"))
			{
				Current->Document.Elements.erase(Current->Document.Elements.begin() + static_cast<ptrdiff_t>(Current->Selected));
				Current->Selected = Current->Document.Elements.empty() ? 0 : (std::min)(Current->Selected, Current->Document.Elements.size() - 1);
				Changed = true;
			}
		}
		if (Changed)
		{
			if (auto Valid = PulseForge::ValidateRuntimeUIDocument(Current->Document); Valid)
			{
				Current->History.Record(Current->Document);
				(void)m_Documents.MarkEdited(Session.Identity);
				Current->Error.clear();
			}
			else
			{
				Current->Document = std::move(Before);
				Current->Error = Valid.error().Message;
			}
		}
		if (!Current->Error.empty()) ImGui::TextWrapped("%s", Current->Error.c_str());
		m_Style.TextMuted("Changes are session-authored and saved with this document's undo/redo history.");
		return true;
	}

	bool EditorRuntimeUIDocumentEditor::Save(EditorDocumentSession& Session)
	{
		const State* Current = GetState(Session);
		return Current && SaveSession(Session, Session.Revision);
	}

	bool EditorRuntimeUIDocumentEditor::Discard(EditorDocumentSession& Session)
	{
		return GetState(Session) && DiscardSession(Session);
	}

	bool EditorRuntimeUIDocumentEditor::DispatchFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command)
	{
		State* Current = GetState(Session);
		if (!Current) return false;
		if (Command == EditorDocumentCommand::Undo && Current->History.Undo())
		{
			Current->Document = Current->History.GetCurrent();
		}
		else if (Command == EditorDocumentCommand::Redo && Current->History.Redo())
		{
			Current->Document = Current->History.GetCurrent();
		}
		else return false;
		(void)m_Documents.MarkEdited(Session.Identity);
		return true;
	}

	bool EditorRuntimeUIDocumentEditor::SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision)
	{
		State* Current = GetState(Session);
		if (!Current || !Current->Registry) return false;
		auto Serialized = PulseForge::SerializeRuntimeUIDocument(Current->Document);
		if (!Serialized) { Current->Error = Serialized.error().Message; return false; }
		for (const auto& Element : Current->Document.Elements)
		{
			if (!Element.Image.IsNil())
			{
				const auto Image = Current->Registry->Find(Element.Image);
				if (!Image || !PulseForge::HasAssetTypeExtension(Image->ProjectRelativePath, PulseForge::AssetFileType::Image))
				{ Current->Error = "An image reference is missing or does not identify a managed image asset"; return false; }
			}
			if (!Element.Font.IsNil())
			{
				const auto Font = Current->Registry->Find(Element.Font);
				if (!Font || !PulseForge::HasAssetTypeExtension(Font->ProjectRelativePath, PulseForge::AssetFileType::Font))
				{ Current->Error = "A font reference is missing or does not identify a managed font asset"; return false; }
			}
		}
		EditorDocumentEditorContext Context{ Session.Identity, Current->ProjectRoot, Session.ProjectRelativePath,
			Session.DisplayName, Current->Registry };
		auto Source = ReadDocument(Context);
		if (!Source || Source->second != Current->SavedBytes)
		{
			Current->Error = Source ? "The Runtime UI source changed outside this draft; reload before saving" : Source.error();
			return false;
		}
		auto TemporaryID = PulseForge::UUID::Generate();
		if (!TemporaryID) { Current->Error = TemporaryID.error().Message; return false; }
		const std::filesystem::path Temporary = Source->first.parent_path() /
			(std::string(".") + Source->first.filename().string() + "." + TemporaryID->ToString() + ".tmp");
		{
			std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
			if (!File) { Current->Error = "Could not create a temporary Runtime UI save file"; return false; }
			File.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
			File.flush();
			if (!File) { std::error_code Error; std::filesystem::remove(Temporary, Error); Current->Error = "Could not write the complete Runtime UI document"; return false; }
		}
		if (auto Replaced = PulseForge::Platform::ReplaceFileAtomically(Temporary, Source->first); !Replaced)
		{
			std::error_code Error;
			std::filesystem::remove(Temporary, Error);
			Current->Error = "Could not publish Runtime UI document: " + Replaced.error().Message;
			return false;
		}
		Current->SavedBytes = *Serialized;
		Current->History.MarkSaved();
		Current->Error.clear();
		(void)m_Documents.MarkSaved(Session.Identity, SavedRevision);
		return true;
	}

	bool EditorRuntimeUIDocumentEditor::DiscardSession(EditorDocumentSession& Session)
	{
		State* Current = GetState(Session);
		if (!Current || !Current->Registry) return false;
		EditorDocumentEditorContext Context{ Session.Identity, Current->ProjectRoot, Session.ProjectRelativePath,
			Session.DisplayName, Current->Registry };
		auto Source = ReadDocument(Context);
		if (!Source) { Current->Error = Source.error(); return false; }
		auto Parsed = PulseForge::ParseRuntimeUIDocument(Source->second);
		if (!Parsed) { Current->Error = Parsed.error().Message; return false; }
		Current->Document = *Parsed;
		Current->History.Reset(Current->Document);
		Current->SavedBytes = std::move(Source->second);
		Current->Error.clear();
		Session.Revision = Session.SavedRevision;
		return true;
	}

	bool EditorRuntimeUIDocumentEditor::UpdateAssetLocation(
		EditorDocumentSession&, const std::filesystem::path& Path, std::string_view)
	{
		return PulseForge::HasAssetTypeExtension(Path, PulseForge::AssetFileType::RuntimeUI);
	}

	std::expected<void, std::string> EditorRuntimeUIDocumentEditor::PrepareClose(const EditorDocumentCloseContext&) const
	{
		return {};
	}
}
