#include "TestSupport.h"

#include "Editor/EditorMeshPickingCache.h"
#include "Editor/EditorSceneEntityCommands.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Scene/Components/MeshRendererComponent.h"

#include <cmath>
#include <utility>

namespace
{
	class TestSceneCommandActions final : public PulseForgeEditor::EditorSceneEntityCommandActions
	{
	public:
		[[nodiscard]] std::expected<void, std::string> RunEditorSceneEdit(
			std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			++EditCount;
			EditDescriptions.push_back(std::move(Description));
			if (RejectNextEdit)
			{
				RejectNextEdit = false;
				return std::unexpected("injected scene edit failure");
			}
			auto Result = Operation();
			if (!Result) return std::unexpected(Result.error().Message);
			return {};
		}

		void ReportEditorSceneCommandError(std::string Message) override { Errors.push_back(std::move(Message)); }
		void SetEditorSceneCommandStatus(std::string Message) override { Statuses.push_back(std::move(Message)); }
		void ReportEditorSceneCommandWarning(std::string Message) override { Warnings.push_back(std::move(Message)); }
		void SetEditorSceneSelection(std::vector<PulseForge::UUID>) override {}
		void SelectSingleEditorEntity(std::optional<PulseForge::UUID>) override {}

		size_t EditCount = 0;
		bool RejectNextEdit = false;
		std::vector<std::string> EditDescriptions;
		std::vector<std::string> Errors;
		std::vector<std::string> Statuses;
		std::vector<std::string> Warnings;
	};
} // namespace

void RunEditorSceneEntityCommandTests(TestRunner& Tests)
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;
	const AssetID SupportMeshID{ 101, 1 };
	const AssetID SelectedMeshID{ 102, 2 };
	const EditorMeshPickingGeometry SupportGeometry{ .Positions = { { -5.0f, 0.0f, -5.0f }, { 5.0f, 0.0f, -5.0f }, { 5.0f, 0.0f, 5.0f },
														 { -5.0f, 0.0f, 5.0f } },
		.Indices = { 0, 2, 1, 0, 3, 2 } };
	const EditorMeshPickingGeometry SelectedGeometry{
		.Positions = { { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.0f, 0.5f, 0.5f } }, .Indices = { 0, 1, 2 }
	};
	const EditorMeshGeometryResolver ResolveMesh = [&](const AssetID& Asset) -> std::expected<const EditorMeshPickingGeometry*, std::string>
	{
		if (Asset == SupportMeshID) return &SupportGeometry;
		if (Asset == SelectedMeshID) return &SelectedGeometry;
		return std::unexpected("unknown mesh asset");
	};

	EditorSceneEntityCommands Commands;
	EditorSceneWorkspaceState Workspace;
	TestSceneCommandActions Actions;
	Scene SceneValue;
	auto Support = SceneValue.CreateEntity("Support");
	auto Selected = SceneValue.CreateEntity("Selected");
	PF_CHECK(Tests, Support && Selected);
	if (!Support || !Selected) return;
	PF_CHECK(Tests, Support->SetMeshRenderer(MeshRendererComponent{ SupportMeshID, std::nullopt }));
	PF_CHECK(Tests, Selected->SetMeshRenderer(MeshRendererComponent{ SelectedMeshID, std::nullopt }));

	auto SupportTransform = Support->GetTransform();
	auto SelectedTransform = Selected->GetTransform();
	PF_CHECK(Tests, SupportTransform && SelectedTransform);
	if (!SupportTransform || !SelectedTransform) return;
	SupportTransform->Scale = { 5.0f, 1.0f, 5.0f };
	SelectedTransform->Translation = { 0.0f, 3.0f, 0.0f };
	SelectedTransform->Scale = { 0.5f, 0.5f, 0.5f };
	PF_CHECK(Tests, Support->SetTransform(*SupportTransform));
	PF_CHECK(Tests, Selected->SetTransform(*SelectedTransform));
	Workspace.Selection().SelectSingle(Selected->GetUUID());

	Commands.SnapSelectionToSurface(&SceneValue, Workspace, false, ResolveMesh, Actions);
	const auto SnappedTransform = Selected->GetTransform();
	PF_CHECK(Tests, SnappedTransform && std::abs(SnappedTransform->Translation.y - 0.25f) < 1.0e-3f);
	PF_CHECK(Tests, Actions.EditCount == 1 && Actions.EditDescriptions.back() == "Snap Selection to Surface");
	PF_CHECK(Tests, !Actions.Statuses.empty() && Actions.Statuses.back().find("Snapped 1 selected") != std::string::npos);
	PF_CHECK(Tests, Actions.Errors.empty());

	const float BeforeRejectedEditY = 4.0f;
	SelectedTransform = Selected->GetTransform();
	PF_CHECK(Tests, SelectedTransform.has_value());
	if (!SelectedTransform) return;
	SelectedTransform->Translation.y = BeforeRejectedEditY;
	PF_CHECK(Tests, Selected->SetTransform(*SelectedTransform));
	Actions.RejectNextEdit = true;
	Commands.SnapSelectionToSurface(&SceneValue, Workspace, false, ResolveMesh, Actions);
	const auto RejectedTransform = Selected->GetTransform();
	PF_CHECK(Tests, RejectedTransform && std::abs(RejectedTransform->Translation.y - BeforeRejectedEditY) < 1.0e-5f);
	PF_CHECK(Tests, Actions.EditCount == 2 && !Actions.Errors.empty());

	const size_t EditsBeforeRuntime = Actions.EditCount;
	Commands.SnapSelectionToSurface(&SceneValue, Workspace, true, ResolveMesh, Actions);
	PF_CHECK(Tests, Actions.EditCount == EditsBeforeRuntime);

	Scene IsolatedScene;
	auto Isolated = IsolatedScene.CreateEntity("Only Selected");
	PF_CHECK(Tests, Isolated.has_value());
	if (!Isolated) return;
	PF_CHECK(Tests, Isolated->SetMeshRenderer(MeshRendererComponent{ SelectedMeshID, std::nullopt }));
	auto IsolatedTransform = Isolated->GetTransform();
	PF_CHECK(Tests, IsolatedTransform.has_value());
	if (!IsolatedTransform) return;
	IsolatedTransform->Translation.y = 3.0f;
	PF_CHECK(Tests, Isolated->SetTransform(*IsolatedTransform));
	EditorSceneWorkspaceState IsolatedWorkspace;
	IsolatedWorkspace.Selection().SelectSingle(Isolated->GetUUID());
	TestSceneCommandActions IsolatedActions;
	Commands.SnapSelectionToSurface(&IsolatedScene, IsolatedWorkspace, false, ResolveMesh, IsolatedActions);
	const auto UnchangedTransform = Isolated->GetTransform();
	PF_CHECK(Tests, UnchangedTransform && std::abs(UnchangedTransform->Translation.y - 3.0f) < 1.0e-5f);
	PF_CHECK(Tests, IsolatedActions.EditCount == 0 && !IsolatedActions.Statuses.empty());
}
