#include "TestAssetSupport.h"

#include "Editor/EditorGraphDocumentEditor.h"

#include <imgui.h>

#include <fstream>
#include <iterator>

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
}

void RunEditorGraphDocumentEditorTests(TestRunner& Tests)
{
	TestGraphDocumentLifecycle(Tests);
}
