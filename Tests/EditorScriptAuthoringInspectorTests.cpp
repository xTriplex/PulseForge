#include "TestEditorSupport.h"

#include "Assets/AssetOperations.h"
#include "Assets/Project.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorInspectorPanel.h"
#include "Editor/EditorPlatformServices.h"
#include "Editor/EditorScriptAuthoringService.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorStyle.h"

#include <imgui_internal.h>

#include <expected>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	class ScopedScriptInspectorContext final
	{
	public:
		ScopedScriptInspectorContext()
		{
			m_Previous = ImGui::GetCurrentContext();
			m_Context = ImGui::CreateContext();
			ImGui::SetCurrentContext(m_Context);
			ImGuiIO& IO = ImGui::GetIO();
			IO.IniFilename = nullptr;
			IO.DisplaySize = ImVec2(960.0f, 720.0f);
			IO.DeltaTime = 1.0f / 60.0f;
			unsigned char* Pixels = nullptr;
			int Width = 0;
			int Height = 0;
			IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
			IO.Fonts->SetTexID(static_cast<ImTextureID>(1));
		}

		~ScopedScriptInspectorContext()
		{
			ImGui::SetCurrentContext(m_Context);
			ImGui::DestroyContext(m_Context);
			ImGui::SetCurrentContext(m_Previous);
		}

		ScopedScriptInspectorContext(const ScopedScriptInspectorContext&) = delete;
		ScopedScriptInspectorContext& operator=(const ScopedScriptInspectorContext&) = delete;

	private:
		ImGuiContext* m_Previous = nullptr;
		ImGuiContext* m_Context = nullptr;
	};

	class ScriptInspectorActions final : public PulseForgeEditor::EditorInspectorPanelActions
	{
	public:
		std::expected<void, std::string> RunInspectorSceneEdit(
			std::string, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			auto Result = Operation();
			if (!Result) return std::unexpected(Result.error().Message);
			++EditCount;
			return {};
		}

		void FinishInspectorSceneEdit() override { ++FinishCount; }
		void ReportInspectorError(std::string Message) override { Errors.push_back(std::move(Message)); }

		std::expected<PulseForge::ScriptSourceMetadata, std::string> ReadScriptSourceMetadata(
			const PulseForge::AssetID& Asset) override
		{
			++MetadataReadCount;
			LastScriptAsset = Asset;
			return Metadata;
		}

		std::expected<void, std::string> OpenScriptAsset(const PulseForge::AssetID& Asset) override
		{
			OpenedScriptAsset = Asset;
			return {};
		}

		std::expected<void, std::string> RequestScriptReload(const PulseForge::Entity&) override
		{
			++ReloadRequestCount;
			return {};
		}

		std::vector<PulseForge::ScriptDiagnostic> GetScriptDiagnostics(
			const PulseForge::UUID& Entity, const PulseForge::AssetID& Asset) const override
		{
			LastDiagnosticEntity = Entity;
			LastDiagnosticAsset = Asset;
			return Diagnostics;
		}

		PulseForge::ScriptSourceMetadata Metadata{
			.HasDeclarations = true,
			.FloatProperties = { { "speed", "Speed", 2.0f, 0.0f, 5.0f }, { "health", "Health", 80.0f, 0.0f, 100.0f } }
		};
		std::vector<PulseForge::ScriptDiagnostic> Diagnostics;
		mutable PulseForge::UUID LastDiagnosticEntity;
		mutable PulseForge::AssetID LastDiagnosticAsset;
		PulseForge::AssetID LastScriptAsset;
		PulseForge::AssetID OpenedScriptAsset;
		int MetadataReadCount = 0;
		int ReloadRequestCount = 0;
		int EditCount = 0;
		int FinishCount = 0;
		std::vector<std::string> Errors;
	};

	ImGuiWindow* FindActiveWindow(ImGuiContext& Context, std::string_view NamePart)
	{
		for (ImGuiWindow* Window : Context.Windows)
			if (Window->LastFrameActive == Context.FrameCount && std::string_view(Window->Name).find(NamePart) != std::string_view::npos)
				return Window;
		return nullptr;
	}
}

void RunEditorScriptAuthoringInspectorTests(TestRunner& Tests)
{
	ScopedScriptInspectorContext Context;
	PulseForge::Scene Scene;
	auto Created = Scene.CreateEntity("Script Inspector defaults");
	PF_CHECK(Tests, Created.has_value());
	if (!Created) return;
	const PulseForge::Entity Entity = *Created;
	constexpr PulseForge::AssetID ScriptAsset{ 0xdaa0000000000000ull, 1 };
	PulseForge::ScriptComponent Script{ ScriptAsset };
	Script.FloatProperties.emplace("health", 120.0f); // Preserve and surface an invalid persisted override.
	Script.FloatProperties.emplace("legacy.speed", 3.5f); // Preserve a value whose declaration was removed.
	PF_CHECK(Tests, Entity.SetScript(Script).has_value());

	ScriptInspectorActions Actions;
	PulseForge::ScriptDiagnostic Diagnostic{
		.Entity = Entity.GetUUID(), .ScriptAsset = ScriptAsset,
		.Code = PulseForge::ScriptDiagnosticCode::CompileFailed, .Message = "fixture compile diagnostic",
		.SourceRelativePath = "Assets/Scripts/Player.lua", .Line = 12
	};
	Actions.Diagnostics.push_back(Diagnostic);
	PulseForgeEditor::EditorSceneWorkspaceState Workspace;
	Workspace.Selection().SelectSingle(Entity.GetUUID());
	PulseForgeEditor::EditorInspectorPanel Panel;
	PulseForgeEditor::EditorStyle Style;
	PulseForgeEditor::EditorAssetSelector AssetSelector;
	const std::vector<PulseForge::AssetRecord> NoAssets;

	ImGui::GetIO().DisplaySize = ImVec2(960.0f, 720.0f);
	ImGui::NewFrame();
	ImGui::SetNextWindowSize(ImVec2(420.0f, 700.0f), ImGuiCond_Always);
	ImGui::Begin("Script authoring Inspector regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
	ImGuiContext& ImGuiState = *ImGui::GetCurrentContext();
	ImGui::GetStateStorage()->SetInt(ImGui::GetID("Script"), 1);
	Panel.DrawContents(&Scene, Workspace, Style, AssetSelector, NoAssets, true, false, Actions);

	ImGuiWindow* ScriptBody = FindActiveWindow(ImGuiState, "/ScriptComponentBody_");
	ImGuiTable* DeclaredTable = ScriptBody ? ImGuiState.Tables.GetByKey(ScriptBody->GetID("ScriptFloatProperties")) : nullptr;
	ImGuiWindow* LegacyBody = FindActiveWindow(ImGuiState, "/ScriptComponentBody_");
	ImGuiTable* LegacyTable = LegacyBody ? ImGuiState.Tables.GetByKey(LegacyBody->GetID("LegacyScriptFloatProperties")) : nullptr;
	PF_CHECK(Tests, DeclaredTable && DeclaredTable->LastFrameActive == ImGuiState.FrameCount && DeclaredTable->CurrentRow == 1);
	PF_CHECK(Tests, LegacyTable && LegacyTable->LastFrameActive == ImGuiState.FrameCount && LegacyTable->CurrentRow == 0);
	PF_CHECK(Tests, Actions.MetadataReadCount == 1 && Actions.LastScriptAsset == ScriptAsset);
	PF_CHECK(Tests, Actions.LastDiagnosticEntity == Entity.GetUUID() && Actions.LastDiagnosticAsset == ScriptAsset);
	PF_CHECK(Tests, Actions.Errors.empty());
	PF_CHECK(Tests, Entity.GetScript()->value().FloatProperties.size() == 2);
	PF_CHECK(Tests, !Entity.GetScript()->value().FloatProperties.contains("speed"));

	ImGui::End();
	ImGui::Render();
	PF_CHECK(Tests, ImGui::GetDrawData() && ImGui::GetDrawData()->Valid);

	const auto RelativeOpen = PulseForgeEditor::EditorPlatformServices::OpenFileWithDefaultApplication("relative.lua");
	PF_CHECK(Tests, !RelativeOpen && RelativeOpen.error().find("absolute path") != std::string::npos);
	const auto MissingOpen = PulseForgeEditor::EditorPlatformServices::OpenFileWithDefaultApplication(
		std::filesystem::absolute("missing-script-inspector-fixture.lua"));
	PF_CHECK(Tests, !MissingOpen && MissingOpen.error().find("regular file") != std::string::npos);

	const auto ProjectID = PulseForge::UUID::Generate();
	PF_CHECK(Tests, ProjectID.has_value());
	if (!ProjectID) return;
	const std::filesystem::path ProjectRoot = std::filesystem::temp_directory_path() /
		std::filesystem::u8path("PulseForge-ScriptAuthoring-" + ProjectID->ToString());
	struct ProjectCleanup final
	{
		std::filesystem::path Root;
		~ProjectCleanup() { std::error_code Error; std::filesystem::remove_all(Root, Error); }
	} Cleanup{ ProjectRoot };
	auto Project = PulseForge::Project::Create(ProjectRoot / "Authoring.pfproj", "Script authoring test");
	PF_CHECK(Tests, Project.has_value());
	if (!Project) return;
	std::filesystem::create_directories(ProjectRoot / "Assets/Scripts");
	constexpr std::string_view Source =
		"--[[PulseForgeScript:1\n"
		"{\"version\":1,\"properties\":[{\"id\":\"move.speed\",\"label\":\"Move speed\",\"type\":\"float\",\"default\":2.0,\"min\":0.0,\"max\":4.0}]}\n"
		"]]\n";
	auto ScriptAssetResult = PulseForge::AssetOperations::CreateAssetFromBytes(Project->GetAssetRegistry(), ProjectRoot,
		std::as_bytes(std::span(Source.data(), Source.size())), std::filesystem::u8path("Assets/Scripts/žluťoučký.lua"));
	PF_CHECK(Tests, ScriptAssetResult.has_value());
	if (!ScriptAssetResult) return;
	auto Metadata = PulseForgeEditor::ReadScriptSourceMetadata(ProjectRoot, Project->GetAssetRegistry(), ScriptAssetResult->ID);
	PF_CHECK(Tests, Metadata && Metadata->FloatProperties.size() == 1 &&
		Metadata->FloatProperties.front().ID == "move.speed" && Metadata->FloatProperties.front().DefaultValue == 2.0f);
}
