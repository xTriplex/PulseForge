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
#include <cerrno>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

		class TemporaryRuntimeUISaveFile final
		{
		public:
			TemporaryRuntimeUISaveFile(const TemporaryRuntimeUISaveFile&) = delete;
			TemporaryRuntimeUISaveFile& operator=(const TemporaryRuntimeUISaveFile&) = delete;
			TemporaryRuntimeUISaveFile(TemporaryRuntimeUISaveFile&& Other) noexcept
				: m_Path(std::move(Other.m_Path)), m_Published(std::exchange(Other.m_Published, true))
#if defined(_WIN32)
				, m_Handle(std::exchange(Other.m_Handle, INVALID_HANDLE_VALUE))
#else
				, m_Descriptor(std::exchange(Other.m_Descriptor, -1)), m_Device(Other.m_Device), m_Inode(Other.m_Inode)
#endif
			{
			}

			~TemporaryRuntimeUISaveFile() noexcept
			{
#if defined(_WIN32)
				if (m_Handle == INVALID_HANDLE_VALUE) return;
				if (!m_Published)
				{
					FILE_DISPOSITION_INFO DeleteFile{};
					DeleteFile.DeleteFile = TRUE;
					(void)SetFileInformationByHandle(m_Handle, FileDispositionInfo, &DeleteFile, sizeof(DeleteFile));
				}
				(void)CloseHandle(m_Handle);
#else
				if (m_Descriptor < 0) return;
				if (!m_Published)
				{
					struct stat Current{};
					if (fstatat(AT_FDCWD, m_Path.c_str(), &Current, AT_SYMLINK_NOFOLLOW) == 0 &&
						Current.st_dev == m_Device && Current.st_ino == m_Inode)
						(void)unlink(m_Path.c_str());
				}
				(void)close(m_Descriptor);
#endif
			}

			[[nodiscard]] static std::expected<TemporaryRuntimeUISaveFile, std::string> Create(
				const std::filesystem::path& Parent)
			{
				for (size_t Attempt = 0; Attempt < 8; ++Attempt)
				{
					auto ID = PulseForge::UUID::Generate();
					if (!ID) return std::unexpected("Could not generate a unique Runtime UI temporary filename: " + ID.error().Message);
					const std::filesystem::path Path = Parent /
						std::filesystem::u8path(".pfui-save-" + ID->ToString() + ".tmp");
#if defined(_WIN32)
					HANDLE Handle = CreateFileW(Path.c_str(), GENERIC_WRITE | DELETE,
						FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
					if (Handle != INVALID_HANDLE_VALUE) return TemporaryRuntimeUISaveFile(Path, Handle);
					const DWORD Code = GetLastError();
					if (Code == ERROR_FILE_EXISTS || Code == ERROR_ALREADY_EXISTS) continue;
					return std::unexpected("Could not exclusively create a temporary Runtime UI save file: " +
						std::error_code(static_cast<int>(Code), std::system_category()).message());
#else
					const int Descriptor = open(Path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
					if (Descriptor >= 0)
					{
						struct stat Created{};
						if (fstat(Descriptor, &Created) != 0)
						{
							const std::string Message = "Could not identify the temporary Runtime UI save file: " +
								std::error_code(errno, std::generic_category()).message();
							(void)close(Descriptor);
							return std::unexpected(Message);
						}
						return TemporaryRuntimeUISaveFile(Path, Descriptor, Created.st_dev, Created.st_ino);
					}
					const int Code = errno;
					if (Code == EEXIST) continue;
					return std::unexpected("Could not exclusively create a temporary Runtime UI save file: " +
						std::error_code(Code, std::generic_category()).message());
#endif
				}
				return std::unexpected("Could not find an unused temporary Runtime UI save filename after eight attempts");
			}

			[[nodiscard]] std::expected<void, std::string> Write(std::string_view Bytes)
			{
#if defined(_WIN32)
				size_t Offset = 0;
				while (Offset < Bytes.size())
				{
					const DWORD Chunk = static_cast<DWORD>((std::min)(Bytes.size() - Offset,
						static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
					DWORD Written = 0;
					if (!WriteFile(m_Handle, Bytes.data() + Offset, Chunk, &Written, nullptr) || Written == 0)
						return std::unexpected("Could not write the complete temporary Runtime UI document: " +
							std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
					Offset += Written;
				}
				if (!FlushFileBuffers(m_Handle))
					return std::unexpected("Could not flush the temporary Runtime UI document: " +
						std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
#else
				size_t Offset = 0;
				while (Offset < Bytes.size())
				{
					const ssize_t Written = write(m_Descriptor, Bytes.data() + Offset, Bytes.size() - Offset);
					if (Written < 0)
					{
						if (errno == EINTR) continue;
						return std::unexpected("Could not write the complete temporary Runtime UI document: " +
							std::error_code(errno, std::generic_category()).message());
					}
					if (Written == 0) return std::unexpected("Could not write the complete temporary Runtime UI document");
					Offset += static_cast<size_t>(Written);
				}
				if (fsync(m_Descriptor) != 0)
					return std::unexpected("Could not flush the temporary Runtime UI document: " +
						std::error_code(errno, std::generic_category()).message());
#endif
				return {};
			}

			[[nodiscard]] std::expected<void, std::string> Publish(const std::filesystem::path& Destination)
			{
				auto Replaced = PulseForge::Platform::ReplaceFileAtomically(m_Path, Destination);
				if (!Replaced) return std::unexpected(Replaced.error().Message);
				m_Published = true;
				return {};
			}

		private:
#if defined(_WIN32)
			TemporaryRuntimeUISaveFile(std::filesystem::path Path, HANDLE Handle)
				: m_Path(std::move(Path)), m_Handle(Handle)
			{
			}
#else
			TemporaryRuntimeUISaveFile(std::filesystem::path Path, int Descriptor, dev_t Device, ino_t Inode)
				: m_Path(std::move(Path)), m_Descriptor(Descriptor), m_Device(Device), m_Inode(Inode)
			{
			}
#endif
			std::filesystem::path m_Path;
			bool m_Published = false;
#if defined(_WIN32)
			HANDLE m_Handle = INVALID_HANDLE_VALUE;
#else
			int m_Descriptor = -1;
			dev_t m_Device{};
			ino_t m_Inode{};
#endif
		};
	}

	EditorRuntimeUIDocumentEditor::EditorRuntimeUIDocumentEditor(
		EditorDocumentManager& Documents,
		EditorDocumentEditorRegistry& Editors,
		EditorStyle& Style,
		RuntimeUIDocumentSaveFailureHook SaveFailureHook)
		: m_Documents(Documents), m_Editors(Editors), m_Style(Style), m_SaveFailureHook(std::move(SaveFailureHook))
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
		if (ImGui::Button("Save") && !Save(Session) && Current->Error.empty())
			Current->Error = "Could not save Runtime UI document";
		ImGui::SameLine();
		ImGui::BeginDisabled(!Session.IsDirty());
		if (ImGui::Button("Discard") && !Discard(Session) && Current->Error.empty())
			Current->Error = "Could not discard Runtime UI changes";
		ImGui::EndDisabled();

		PulseForge::RuntimeUIDocument Draft = Current->Document;
		bool Changed = false;
		Changed |= ImGui::DragFloat2("Design size", &Draft.DesignSize.x, 1.0f, 1.0f, 8192.0f);
		ImGui::Text("Version %u · %zu / %zu elements", Draft.Version, Draft.Elements.size(),
			PulseForge::RuntimeUIDocument::MaximumElements);
		ImGui::Separator();
		if (ImGui::Button("Add Image"))
		{
			auto ID = PulseForge::UUID::Generate();
			if (ID && Draft.Elements.size() < PulseForge::RuntimeUIDocument::MaximumElements)
			{
				PulseForge::RuntimeUIElement Element;
				Element.ID = *ID;
				Element.Kind = PulseForge::RuntimeUIElementKind::Image;
				for (const auto& Asset : Current->Registry->GetAssets())
					if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Image))
					{ Element.Image = Asset.ID; break; }
				Draft.Elements.push_back(Element);
				Current->Selected = Draft.Elements.size() - 1;
				Changed = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Add Text"))
		{
			auto ID = PulseForge::UUID::Generate();
			if (ID && Draft.Elements.size() < PulseForge::RuntimeUIDocument::MaximumElements)
			{
				PulseForge::RuntimeUIElement Element;
				Element.ID = *ID;
				Element.Kind = PulseForge::RuntimeUIElementKind::Text;
				for (const auto& Asset : Current->Registry->GetAssets())
					if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Font))
					{ Element.Font = Asset.ID; break; }
				Draft.Elements.push_back(Element);
				Current->Selected = Draft.Elements.size() - 1;
				Changed = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Add Button"))
		{
			auto ID = PulseForge::UUID::Generate();
			if (ID && Draft.Elements.size() < PulseForge::RuntimeUIDocument::MaximumElements)
			{
				PulseForge::RuntimeUIElement Element;
				Element.ID = *ID;
				Element.Kind = PulseForge::RuntimeUIElementKind::Button;
				for (const auto& Asset : Current->Registry->GetAssets())
					if (PulseForge::HasAssetTypeExtension(Asset.ProjectRelativePath, PulseForge::AssetFileType::Font))
					{ Element.Font = Asset.ID; break; }
				Element.Action = "activate";
				Element.Text = "Button";
				Draft.Elements.push_back(Element);
				Current->Selected = Draft.Elements.size() - 1;
				Changed = true;
			}
		}
		if (ImGui::BeginChild("RuntimeUIElements", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders))
			for (size_t Index = 0; Index < Draft.Elements.size(); ++Index)
			{
				const auto& Element = Draft.Elements[Index];
				const char* Kind = Element.Kind == PulseForge::RuntimeUIElementKind::Image ? "Image" :
					Element.Kind == PulseForge::RuntimeUIElementKind::Text ? "Text" : "Button";
				const std::string Label = std::string(Kind) + "  " + Element.ID.ToString();
				if (ImGui::Selectable(Label.c_str(), Current->Selected == Index)) Current->Selected = Index;
			}
		ImGui::EndChild();
		if (Current->Selected < Draft.Elements.size())
		{
			auto& Element = Draft.Elements[Current->Selected];
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
				Draft.Elements.erase(Draft.Elements.begin() + static_cast<ptrdiff_t>(Current->Selected));
				Current->Selected = Draft.Elements.empty() ? 0 : (std::min)(Current->Selected, Draft.Elements.size() - 1);
				Changed = true;
			}
		}
		if (Changed)
		{
			(void)ApplyDraftEdit(Session, std::move(Draft));
		}
		if (!Current->Error.empty()) ImGui::TextWrapped("%s", Current->Error.c_str());
		m_Style.TextMuted("Changes are session-authored and saved with this document's undo/redo history.");
		return true;
	}

	bool EditorRuntimeUIDocumentEditor::Save(EditorDocumentSession& Session)
	{
		const State* Current = GetState(Session);
		return Current && m_Editors.Save(Session);
	}

	bool EditorRuntimeUIDocumentEditor::Discard(EditorDocumentSession& Session)
	{
		return GetState(Session) && m_Editors.Discard(Session);
	}

	const PulseForge::RuntimeUIDocument* EditorRuntimeUIDocumentEditor::GetDraft(const EditorDocumentSession& Session) const noexcept
	{
		const State* Current = GetState(Session);
		return Current ? &Current->Document : nullptr;
	}

	std::string_view EditorRuntimeUIDocumentEditor::GetLastError(const EditorDocumentSession& Session) const noexcept
	{
		const State* Current = GetState(Session);
		return Current ? std::string_view(Current->Error) : std::string_view{};
	}

	std::expected<void, std::string> EditorRuntimeUIDocumentEditor::ApplyDraftEdit(
		EditorDocumentSession& Session, PulseForge::RuntimeUIDocument Draft)
	{
		State* Current = GetState(Session);
		if (!Current || !Current->Registry)
			return std::unexpected("Runtime UI document draft is unavailable");
		auto SerializedDraft = PulseForge::SerializeRuntimeUIDocument(Draft);
		if (!SerializedDraft)
		{
			Current->Error = SerializedDraft.error().Message;
			return std::unexpected(Current->Error);
		}
		for (const auto& Element : Draft.Elements)
		{
			if (!Element.Image.IsNil())
			{
				const auto Image = Current->Registry->Find(Element.Image);
				if (!Image || !PulseForge::HasAssetTypeExtension(Image->ProjectRelativePath, PulseForge::AssetFileType::Image))
				{
					Current->Error = "An image reference must identify a current managed image asset";
					return std::unexpected(Current->Error);
				}
			}
			if (!Element.Font.IsNil())
			{
				const auto Font = Current->Registry->Find(Element.Font);
				if (!Font || !PulseForge::HasAssetTypeExtension(Font->ProjectRelativePath, PulseForge::AssetFileType::Font))
				{
					Current->Error = "A font reference must identify a current managed font asset";
					return std::unexpected(Current->Error);
				}
			}
		}
		const auto SerializedCurrent = PulseForge::SerializeRuntimeUIDocument(Current->Document);
		if (SerializedCurrent && *SerializedCurrent == *SerializedDraft)
		{
			Current->Error.clear();
			return {};
		}
		try
		{
			Current->History.Record(Draft);
		}
		catch (const std::exception& Exception)
		{
			Current->Error = std::string("Could not record Runtime UI document edit: ") + Exception.what();
			return std::unexpected(Current->Error);
		}
		if (!m_Documents.MarkEdited(Session.Identity))
		{
			(void)Current->History.Undo();
			Current->Error = "The Runtime UI document session can no longer accept edits";
			return std::unexpected(Current->Error);
		}
		Current->Document = std::move(Draft);
		Current->Error.clear();
		return {};
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
		try
		{
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
			if (m_SaveFailureHook)
				if (auto Failure = m_SaveFailureHook(RuntimeUIDocumentSaveStage::BeforeTemporaryCreate))
				{
					Current->Error = std::move(*Failure);
					return false;
				}
			auto Temporary = TemporaryRuntimeUISaveFile::Create(Source->first.parent_path());
			if (!Temporary) { Current->Error = Temporary.error(); return false; }
			if (m_SaveFailureHook)
				if (auto Failure = m_SaveFailureHook(RuntimeUIDocumentSaveStage::BeforeTemporaryWrite))
				{
					Current->Error = std::move(*Failure);
					return false;
				}
			auto Written = Temporary->Write(*Serialized);
			if (!Written)
			{
				Current->Error = Written.error();
				return false;
			}
			if (m_SaveFailureHook)
				if (auto Failure = m_SaveFailureHook(RuntimeUIDocumentSaveStage::BeforeAtomicReplace))
				{
					Current->Error = std::move(*Failure);
					return false;
				}
			if (auto Replaced = Temporary->Publish(Source->first); !Replaced)
			{
				Current->Error = "Could not publish Runtime UI document: " + Replaced.error();
				return false;
			}
			Current->SavedBytes = *Serialized;
			Current->History.MarkSaved();
			Current->Error.clear();
			(void)SavedRevision; // The editor registry commits the saved revision after this callback succeeds.
			return true;
		}
		catch (const std::filesystem::filesystem_error& Exception)
		{
			Current->Error = std::string("Runtime UI save failed: ") + Exception.what();
			return false;
		}
		catch (const std::exception& Exception)
		{
			Current->Error = std::string("Runtime UI save failed: ") + Exception.what();
			return false;
		}
		catch (...)
		{
			Current->Error = "Runtime UI save failed due to an unknown filesystem error";
			return false;
		}
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
