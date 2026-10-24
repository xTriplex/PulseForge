#include "TestEditorSupport.h"

#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorInspectorPanel.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorStyle.h"

#include <imgui_internal.h>

#include <initializer_list>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	class ScopedInspectorImGuiContext final
	{
	public:
		ScopedInspectorImGuiContext()
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

		~ScopedInspectorImGuiContext()
		{
			ImGui::SetCurrentContext(m_Context);
			ImGui::DestroyContext(m_Context);
			ImGui::SetCurrentContext(m_Previous);
		}

		ScopedInspectorImGuiContext(const ScopedInspectorImGuiContext&) = delete;
		ScopedInspectorImGuiContext& operator=(const ScopedInspectorImGuiContext&) = delete;

	private:
		ImGuiContext* m_Previous = nullptr;
		ImGuiContext* m_Context = nullptr;
	};

	class InspectorActions final : public PulseForgeEditor::EditorInspectorPanelActions
	{
	public:
		void FinishInspectorSceneEdit() override { ++FinishCount; }

		std::expected<void, std::string> RunInspectorSceneEdit(
			std::string, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			auto Result = Operation();
			if (!Result) return std::unexpected(Result.error().Message);
			++CommandCount;
			return {};
		}

		void ReportInspectorError(std::string Message) override { Errors.push_back(std::move(Message)); }

		int FinishCount = 0;
		int CommandCount = 0;
		std::vector<std::string> Errors;
	};

	ImGuiWindow* FindActiveChildWindow(ImGuiContext& Context, std::string_view NamePart)
	{
		for (ImGuiWindow* Window : Context.Windows)
			if (Window->LastFrameActive == Context.FrameCount && std::string_view(Window->Name).find(NamePart) != std::string_view::npos)
				return Window;
		return nullptr;
	}

	void SetScriptFloatProperties(const PulseForge::Entity& Entity, std::initializer_list<std::pair<std::string, float>> Properties)
	{
		auto Result = Entity.GetScript();
		IM_ASSERT(Result && Result->has_value());
		PulseForge::ScriptComponent Script = **Result;
		Script.FloatProperties.clear();
		for (const auto& [Identifier, Value] : Properties)
			Script.FloatProperties.emplace(Identifier, Value);
		const auto Updated = Entity.SetScript(Script);
		IM_ASSERT(Updated.has_value());
	}

	void DrawInspectorFrame(TestRunner& Tests, PulseForgeEditor::EditorInspectorPanel& Panel,
		PulseForgeEditor::EditorSceneWorkspaceState& Workspace, PulseForgeEditor::EditorStyle& Style,
		PulseForgeEditor::EditorAssetSelector& AssetSelector, PulseForge::Scene& Scene, InspectorActions& Actions,
		const std::vector<PulseForge::AssetRecord>& NoAssets, int ExpectedFloatRows)
	{
		ImGui::GetIO().DisplaySize = ImVec2(960.0f, 720.0f);
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(260.0f, 560.0f), ImGuiCond_Always);
		ImGui::Begin("Inspector regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::GetStateStorage()->SetInt(ImGui::GetID("Mesh Renderer"), 1);
		ImGui::GetStateStorage()->SetInt(ImGui::GetID("Script"), 1);

		ImGuiContext& Context = *ImGui::GetCurrentContext();
		const int InitialColorDepth = Context.ColorStack.Size;
		const int InitialStyleVarDepth = Context.StyleVarStack.Size;
		Panel.DrawContents(&Scene, Workspace, Style, AssetSelector, NoAssets, false, false, Actions);
		PF_CHECK(Tests, Context.CurrentTable == nullptr);
		PF_CHECK(Tests, Context.ColorStack.Size == InitialColorDepth);
		PF_CHECK(Tests, Context.StyleVarStack.Size == InitialStyleVarDepth);

		ImGuiWindow* MeshBody = FindActiveChildWindow(Context, "/MeshRendererComponentBody_");
		ImGuiTable* MeshTable = MeshBody ? Context.Tables.GetByKey(MeshBody->GetID("MeshRendererProperties")) : nullptr;
		PF_CHECK(Tests, MeshTable && MeshTable->LastFrameActive == Context.FrameCount && MeshTable->ColumnsCount == 2 &&
							MeshTable->CurrentRow == 1); // Both Mesh Asset and Material rows were submitted by the panel.

		ImGuiWindow* ScriptBody = FindActiveChildWindow(Context, "/ScriptComponentBody_");
		ImGuiTable* FloatTable = ScriptBody ? Context.Tables.GetByKey(ScriptBody->GetID("ScriptFloatProperties")) : nullptr;
		if (ExpectedFloatRows == 0) PF_CHECK(Tests, !FloatTable || FloatTable->LastFrameActive != Context.FrameCount);
		else
			PF_CHECK(Tests, FloatTable && FloatTable->LastFrameActive == Context.FrameCount && FloatTable->ColumnsCount == 2 &&
								FloatTable->CurrentRow == ExpectedFloatRows - 1);

		ImGui::End();
		ImGui::Render();
		const ImDrawData* DrawData = ImGui::GetDrawData();
		PF_CHECK(Tests, DrawData && DrawData->Valid);
		PF_CHECK(Tests, Context.ColorStack.Size == 0);
		PF_CHECK(Tests, Context.StyleVarStack.Size == 0);
		PF_CHECK(Tests, Actions.Errors.empty());
	}
} // namespace

void RunEditorInspectorRegressionTests(TestRunner& Tests)
{
	ScopedInspectorImGuiContext Context;
	PulseForge::Scene Scene;
	auto Created = Scene.CreateEntity("Inspector regression entity");
	PF_CHECK(Tests, Created.has_value());
	if (!Created) return;
	const PulseForge::Entity Entity = *Created;
	PulseForgeEditor::EditorSceneWorkspaceState Workspace;
	Workspace.Selection().SelectSingle(Entity.GetUUID());
	PulseForgeEditor::EditorInspectorPanel Panel;
	PulseForgeEditor::EditorStyle Style;
	PulseForgeEditor::EditorAssetSelector AssetSelector;
	InspectorActions Actions;
	const std::vector<PulseForge::AssetRecord> NoAssets;
	const PulseForge::AssetID ScriptAsset{ 1, 1 };
	const PulseForge::AssetID MeshAsset{ 2, 2 };
	PF_CHECK(Tests, Entity.SetScript(PulseForge::ScriptComponent{ ScriptAsset }).has_value());
	PF_CHECK(Tests, Entity.SetMeshRenderer(PulseForge::MeshRendererComponent{ MeshAsset, std::nullopt }).has_value());

	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 0);
	SetScriptFloatProperties(Entity, { { "speed", 1.0f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 1);
	SetScriptFloatProperties(Entity, { { "health", 100.0f }, { "speed", 1.0f }, { "weight", 2.5f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 3);
	SetScriptFloatProperties(Entity, { { "health", 100.0f }, { "weight", 2.5f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 2);
	SetScriptFloatProperties(Entity, { { "weight", 2.5f } });
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 1);
	SetScriptFloatProperties(Entity, {});
	DrawInspectorFrame(Tests, Panel, Workspace, Style, AssetSelector, Scene, Actions, NoAssets, 0);
}
