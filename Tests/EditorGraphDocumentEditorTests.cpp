#include "TestAssetSupport.h"

#include "Editor/EditorGraphDocumentEditor.h"
#include "Editor/EditorGraphModel.h"

#include <imgui.h>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace PulseForgeEditor
{
	struct EditorGraphDocumentEditorTestAccess final
	{
		static EditorGraphPropertyEditResult Process(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			const std::vector<EditorGraphPropertyEditEvent>& Events)
		{
			return Editor.ProcessPropertyEditEvents(Session, Events, false);
		}
	};
}

namespace
{
	struct TemporaryGraphProject final
	{
		std::filesystem::path Path;
		~TemporaryGraphProject()
		{
			std::error_code Error;
			if (!Path.empty()) std::filesystem::remove_all(Path, Error);
		}
	};

	struct ImGuiContextScope final
	{
		ImGuiContext* Context = ImGui::CreateContext();
		~ImGuiContextScope() { if (Context) ImGui::DestroyContext(Context); }
	};

	std::optional<std::string> ReadBytes(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::nullopt;
		return std::string(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
	}

	bool WriteBytes(const std::filesystem::path& Path, std::string_view Bytes)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
		Output.flush();
		return static_cast<bool>(Output);
	}

	const PulseForgeEditor::EditorGraphPin* FindPin(const PulseForgeEditor::EditorGraph& Graph,
		std::string_view NodeKey, std::string_view PinKey)
	{
		for (const auto& Node : Graph.Nodes)
			if (Node.TypeKey == NodeKey)
			{
				const auto It = std::find_if(Node.Pins.begin(), Node.Pins.end(), [PinKey](const auto& Pin) { return Pin.Key == PinKey; });
				if (It != Node.Pins.end()) return &*It;
			}
		return nullptr;
	}

	std::optional<PulseForgeEditor::EditorGraph> ReadMaterialFixtureGraph(std::string_view Source)
	{
		auto Graph = PulseForgeEditor::DeserializeEditorGraph(Source, PulseForgeEditor::MakeMaterialExpressionFixtureSchema());
		return Graph ? std::optional<PulseForgeEditor::EditorGraph>(std::move(*Graph)) : std::nullopt;
	}

	void TestGraphDocumentLifecycle(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		TemporaryGraphProject Temporary;
		const auto ID = UUID::Generate();
		PF_CHECK(Tests, ID.has_value());
		if (!ID) return;
		Temporary.Path = std::filesystem::temp_directory_path() / ID->ToString();
		std::error_code Error;
		std::filesystem::create_directories(Temporary.Path, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto Project = PulseForge::Project::Create(Temporary.Path / "GraphFixtureProject.pfproj", "Graph fixture test");
		PF_CHECK(Tests, Project.has_value());
		if (!Project) return;

		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorGraphDocumentEditor GraphEditor(Documents, Editors);
		PF_CHECK(Tests, GraphEditor.Register().has_value());
		const auto Material = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		const auto Gameplay = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::GameplayEvent);
		PF_CHECK(Tests, Material.has_value() && Gameplay.has_value());
		if (!Material || !Gameplay) return;

		const UUID ProjectID = Project->GetDescription().ID;
		const EditorDocumentIdentity MaterialIdentity{ ProjectID, Material->GraphID, EditorDocumentKind::Graph };
		const EditorDocumentIdentity GameplayIdentity{ ProjectID, Gameplay->GraphID, EditorDocumentKind::Graph };
		const auto OpenGraph = [&](const EditorGraphFixtureLocation& Fixture, const EditorDocumentIdentity& Identity)
		{
			auto Opened = Documents.Open(Identity, Fixture.ProjectRelativePath, Fixture.DisplayName,
				EditorDocumentOpenPreference::MainDocumentStrip);
			PF_CHECK(Tests, Opened.has_value());
			if (!Opened) return static_cast<EditorDocumentSession*>(nullptr);
			auto* Session = Documents.Find(Identity);
			const EditorDocumentEditorContext Context{ Identity, Project->GetRootPath(), Fixture.ProjectRelativePath,
				Fixture.DisplayName, nullptr };
			PF_CHECK(Tests, Session && Editors.Load(*Session, Context));
			return Session;
		};
		auto* MaterialSession = OpenGraph(*Material, MaterialIdentity);
		auto* GameplaySession = OpenGraph(*Gameplay, GameplayIdentity);
		PF_CHECK(Tests, MaterialSession && GameplaySession);
		if (!MaterialSession || !GameplaySession) return;
		PF_CHECK(Tests, MaterialSession->EditorState && GameplaySession->EditorState &&
			MaterialSession->EditorState.get() != GameplaySession->EditorState.get());
		PF_CHECK(Tests, !MaterialSession->IsDirty() && !GameplaySession->IsDirty());
		const auto MaterialState = MaterialSession->EditorState.get();
		const uint64_t MaterialGeneration = MaterialSession->SessionGeneration;
		const auto NativeHost = Documents.CreateNativeHost(EditorDocumentKind::Graph);
		PF_CHECK(Tests, NativeHost.has_value());
		if (!NativeHost) return;
		PF_CHECK(Tests, Documents.Transfer(MaterialIdentity, *NativeHost, 0).has_value());
		PF_CHECK(Tests, MaterialSession->EditorState.get() == MaterialState &&
			MaterialSession->SessionGeneration == MaterialGeneration && MaterialSession->HostID == *NativeHost);

		const auto SourcePath = Project->GetRootPath() / Material->ProjectRelativePath;
		const auto OriginalBytes = ReadBytes(SourcePath);
		PF_CHECK(Tests, OriginalBytes.has_value());
		if (OriginalBytes)
		{
			const std::string ExternalBytes = "\n" + *OriginalBytes;
			PF_CHECK(Tests, WriteBytes(SourcePath, ExternalBytes));
			PF_CHECK(Tests, Documents.MarkEdited(MaterialIdentity));
			PF_CHECK(Tests, !GraphEditor.Save(*MaterialSession));
			PF_CHECK(Tests, ReadBytes(SourcePath) == ExternalBytes && MaterialSession->IsDirty());
			PF_CHECK(Tests, Editors.Discard(*MaterialSession));
			PF_CHECK(Tests, !MaterialSession->IsDirty() && ReadBytes(SourcePath) == ExternalBytes);
		}
		PF_CHECK(Tests, GraphEditor.Save(*GameplaySession) && !GameplaySession->IsDirty());
		PF_CHECK(Tests, ReadBytes(Project->GetRootPath() / Gameplay->ProjectRelativePath).has_value());
	}

	void TestGraphDocumentSaveRevisionAccounting(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		TemporaryGraphProject Temporary;
		const auto ProjectID = UUID::Generate();
		PF_CHECK(Tests, ProjectID.has_value());
		if (!ProjectID) return;
		Temporary.Path = std::filesystem::temp_directory_path() / ProjectID->ToString();
		std::error_code Error;
		std::filesystem::create_directories(Temporary.Path, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto Project = PulseForge::Project::Create(Temporary.Path / "GraphSaveProject.pfproj", "Graph save test");
		PF_CHECK(Tests, Project.has_value());
		if (!Project) return;

		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorGraphDocumentEditor GraphEditor(Documents, Editors);
		PF_CHECK(Tests, GraphEditor.Register().has_value());
		const auto Fixture = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		PF_CHECK(Tests, Fixture.has_value());
		if (!Fixture) return;
		const EditorDocumentIdentity Identity{ Project->GetDescription().ID, Fixture->GraphID, EditorDocumentKind::Graph };
		auto Opened = Documents.Open(Identity, Fixture->ProjectRelativePath, Fixture->DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, Opened.has_value());
		if (!Opened) return;
		auto* Session = Documents.Find(Identity);
		const EditorDocumentEditorContext Context{ Identity, Project->GetRootPath(), Fixture->ProjectRelativePath,
			Fixture->DisplayName, nullptr };
		PF_CHECK(Tests, Session && Editors.Load(*Session, Context));
		if (!Session) return;
		const auto SourcePath = Project->GetRootPath() / Fixture->ProjectRelativePath;
		const auto BaselineBytes = ReadBytes(SourcePath);
		PF_CHECK(Tests, BaselineBytes.has_value());
		if (!BaselineBytes) return;
		auto BaselineGraph = ReadMaterialFixtureGraph(*BaselineBytes);
		PF_CHECK(Tests, BaselineGraph.has_value());
		if (!BaselineGraph) return;
		const auto* Surface = FindPin(*BaselineGraph, "material.output", "surface");
		PF_CHECK(Tests, Surface != nullptr);
		if (!Surface) return;
		const UUID SurfaceID = Surface->ID;
		const auto InitialValue = std::get<EditorGraphVector3>(Surface->DefaultValue);
		const auto Process = [&](EditorGraphValue Value, bool Activated, bool Changed, bool Deactivated)
		{
			return EditorGraphDocumentEditorTestAccess::Process(GraphEditor, *Session,
				{ { SurfaceID, std::move(Value), Activated, Changed, Deactivated } });
		};
		const auto SourceValue = [&](const std::optional<std::string>& Bytes)
		{
			const auto Graph = Bytes ? ReadMaterialFixtureGraph(*Bytes) : std::nullopt;
			const auto* Pin = Graph ? FindPin(*Graph, "material.output", "surface") : nullptr;
			return Pin ? std::optional<EditorGraphVector3>(std::get<EditorGraphVector3>(Pin->DefaultValue)) : std::nullopt;
		};

		// Keep one continuous edit active through several content updates, then save via
		// the registered graph adapter and its real file publication callback.
		const EditorGraphVector3 Middle{ 0.6, 0.6, 0.6 };
		const EditorGraphVector3 Final{ 0.7, 0.8, 0.9 };
		auto Edit = Process(Middle, true, true, false);
		PF_CHECK(Tests, Edit.Error.empty() && Edit.GraphChanged && Edit.PropertyInteractionActive && Edit.TransactionOpen);
		Edit = Process(Final, false, true, false);
		PF_CHECK(Tests, Edit.Error.empty() && Edit.GraphChanged && Edit.PropertyInteractionActive && Edit.TransactionOpen);
		PF_CHECK(Tests, Session->IsDirty());
		const uint64_t RevisionBeforeSave = Session->Revision;
		const uint64_t SavedRevisionBeforeSave = Session->SavedRevision;
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Final));
		PF_CHECK(Tests, Session->Revision == RevisionBeforeSave && Session->SavedRevision == RevisionBeforeSave && !Session->IsDirty());
		PF_CHECK(Tests, SavedRevisionBeforeSave < RevisionBeforeSave);
		const auto Finished = EditorGraphDocumentEditorTestAccess::Process(GraphEditor, *Session, {});
		PF_CHECK(Tests, !Finished.PropertyInteractionActive && !Finished.TransactionOpen);

		// One undo restores the pre-interaction value; redo restores the saved value.
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(InitialValue));
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Final));
		PF_CHECK(Tests, !Session->IsDirty());

		// An unchanged active property on a dirty graph must not advance the document
		// revision while Save is finalizing its interaction.
		Edit = Process(Middle, true, true, true);
		PF_CHECK(Tests, Edit.Error.empty() && Edit.GraphChanged && !Edit.TransactionOpen && Session->IsDirty());
		Edit = Process(Middle, true, false, false);
		PF_CHECK(Tests, Edit.PropertyInteractionActive && Edit.TransactionOpen);
		const uint64_t DirtyNoOpRevision = Session->Revision;
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, Session->Revision == DirtyNoOpRevision && !Session->IsDirty());
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Middle));

		// Create a clean graph with a Redo branch; finishing an unchanged interaction
		// during Save must preserve it.
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, !Session->IsDirty());
		Edit = Process(InitialValue, true, false, false);
		PF_CHECK(Tests, Edit.PropertyInteractionActive && Edit.TransactionOpen && !Session->IsDirty());
		const uint64_t CleanNoOpRevision = Session->Revision;
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, Session->Revision == CleanNoOpRevision && !Session->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Middle));
		PF_CHECK(Tests, !Session->IsDirty());

		int CloseSaveCalls = 0;
		const auto CloseResult = Documents.Close(Identity, EditorDocumentCloseChoice::Save,
			[&](const EditorDocumentSession&, uint64_t) { ++CloseSaveCalls; return true; },
			[](const EditorDocumentSession&) { return false; });
		PF_CHECK(Tests, CloseResult == EditorDocumentCloseStatus::Closed && CloseSaveCalls == 0);

		// A source conflict still rejects Save without losing the committed draft or
		// replacing the adapter's saved-source baseline.
		const auto ConflictFixture = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		PF_CHECK(Tests, ConflictFixture.has_value());
		if (!ConflictFixture) return;
		const EditorDocumentIdentity ConflictIdentity{ Project->GetDescription().ID, ConflictFixture->GraphID, EditorDocumentKind::Graph };
		PF_CHECK(Tests, Documents.Open(ConflictIdentity, ConflictFixture->ProjectRelativePath, ConflictFixture->DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip).has_value());
		auto* ConflictSession = Documents.Find(ConflictIdentity);
		const EditorDocumentEditorContext ConflictContext{ ConflictIdentity, Project->GetRootPath(), ConflictFixture->ProjectRelativePath,
			ConflictFixture->DisplayName, nullptr };
		PF_CHECK(Tests, ConflictSession && Editors.Load(*ConflictSession, ConflictContext));
		if (!ConflictSession) return;
		const auto ConflictPath = Project->GetRootPath() / ConflictFixture->ProjectRelativePath;
		const auto ConflictBaseline = ReadBytes(ConflictPath);
		const auto ConflictGraph = ConflictBaseline ? ReadMaterialFixtureGraph(*ConflictBaseline) : std::nullopt;
		const auto* ConflictSurface = ConflictGraph ? FindPin(*ConflictGraph, "material.output", "surface") : nullptr;
		PF_CHECK(Tests, ConflictSurface != nullptr);
		if (!ConflictSurface) return;
		const auto ConflictSurfaceID = ConflictSurface->ID;
		const auto ConflictSavedRevision = ConflictSession->SavedRevision;
		auto ConflictEdit = EditorGraphDocumentEditorTestAccess::Process(GraphEditor, *ConflictSession,
			{ { ConflictSurfaceID, EditorGraphVector3{ 0.3, 0.4, 0.5 }, true, true, false } });
		PF_CHECK(Tests, ConflictEdit.PropertyInteractionActive && ConflictSession->IsDirty());
		const std::string ExternalBytes = *ConflictBaseline + "\n";
		PF_CHECK(Tests, WriteBytes(ConflictPath, ExternalBytes));
		PF_CHECK(Tests, !GraphEditor.Save(*ConflictSession));
		PF_CHECK(Tests, ReadBytes(ConflictPath) == ExternalBytes && ConflictSession->IsDirty());
		PF_CHECK(Tests, ConflictSession->SavedRevision == ConflictSavedRevision);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*ConflictSession, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, !ConflictSession->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*ConflictSession, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, ConflictSession->IsDirty());
		PF_CHECK(Tests, Editors.Discard(*ConflictSession) && !ConflictSession->IsDirty());
	}
}

void RunEditorGraphDocumentEditorTests(TestRunner& Tests)
{
	TestGraphDocumentLifecycle(Tests);
	TestGraphDocumentSaveRevisionAccounting(Tests);
}
