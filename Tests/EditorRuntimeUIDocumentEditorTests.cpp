#include "TestAssetSupport.h"

#include "Assets/AssetMetadata.h"
#include "Assets/AssetOperations.h"
#include "Editor/EditorRuntimeUIDocumentEditor.h"
#include "Editor/EditorStyle.h"

#include <fstream>
#include <iterator>
#include <limits>
#include <optional>

namespace
{
	struct TemporaryProjectRoot final
	{
		std::filesystem::path Path;
		~TemporaryProjectRoot()
		{
			std::error_code Error;
			if (!Path.empty()) std::filesystem::remove_all(Path, Error);
		}
	};

	[[nodiscard]] std::optional<std::string> ReadFileBytes(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::nullopt;
		return std::string(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
	}

	[[nodiscard]] bool WriteFileBytes(const std::filesystem::path& Path, std::string_view Bytes)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
		Output.flush();
		return static_cast<bool>(Output);
	}

	[[nodiscard]] std::expected<PulseForge::AssetRecord, std::string> CreateManagedAsset(
		PulseForge::Project& Project,
		std::string_view Contents,
		const std::filesystem::path& RelativePath)
	{
		const std::filesystem::path Source = Project.GetRootPath() / RelativePath;
		std::error_code Error;
		std::filesystem::create_directories(Source.parent_path(), Error);
		if (Error) return std::unexpected("Could not create managed fixture directory: " + Error.message());
		const auto Identifier = PulseForge::UUID::Generate();
		if (!Identifier) return std::unexpected(Identifier.error().Message);
		std::ofstream Output(Source, std::ios::binary | std::ios::out | std::ios::trunc);
		if (!Output) return std::unexpected("Could not create managed fixture source");
		Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		Output.flush();
		if (!Output) return std::unexpected("Could not write managed fixture source");
		Output.close();
		if (Output.fail()) return std::unexpected("Could not finish managed fixture source");
		const std::filesystem::path Sidecar = PulseForge::AssetMetadataSerializer::GetSidecarPath(Source);
		std::ofstream Metadata(Sidecar, std::ios::binary | std::ios::out | std::ios::trunc);
		if (!Metadata) return std::unexpected("Could not create managed fixture metadata");
		const std::string MetadataJson = "{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
			Identifier->ToString() + "\",\"importSettings\":{}}\n";
		Metadata.write(MetadataJson.data(), static_cast<std::streamsize>(MetadataJson.size()));
		Metadata.flush();
		if (!Metadata) return std::unexpected("Could not write managed fixture metadata");
		Metadata.close();
		if (Metadata.fail()) return std::unexpected("Could not finish managed fixture metadata");
		return PulseForge::AssetRecord{ *Identifier, RelativePath };
	}

	[[nodiscard]] size_t CountSaveTemporaryFiles(const std::filesystem::path& Directory)
	{
		size_t Count = 0;
		std::error_code Error;
		for (std::filesystem::directory_iterator Iterator(Directory, Error), End; !Error && Iterator != End; Iterator.increment(Error))
		{
			const std::filesystem::path Name = Iterator->path().filename();
			if (Name.native().starts_with(
#if defined(_WIN32)
				L".pfui-save-"
#else
				".pfui-save-"
#endif
				)) ++Count;
		}
		return Error ? (std::numeric_limits<size_t>::max)() : Count;
	}

	[[nodiscard]] size_t CountRuntimeUIDocuments(const std::filesystem::path& Directory)
	{
		size_t Count = 0;
		std::error_code Error;
		for (std::filesystem::directory_iterator Iterator(Directory, Error), End; !Error && Iterator != End; Iterator.increment(Error))
			if (Iterator->path().extension() == ".pfui") ++Count;
		return Error ? (std::numeric_limits<size_t>::max)() : Count;
	}

	void TestRuntimeUIDocumentEditorAuthoring(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		const auto TemporaryID = UUID::Generate();
		PF_CHECK(Tests, TemporaryID.has_value());
		if (!TemporaryID) return;

		TemporaryProjectRoot Temporary;
		Temporary.Path = std::filesystem::temp_directory_path() /
			(std::filesystem::path(u8"雪 Ω") / std::filesystem::path(TemporaryID->ToString()));
		std::error_code FilesystemError;
		std::filesystem::create_directories(Temporary.Path, FilesystemError);
		PF_CHECK(Tests, !FilesystemError);
		if (FilesystemError) return;

		auto ProjectResult = Project::Create(Temporary.Path / std::filesystem::path(u8"Project 编辑 Ω.pfproj"), "UI authoring test");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;
		Project& ProjectValue = *ProjectResult;
		const std::filesystem::path ImagePath = std::filesystem::path(u8"Assets") /
			std::filesystem::path(u8"Images") / std::filesystem::path(u8"Pattern image.png");
		auto ImageAsset = CreateManagedAsset(ProjectValue, "not rasterized by the document adapter", ImagePath);
		PF_CHECK(Tests, ImageAsset.has_value());
		if (!ImageAsset) return;
		const std::filesystem::path InitialFontPath = std::filesystem::path(u8"Assets") /
			std::filesystem::path(u8"Fonts") / std::filesystem::path(u8"Initial font.ttf");
		auto InitialFontAsset = CreateManagedAsset(ProjectValue, "initial managed font source", InitialFontPath);
		const std::filesystem::path FontPath = std::filesystem::path(u8"Assets") /
			std::filesystem::path(u8"Fonts") / std::filesystem::path(u8"Authoring test.ttf");
		auto FontAsset = CreateManagedAsset(ProjectValue, "changed managed font source", FontPath);
		PF_CHECK(Tests, InitialFontAsset.has_value() && FontAsset.has_value());
		if (!InitialFontAsset || !FontAsset) return;

		const auto ElementID = UUID::Generate();
		PF_CHECK(Tests, ElementID.has_value());
		if (!ElementID) return;
		RuntimeUIDocument InitialDocument;
		RuntimeUIElement InitialText;
		InitialText.ID = *ElementID;
		InitialText.Kind = RuntimeUIElementKind::Text;
		InitialText.Font = InitialFontAsset->ID;
		InitialText.Text = "Saved text";
		InitialDocument.Elements.push_back(InitialText);
		const auto InitialBytes = SerializeRuntimeUIDocument(InitialDocument);
		PF_CHECK(Tests, InitialBytes.has_value());
		if (!InitialBytes) return;

		std::u8string LongName = u8"Runtime UI snow 雪 Ω ";
		LongName.append(100, u8'x');
		LongName += u8" document.pfui";
		const std::filesystem::path UIPath = std::filesystem::path(u8"Assets") /
			std::filesystem::path(u8"UI drafts 雪") / std::filesystem::path(LongName);
		const std::filesystem::path SourcePath = ProjectValue.GetRootPath() / UIPath;
#if defined(_WIN32)
		std::u8string LegacyTemporaryName = std::u8string(u8".") + LongName + u8".";
		for (const char Character : TemporaryID->ToString())
			LegacyTemporaryName.push_back(static_cast<char8_t>(Character));
		LegacyTemporaryName += u8".tmp";
		const std::filesystem::path LegacyTemporaryPath = SourcePath.parent_path() / LegacyTemporaryName;
		PF_CHECK(Tests, SourcePath.native().size() < 260);
		PF_CHECK(Tests, LegacyTemporaryPath.native().size() >= 260);
#endif
		auto UIAsset = CreateManagedAsset(ProjectValue, *InitialBytes, UIPath);
		PF_CHECK(Tests, UIAsset.has_value());
		if (!UIAsset) return;
		const auto WrongFormatAsset = CreateManagedAsset(ProjectValue,
			"{\"format\":\"WrongFormat\",\"version\":1}", std::filesystem::path(u8"Assets/UI drafts/wrong-format.pfui"));
		const auto UnsupportedVersionAsset = CreateManagedAsset(ProjectValue,
			"{\"format\":\"PulseForgeRuntimeUI\",\"version\":99,\"designSize\":[960,540],\"elements\":[]}",
			std::filesystem::path(u8"Assets/UI drafts/unsupported-version.pfui"));
		const auto MalformedAsset = CreateManagedAsset(ProjectValue, "{not json",
			std::filesystem::path(u8"Assets/UI drafts/malformed.pfui"));
		RuntimeUIDocument ValidMissingDocument;
		ValidMissingDocument.Elements.push_back(InitialText);
		const auto ValidMissingBytes = SerializeRuntimeUIDocument(ValidMissingDocument);
		const auto MissingAsset = ValidMissingBytes
			? CreateManagedAsset(ProjectValue, *ValidMissingBytes, std::filesystem::path(u8"Assets/UI drafts/missing.pfui"))
			: std::expected<AssetRecord, std::string>(std::unexpected("Could not serialize missing-source fixture"));
		PF_CHECK(Tests, WrongFormatAsset && UnsupportedVersionAsset && MalformedAsset && MissingAsset);
		if (!WrongFormatAsset || !UnsupportedVersionAsset || !MalformedAsset || !MissingAsset) return;
		const auto InitialRegistry = ProjectValue.GetAssetRegistry().Rebuild(Temporary.Path);
		PF_CHECK(Tests, InitialRegistry.has_value());
		if (!InitialRegistry) return;
		const bool RemovedMissingSource = std::filesystem::remove(ProjectValue.GetRootPath() / MissingAsset->ProjectRelativePath, FilesystemError);
		PF_CHECK(Tests, RemovedMissingSource && !FilesystemError);
		if (!RemovedMissingSource || FilesystemError) return;
		const bool RemovedMissingMetadata = std::filesystem::remove(
			AssetMetadataSerializer::GetSidecarPath(ProjectValue.GetRootPath() / MissingAsset->ProjectRelativePath), FilesystemError);
		PF_CHECK(Tests, RemovedMissingMetadata && !FilesystemError);
		if (!RemovedMissingMetadata || FilesystemError) return;

		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorStyle Style;
		std::optional<RuntimeUIDocumentSaveStage> FailAtStage;
		EditorRuntimeUIDocumentEditor DocumentEditor(Documents, Editors, Style,
			[&FailAtStage](RuntimeUIDocumentSaveStage Stage) -> std::optional<std::string>
			{
				if (FailAtStage != Stage) return std::nullopt;
				FailAtStage.reset();
				return "injected Runtime UI save failure";
			});
		PF_CHECK(Tests, DocumentEditor.Register().has_value());

		const UUID ProjectID = ProjectValue.GetDescription().ID;
		const auto TryInvalidOpen = [&](const AssetID& Asset, const std::filesystem::path& RelativePath)
		{
			const EditorDocumentIdentity Identity{ ProjectID, Asset, EditorDocumentKind::RuntimeUI };
			auto Opened = Documents.Open(Identity, RelativePath, "Invalid UI fixture", EditorDocumentOpenPreference::MainDocumentStrip);
			PF_CHECK(Tests, Opened.has_value());
			if (!Opened) return;
			EditorDocumentSession* Session = Documents.Find(Identity);
			PF_CHECK(Tests, Session != nullptr);
			if (!Session) return;
			const EditorDocumentEditorContext Context{ Identity, ProjectValue.GetRootPath(), RelativePath,
				"Invalid UI fixture", &ProjectValue.GetAssetRegistry() };
			PF_CHECK(Tests, !DocumentEditor.Open(*Session, Context));
			PF_CHECK(Tests, !Session->EditorState);
		};
		TryInvalidOpen(WrongFormatAsset->ID, WrongFormatAsset->ProjectRelativePath);
		TryInvalidOpen(UnsupportedVersionAsset->ID, UnsupportedVersionAsset->ProjectRelativePath);
		TryInvalidOpen(MalformedAsset->ID, MalformedAsset->ProjectRelativePath);
		TryInvalidOpen(MissingAsset->ID, MissingAsset->ProjectRelativePath);
		TryInvalidOpen(ImageAsset->ID, ImageAsset->ProjectRelativePath);

		const EditorDocumentIdentity Identity{ ProjectID, UIAsset->ID, EditorDocumentKind::RuntimeUI };
		auto Opened = Documents.Open(Identity, UIPath, "Runtime UI document", EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, Opened.has_value());
		if (!Opened) return;
		EditorDocumentSession* Session = Documents.Find(Identity);
		PF_CHECK(Tests, Session != nullptr);
		if (!Session) return;
		const EditorDocumentEditorContext Context{ Identity, ProjectValue.GetRootPath(), UIPath,
			"Runtime UI document", &ProjectValue.GetAssetRegistry() };
		PF_CHECK(Tests, DocumentEditor.Open(*Session, Context));
		PF_CHECK(Tests, Session->Identity.Project == ProjectID && Session->Identity.Asset == UIAsset->ID &&
			Session->Identity.Kind == EditorDocumentKind::RuntimeUI);
		PF_CHECK(Tests, !Session->IsDirty() && Session->Revision == Session->SavedRevision);
		PF_CHECK(Tests, !DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		const RuntimeUIDocument* Draft = DocumentEditor.GetDraft(*Session);
		PF_CHECK(Tests, Draft && Draft->Elements.size() == 1 && Draft->Elements[0].Text == "Saved text");
		if (!Draft) return;

		RuntimeUIDocument InvalidHierarchy = *Draft;
		InvalidHierarchy.Elements[0].Parent = InvalidHierarchy.Elements[0].ID;
		const auto RejectedHierarchy = DocumentEditor.ApplyDraftEdit(*Session, std::move(InvalidHierarchy));
		PF_CHECK(Tests, !RejectedHierarchy && !DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements[0].Text == "Saved text");
		PF_CHECK(Tests, !DocumentEditor.GetLastError(*Session).empty());
		RuntimeUIDocument InvalidReference = *DocumentEditor.GetDraft(*Session);
		InvalidReference.Elements[0].Image = UIAsset->ID;
		PF_CHECK(Tests, !DocumentEditor.ApplyDraftEdit(*Session, std::move(InvalidReference)));
		PF_CHECK(Tests, !DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));

		const auto OriginalBytes = ReadFileBytes(SourcePath);
		PF_CHECK(Tests, OriginalBytes == InitialBytes);
		RuntimeUIDocument TextEdit = *DocumentEditor.GetDraft(*Session);
		TextEdit.Elements[0].Text = "Edited Ω text";
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(TextEdit)).has_value());
		PF_CHECK(Tests, Session->IsDirty() && ReadFileBytes(SourcePath) == OriginalBytes);
		RuntimeUIDocument LayoutEdit = *DocumentEditor.GetDraft(*Session);
		LayoutEdit.Elements[0].AnchorMinimum = { 0.25f, 0.1f };
		LayoutEdit.Elements[0].AnchorMaximum = { 0.25f, 0.1f };
		LayoutEdit.Elements[0].OffsetMaximum = { 420.0f, 90.0f };
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(LayoutEdit)).has_value());
		RuntimeUIDocument ImageEdit = *DocumentEditor.GetDraft(*Session);
		ImageEdit.Elements[0].Image = ImageAsset->ID;
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(ImageEdit)).has_value());
		RuntimeUIDocument FontEdit = *DocumentEditor.GetDraft(*Session);
		FontEdit.Elements[0].Font = FontAsset->ID;
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(FontEdit)).has_value());
		PF_CHECK(Tests, DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements[0].Font == InitialFontAsset->ID);
		PF_CHECK(Tests, DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements[0].Font == FontAsset->ID);

		const auto AddedImageID = UUID::Generate();
		PF_CHECK(Tests, AddedImageID.has_value());
		if (!AddedImageID) return;
		RuntimeUIDocument AddedImageDraft = *DocumentEditor.GetDraft(*Session);
		RuntimeUIElement AddedImage;
		AddedImage.ID = *AddedImageID;
		AddedImage.Kind = RuntimeUIElementKind::Image;
		AddedImage.Image = ImageAsset->ID;
		AddedImageDraft.Elements.push_back(AddedImage);
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(AddedImageDraft)).has_value());
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements.size() == 2 &&
			DocumentEditor.GetDraft(*Session)->Elements[1].ID == *AddedImageID);
		PF_CHECK(Tests, DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements.size() == 1);
		PF_CHECK(Tests, DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements.size() == 2);
		RuntimeUIDocument RemovedImageDraft = *DocumentEditor.GetDraft(*Session);
		RemovedImageDraft.Elements.erase(RemovedImageDraft.Elements.begin() + 1);
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(RemovedImageDraft)).has_value());
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements.size() == 1);
		PF_CHECK(Tests, DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements.size() == 2);
		PF_CHECK(Tests, DocumentEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements.size() == 1);

		const auto SavedDraft = SerializeRuntimeUIDocument(*DocumentEditor.GetDraft(*Session));
		PF_CHECK(Tests, SavedDraft.has_value());
		PF_CHECK(Tests, Editors.Save(*Session));
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, ReadFileBytes(SourcePath) == SavedDraft);
		PF_CHECK(Tests, std::filesystem::exists(SourcePath) && CountRuntimeUIDocuments(SourcePath.parent_path()) == 1);
		PF_CHECK(Tests, CountSaveTemporaryFiles(SourcePath.parent_path()) == 0);
		auto ReloadedDocument = ReadFileBytes(SourcePath);
		const auto ParsedSaved = ReloadedDocument ? ParseRuntimeUIDocument(*ReloadedDocument)
			: std::expected<RuntimeUIDocument, RuntimeUIError>(std::unexpected(RuntimeUIError{ "Saved file is missing" }));
		PF_CHECK(Tests, ParsedSaved && ParsedSaved->Elements.size() == 1 && ParsedSaved->Elements[0].ID == *ElementID &&
			ParsedSaved->Elements[0].Image == ImageAsset->ID &&
			ParsedSaved->Elements[0].Font == FontAsset->ID && ParsedSaved->Elements[0].Text == "Edited Ω text");
		{
			EditorDocumentManager ReopenedDocuments;
			EditorDocumentEditorRegistry ReopenedEditors;
			EditorStyle ReopenedStyle;
			EditorRuntimeUIDocumentEditor ReopenedAdapter(ReopenedDocuments, ReopenedEditors, ReopenedStyle);
			PF_CHECK(Tests, ReopenedAdapter.Register().has_value());
			auto ReopenedSessionResult = ReopenedDocuments.Open(
				Identity, UIPath, "Reopened Runtime UI document", EditorDocumentOpenPreference::MainDocumentStrip);
			PF_CHECK(Tests, ReopenedSessionResult.has_value());
			if (ReopenedSessionResult)
			{
				EditorDocumentSession* ReopenedSession = ReopenedDocuments.Find(Identity);
				const EditorDocumentEditorContext ReopenedContext{ Identity, ProjectValue.GetRootPath(), UIPath,
					"Reopened Runtime UI document", &ProjectValue.GetAssetRegistry() };
				PF_CHECK(Tests, ReopenedSession && ReopenedAdapter.Open(*ReopenedSession, ReopenedContext));
				const RuntimeUIDocument* ReopenedDraft = ReopenedSession ? ReopenedAdapter.GetDraft(*ReopenedSession) : nullptr;
				PF_CHECK(Tests, ReopenedDraft && ReopenedDraft->Elements.size() == 1 && ReopenedDraft->Elements[0].ID == *ElementID &&
					ReopenedDraft->Elements[0].Font == FontAsset->ID && ReopenedDraft->Elements[0].Image == ImageAsset->ID);
				PF_CHECK(Tests, ReopenedSession && !ReopenedSession->IsDirty());
			}
		}

		for (const RuntimeUIDocumentSaveStage Stage : { RuntimeUIDocumentSaveStage::BeforeTemporaryCreate,
			 RuntimeUIDocumentSaveStage::BeforeTemporaryWrite, RuntimeUIDocumentSaveStage::BeforeAtomicReplace })
		{
			RuntimeUIDocument DraftEdit = *DocumentEditor.GetDraft(*Session);
			DraftEdit.Elements[0].Text += " retry";
			PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(DraftEdit)).has_value());
			const auto BeforeFailedSave = ReadFileBytes(SourcePath);
			const std::string IntendedDraft = DocumentEditor.GetDraft(*Session)->Elements[0].Text;
			FailAtStage = Stage;
			PF_CHECK(Tests, !Editors.Save(*Session));
			PF_CHECK(Tests, ReadFileBytes(SourcePath) == BeforeFailedSave && Session->IsDirty());
			PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements[0].Text == IntendedDraft);
			PF_CHECK(Tests, DocumentEditor.GetLastError(*Session).find("injected Runtime UI save failure") != std::string_view::npos);
			PF_CHECK(Tests, CountSaveTemporaryFiles(SourcePath.parent_path()) == 0);
			PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
			PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
			PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements[0].Text == IntendedDraft);
			PF_CHECK(Tests, Editors.Save(*Session));
			PF_CHECK(Tests, !Session->IsDirty() && Session->Revision == Session->SavedRevision &&
				CountSaveTemporaryFiles(SourcePath.parent_path()) == 0);
		}

		RuntimeUIDocument MissingReferenceDraft = *DocumentEditor.GetDraft(*Session);
		MissingReferenceDraft.Elements[0].Text = "Preserved with missing font";
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(MissingReferenceDraft)).has_value());
		const auto BeforeMissingReferenceSave = ReadFileBytes(SourcePath);
		const std::filesystem::path FontSource = ProjectValue.GetRootPath() / FontPath;
		const bool RemovedFontSource = std::filesystem::remove(FontSource, FilesystemError);
		PF_CHECK(Tests, RemovedFontSource && !FilesystemError);
		if (!RemovedFontSource || FilesystemError) return;
		const bool RemovedFontMetadata = std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(FontSource), FilesystemError);
		PF_CHECK(Tests, RemovedFontMetadata && !FilesystemError);
		if (!RemovedFontMetadata || FilesystemError) return;
		const auto MissingReferenceRegistry = ProjectValue.GetAssetRegistry().Rebuild(Temporary.Path);
		PF_CHECK(Tests, MissingReferenceRegistry.has_value());
		if (!MissingReferenceRegistry) return;
		PF_CHECK(Tests, !Editors.Save(*Session));
		PF_CHECK(Tests, ReadFileBytes(SourcePath) == BeforeMissingReferenceSave && Session->IsDirty());
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements[0].Text == "Preserved with missing font");
		PF_CHECK(Tests, DocumentEditor.GetLastError(*Session).find("font reference") != std::string_view::npos);
		RuntimeUIDocument RecoverMissingReference = *DocumentEditor.GetDraft(*Session);
		RecoverMissingReference.Elements[0].Font = InitialFontAsset->ID;
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(RecoverMissingReference)).has_value());
		PF_CHECK(Tests, Editors.Save(*Session) && !Session->IsDirty());

		RuntimeUIDocument ExternalConflictDraft = *DocumentEditor.GetDraft(*Session);
		ExternalConflictDraft.Elements[0].Text = "Local dirty draft";
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(ExternalConflictDraft)).has_value());
		RuntimeUIDocument ExternalDocument = *DocumentEditor.GetDraft(*Session);
		ExternalDocument.Elements[0].Text = "Independent external edit";
		const auto ExternalBytes = SerializeRuntimeUIDocument(ExternalDocument);
		const bool WroteExternalBytes = ExternalBytes && WriteFileBytes(SourcePath, *ExternalBytes);
		PF_CHECK(Tests, WroteExternalBytes);
		if (!WroteExternalBytes) return;
		PF_CHECK(Tests, !Editors.Save(*Session));
		PF_CHECK(Tests, ReadFileBytes(SourcePath) == ExternalBytes && Session->IsDirty());
		PF_CHECK(Tests, DocumentEditor.GetDraft(*Session)->Elements[0].Text == "Local dirty draft");
		PF_CHECK(Tests, !DocumentEditor.GetLastError(*Session).empty());
		const bool WroteMalformedBytes = WriteFileBytes(SourcePath, "{truncated external JSON");
		PF_CHECK(Tests, WroteMalformedBytes);
		if (!WroteMalformedBytes) return;
		PF_CHECK(Tests, !Editors.Discard(*Session));
		PF_CHECK(Tests, Session->IsDirty() && DocumentEditor.GetDraft(*Session)->Elements[0].Text == "Local dirty draft");
		const bool RestoredExternalBytes = WriteFileBytes(SourcePath, *ExternalBytes);
		PF_CHECK(Tests, RestoredExternalBytes);
		if (!RestoredExternalBytes) return;
		PF_CHECK(Tests, Editors.Discard(*Session));
		PF_CHECK(Tests, !Session->IsDirty() && DocumentEditor.GetDraft(*Session)->Elements[0].Text == "Independent external edit");

		RuntimeUIDocument MissingSourceDraft = *DocumentEditor.GetDraft(*Session);
		MissingSourceDraft.Elements[0].Text = "Preserved while source is absent";
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(MissingSourceDraft)).has_value());
		const auto SavedBeforeMissing = ReadFileBytes(SourcePath);
		PF_CHECK(Tests, SavedBeforeMissing.has_value());
		if (!SavedBeforeMissing) return;
		const bool RemovedSourceForFailure = std::filesystem::remove(SourcePath, FilesystemError);
		PF_CHECK(Tests, RemovedSourceForFailure && !FilesystemError);
		if (!RemovedSourceForFailure || FilesystemError) return;
		PF_CHECK(Tests, !Editors.Save(*Session));
		PF_CHECK(Tests, Session->IsDirty() && DocumentEditor.GetDraft(*Session)->Elements[0].Text == "Preserved while source is absent");
		const bool RestoredMissingSource = WriteFileBytes(SourcePath, *SavedBeforeMissing);
		PF_CHECK(Tests, RestoredMissingSource);
		if (!RestoredMissingSource) return;
		PF_CHECK(Tests, Editors.Save(*Session));

		RuntimeUIDocument MoveDraft = *DocumentEditor.GetDraft(*Session);
		MoveDraft.Elements[0].Text = "Saved after managed move";
		PF_CHECK(Tests, DocumentEditor.ApplyDraftEdit(*Session, std::move(MoveDraft)).has_value());
		const std::filesystem::path MovedPath = std::filesystem::path(u8"Assets") /
			std::filesystem::path(u8"UI drafts 雪") / std::filesystem::path(u8"Moved UI asset.pfui");
		auto MovedAsset = AssetOperations::Move(ProjectValue.GetAssetRegistry(), ProjectValue.GetRootPath(), UIAsset->ID, MovedPath);
		PF_CHECK(Tests, MovedAsset && MovedAsset->ID == UIAsset->ID && MovedAsset->ProjectRelativePath == MovedPath);
		if (!MovedAsset || MovedAsset->ID != UIAsset->ID || MovedAsset->ProjectRelativePath != MovedPath) return;
		const bool UpdatedLocation = Editors.UpdateAssetLocation(*Session, MovedPath, "Moved UI asset");
		PF_CHECK(Tests, UpdatedLocation);
		if (!UpdatedLocation) return;
		const bool SavedAfterMove = Editors.Save(*Session);
		PF_CHECK(Tests, SavedAfterMove);
		if (SavedAfterMove)
		{
			PF_CHECK(Tests, !std::filesystem::exists(SourcePath));
			const auto MovedBytes = ReadFileBytes(ProjectValue.GetRootPath() / MovedPath);
			const auto MovedDocument = MovedBytes ? ParseRuntimeUIDocument(*MovedBytes)
				: std::expected<RuntimeUIDocument, RuntimeUIError>(std::unexpected(RuntimeUIError{ "Moved file is missing" }));
			PF_CHECK(Tests, MovedDocument && MovedDocument->Elements[0].Text == "Saved after managed move");
		}
	}
}

void RunEditorRuntimeUIDocumentEditorTests(TestRunner& Tests)
{
	TestRuntimeUIDocumentEditorAuthoring(Tests);
}
